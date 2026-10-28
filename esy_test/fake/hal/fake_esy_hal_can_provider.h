/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAKE_ESY_HAL_CAN_PROVIDER_H
#define FAKE_ESY_HAL_CAN_PROVIDER_H

#include "esy_common/defs/peripheral/esy_can_defs.h"

/** @brief Observable provider hooks for call counts and injected results. */
typedef enum {
  FAKE_ESY_CAN_INIT = 0, /**< Native initialization. */
  FAKE_ESY_CAN_DEINIT, /**< Native retirement and release. */
  FAKE_ESY_CAN_START, /**< Start request. */
  FAKE_ESY_CAN_STOP, /**< Stop request. */
  FAKE_ESY_CAN_SEND, /**< One frame submission. */
  FAKE_ESY_CAN_RECEIVE, /**< One frame read. */
  FAKE_ESY_CAN_FILTERS, /**< Complete filter replacement. */
  FAKE_ESY_CAN_STATUS, /**< Diagnostic query. */
  FAKE_ESY_CAN_RECOVER, /**< Recovery request. */
  FAKE_ESY_CAN_CAPS, /**< Capability query. */
  FAKE_ESY_CAN_HOOK_COUNT /**< Number of controllable provider hooks. */
} fake_esy_hal_can_hook_t;

/** @brief Maximum retained filter values in the reusable provider fake. */
#define FAKE_ESY_CAN_FILTER_COUNT 28u

/**
 * @brief Resettable deterministic provider state for host HAL/consumer tests.
 * This fake models one resource owner; it does not simulate bxCAN registers.
 * Injected start/stop/recover/deinit timeouts enter FAULT and retain ownership
 * for successful stop/deinit cleanup before restart.
 * Error-producing output hooks intentionally write their borrowed output so
 * tests can verify public HAL output isolation. Never use with real hardware.
 */
typedef struct {
  esy_can_t *owner; /**< Current fake resource owner, or NULL. */
  esy_can_config_t config; /**< Most recently submitted configuration. */
  esy_err_t results[FAKE_ESY_CAN_HOOK_COUNT]; /**< Persistent injected hook results. */
  uint32_t calls[FAKE_ESY_CAN_HOOK_COUNT]; /**< Hook attempt counters. */
  uint32_t caps; /**< Capability query result. */
  esy_can_status_t status; /**< Externally controlled diagnostic state. */
  esy_can_frame_t rx_frame; /**< Receive output, including on injected failure. */
  esy_can_frame_t tx_frame; /**< Last successfully accepted transmit value. */
  esy_can_filter_t filters[FAKE_ESY_CAN_FILTER_COUNT]; /**< Last successful rules. */
  size_t filter_count; /**< Last successful rule count. */
  bool retain_failed_init; /**< Keep ctx/ownership after an injected init error. */
} fake_esy_hal_can_provider_t;

/** @brief Reset the singleton; all hooks succeed except receive, which is BUSY. */
void fake_esy_hal_can_provider_reset(void);

/** @brief Mutable controls/observations; reset only after live test users retire. */
fake_esy_hal_can_provider_t *fake_esy_hal_can_provider_state(void);

#endif /* FAKE_ESY_HAL_CAN_PROVIDER_H */
