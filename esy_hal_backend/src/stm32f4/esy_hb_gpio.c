/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_gpio.h"

#include "esy_chip/stm32f4_gpio_types.h"
#include "stm32f4xx_hal.h"

#define STM32F4_GPIO_PINS_PER_PORT   16u
#define STM32F4_GPIO_PORT_COUNT      9u
#define STM32F4_GPIO_EXTI_LINE_COUNT STM32F4_GPIO_PINS_PER_PORT
#define STM32F4_GPIO_MAX_PIN \
  (STM32F4_GPIO_PORT_COUNT * STM32F4_GPIO_PINS_PER_PORT)
#define PIN2PORT(pin) \
  ((GPIO_TypeDef *)(GPIOA_BASE + 0x400U * STM32F4_GPIO_PORT(pin)))

typedef struct {
  esy_gpio_irq_handler_t handler;
  void *arg;
  uint8_t pin;
  uint8_t priority;
  bool enabled;
  bool wake_source;
} gpio_irq_line_t;

typedef struct {
  gpio_irq_line_t lines[STM32F4_GPIO_EXTI_LINE_COUNT];
} esy_hb_gpio_ctx_t;

static esy_hb_gpio_ctx_t s_ctx;

static uint32_t get_pin_mask(uint8_t pin) {
  return (1U << STM32F4_GPIO_PIN(pin));
}

static bool is_valid_pin(uint8_t pin) {
  GPIO_TypeDef *port = PIN2PORT(pin);

  return (pin < STM32F4_GPIO_MAX_PIN) && IS_GPIO_ALL_INSTANCE(port);
}

static esy_err_t enable_port_clock(GPIO_TypeDef *port) {
  if (port == GPIOA)
    __HAL_RCC_GPIOA_CLK_ENABLE();
  else if (port == GPIOB)
    __HAL_RCC_GPIOB_CLK_ENABLE();
  else if (port == GPIOC)
    __HAL_RCC_GPIOC_CLK_ENABLE();
  else if (port == GPIOD)
    __HAL_RCC_GPIOD_CLK_ENABLE();
  else if (port == GPIOE)
    __HAL_RCC_GPIOE_CLK_ENABLE();
#ifdef GPIOF
  else if (port == GPIOF)
    __HAL_RCC_GPIOF_CLK_ENABLE();
#endif
#ifdef GPIOG
  else if (port == GPIOG)
    __HAL_RCC_GPIOG_CLK_ENABLE();
#endif
  else if (port == GPIOH)
    __HAL_RCC_GPIOH_CLK_ENABLE();
#ifdef GPIOI
  else if (port == GPIOI)
    __HAL_RCC_GPIOI_CLK_ENABLE();
#endif
  else
    return ESY_ERR_INVALID_ARG;

  return ESY_OK;
}

static IRQn_Type get_irqn(uint8_t pin) {
  switch (STM32F4_GPIO_PIN(pin)) {
    case 0:
      return EXTI0_IRQn;
    case 1:
      return EXTI1_IRQn;
    case 2:
      return EXTI2_IRQn;
    case 3:
      return EXTI3_IRQn;
    case 4:
      return EXTI4_IRQn;
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
      return EXTI9_5_IRQn;
    default:
      return EXTI15_10_IRQn;
  }
}

/* Task-side GPIO register/binding updates share state with EXTI dispatch and
 * other task owners. PRIMASK protects the short register RMW/publication window;
 * atomics alone cannot publish an EXTI route and its callback as one operation.
 * An ISR cannot perform lifecycle operations or wait for another callback. */
static uint32_t gpio_lock(void) {
  uint32_t key = __get_PRIMASK();
  __disable_irq();
  return key;
}

static void gpio_unlock(uint32_t key) {
  __set_PRIMASK(key);
}

static bool gpio_group_has_owner(IRQn_Type irqn) {
  for (uint8_t i = 0u; i < STM32F4_GPIO_EXTI_LINE_COUNT; ++i) {
    if (s_ctx.lines[i].handler != NULL && get_irqn(i) == irqn) return true;
  }
  return false;
}

static bool gpio_owns_line(uint8_t pin) {
  const gpio_irq_line_t *line = &s_ctx.lines[STM32F4_GPIO_PIN(pin)];
  return line->handler != NULL && line->pin == pin;
}

static void gpio_mask_line(uint8_t pin) {
  uint32_t mask = get_pin_mask(pin);
  EXTI->IMR &= ~mask;
  __HAL_GPIO_EXTI_CLEAR_IT(mask);
  s_ctx.lines[STM32F4_GPIO_PIN(pin)].enabled = false;
}

