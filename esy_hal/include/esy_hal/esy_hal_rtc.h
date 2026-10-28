/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_RTC_H
#define ESY_HAL_RTC_H

#include <stdbool.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_rtc_defs.h"
#include "esy_common/defs/base/esy_time_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Immutable operations for a calendar RTC, without hardware SDK types.
 *
 * All hooks are synchronous, externally serialized, non-ISR operations. The
 * provider controls latency and may perform bounded bus I/O. get/set_datetime
 * are required. Advertised optional capabilities require all corresponding
 * hooks. Hooks return ESY_OK or a public error; failed output operations must
 * leave outputs unchanged. The facade additionally validates calendar outputs.
 * The provider remains initialized and exclusively borrowed until detach.
 */
typedef struct {
  uint32_t capabilities; /**< ESY_RTC_CAP_* mask; unknown bits are ignored. */
  esy_err_t (*get_datetime)(void *ctx, esy_datetime_t *out_datetime);
  /**< Read whole-second UTC Gregorian calendar time, without leap seconds. */
  esy_err_t (*set_datetime)(void *ctx, const esy_datetime_t *datetime);
  /**< Set UTC; preserve pending alarms and evaluate an armed alarm after a step. */
  esy_err_t (*alarm_set)(void *ctx, const esy_datetime_t *datetime);
  /**< Replace alarm with a strictly future UTC deadline; clear old pending. */
  esy_err_t (*alarm_cancel)(void *ctx);
  /**< Permanently retire armed and pending alarm state; repeated cancel is OK. */
  esy_err_t (*alarm_get_pending)(void *ctx, bool clear, bool *out_pending);
  /**< Sample latched expiration; clear=true consumes it. It never rearms itself. */
  esy_err_t (*set_calibration)(void *ctx, int32_t correction_ppb);
  /**< Apply signed rate correction in parts per billion; positive runs faster. */
  esy_err_t (*get_calibration)(void *ctx, int32_t *out_correction_ppb);
  /**< Read effective signed correction; provider documents range/resolution. */
} esy_hal_rtc_ops_t;

/** @brief Borrowed provider binding copied by init; context may be NULL. */
typedef struct {
  const esy_hal_rtc_ops_t *ops; /**< Immutable table alive until detach. */
  void *provider_ctx; /**< Provider-owned state alive until detach. */
} esy_hal_rtc_config_t;

/**
 * @brief Caller-owned facade, initialized with {0} before first init.
 *
 * Fields belong to the HAL. Do not copy or clear an attached handle. One owner
 * serializes every operation and provider access; no operation is ISR-safe.
 */
typedef struct {
  const esy_hal_rtc_ops_t *ops; /**< Private borrowed dispatch binding. */
  void *provider_ctx; /**< Private borrowed provider state. */
} esy_hal_rtc_t;

/**
 * @brief Attach an already initialized provider without modifying its state.
 * @return ESY_OK, INVALID_ARG for invalid hooks/arguments, or STATE if attached.
 * @note Failure borrows nothing. No allocation, native init or alarm is started.
 * A retained but unset calendar may return STATE until the first set_datetime.
 * Keep the table/context stable until deinit; this facade never owns provider
 * lifecycle or bus objects.
 */
esy_err_t esy_hal_rtc_init(esy_hal_rtc_t *rtc,
                          const esy_hal_rtc_config_t *config);
/**
 * @brief Detach the borrowed provider; alarms and provider resources are retained.
 * @return ESY_OK, INVALID_ARG for NULL or STATE if already detached.
 * @note Success restores {0}, permitting reinit. Cancel alarms explicitly before
 * releasing their provider; no callbacks are registered by this facade.
 */
esy_err_t esy_hal_rtc_deinit(esy_hal_rtc_t *rtc);
/** @brief Query optional capabilities; output unchanged on invalid state/error. */
esy_err_t esy_hal_rtc_get_caps(esy_hal_rtc_t *rtc, uint32_t *out_caps);
/**
 * @brief Read UTC in 1970..9999; output unchanged on error.
 * @return ESY_OK, INVALID_ARG, STATE or provider error; IO for invalid provider data.
 */
esy_err_t esy_hal_rtc_get_datetime(esy_hal_rtc_t *rtc,
                                  esy_datetime_t *out_datetime);
/** @brief Set valid UTC calendar; invalid dates return INVALID_ARG without I/O. */
esy_err_t esy_hal_rtc_set_datetime(esy_hal_rtc_t *rtc,
                                  const esy_datetime_t *datetime);
