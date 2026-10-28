/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_GPIO_DEFS_H
#define ESY_GPIO_DEFS_H

/**
 * @file esy_gpio_defs.h
 * @brief GPIO modes, interrupt modes, and IRQ-related type definitions.
 *
 * - Provides GPIO working mode enums (input/output/pull-up/pull-down).
 * - Provides GPIO interrupt trigger mode enums.
 * - Provides GPIO interrupt registration configuration structures.
 */

#include <stdbool.h>
#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* GPIO Type Definitions                                                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief GPIO operating mode.
 */
typedef enum {
  ESY_GPIO_MODE_INPUT,         /**< Input mode */
  ESY_GPIO_MODE_OUTPUT,        /**< Output mode */
  ESY_GPIO_MODE_INPUT_PULLUP,  /**< Input with pull-up resistor */
  ESY_GPIO_MODE_INPUT_PULLDOWN /**< Input with pull-down resistor */
} esy_gpio_mode_t;

/** @brief GPIO input pull configuration. */
typedef enum {
  ESY_GPIO_PULL_NONE, /**< No internal pull resistor */
  ESY_GPIO_PULL_UP,   /**< Enable internal pull-up resistor */
  ESY_GPIO_PULL_DOWN  /**< Enable internal pull-down resistor */
} esy_gpio_pull_t;

/**
 * @brief GPIO interrupt trigger mode.
 */
typedef enum {
  ESY_GPIO_IRQ_NONE,    /**< No interrupt */
  ESY_GPIO_IRQ_RISING,  /**< Rising edge trigger */
  ESY_GPIO_IRQ_FALLING, /**< Falling edge trigger */
  ESY_GPIO_IRQ_BOTH     /**< Both rising and falling edge trigger */
} esy_gpio_irq_mode_t;

/** @brief One GPIO electrical state. */
typedef struct esy_gpio_state_s {
  esy_gpio_mode_t mode; /**< Pin mode applied for this state. */
  bool output_value;    /**< Output level applied when mode is output. */
} esy_gpio_state_t;

/** @brief Logical-device GPIO power states. */
typedef enum {
  ESY_GPIO_PM_STATE_DEFAULT = 0,
  ESY_GPIO_PM_STATE_IDLE,
  ESY_GPIO_PM_STATE_SLEEP,
  ESY_GPIO_PM_STATE_COUNT
} esy_gpio_pm_state_id_t;

/** @brief GPIO handle initialization configuration. */
typedef struct esy_gpio_config_s {
  uint8_t pin;             /**< Platform pin identifier. */
  esy_gpio_mode_t mode;    /**< Normal operating mode. */
  bool output_value;       /**< Initial value when @ref mode is output. */
} esy_gpio_config_t;

/** @brief Opaque HAL state capacity, scaled to the target pointer width. */
#define ESY_GPIO_HAL_STORAGE_BYTES \
  (((2u * sizeof(void *) + 2u * sizeof(esy_gpio_state_t) + \
     sizeof(esy_gpio_pm_state_id_t) + sizeof(uint8_t) + 2u * sizeof(bool) - 1u) / \
    sizeof(void *)) * sizeof(void *))

/**
 * @brief Caller-owned GPIO resource handle.
 *
 * Initialize with {0}, keep its address stable, and use public HAL queries for
 * configuration. Electrical state, wake ownership and PM claims are private.
 */
typedef struct esy_gpio_s {
  union {
    void *alignment; /**< Private state alignment; never access directly. */
    uint8_t bytes[ESY_GPIO_HAL_STORAGE_BYTES]; /**< HAL storage. */
  } hal; /**< Opaque caller-owned HAL state; never copy or clear while live. */
} esy_gpio_t;

/**
 * @brief GPIO interrupt handler prototype.
 *
 * @param[in] pin Platform-defined GPIO pin value.
 * @param[in] arg User-provided argument
 *
 * @note The handler is typically executed in interrupt context;
 *       blocking operations must be avoided.
 */
typedef void (*esy_gpio_irq_handler_t)(uint8_t pin, void *arg);

/**
 * @brief GPIO interrupt registration configuration.
 */
typedef struct {
  uint8_t pin;                    /**< Encoded GPIO pin value */
  esy_gpio_irq_mode_t irq_mode;   /**< Interrupt trigger mode */
  esy_gpio_irq_handler_t handler; /**< Interrupt handler */
  void *arg;                      /**< User-defined argument */
  uint8_t priority; /**< Interrupt priority (platform-defined; smaller value =
                       higher priority) */
  esy_gpio_pull_t pull; /**< Input pull applied with the IRQ mode */
} esy_gpio_irq_config_t;

#endif /* ESY_GPIO_DEFS_H */
