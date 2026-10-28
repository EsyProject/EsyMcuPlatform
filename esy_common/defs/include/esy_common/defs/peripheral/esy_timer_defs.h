/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_TIMER_DEFS_H
#define ESY_TIMER_DEFS_H

/**
 * @file esy_timer_defs.h
 * @brief Timer task definitions and related types.
 *
 * This file defines timer modes, clock sources, callback types,
 * and timer task configuration structures.
 *
 * HAL backends or upper-level schedulers use these definitions
 * to create and manage timer-based tasks.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Callback types                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief Timer callback function prototype.
 *
 * @param[in] arg User-registered callback argument.
 *
 * @note
 * The callback may be executed in interrupt context.
 * Avoid blocking operations or long-running computations.
 */
typedef void (*esy_timer_callback_t)(void *arg);

/* -------------------------------------------------------------------------- */
/* Enum types                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief Timer resolution mode.
 */
typedef enum {
  ESY_TIMER_MODE_MS,  /**< Millisecond-based timer */
  ESY_TIMER_MODE_US   /**< Microsecond-based timer */
} esy_timer_mode_t;

/**
 * @brief Timer clock source selection.
 */
typedef enum {
  ESY_TIMER_SOURCE_DEFAULT,   /**< Default timer clock source */
  ESY_TIMER_SOURCE_LOW_POWER  /**< Low-power timer clock source */
} esy_timer_source_t;

/* -------------------------------------------------------------------------- */
/* Configuration structures                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief Internal timer task initialization configuration.
 *
 * Used by the HAL or board layer to select the concrete
 * hardware timer source and interrupt priority.
 */
typedef struct {
  esy_timer_source_t source;  /**< Timer clock source */
  uint8_t irq_priority;       /**< Interrupt priority (lower value = higher priority) */
} esy_timer_task_inner_config_t;

/**
 * @brief Timer task description structure.
 *
 * Describes a one-shot or periodic timer task.
 * Upper layers use this structure to start or manage timer tasks.
 */
typedef struct {
  int32_t interval;               /**< Timer interval; unit is determined by @ref mode (ms/us) */
  esy_timer_callback_t callback;  /**< Timer callback function */
  void *callback_arg;             /**< Callback argument passed to @ref callback */
  bool repeat;                    /**< true = periodic task; false = one-shot task */
  esy_timer_mode_t mode;          /**< Timer resolution mode (ms or us) */

  esy_timer_task_inner_config_t inner; /**< Platform-specific internal configuration */
} esy_timer_task_config_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_TIMER_DEFS_H */
