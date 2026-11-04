/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_LOW_POWER_H
#define ESY_HAL_LOW_POWER_H

#include <stdbool.h>

#include "esy_common/defs/system/esy_power_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Result captured by one resumable HAL power transaction. */
typedef struct esy_hal_power_result_s {
  esy_power_state_t state;
  bool entered;
  esy_err_t entry_status;
  esy_err_t fatal_error;
} esy_hal_power_result_t;

/** @brief Caller-owned single-owner HAL power transaction. */
typedef struct esy_hal_power_transaction_s {
  esy_hal_power_result_t result;
  uint32_t phase;
  bool driver_prepared;
} esy_hal_power_transaction_t;

/** @brief Reset a transaction before its first use or after completion. */
void esy_hal_power_transaction_init(
    esy_hal_power_transaction_t *transaction);

/** @brief Return resumable state support and conservative timing limits. */
esy_err_t esy_hal_power_get_capabilities(
    esy_power_capabilities_t *out_capabilities);

/**
 * @brief Perform one explicitly requested sleep from a schedulable owner task.
 *
 * This synchronous control operation may wait in the native provider. It is
 * separate from atomic power transactions and never belongs in enter_atomic,
 * a critical section, ISR, suspended scheduler, or callback dispatched there.
 * One system owner serializes calls and owns native sleep/wakeup configuration.
 * No automatic device-graph suspension, quiescence or power-lock acquisition is
 * performed. Stop network borrowers, Wi-Fi/Bluetooth, Flash/OTA and active I/O;
 * make hardware outputs safe and quiesce both cores before entry. Peer tasks
 * must remain interruptible: the SDK owns its cross-core sleep handshake.
 *
 * ESP32 supports RETENTION through manual Light-sleep with a positive timer
 * delay. Existing HAL GPIO wake bindings are borrowed unchanged. No other
 * native wake source, native timer wake owner or concurrent native sleep/PM
 * reconfiguration is permitted. Native automatic Light-sleep is rejected.
 * SDK PM locks govern automatic sleep and do not prevent this explicit entry;
 * they cannot replace the caller's quiescence barrier. A running RTC watchdog
 * rejects entry; quiesce its monitor/owner before disabling it.
 *
 * ESP32 advances its monotonic ESP Timer after wake; this path does not settle
 * FreeRTOS ticks. All task/OS timer waits must be quiescent, and the owner uses
 * elapsed_us or the monotonic clock to restart application deadlines. Pending
 * native timer callbacks can run after wake and remain their owners' concern.
 * This API claims no hard latency bound or automatic tickless support.
 *
 * The owned timer wake source is removed on every returning path after setup;
 * borrowed GPIO configuration is never changed. Cleanup failure retains native
 * timer ownership: retain the same system owner and retry this call. That retry
 * only retires the timer, reports entered=false, and never starts a new sleep.
 * Only after cleanup succeeds may native wake ownership be transferred.
 *
 * @param[in] config Request; pointers are borrowed only during this call.
 * @param[out] out_result Written on a valid request, including provider failure.
 * Unchanged for NULL arguments, invalid state or a zero duration.
 * @retval ESY_OK Sleep returned, or an outstanding cleanup was completed.
 * @retval ESY_ERR_BUSY Native entry rejected/too short, or RTC watchdog active.
 * @retval ESY_ERR_STATE Invalid execution context or automatic sleep active.
 * @retval ESY_ERR_NOT_SUPPORTED Selected port has no explicit sleep provider.
 * @retval ESY_ERR_INVALID_ARG Invalid pointers, state or duration.
 * @retval <0 Provider/setup/cleanup failure; inspect entered before recovery.
 */
esy_err_t esy_hal_power_sleep(const esy_power_sleep_config_t *config,
                               esy_power_sleep_result_t *out_result);

/** @brief Prepare one resumable transaction with interrupts enabled. */
esy_err_t esy_hal_power_transaction_begin(
    esy_hal_power_transaction_t *transaction, esy_power_state_t state);

/**
 * @brief Enter hardware with interrupts masked and perform early restoration.
 *
 * A non-OK fatal_error means clocks could not be restored. The caller must
 * keep interrupts masked and enter its fail-stop/reset path immediately.
 */
esy_err_t esy_hal_power_transaction_enter_atomic(
    esy_hal_power_transaction_t *transaction);

/** @brief Complete a returning transaction with interrupts still masked. */
esy_err_t esy_hal_power_transaction_complete(
    esy_hal_power_transaction_t *transaction,
    esy_hal_power_result_t *out_result);

/** @brief Abort a prepared transaction before hardware entry. */
esy_err_t esy_hal_power_transaction_abort(
    esy_hal_power_transaction_t *transaction);

/** @brief Reset-class shutdown modes excluded from automatic idle policy. */
typedef enum esy_hal_shutdown_mode_e {
  ESY_HAL_SHUTDOWN_MODE_STANDBY = 0,
  ESY_HAL_SHUTDOWN_MODE_COUNT
} esy_hal_shutdown_mode_t;

/**
 * @brief Enter an explicit reset-class state. Success does not return.
 * ESP32 requires the same task/system-quiescence preconditions as power_sleep.
 * STANDBY maps to native Deep-sleep and preserves caller-configured native wake
 * sources; the caller exclusively owns and validates those sources. This API
 * does not configure a deep-sleep timer or convert light-sleep GPIO bindings.
 * Without a compatible wake source, only external reset resumes execution.
 * Wake starts a new boot; live handles and monotonic deadlines do not survive.
 * Native automatic Light-sleep, a retained explicit-sleep timer, or an enabled
 * RTC watchdog reject shutdown without entering hardware.
 */
esy_err_t esy_hal_shutdown(esy_hal_shutdown_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_LOW_POWER_H */
