/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_TIME_H
#define ESY_TIME_H

/**
 * @file esy_time.h
 * @brief Caller-owned UTC clock anchored to an injected monotonic clock.
 *
 * One task or baremetal main-loop owner serializes all calls and providers.
 * No function is ISR safe. There are no workers, OS dependencies or allocation.
 * Providers run synchronously and must have a bounded runtime, never reenter
 * this handle, and keep their context alive through successful deinit.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/base/esy_time_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque handle capacity, not a stable ABI or serialization format. */
#define ESY_TIME_STORAGE_BYTES 128u

/**
 * @brief Sample a monotonic unsigned 64-bit microsecond counter.
 * @param[in] provider_ctx Borrowed provider context; may be NULL.
 * @param[out] microseconds Required output, written only on success.
 * @return ESY_OK or a negative provider error propagated to the caller.
 * @note Wrap is modulo 2^64 (about 584,542 years). The clock must not reset,
 * run backwards or change rate while initialized. It must count sleep time
 * when UTC must progress during sleep. An anchor-to-observation interval must
 * be below 2^63 microseconds; UTC's supported range is smaller than this.
 */
typedef esy_err_t (*esy_time_monotonic_fn)(void *provider_ctx,
                                         uint64_t *microseconds);

/** @brief Configuration copied by init; the provider context remains borrowed. */
typedef struct {
    esy_time_monotonic_fn monotonic_us; /**< Required microsecond clock. */
    void *provider_ctx; /**< Optional clock context owned by the caller. */
} esy_time_config_t;

/**
 * @brief Caller-owned handle requiring `{0}` before first init.
 * @note Do not copy, move, modify or clear a live handle. Successful deinit
 * restores its zero state. All inputs/outputs must be disjoint from the handle.
 */
typedef struct {
    union {
        uint8_t bytes[ESY_TIME_STORAGE_BYTES]; /**< Private state. */
        max_align_t alignment; /**< Private state alignment. */
    } storage; /**< In-place storage; no backing buffers required. */
} esy_time_t;

/** @brief Clock validity; initialization alone does not establish UTC. */
typedef struct {
    bool valid; /**< An explicit successful set has established a UTC anchor. */
} esy_time_status_t;

/**
 * @brief Initialize an invalid UTC clock without sampling the provider.
 * @param[in,out] time Zero-initialized or successfully deinitialized handle.
 * @param[in] config Required configuration, copied on success.
 * @return ESY_OK, ESY_ERR_INVALID_ARG for NULL/missing provider, or
 * ESY_ERR_STATE for repeated init. Failure leaves the handle unchanged.
 */
esy_err_t esy_time_init(esy_time_t *time, const esy_time_config_t *config);

/**
 * @brief Release the borrowed clock provider without calling it.
 * @param[in,out] time Initialized or zero handle.
 * @return ESY_OK (including repeated deinit), INVALID_ARG for NULL, or STATE
 * for an invalid/copied handle. Reinit is legal after success.
 */
esy_err_t esy_time_deinit(esy_time_t *time);

/**
 * @brief Step UTC to a validated value sampled at the provider call.
 * @param[in,out] time Initialized handle.
 * @param[in] timestamp UTC Unix seconds 0..ESY_TIME_MAX_SECONDS and normalized
 * nanoseconds 0..999999999. The input is copied.
 * @return ESY_OK, INVALID_ARG, STATE, or a propagated provider error.
 * @note Forward and backward steps are explicit; monotonic scheduling must
 * use the provider clock, not UTC. Failure preserves the previous anchor.
 */
esy_err_t esy_time_set(esy_time_t *time, const esy_timestamp_t *timestamp);

/**
 * @brief Read UTC by adding elapsed monotonic microseconds to its anchor.
 * @param[in] time Initialized handle with a valid UTC anchor.
 * @param[out] timestamp Required normalized UTC output; unchanged on failure.
 * @return ESY_OK, INVALID_ARG, STATE for invalid/unset clock, an elapsed
 * interval >=2^63 us or UTC range exhaustion, or a propagated provider error.
 * @note Precision is one microsecond plus the precision of the UTC anchor.
 */
esy_err_t esy_time_now(const esy_time_t *time, esy_timestamp_t *timestamp);

/**
 * @brief Invalidate UTC until the next explicit successful set.
 * @param[in,out] time Initialized handle.
 * @return ESY_OK, INVALID_ARG or STATE. No provider call is made.
 */
esy_err_t esy_time_invalidate(esy_time_t *time);

/**
 * @brief Query whether a UTC anchor exists without sampling its provider.
 * @param[in] time Initialized handle.
 * @param[out] status Required output, unchanged on error.
 * @return ESY_OK, INVALID_ARG or STATE. Validity does not assert freshness,
 * clock accuracy or the current health of the monotonic provider.
 */
esy_err_t esy_time_get_status(const esy_time_t *time, esy_time_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* ESY_TIME_H */
