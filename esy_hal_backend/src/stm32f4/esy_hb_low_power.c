/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_low_power.h"

#include "stm32f4xx_hal.h"

#define ESY_STM32F4_US_PER_MS         1000u
#define ESY_STM32F4_CYCLES_PER_SECOND 1000000u
#define ESY_STM32F4_HSE_TIMEOUT_US \
  ((uint32_t)HSE_TIMEOUT_VALUE * ESY_STM32F4_US_PER_MS)
#define ESY_STM32F4_HSI_TIMEOUT_US \
  ((uint32_t)HSI_TIMEOUT_VALUE * ESY_STM32F4_US_PER_MS)
#define ESY_STM32F4_PLL_TIMEOUT_US          2000u
#define ESY_STM32F4_CLOCK_SWITCH_TIMEOUT_US 1000u
#define ESY_STM32F4_RESTORE_WAIT_US                                 \
  (ESY_STM32F4_HSE_TIMEOUT_US + (2u * ESY_STM32F4_HSI_TIMEOUT_US) + \
   (2u * ESY_STM32F4_PLL_TIMEOUT_US) + ESY_STM32F4_CLOCK_SWITCH_TIMEOUT_US)
#define ESY_STM32F4_RESTORE_GUARD_US \
  ((ESY_STM32F4_RESTORE_WAIT_US / 10u) + 1000u)
#define ESY_STM32F4_STOP_EXIT_LATENCY_US \
  (ESY_STM32F4_RESTORE_WAIT_US + ESY_STM32F4_RESTORE_GUARD_US)

typedef struct {
  RCC_OscInitTypeDef saved_oscillator_config;
  RCC_ClkInitTypeDef saved_clock_config;
  uint32_t saved_flash_latency;
  uint32_t saved_pll_config;
  uint32_t saved_hclk_hz;
  esy_hb_low_power_mode_t prepared_mode;
  bool prepared;
} esy_hb_low_power_ctx_t;

typedef struct {
  uint32_t saved_demcr;
  uint32_t saved_dwt_ctrl;
} esy_hb_low_power_cycle_timer_t;

typedef struct {
  uint32_t start_cycle;
  uint32_t timeout_cycles;
  uint32_t remaining_iterations;
} esy_hb_low_power_poll_budget_t;

static esy_hb_low_power_ctx_t s_ctx;

static void cycle_timer_stop(const esy_hb_low_power_cycle_timer_t *timer) {
  if ((timer->saved_dwt_ctrl & DWT_CTRL_CYCCNTENA_Msk) == 0u) {
    DWT->CTRL &= ~DWT_CTRL_CYCCNTENA_Msk;
  }
  if ((timer->saved_demcr & CoreDebug_DEMCR_TRCENA_Msk) == 0u) {
    CoreDebug->DEMCR &= ~CoreDebug_DEMCR_TRCENA_Msk;
  }
}

static bool cycle_timer_start(esy_hb_low_power_cycle_timer_t *timer) {
  timer->saved_demcr = CoreDebug->DEMCR;
  timer->saved_dwt_ctrl = DWT->CTRL;

  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0u) {
    cycle_timer_stop(timer);
    return false;
  }

  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  __DSB();
  __ISB();

  const uint32_t start_cycle = DWT->CYCCNT;
  __NOP();
  __NOP();
  __NOP();
  __NOP();
  if (DWT->CYCCNT == start_cycle) {
    cycle_timer_stop(timer);
    return false;
  }
  return true;
}

static uint32_t timeout_cycles(uint32_t timeout_us, uint32_t hclk_hz) {
  const uint64_t scaled_cycles = (uint64_t)hclk_hz * timeout_us;
  const uint64_t cycles = (scaled_cycles + ESY_STM32F4_CYCLES_PER_SECOND - 1u) /
                          ESY_STM32F4_CYCLES_PER_SECOND;
  if (cycles == 0u) return 1u;
  return (cycles > UINT32_MAX) ? UINT32_MAX : (uint32_t)cycles;
}