/* Called with IRQs masked from task context. Peers retain their pending bits,
 * binding and NVIC state; only the final registered owner retires the group. */
static void gpio_detach_line(uint8_t pin) {
  if (!gpio_owns_line(pin)) return;
  gpio_mask_line(pin);
  s_ctx.lines[STM32F4_GPIO_PIN(pin)] = (gpio_irq_line_t){0};
  IRQn_Type irqn = get_irqn(pin);
  if (!gpio_group_has_owner(irqn)) {
    HAL_NVIC_DisableIRQ(irqn);
    HAL_NVIC_ClearPendingIRQ(irqn);
  }
}

esy_err_t esy_hb_gpio_init(uint8_t pin, esy_gpio_mode_t mode) {
  GPIO_InitTypeDef init = {0};
  if (!is_valid_pin(pin)) return ESY_ERR_INVALID_ARG;
  if (__get_IPSR() != 0u) return ESY_ERR_STATE;
  init.Pin = get_pin_mask(pin);
  switch (mode) {
    case ESY_GPIO_MODE_INPUT:
      init.Mode = GPIO_MODE_INPUT;
      init.Pull = GPIO_NOPULL;
      break;
    case ESY_GPIO_MODE_INPUT_PULLUP:
      init.Mode = GPIO_MODE_INPUT;
      init.Pull = GPIO_PULLUP;
      break;
    case ESY_GPIO_MODE_INPUT_PULLDOWN:
      init.Mode = GPIO_MODE_INPUT;
      init.Pull = GPIO_PULLDOWN;
      break;
    case ESY_GPIO_MODE_OUTPUT:
      init.Mode = GPIO_MODE_OUTPUT_PP;
      init.Pull = GPIO_NOPULL;
      init.Speed = GPIO_SPEED_FREQ_LOW;
      break;
    default:
      return ESY_ERR_INVALID_ARG;
  }
  uint32_t key = gpio_lock();
  esy_err_t result = enable_port_clock(PIN2PORT(pin));
  if (result == ESY_OK) HAL_GPIO_Init(PIN2PORT(pin), &init);
  gpio_unlock(key);
  return result;
}

void esy_hb_gpio_deinit(uint8_t pin) {
  if (!is_valid_pin(pin) || __get_IPSR() != 0u) return;
  uint32_t key = gpio_lock();
  gpio_detach_line(pin);
  /* The native provider changes EXTI only if EXTICR still selects this port.
   * A normal GPIO on another port of the same line cannot detach its peer. */
  HAL_GPIO_DeInit(PIN2PORT(pin), get_pin_mask(pin));
  gpio_unlock(key);
}

