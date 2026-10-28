/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAKE_ESY_HAL_FAULT_H
#define FAKE_ESY_HAL_FAULT_H

#include "esy_hal/esy_hal_fault.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Observable normal-context binding and fatal notification state. */
typedef struct {
  esy_err_t init_result; /**< Forced binding result; default ESY_OK. */
  uint32_t init_count; /**< Number of attempted initializations. */
  esy_fault_config_t config; /**< Last successful copied binding. */
  uint32_t raise_count; /**< Number of fatal notifications. */
  esy_fault_reason_t reason; /**< Last fatal classification. */
  uint32_t detail0, detail1; /**< Last opaque fatal details. */
} fake_esy_hal_fault_state_t;

/** @brief Reset the fake binding and observations.
 * Inject faults by calling config.capture with a readable snapshot. The fake
 * terminal raise API traps rather than returning to normal execution.
 */
void fake_esy_hal_fault_reset(void);
/** @brief Borrow the fake state from the serialized host test owner. */
fake_esy_hal_fault_state_t *fake_esy_hal_fault_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESY_HAL_FAULT_H */