static esy_hb_low_power_poll_budget_t poll_budget_start(uint32_t timeout_us,
                                                            uint32_t hclk_hz) {
  const uint32_t budget_cycles = timeout_cycles(timeout_us, hclk_hz);
  return (esy_hb_low_power_poll_budget_t){
      .start_cycle = DWT->CYCCNT,
      .timeout_cycles = budget_cycles,
      .remaining_iterations = budget_cycles,
  };
}

static bool poll_budget_expired(esy_hb_low_power_poll_budget_t *budget) {
  if ((uint32_t)(DWT->CYCCNT - budget->start_cycle) >= budget->timeout_cycles) {
    return true;
  }
  if (budget->remaining_iterations == 0u) return true;
  budget->remaining_iterations--;
  return false;
}

static bool wait_for_rcc_flag(uint32_t flag, FlagStatus expected,
                              uint32_t timeout_us, uint32_t hclk_hz) {
  esy_hb_low_power_poll_budget_t budget =
      poll_budget_start(timeout_us, hclk_hz);
  while (__HAL_RCC_GET_FLAG(flag) != expected) {
    if (poll_budget_expired(&budget)) return false;
  }
  return true;
}

static bool wait_for_sysclk_source(uint32_t source, uint32_t min_hclk_hz) {
  esy_hb_low_power_poll_budget_t budget =
      poll_budget_start(ESY_STM32F4_CLOCK_SWITCH_TIMEOUT_US, min_hclk_hz);
  const uint32_t expected_source = source << RCC_CFGR_SWS_Pos;
  while (__HAL_RCC_GET_SYSCLK_SOURCE() != expected_source) {
    if (poll_budget_expired(&budget)) return false;
  }
  return true;
}

static uint32_t hsi_hclk_hz(void) {
  const uint32_t ahb_prescaler_index =
      (RCC->CFGR & RCC_CFGR_HPRE) >> RCC_CFGR_HPRE_Pos;
  return HSI_VALUE >> AHBPrescTable[ahb_prescaler_index];
}

esy_err_t esy_hb_low_power_get_capabilities(
    esy_hb_low_power_capabilities_t *out_capabilities) {
  if (out_capabilities == NULL) return ESY_ERR_INVALID_ARG;
  *out_capabilities = (esy_hb_low_power_capabilities_t){
      .supported_mode_mask =
          ESY_HB_LOW_POWER_MODE_MASK(ESY_HB_LOW_POWER_MODE_SLEEP) |
          ESY_HB_LOW_POWER_MODE_MASK(ESY_HB_LOW_POWER_MODE_STOP),
      .entry_latency_us = {1u, 50u},
      .exit_latency_us = {1u, ESY_STM32F4_STOP_EXIT_LATENCY_US},
      .min_residency_us = {0u, 1000u},
  };
  return ESY_OK;
}

esy_err_t esy_hb_low_power_prepare(esy_hb_low_power_mode_t mode) {
  if (mode >= ESY_HB_LOW_POWER_MODE_COUNT) {
    return ESY_ERR_INVALID_ARG;
  }
  if (s_ctx.prepared) return ESY_ERR_STATE;
  if (mode == ESY_HB_LOW_POWER_MODE_STOP) {
    HAL_RCC_GetOscConfig(&s_ctx.saved_oscillator_config);
    HAL_RCC_GetClockConfig(&s_ctx.saved_clock_config,
                           &s_ctx.saved_flash_latency);
    s_ctx.saved_pll_config = RCC->PLLCFGR;
    s_ctx.saved_hclk_hz = HAL_RCC_GetHCLKFreq();
  }
  s_ctx.prepared_mode = mode;
  s_ctx.prepared = true;
  return ESY_OK;
}

