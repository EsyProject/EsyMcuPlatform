/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hb_low_power.h
 * @brief HAL backend low-power mode management interface.
 *
 * This interface provides atomic hardware entry for platform low-power modes.
 *
 * Design notes:
 * - Resumable modes are categorized as Sleep and Stop.
 * - Reset-class shutdown is exposed through a separate API.
 * - Kernel tick control remains owned by the OS adapter.
 * - A port that loses clocks performs early clock restoration before atomic
 *   entry returns; the HAL wrapper owns fatal-policy propagation.
 *
 * Concurrency and safety:
 * - Before entering any low-power mode, the caller must ensure that critical
 *   peripherals have saved state and required interrupts are configured as
 *   wakeup sources.
 * - prepare, enter, and cancel form a single-owner transaction. The caller
 *   provides external serialization.
 */

#ifndef ESY_HB_LOW_POWER_H
#define ESY_HB_LOW_POWER_H

#include <stdbool.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Low-power hardware modes exposed by a HAL backend port. */
typedef enum esy_hb_low_power_mode_e {
  ESY_HB_LOW_POWER_MODE_SLEEP = 0,
  ESY_HB_LOW_POWER_MODE_STOP,
  ESY_HB_LOW_POWER_MODE_COUNT
} esy_hb_low_power_mode_t;

/** @brief Build a capability bit for an @ref esy_hb_low_power_mode_t. */
#define ESY_HB_LOW_POWER_MODE_MASK(mode) (1u << (uint32_t)(mode))

/**
 * @brief Return the low-power modes implemented by the active HAL backend port.
 *
 * The result reports hardware entry implementations only. HAL-level
 * requirements such as a deadline timer and clocksource are applied by
 * `esy_hal` and the power manager. Early clock restoration remains part
 * of the HAL backend transaction on ports that lose their run clock.
 */
typedef struct esy_hb_low_power_capabilities_s {
  uint32_t supported_mode_mask;
  uint32_t entry_latency_us[ESY_HB_LOW_POWER_MODE_COUNT];
  uint32_t exit_latency_us[ESY_HB_LOW_POWER_MODE_COUNT];
  uint32_t min_residency_us[ESY_HB_LOW_POWER_MODE_COUNT];
} esy_hb_low_power_capabilities_t;

/** @brief Return port capabilities and conservative timing constraints. */
extern esy_err_t esy_hb_low_power_get_capabilities(
    esy_hb_low_power_capabilities_t *out_capabilities);

/** @brief Hardware-entry and early-recovery outcome. */
typedef struct esy_hb_low_power_result_s {
  bool entered;
  esy_err_t fatal_error;
} esy_hb_low_power_result_t;

/**
 * @brief Prepare hardware and any early-restore snapshot for one entry.
 *
 * A failure is fail-atomic. A successful call must be followed by exactly one
 * enter or cancel call.
 */
extern esy_err_t esy_hb_low_power_prepare(esy_hb_low_power_mode_t mode);

/**
 * @brief Enter one low-power hardware mode.
 *
 * Typically only the CPU clock is stopped while peripherals remain running.
 * Wakeup latency is minimal.
 *
 * Suitable for short idle periods requiring fast response.
 *
 * @param[in] mode Hardware mode to enter.
 * @param[out] out_result Entry and early-recovery outcome.
 * @retval ESY_OK Returned after wake.
 * @retval ESY_ERR_INVALID_ARG Invalid mode or missing required configuration.
 * @retval ESY_ERR_NOT_SUPPORTED The active port has no CPU-idle implementation.
 */
extern esy_err_t esy_hb_low_power_enter(
    esy_hb_low_power_mode_t mode,
    esy_hb_low_power_result_t *out_result);

/** @brief Cancel a prepared entry that did not consume its snapshot. */
extern esy_err_t esy_hb_low_power_cancel(esy_hb_low_power_mode_t mode);

typedef enum esy_hb_shutdown_mode_e {
  ESY_HB_SHUTDOWN_MODE_STANDBY = 0,
  ESY_HB_SHUTDOWN_MODE_COUNT
} esy_hb_shutdown_mode_t;

/** @brief Enter a reset-class shutdown state. Success does not return. */
extern esy_err_t esy_hb_shutdown(esy_hb_shutdown_mode_t mode);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_HB_LOW_POWER_H */
