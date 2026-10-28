/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_WAKEUP_TIMER_H
#define ESY_HAL_WAKEUP_TIMER_H

#include <stdbool.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/system/esy_power_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque alarm storage capacity; backend types remain private. */
#define ESY_HAL_WAKEUP_TIMER_STORAGE_BYTES 24u

/**
 * @brief Caller-owned storage for a physical wakeup alarm; first state is {0}.
 *
 * One externally serialized owner core performs every operation. Keep storage
 * at a stable address until successful deinit; never clear/copy a live alarm.
 * Normal lifecycle: init -> prepare -> arm -> capture/cancel -> finish -> deinit.
 * Before arming, a prepared transaction may instead be closed with abort.
 * The *_atomic operations are bounded, non-waiting parts of the owner's local
 * IRQ-masked power transaction. The suffix does not provide intercore locking;
 * callers must exclude ISR/other-core access. All other operations run outside
 * that atomic transaction and must not be called from an ISR.
 */
typedef struct esy_hal_wakeup_timer_s {
  uint64_t storage[ESY_HAL_WAKEUP_TIMER_STORAGE_BYTES / sizeof(uint64_t)];
} esy_hal_wakeup_timer_t;

/** @brief Consumer-selected hardware alarm resource. */
typedef struct esy_hal_wakeup_timer_config_s {
  /** @brief Hardware alarm selected by the consumer; validity is port-specific. */
  uint8_t alarm_index;
} esy_hal_wakeup_timer_config_t;

/**
 * @brief Claim the configured alarm using caller-owned zero-initialized storage.
 * @param[in,out] timer Stable handle, initially {0} or successfully deinitialized.
 * @param[in] config Alarm selection, copied before return.
 * @retval ESY_OK Alarm claimed and ready for prepare.
 * @retval ESY_ERR_STATE Repeated init; the original alarm is unchanged.
 * @retval ESY_ERR_BUSY Alarm already owned elsewhere.
 * @retval ESY_ERR_INVALID_ARG NULL argument or invalid alarm selection.
 * @retval ESY_ERR_NOT_SUPPORTED No alarm provider on the selected port.
 * @note Failed init retains no new resource and permits retry. No callbacks or
 * caller buffers are borrowed. Other native errors are propagated unchanged.
 */
esy_err_t esy_hal_wakeup_timer_init(
    esy_hal_wakeup_timer_t *timer,
    const esy_hal_wakeup_timer_config_t *config);
/**
 * @brief Release an initialized alarm after its power transaction is closed.
 * @param[in,out] timer Initialized handle owned by the calling core.
 * @retval ESY_OK Ownership released; storage may be reused or initialized again.
 * @retval ESY_ERR_BUSY Prepared/armed work remains; finish or abort it first.
 * @retval ESY_ERR_INVALID_ARG NULL or uninitialized handle.
 * @retval ESY_ERR_NOT_SUPPORTED The selected port has no provider.
 * @note Any failure retains the alarm and requires cleanup/retry; clearing its
 * storage is forbidden. Repeated deinit after success is not an idempotent API.
 */
esy_err_t esy_hal_wakeup_timer_deinit(esy_hal_wakeup_timer_t *timer);
/**
 * @brief Query states in which this alarm's clock is supported.
 * @param[in] timer Initialized alarm.
 * @param[out] out_state_mask Supported ESY_POWER_STATE_MASK bits on success.
 * An unsupported provider may clear the mask to zero; check status before use.
 * ACTIVE can describe clock availability without permitting prepare.
 * @return ESY_OK, ESY_ERR_INVALID_ARG, ESY_ERR_NOT_SUPPORTED or native error.
 */
esy_err_t esy_hal_wakeup_timer_get_supported_state_mask(
    esy_hal_wakeup_timer_t *timer, uint32_t *out_state_mask);
/**
 * @brief Sample the alarm's monotonic microsecond clock without waiting.
 * @param[in] timer Initialized alarm on its owner core.
 * @param[out] out_time_us Absolute clock value; unchanged on failure.
 * @return ESY_OK, ESY_ERR_INVALID_ARG, ESY_ERR_NOT_SUPPORTED or native error.
 * @note May run inside the owner's IRQ-masked atomic phase. The sampled epoch
 * is also used by arm_absolute_atomic; it is not a relative delay.
 */
