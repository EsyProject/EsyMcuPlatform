/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_can.h"

#include <stdatomic.h>
#include <string.h>

#include "stm32f4xx_hal.h"

#if !defined(CAN1) || !defined(CAN2)
#error "The CAN backend requires the verified dual-bxCAN STM32F4 provider"
#endif

#define STM32F4_CAN_FILTER_COUNT 28u
#define STM32F4_CAN_FILTER_MASK ((1u << STM32F4_CAN_FILTER_COUNT) - 1u)
#define STM32F4_CAN_IDE (1u << 2)
#define STM32F4_CAN_RTR (1u << 1)
#define STM32F4_CAN_STD_SHIFT 21u
#define STM32F4_CAN_EXT_SHIFT 3u
#define STM32F4_CAN_DLC_MASK 0x0fu
#define STM32F4_CAN_RX_COUNT_MASK 0x03u
#define STM32F4_CAN_ALL_EMPTY (CAN_TSR_TME0 | CAN_TSR_TME1 | CAN_TSR_TME2)
#define STM32F4_CAN_ALL_ABORT (CAN_TSR_ABRQ0 | CAN_TSR_ABRQ1 | CAN_TSR_ABRQ2)
#define STM32F4_CAN_CLOCK_MASK (RCC_APB1ENR_CAN1EN | RCC_APB1ENR_CAN2EN)
#define STM32F4_CAN_FILTER_SPLIT_SHIFT 8u
#define STM32F4_CAN_ERROR_WARNING (1u << 0)
#define STM32F4_CAN_ERROR_PASSIVE (1u << 1)
#define STM32F4_CAN_ERROR_BUS_OFF (1u << 2)
#define STM32F4_CAN_LEC_SHIFT 4u
#define STM32F4_CAN_LEC_MASK 0x07u
#define STM32F4_CAN_TEC_SHIFT 16u
#define STM32F4_CAN_REC_SHIFT 24u
#define STM32F4_CAN_ERROR_COUNT_MASK 0xffu

typedef struct {
  CAN_TypeDef *instance;
  uint32_t prior_clock_gates;
  uint32_t poll_limit;
  esy_can_mode_t mode;
  esy_can_state_t state;
  uint8_t phys_index;
  bool rx_overrun;
} stm32f4_can_runtime_t;

typedef struct {
  _Atomic(esy_can_t *) owner;
  stm32f4_can_runtime_t runtime;
} esy_hb_can_ctx_t;

static esy_hb_can_ctx_t s_ctx;

static stm32f4_can_runtime_t *can_context(const esy_can_t *can) {
  if (can == NULL || can->ctx != &s_ctx.runtime ||
      atomic_load_explicit(&s_ctx.owner, memory_order_acquire) != can) {
    return NULL;
  }
  return &s_ctx.runtime;
}

static esy_err_t validate_config(const esy_can_config_t *config) {
  if (config == NULL) return ESY_ERR_INVALID_ARG;
  if (config->fd) return ESY_ERR_NOT_SUPPORTED;
  if (config->phys_index != 1u && config->phys_index != 2u)
    return ESY_ERR_NOT_SUPPORTED;
  if ((config->mode != ESY_CAN_MODE_NORMAL &&
       config->mode != ESY_CAN_MODE_LOOPBACK &&
       config->mode != ESY_CAN_MODE_LISTEN_ONLY) ||
      config->nominal.prescaler == 0u || config->nominal.prescaler > 1024u ||
      config->nominal.segment1 == 0u || config->nominal.segment1 > 16u ||
      config->nominal.segment2 == 0u || config->nominal.segment2 > 8u ||
      config->nominal.sjw == 0u || config->nominal.sjw > 4u ||
      config->nominal.sjw > config->nominal.segment2 ||
      config->poll_limit == 0u || config->poll_limit > ESY_CAN_MAX_POLL_LIMIT) {
    return ESY_ERR_INVALID_ARG;
  }
  return ESY_OK;
}

/* No timer is used: each transition observes at most poll_limit register values.
 * Preemption extends elapsed time but cannot restart an expired transition. */
static esy_err_t wait_register(stm32f4_can_runtime_t *runtime,
                               volatile uint32_t *reg, uint32_t mask,
                               uint32_t expected) {
  for (uint32_t observed = 0u; observed < runtime->poll_limit; ++observed) {
    if ((*reg & mask) == expected) return ESY_OK;
  }
  runtime->state = ESY_CAN_STATE_FAULT;
  return ESY_ERR_TIMEOUT;
}