/**
 * @brief Arm/replace a whole-second, strictly future UTC alarm.
 * @return ESY_OK, INVALID_ARG, STATE for a due deadline, NOT_SUPPORTED or provider error.
 * @note Failure preserves the previous alarm; calendar steps can cause expiration.
 */
esy_err_t esy_hal_rtc_alarm_set(esy_hal_rtc_t *rtc,
                               const esy_datetime_t *datetime);
/** @brief Retire armed/pending state; NOT_SUPPORTED without the ALARM capability. */
esy_err_t esy_hal_rtc_alarm_cancel(esy_hal_rtc_t *rtc);
/**
 * @brief Sample latched alarm expiration, optionally consuming the pending flag.
 * @note Expiration retires the alarm permanently. Completed, consumed or canceled
 * alarms remain inactive after clock wraps or calendar steps; only alarm_set rearms.
 */
esy_err_t esy_hal_rtc_alarm_get_pending(esy_hal_rtc_t *rtc, bool clear,
                                       bool *out_pending);
/** @brief Set signed rate correction in ppb; optional provider-specific range. */
esy_err_t esy_hal_rtc_set_calibration(esy_hal_rtc_t *rtc,
                                     int32_t correction_ppb);
/** @brief Read effective rate correction in ppb; output unchanged on error. */
esy_err_t esy_hal_rtc_get_calibration(esy_hal_rtc_t *rtc,
                                     int32_t *out_correction_ppb);

/** @brief Capacity of opaque, caller-owned software RTC storage. */
#define ESY_HAL_RTC_SOFTWARE_STORAGE_BYTES 128u
/** @brief Inclusive software correction magnitude in ppb (1000 ppm). */
#define ESY_HAL_RTC_SOFTWARE_MAX_CALIBRATION_PPB INT32_C(1000000)

/**
 * @brief Monotonic microsecond sample, output unchanged on error.
 *
 * Counter is unsigned 64-bit modulo 2^64 (~584,542 years). Successive samples
 * must be less than one full counter period apart; each accepted interval must
 * also fit the supported UTC span. A single wrap is supported. Reset/rollback
 * usually returns STATE until explicit set_datetime rebases. Time advances only
 * while this source runs: choose an always-on source for sleep accounting. The
 * source and borrowed ctx remain alive until software deinit; no ISR calls.
 */
typedef esy_err_t (*esy_hal_rtc_monotonic_us_fn)(void *ctx,
                                               uint64_t *out_time_us);
/** @brief Software provider configuration, copied by software_init. */
typedef struct {
  esy_hal_rtc_monotonic_us_fn monotonic_us; /**< Required monotonic clock. */
  void *clock_ctx; /**< Borrowed context; NULL allowed. */
  esy_datetime_t initial_datetime; /**< Initial UTC at the first clock sample. */
} esy_hal_rtc_software_config_t;

/**
 * @brief Opaque software provider storage, initially {0}, kept at a stable address.
 * No heap, OS object, timer, callback or hardware resource is created. Callers
 * serialize access through at most one attached facade. Do not clear/copy a live
 * provider. Detach its facade before software_deinit; this ordering is a caller
 * precondition because the provider cannot observe external borrowed handles.
 */
typedef struct {
  ESY_ALIGNED_STORAGE(storage, ESY_HAL_RTC_SOFTWARE_STORAGE_BYTES);
} esy_hal_rtc_software_t;
/**
 * @brief Initialize a software RTC with zero correction and no active alarm.
 * @return ESY_OK, INVALID_ARG, STATE for repeated init or clock-provider error.
 * @note Failure retains no new borrow and permits retry. The clock is called
 * exactly once; its latency defines initialization cost.
 */
esy_err_t esy_hal_rtc_software_init(
    esy_hal_rtc_software_t *software,
    const esy_hal_rtc_software_config_t *config);
/**
 * @brief Clear a detached provider and its alarms, permitting reinitialization.
 * @return ESY_OK, INVALID_ARG for NULL or STATE if not initialized.
 */
esy_err_t esy_hal_rtc_software_deinit(esy_hal_rtc_software_t *software);
/**
 * @brief Obtain the immutable software provider table for esy_hal_rtc_init.
 * @note Bind provider_ctx to initialized esy_hal_rtc_software_t storage. Reading
 * time/alarm or changing rate performs one clock sample and constant work.
 * No background task is needed; alarm pending is evaluated on owner access.
 * UTC range exhaustion returns STATE until set_datetime explicitly rebases.
 */
const esy_hal_rtc_ops_t *esy_hal_rtc_software_get_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_RTC_H */
