/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAKE_ESY_HAL_WIFI_H
#define FAKE_ESY_HAL_WIFI_H

#include "esy_hal/esy_hal_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Deterministic backend state. Configure only with callers quiescent.
 * Backend calls are serialized by the test's process/PM ownership contract.
 */
typedef struct {
  esy_err_t start_result;
  esy_err_t process_result;
  esy_err_t address_result;
  esy_err_t runtime_result;
  bool connected;
  uint32_t start_calls;
  uint32_t process_calls;
  uint32_t address_calls;
  uint32_t deinit_calls;
  uint32_t runtime_calls;
  uint32_t progress_calls;
  uint32_t enter_calls;
  uint32_t leave_calls;
  void *last_context; /**< Most recent context received by a default runtime hook. */
  /** Descriptor copied by a successful query; replace only while quiescent. */
  esy_wifi_network_runtime_t network_runtime;
  esy_wifi_ipv4_addr_t address;
  esy_wifi_config_t last_config;
} fake_esy_hal_wifi_state_t;

/** @brief Reset the quiescent fake to successful operations and a test IPv4. */
void fake_esy_hal_wifi_reset(void);
/** @brief Borrow fake state for deterministic setup and observations. */
fake_esy_hal_wifi_state_t *fake_esy_hal_wifi_state(void);

#ifdef __cplusplus
}
#endif
#endif /* FAKE_ESY_HAL_WIFI_H */