static void reset_domain(void) {
  __HAL_RCC_CAN2_FORCE_RESET();
  __HAL_RCC_CAN1_FORCE_RESET();
  __HAL_RCC_CAN1_RELEASE_RESET();
  __HAL_RCC_CAN2_RELEASE_RESET();
}

static void disable_filters(uint8_t phys_index) {
  /* FMR reserved bits have nonzero reset values and must be preserved. */
  CAN1->FMR |= CAN_FMR_FINIT;
  MODIFY_REG(CAN1->FMR, CAN_FMR_CAN2SB,
      ((phys_index == 1u ? STM32F4_CAN_FILTER_COUNT : 0u)
       << STM32F4_CAN_FILTER_SPLIT_SHIFT));
  CAN1->FA1R = 0u;
  CAN1->FM1R = 0u;
  CAN1->FS1R = STM32F4_CAN_FILTER_MASK;
  CAN1->FFA1R = 0u;
  CAN1->FMR &= ~CAN_FMR_FINIT;
}

static esy_err_t abort_transmit(stm32f4_can_runtime_t *runtime) {
  runtime->instance->TSR = STM32F4_CAN_ALL_ABORT;
  return wait_register(runtime, &runtime->instance->TSR,
                       STM32F4_CAN_ALL_EMPTY, STM32F4_CAN_ALL_EMPTY);
}

static esy_can_state_t observed_state(stm32f4_can_runtime_t *runtime,
                                     uint32_t error) {
  if (runtime->state == ESY_CAN_STATE_STOPPED ||
      runtime->state == ESY_CAN_STATE_FAULT) {
    return runtime->state;
  }
  if ((error & STM32F4_CAN_ERROR_BUS_OFF) != 0u) {
    if (runtime->state != ESY_CAN_STATE_RECOVERING)
      runtime->state = ESY_CAN_STATE_BUS_OFF;
  } else {
    runtime->state = (error & STM32F4_CAN_ERROR_PASSIVE) != 0u
                         ? ESY_CAN_STATE_ERROR_PASSIVE
                         : ESY_CAN_STATE_ERROR_ACTIVE;
  }
  return runtime->state;
}

esy_err_t esy_hb_can_init(esy_can_t *can, const esy_can_config_t *config) {
  if (can == NULL) return ESY_ERR_INVALID_ARG;
  if (can->ctx != NULL || can->ops != NULL) return ESY_ERR_STATE;
  esy_err_t result = validate_config(config);
  if (result != ESY_OK) return result;
  if (__get_IPSR() != 0u) return ESY_ERR_STATE;

  /* One owner controls the shared clocks, reset and 28-bank filter fabric.
   * The atomic claim prevents another init from resetting an existing owner. */
  esy_can_t *expected = NULL;
  if (!atomic_compare_exchange_strong_explicit(
          &s_ctx.owner, &expected, can, memory_order_acq_rel,
          memory_order_acquire)) {
    return ESY_ERR_BUSY;
  }
  stm32f4_can_runtime_t *runtime = &s_ctx.runtime;
  *runtime = (stm32f4_can_runtime_t){
      .instance = config->phys_index == 1u ? CAN1 : CAN2,
      .prior_clock_gates = RCC->APB1ENR & STM32F4_CAN_CLOCK_MASK,
      .poll_limit = config->poll_limit,
      .mode = config->mode,
      .state = ESY_CAN_STATE_FAULT,
      .phys_index = config->phys_index,
  };
  can->ctx = runtime;
  __HAL_RCC_CAN1_CLK_ENABLE();
  if (config->phys_index == 2u) __HAL_RCC_CAN2_CLK_ENABLE();
  reset_domain();

  CAN_TypeDef *instance = runtime->instance;
  instance->MCR = CAN_MCR_INRQ;
  result = wait_register(runtime, &instance->MSR, CAN_MSR_INAK, CAN_MSR_INAK);
  if (result != ESY_OK) return result;
  instance->IER = 0u;
  instance->MCR = CAN_MCR_INRQ | CAN_MCR_RFLM |
      (config->auto_retransmit ? 0u : CAN_MCR_NART);
  instance->BTR = ((uint32_t)config->nominal.prescaler - 1u) |
      (((uint32_t)config->nominal.segment1 - 1u) << 16u) |
      (((uint32_t)config->nominal.segment2 - 1u) << 20u) |
      (((uint32_t)config->nominal.sjw - 1u) << 24u) |
      (config->mode == ESY_CAN_MODE_LOOPBACK ? CAN_BTR_LBKM : 0u) |
      (config->mode == ESY_CAN_MODE_LISTEN_ONLY ? CAN_BTR_SILM : 0u);
  disable_filters(config->phys_index);
  runtime->state = ESY_CAN_STATE_STOPPED;
  return ESY_OK;
}

