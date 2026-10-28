/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAKE_ESY_HAL_BOOT_H
#define FAKE_ESY_HAL_BOOT_H

#include "esy_hal/esy_hal_boot.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Observable HAL layout provider with caller-configured physical regions. */
typedef struct {
  uint32_t last_jump_address; /**< Last requested application base. */
  uint32_t jump_count; /**< Number of hardware handoff requests. */
  esy_bootloader_layout_t layout; /**< Layout copied on successful queries. */
  esy_err_t result; /**< Configurable query result. */
  uint32_t query_count; /**< Number of layout queries. */
  esy_boot_runtime_t runtime; /**< Current image identity and update permission. */
  esy_err_t runtime_result; /**< Configurable runtime query status. */
  uint32_t runtime_query_count; /**< Number of runtime queries. */
  esy_err_t pending_result; /**< Configurable next-image submission status. */
  uint32_t pending_count; /**< Number of next-image submissions. */
  uint32_t pending_address; /**< Last submitted image address. */
  uint32_t pending_size; /**< Last submitted image length. */
  esy_err_t confirm_result; /**< Configurable native confirmation status. */
  uint32_t confirm_count; /**< Number of native confirmation requests. */
} fake_esy_hal_boot_state_t;

/** @brief Reset to an empty direct layout, updates allowed and successful results. */
void fake_esy_hal_boot_reset(void);
/** @brief Borrow the fake context for test configuration and observation. */
fake_esy_hal_boot_state_t *fake_esy_hal_boot_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESY_HAL_BOOT_H */