esy_err_t esy_hal_wakeup_timer_now_atomic(
    esy_hal_wakeup_timer_t *timer, uint64_t *out_time_us);
/**
 * @brief Prepare an unarmed alarm before entering the atomic power phase.
 * @param[in,out] timer Initialized alarm without an outstanding transaction.
 * @param state Requested low-power state; unsupported states are rejected.
 * @retval ESY_OK Prepared; call arm or abort to close this transaction.
 * @retval ESY_ERR_STATE Already prepared/armed.
 * @retval ESY_ERR_NOT_SUPPORTED The state or provider is unsupported.
 * @retval ESY_ERR_INVALID_ARG Invalid handle.
 * @note Failure leaves no new preparation; success prevents deinit until finish
 * or abort. Pico currently prepares CPU_IDLE only.
 */
esy_err_t esy_hal_wakeup_timer_prepare(esy_hal_wakeup_timer_t *timer,
                                            esy_power_state_t state);
/**
 * @brief Arm a prepared alarm at a future absolute microsecond deadline.
 * @param[in,out] timer Prepared, unarmed alarm in the owner's IRQ-masked phase.
 * @param state State accepted by prepare.
 * @param wake_time_us Deadline in the epoch returned by now_atomic.
 * @retval ESY_OK Armed; capture/cancel is required even after early wakeup.
 * @retval ESY_ERR_STATE Wrong phase or deadline already due.
 * @retval ESY_ERR_BUSY Deadline was missed while programming the alarm.
 * @retval ESY_ERR_INVALID_ARG Invalid handle.
 * @retval ESY_ERR_NOT_SUPPORTED State/provider unsupported.
 * @note Failure leaves the prepared transaction unarmed; abort it outside the
 * atomic phase. The caller must not start a second arm concurrently.
 */
esy_err_t esy_hal_wakeup_timer_arm_absolute_atomic(
    esy_hal_wakeup_timer_t *timer, esy_power_state_t state,
    uint64_t wake_time_us);
/**
 * @brief Sample deadline expiration and cancel an armed alarm atomically.
 * @param[in,out] timer Prepared and armed alarm in the IRQ-masked owner phase.
 * @param[out] out_expired True when its absolute deadline has elapsed; unchanged
 * on error. This describes the deadline, not which source woke the CPU.
 * @return ESY_OK, ESY_ERR_STATE for a wrong phase, ESY_ERR_INVALID_ARG,
 * ESY_ERR_NOT_SUPPORTED or native error.
 * @note Success retires the alarm and masks its IRQ until finish. Failure retains
 * ownership and phase; retry the failed operation before releasing storage.
 */
esy_err_t esy_hal_wakeup_timer_capture_and_cancel_atomic(
    esy_hal_wakeup_timer_t *timer, bool *out_expired);
/**
 * @brief Complete a captured/cancelled transaction outside the atomic phase.
 * @param[in,out] timer Prepared, unarmed alarm after successful capture/cancel.
 * @return ESY_OK, ESY_ERR_STATE for a wrong phase, ESY_ERR_INVALID_ARG,
 * ESY_ERR_NOT_SUPPORTED or native error.
 * @note Success restores the alarm IRQ and permits another prepare or deinit.
 * Failure retains ownership and requires retry; finish is not repeatable after
 * success without a new complete transaction.
 */
esy_err_t esy_hal_wakeup_timer_finish(esy_hal_wakeup_timer_t *timer);
/**
 * @brief Undo preparation when the transaction was never successfully armed.
 * @param[in,out] timer Prepared alarm, neither armed nor captured/cancelled.
 * @return ESY_OK, ESY_ERR_STATE for a wrong phase, ESY_ERR_INVALID_ARG,
 * ESY_ERR_NOT_SUPPORTED or native error.
 * @note Run outside the atomic phase. Success permits prepare/deinit; failure
 * retains ownership. Armed work must use capture/cancel followed by finish.
 */
esy_err_t esy_hal_wakeup_timer_abort(esy_hal_wakeup_timer_t *timer);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_WAKEUP_TIMER_H */