esy_err_t esy_hb_can_start(esy_can_t *can) {
  if (can == NULL) return ESY_ERR_INVALID_ARG;
  stm32f4_can_runtime_t *runtime = can_context(can);
  if (runtime == NULL || __get_IPSR() != 0u ||
      runtime->state != ESY_CAN_STATE_STOPPED) return ESY_ERR_STATE;
  runtime->instance->MCR &= ~(CAN_MCR_INRQ | CAN_MCR_SLEEP);
  esy_err_t result = wait_register(runtime, &runtime->instance->MSR,
                                   CAN_MSR_INAK, 0u);
  if (result == ESY_OK) runtime->state = ESY_CAN_STATE_ERROR_ACTIVE;
  return result;
}

esy_err_t esy_hb_can_stop(esy_can_t *can) {
  if (can == NULL) return ESY_ERR_INVALID_ARG;
  stm32f4_can_runtime_t *runtime = can_context(can);
  if (runtime == NULL || __get_IPSR() != 0u) return ESY_ERR_STATE;
  if (runtime->state == ESY_CAN_STATE_STOPPED) return ESY_OK;
  esy_err_t result = abort_transmit(runtime);
  if (result != ESY_OK) return result;
  runtime->instance->MCR =
      (runtime->instance->MCR & ~CAN_MCR_SLEEP) | CAN_MCR_INRQ;
  result = wait_register(runtime, &runtime->instance->MSR,
                         CAN_MSR_INAK, CAN_MSR_INAK);
  if (result == ESY_OK) runtime->state = ESY_CAN_STATE_STOPPED;
  return result;
}

esy_err_t esy_hb_can_deinit(esy_can_t *can) {
  if (can == NULL) return ESY_ERR_INVALID_ARG;
  if (can->ctx == NULL && can->ops == NULL) return ESY_OK;
  stm32f4_can_runtime_t *runtime = can_context(can);
  if (runtime == NULL || __get_IPSR() != 0u) return ESY_ERR_STATE;
  esy_err_t result = esy_hb_can_stop(can);
  if (result != ESY_OK) return result;
  runtime->instance->IER = 0u;
  reset_domain();
  MODIFY_REG(RCC->APB1ENR, STM32F4_CAN_CLOCK_MASK, runtime->prior_clock_gates);
  (void)RCC->APB1ENR;
  *runtime = (stm32f4_can_runtime_t){0};
  can->ctx = NULL;
  atomic_store_explicit(&s_ctx.owner, NULL, memory_order_release);
  return ESY_OK;
}