void esy_hb_gpio_write(uint8_t pin, bool value) {
  if (!is_valid_pin(pin)) return;
  HAL_GPIO_WritePin(PIN2PORT(pin), get_pin_mask(pin),
                    value ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

bool esy_hb_gpio_read(uint8_t pin) {
  if (!is_valid_pin(pin)) return false;
  return HAL_GPIO_ReadPin(PIN2PORT(pin), get_pin_mask(pin)) == GPIO_PIN_SET;
}

static esy_err_t gpio_attach_irq(const esy_gpio_irq_config_t *config,
                                 bool wake_source) {
  if (config == NULL || config->handler == NULL || !is_valid_pin(config->pin) ||
      config->priority >= (1u << __NVIC_PRIO_BITS)) return ESY_ERR_INVALID_ARG;
  if (__get_IPSR() != 0u) return ESY_ERR_STATE;
  GPIO_InitTypeDef init = {.Pin = get_pin_mask(config->pin)};
  switch (config->irq_mode) {
    case ESY_GPIO_IRQ_RISING: init.Mode = GPIO_MODE_IT_RISING; break;
    case ESY_GPIO_IRQ_FALLING: init.Mode = GPIO_MODE_IT_FALLING; break;
    case ESY_GPIO_IRQ_BOTH: init.Mode = GPIO_MODE_IT_RISING_FALLING; break;
    default: return ESY_ERR_INVALID_ARG;
  }
  switch (config->pull) {
    case ESY_GPIO_PULL_NONE: init.Pull = GPIO_NOPULL; break;
    case ESY_GPIO_PULL_UP: init.Pull = GPIO_PULLUP; break;
    case ESY_GPIO_PULL_DOWN: init.Pull = GPIO_PULLDOWN; break;
    default: return ESY_ERR_INVALID_ARG;
  }

  uint32_t key = gpio_lock();
  uint8_t index = STM32F4_GPIO_PIN(config->pin);
  IRQn_Type irqn = get_irqn(config->pin);
  if (s_ctx.lines[index].handler != NULL) {
    gpio_unlock(key);
    return ESY_ERR_STATE;
  }
  bool first_owner = true;
  for (uint8_t i = 0u; i < STM32F4_GPIO_EXTI_LINE_COUNT; ++i) {
    const gpio_irq_line_t *peer = &s_ctx.lines[i];
    if (peer->handler == NULL || get_irqn(i) != irqn) continue;
    first_owner = false;
    if (peer->priority != config->priority) {
      gpio_unlock(key);
      return ESY_ERR_STATE;
    }
  }
  esy_err_t result = enable_port_clock(PIN2PORT(config->pin));
  if (result != ESY_OK) {
    gpio_unlock(key);
    return result;
  }
  EXTI->IMR &= ~get_pin_mask(config->pin);
  __HAL_GPIO_EXTI_CLEAR_IT(get_pin_mask(config->pin));
  HAL_GPIO_Init(PIN2PORT(config->pin), &init);
  s_ctx.lines[index] = (gpio_irq_line_t){
      .handler = config->handler, .arg = config->arg, .pin = config->pin,
      .priority = config->priority, .enabled = true, .wake_source = wake_source,
  };
  if (first_owner) {
    HAL_NVIC_ClearPendingIRQ(irqn);
    HAL_NVIC_SetPriority(irqn, config->priority, 0u);
    HAL_NVIC_EnableIRQ(irqn);
  }
  gpio_unlock(key);
  return ESY_OK;
}

esy_err_t esy_hb_gpio_attach_irq(const esy_gpio_irq_config_t *config) {
  return gpio_attach_irq(config, false);
}

void esy_hb_gpio_enable_irq(uint8_t pin) {
  if (!is_valid_pin(pin) || __get_IPSR() != 0u) return;
  uint32_t key = gpio_lock();
  if (gpio_owns_line(pin) && !s_ctx.lines[STM32F4_GPIO_PIN(pin)].enabled) {
    __HAL_GPIO_EXTI_CLEAR_IT(get_pin_mask(pin));
    s_ctx.lines[STM32F4_GPIO_PIN(pin)].enabled = true;
    EXTI->IMR |= get_pin_mask(pin);
  }
  gpio_unlock(key);
}

void esy_hb_gpio_disable_irq(uint8_t pin) {
  if (!is_valid_pin(pin) || __get_IPSR() != 0u) return;
  uint32_t key = gpio_lock();
  if (gpio_owns_line(pin)) gpio_mask_line(pin);
  gpio_unlock(key);
}

esy_err_t esy_hb_gpio_enable_wakeup(const esy_gpio_irq_config_t *config,
                                    bool *out_cleanup_required) {
  if (out_cleanup_required == NULL) return ESY_ERR_INVALID_ARG;
  *out_cleanup_required = false;
  esy_err_t ret = gpio_attach_irq(config, true);
  *out_cleanup_required = ret == ESY_OK;
  return ret;
}

esy_err_t esy_hb_gpio_disable_wakeup(uint8_t pin) {
  if (!is_valid_pin(pin)) return ESY_ERR_INVALID_ARG;
  if (__get_IPSR() != 0u) return ESY_ERR_STATE;
  uint32_t key = gpio_lock();
  if (gpio_owns_line(pin) && s_ctx.lines[STM32F4_GPIO_PIN(pin)].wake_source)
    gpio_detach_line(pin);
  gpio_unlock(key);
  return ESY_OK;
}

void esy_hb_gpio_handle_irq(uint8_t irq_group) {
  if (irq_group >= STM32F4_GPIO_EXTI_LINE_COUNT) return;
  uint8_t first = irq_group < 5u ? irq_group : irq_group < 10u ? 5u : 10u;
  uint8_t last = irq_group < 5u ? irq_group : irq_group < 10u ? 9u : 15u;
  for (uint8_t i = first; i <= last; ++i) {
    uint32_t mask = 1u << i;
    gpio_irq_line_t *line = &s_ctx.lines[i];
    if ((EXTI->PR & EXTI->IMR & mask) == 0u) continue;
    __HAL_GPIO_EXTI_CLEAR_IT(mask);
    if (line->enabled && line->handler != NULL)
      line->handler(line->pin, line->arg);
  }
}