static esy_err_t restore_high_speed_oscillators(void) {
  const uint32_t restore_hclk_hz = hsi_hclk_hz();
  __HAL_RCC_HSI_ENABLE();
  if (!wait_for_rcc_flag(RCC_FLAG_HSIRDY, SET, ESY_STM32F4_HSI_TIMEOUT_US,
                         restore_hclk_hz)) {
    return ESY_ERR_TIMEOUT;
  }
  __HAL_RCC_HSI_CALIBRATIONVALUE_ADJUST(
      s_ctx.saved_oscillator_config.HSICalibrationValue);

  __HAL_RCC_HSE_CONFIG(s_ctx.saved_oscillator_config.HSEState);
  const FlagStatus expected_hse_state =
      (s_ctx.saved_oscillator_config.HSEState == RCC_HSE_OFF) ? RESET : SET;
  if (!wait_for_rcc_flag(RCC_FLAG_HSERDY, expected_hse_state,
                         ESY_STM32F4_HSE_TIMEOUT_US, restore_hclk_hz)) {
    return ESY_ERR_TIMEOUT;
  }

  __HAL_RCC_PLL_DISABLE();
  if (!wait_for_rcc_flag(RCC_FLAG_PLLRDY, RESET, ESY_STM32F4_PLL_TIMEOUT_US,
                         restore_hclk_hz)) {
    return ESY_ERR_TIMEOUT;
  }
  WRITE_REG(RCC->PLLCFGR, s_ctx.saved_pll_config);
  if (s_ctx.saved_oscillator_config.PLL.PLLState == RCC_PLL_ON) {
    __HAL_RCC_PLL_ENABLE();
    if (!wait_for_rcc_flag(RCC_FLAG_PLLRDY, SET, ESY_STM32F4_PLL_TIMEOUT_US,
                           restore_hclk_hz)) {
      return ESY_ERR_TIMEOUT;
    }
  }
  return ESY_OK;
}

static esy_err_t restore_bus_clocks(void) {
  const RCC_ClkInitTypeDef *clock_config = &s_ctx.saved_clock_config;
  if (s_ctx.saved_flash_latency > __HAL_FLASH_GET_LATENCY()) {
    __HAL_FLASH_SET_LATENCY(s_ctx.saved_flash_latency);
    if (__HAL_FLASH_GET_LATENCY() != s_ctx.saved_flash_latency) {
      return ESY_ERR_IO;
    }
  }

  if ((clock_config->ClockType & RCC_CLOCKTYPE_HCLK) != 0u) {
    if ((clock_config->ClockType & RCC_CLOCKTYPE_PCLK1) != 0u) {
      MODIFY_REG(RCC->CFGR, RCC_CFGR_PPRE1, RCC_HCLK_DIV16);
    }
    if ((clock_config->ClockType & RCC_CLOCKTYPE_PCLK2) != 0u) {
      MODIFY_REG(RCC->CFGR, RCC_CFGR_PPRE2, RCC_HCLK_DIV16 << 3u);
    }
    MODIFY_REG(RCC->CFGR, RCC_CFGR_HPRE, clock_config->AHBCLKDivider);
  }

  if ((clock_config->ClockType & RCC_CLOCKTYPE_SYSCLK) != 0u) {
    const uint32_t pre_switch_hclk_hz = hsi_hclk_hz();
    const uint32_t min_switch_hclk_hz =
        (pre_switch_hclk_hz < s_ctx.saved_hclk_hz) ? pre_switch_hclk_hz
                                                   : s_ctx.saved_hclk_hz;
    __HAL_RCC_SYSCLK_CONFIG(clock_config->SYSCLKSource);
    if (!wait_for_sysclk_source(clock_config->SYSCLKSource,
                                min_switch_hclk_hz)) {
      return ESY_ERR_TIMEOUT;
    }
  }

  if (s_ctx.saved_flash_latency < __HAL_FLASH_GET_LATENCY()) {
    __HAL_FLASH_SET_LATENCY(s_ctx.saved_flash_latency);
    if (__HAL_FLASH_GET_LATENCY() != s_ctx.saved_flash_latency) {
      return ESY_ERR_IO;
    }
  }

  if ((clock_config->ClockType & RCC_CLOCKTYPE_PCLK1) != 0u) {
    MODIFY_REG(RCC->CFGR, RCC_CFGR_PPRE1, clock_config->APB1CLKDivider);
  }
  if ((clock_config->ClockType & RCC_CLOCKTYPE_PCLK2) != 0u) {
    MODIFY_REG(RCC->CFGR, RCC_CFGR_PPRE2, clock_config->APB2CLKDivider << 3u);
  }

  SystemCoreClockUpdate();
  return ESY_OK;
}