esy_err_t esy_hb_can_send(esy_can_t *can, const esy_can_frame_t *frame) {
  if (can == NULL || frame == NULL) return ESY_ERR_INVALID_ARG;
  if (frame->fd || frame->brs) return ESY_ERR_NOT_SUPPORTED;
  if (frame->length > ESY_CAN_CLASSIC_MAX_DATA || frame->esi ||
      frame->id > (frame->extended ? ESY_CAN_EXT_ID_MAX : ESY_CAN_STD_ID_MAX))
    return ESY_ERR_INVALID_ARG;
  stm32f4_can_runtime_t *runtime = can_context(can);
  if (runtime == NULL || __get_IPSR() != 0u) return ESY_ERR_STATE;
  esy_can_state_t state = observed_state(runtime, runtime->instance->ESR);
  if ((state != ESY_CAN_STATE_ERROR_ACTIVE &&
       state != ESY_CAN_STATE_ERROR_PASSIVE) ||
      runtime->mode == ESY_CAN_MODE_LISTEN_ONLY) return ESY_ERR_STATE;

  uint32_t status = runtime->instance->TSR;
  uint32_t mailbox;
  if ((status & CAN_TSR_TME0) != 0u) mailbox = 0u;
  else if ((status & CAN_TSR_TME1) != 0u) mailbox = 1u;
  else if ((status & CAN_TSR_TME2) != 0u) mailbox = 2u;
  else return ESY_ERR_BUSY;
  CAN_TxMailBox_TypeDef *tx = &runtime->instance->sTxMailBox[mailbox];
  uint8_t data[ESY_CAN_CLASSIC_MAX_DATA] = {0};
  if (!frame->rtr) memcpy(data, frame->data, frame->length);
  tx->TIR = (frame->id << (frame->extended ? STM32F4_CAN_EXT_SHIFT
                                                        : STM32F4_CAN_STD_SHIFT)) |
      (frame->extended ? STM32F4_CAN_IDE : 0u) |
      (frame->rtr ? STM32F4_CAN_RTR : 0u);
  tx->TDTR = frame->length;
  tx->TDLR = (uint32_t)data[0] | ((uint32_t)data[1] << 8u) |
      ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
  tx->TDHR = (uint32_t)data[4] | ((uint32_t)data[5] << 8u) |
      ((uint32_t)data[6] << 16u) | ((uint32_t)data[7] << 24u);
  tx->TIR |= CAN_TI0R_TXRQ;
  return ESY_OK;
}

esy_err_t esy_hb_can_receive(esy_can_t *can, esy_can_frame_t *frame) {
  if (can == NULL || frame == NULL) return ESY_ERR_INVALID_ARG;
  stm32f4_can_runtime_t *runtime = can_context(can);
  if (runtime == NULL || __get_IPSR() != 0u ||
      runtime->state == ESY_CAN_STATE_FAULT) return ESY_ERR_STATE;
  uint32_t fifo = runtime->instance->RF0R;
  runtime->rx_overrun |= (fifo & CAN_RF0R_FOVR0) != 0u;
  if ((fifo & STM32F4_CAN_RX_COUNT_MASK) == 0u) return ESY_ERR_BUSY;
  CAN_FIFOMailBox_TypeDef *rx = &runtime->instance->sFIFOMailBox[0];
  uint32_t identifier = rx->RIR;
  uint32_t length = rx->RDTR & STM32F4_CAN_DLC_MASK;
  uint32_t low = rx->RDLR;
  uint32_t high = rx->RDHR;
  runtime->instance->RF0R = CAN_RF0R_RFOM0;
  /* Classical wire DLC 9..15 also represents eight payload/request bytes. */
  if (length > ESY_CAN_CLASSIC_MAX_DATA) length = ESY_CAN_CLASSIC_MAX_DATA;

  esy_can_frame_t value = {0};
  value.extended = (identifier & STM32F4_CAN_IDE) != 0u;
  value.rtr = (identifier & STM32F4_CAN_RTR) != 0u;
  value.id = identifier >> (value.extended ? STM32F4_CAN_EXT_SHIFT
                                         : STM32F4_CAN_STD_SHIFT);
  value.length = (uint8_t)length;
  if (!value.rtr) {
    for (uint8_t i = 0u; i < value.length; ++i) {
      value.data[i] = (uint8_t)((i < 4u ? low : high) >> ((i % 4u) * 8u));
    }
  }
  *frame = value;
  return ESY_OK;
}

esy_err_t esy_hb_can_set_filters(esy_can_t *can,
                                const esy_can_filter_t *filters, size_t count) {
  if (can == NULL || (filters == NULL && count != 0u) ||
      count > STM32F4_CAN_FILTER_COUNT) return ESY_ERR_INVALID_ARG;
  for (size_t i = 0u; i < count; ++i) {
    uint32_t limit = filters[i].extended ? ESY_CAN_EXT_ID_MAX : ESY_CAN_STD_ID_MAX;
    if (filters[i].id > limit || filters[i].mask > limit)
      return ESY_ERR_INVALID_ARG;
  }
  stm32f4_can_runtime_t *runtime = can_context(can);
  if (runtime == NULL || __get_IPSR() != 0u ||
      runtime->state != ESY_CAN_STATE_STOPPED) return ESY_ERR_STATE;
  disable_filters(runtime->phys_index);
  CAN1->FMR |= CAN_FMR_FINIT;
  for (size_t i = 0u; i < count; ++i) {
    uint32_t shift = filters[i].extended ? STM32F4_CAN_EXT_SHIFT
                                        : STM32F4_CAN_STD_SHIFT;
    CAN1->sFilterRegister[i].FR1 = (filters[i].id << shift) |
        (filters[i].extended ? STM32F4_CAN_IDE : 0u) |
        (filters[i].rtr ? STM32F4_CAN_RTR : 0u);
    CAN1->sFilterRegister[i].FR2 = (filters[i].mask << shift) |
        STM32F4_CAN_IDE | (filters[i].match_rtr ? STM32F4_CAN_RTR : 0u);
  }
  CAN1->FA1R = count == 0u ? 0u : (1u << count) - 1u;
  CAN1->FMR &= ~CAN_FMR_FINIT;
  return ESY_OK;
}

