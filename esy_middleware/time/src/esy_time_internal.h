/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_TIME_INTERNAL_H
#define ESY_TIME_INTERNAL_H

#include "esy_middleware/time/esy_time.h"

/**
 * @brief Commit UTC only if its final monotonic anchor sample is before expiry.
 * @param[in,out] time Initialized clock owned by this module's serialized caller.
 * @param[in] timestamp Valid normalized UTC, disjoint from the handle.
 * @param[in] started_us Start sample from the same counter epoch as time's clock.
 * @param[in] timeout_us Finite timeout in microseconds, 1..UINT32_MAX.
 * @return Errors from esy_time_set, or ESY_ERR_TIMEOUT at elapsed >= timeout_us.
 * @note Module-private SNTP integration. No full 2^64-us counter cycle may pass
 * between start and the anchor sample. Every failure preserves the old anchor
 * and validity. This function creates no timer and never rearms an exchange.
 */
esy_err_t esy_time_set_before(esy_time_t *time, const esy_timestamp_t *timestamp,
                              uint64_t started_us, uint32_t timeout_us);

#endif /* ESY_TIME_INTERNAL_H */