static esy_err_t restore_hsi_state(void) {
  if (s_ctx.saved_oscillator_config.HSIState != RCC_HSI_OFF) return ESY_OK;
  if (__HAL_RCC_GET_SYSCLK_SOURCE() == RCC_CFGR_SWS_HSI) {
    return ESY_ERR_IO;
  }
  if ((s_ctx.saved_oscillator_config.PLL.PLLState == RCC_PLL_ON) &&
      ((s_ctx.saved_pll_config & RCC_PLLCFGR_PLLSRC) ==
       RCC_PLLCFGR_PLLSRC_HSI)) {
    return ESY_ERR_IO;
  }

  __HAL_RCC_HSI_DISABLE();
  if (!wait_for_rcc_flag(RCC_FLAG_HSIRDY, RESET, ESY_STM32F4_HSI_TIMEOUT_US,
                         s_ctx.saved_hclk_hz)) {
    return ESY_ERR_TIMEOUT;
  }
  return ESY_OK;
}

static esy_err_t restore_stop_clocks(void) {
  esy_hb_low_power_cycle_timer_t timer;
  if (!cycle_timer_start(&timer)) return ESY_ERR_IO;

  esy_err_t result = restore_high_speed_oscillators();
  if (result == ESY_OK) result = restore_bus_clocks();
  if (result == ESY_OK) result = restore_hsi_state();

  cycle_timer_stop(&timer);
  if (result != ESY_OK) return result;
  return ESY_OK;
}

esy_err_t esy_hb_low_power_enter(
    esy_hb_low_power_mode_t mode,
    esy_hb_low_power_result_t *out_result) {
  if (out_result == NULL) return ESY_ERR_INVALID_ARG;
  *out_result = (esy_hb_low_power_result_t){0};
  if (!s_ctx.prepared || (s_ctx.prepared_mode != mode)) {
    return ESY_ERR_STATE;
  }
  switch (mode) {
    case ESY_HB_LOW_POWER_MODE_SLEEP:
      __DSB();
      HAL_PWR_EnterSLEEPMode(PWR_MAINREGULATOR_ON, PWR_SLEEPENTRY_WFI);
      __ISB();
      out_result->entered = true;
      s_ctx.prepared = false;
      return ESY_OK;
    case ESY_HB_LOW_POWER_MODE_STOP:
      __DSB();
      HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);
      __ISB();
      out_result->entered = true;
      out_result->fatal_error = restore_stop_clocks();
      s_ctx.prepared = false;
      if (out_result->fatal_error != ESY_OK) {
        return out_result->fatal_error;
      }
      return ESY_OK;
    default:
      return ESY_ERR_INVALID_ARG;
  }
}

esy_err_t esy_hb_low_power_cancel(esy_hb_low_power_mode_t mode) {
  if (mode >= ESY_HB_LOW_POWER_MODE_COUNT) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!s_ctx.prepared || (s_ctx.prepared_mode != mode)) {
    return ESY_ERR_STATE;
  }
  s_ctx.prepared = false;
  return ESY_OK;
}

esy_err_t esy_hb_shutdown(esy_hb_shutdown_mode_t mode) {
  if (mode != ESY_HB_SHUTDOWN_MODE_STANDBY) {
    return ESY_ERR_INVALID_ARG;
  }
  __DSB();
  HAL_PWR_EnterSTANDBYMode();
  return ESY_ERR_STATE;
}