esy_err_t esy_hb_can_get_status(esy_can_t *can, esy_can_status_t *status) {
  if (can == NULL || status == NULL) return ESY_ERR_INVALID_ARG;
  stm32f4_can_runtime_t *runtime = can_context(can);
  if (runtime == NULL || __get_IPSR() != 0u) return ESY_ERR_STATE;
  uint32_t error = runtime->instance->ESR;
  uint32_t fifo = runtime->instance->RF0R;
  uint32_t transmit = runtime->instance->TSR;
  runtime->rx_overrun |= (fifo & CAN_RF0R_FOVR0) != 0u;
  esy_can_status_t value = {
      .state = observed_state(runtime, error),
      .tx_error_count = (uint16_t)((error >> STM32F4_CAN_TEC_SHIFT) &
                                  STM32F4_CAN_ERROR_COUNT_MASK),
      .rx_error_count = (uint16_t)((error >> STM32F4_CAN_REC_SHIFT) &
                                  STM32F4_CAN_ERROR_COUNT_MASK),
      .last_error = (uint8_t)((error >> STM32F4_CAN_LEC_SHIFT) & STM32F4_CAN_LEC_MASK),
      .warning = (error & STM32F4_CAN_ERROR_WARNING) != 0u,
      .rx_overrun = runtime->rx_overrun,
      .tx_pending = (uint8_t)(((transmit & CAN_TSR_TME0) == 0u) +
                              ((transmit & CAN_TSR_TME1) == 0u) +
                              ((transmit & CAN_TSR_TME2) == 0u)),
      .rx_pending = (uint8_t)(fifo & STM32F4_CAN_RX_COUNT_MASK),
  };
  *status = value;
  return ESY_OK;
}

esy_err_t esy_hb_can_recover(esy_can_t *can) {
  if (can == NULL) return ESY_ERR_INVALID_ARG;
  stm32f4_can_runtime_t *runtime = can_context(can);
  if (runtime == NULL || __get_IPSR() != 0u) return ESY_ERR_STATE;
  if (runtime->state == ESY_CAN_STATE_RECOVERING) return ESY_ERR_BUSY;
  if (runtime->state == ESY_CAN_STATE_STOPPED ||
      runtime->state == ESY_CAN_STATE_FAULT ||
      (runtime->instance->ESR & STM32F4_CAN_ERROR_BUS_OFF) == 0u)
    return ESY_ERR_STATE;
  esy_err_t result = abort_transmit(runtime);
  if (result != ESY_OK) return result;
  runtime->instance->MCR |= CAN_MCR_INRQ;
  result = wait_register(runtime, &runtime->instance->MSR,
                         CAN_MSR_INAK, CAN_MSR_INAK);
  if (result != ESY_OK) return result;
  runtime->instance->MCR &= ~CAN_MCR_INRQ;
  result = wait_register(runtime, &runtime->instance->MSR, CAN_MSR_INAK, 0u);
  if (result == ESY_OK) runtime->state = ESY_CAN_STATE_RECOVERING;
  return result;
}

esy_err_t esy_hb_can_get_caps(const esy_can_t *can, uint32_t *caps) {
  if (can == NULL || caps == NULL) return ESY_ERR_INVALID_ARG;
  if (can_context(can) == NULL || __get_IPSR() != 0u) return ESY_ERR_STATE;
  *caps = ESY_CAN_CAP_LOOPBACK | ESY_CAN_CAP_LISTEN_ONLY |
      ESY_CAN_CAP_FILTER | ESY_CAN_CAP_RECOVERY;
  return ESY_OK;
}
