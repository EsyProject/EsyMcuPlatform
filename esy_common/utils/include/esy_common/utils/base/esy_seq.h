/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_COMMON_UTILS_ESY_SEQ_H
#define ESY_COMMON_UTILS_ESY_SEQ_H

/**
 * @file esy_seq.h
 * @brief Common wrap-safe uint32 sequence number helpers.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check whether sequence number @p a is before @p b.
 *
 * The comparison is wrap-safe when the valid distance is less than 2^31.
 *
 * @param[in] a First sequence number.
 * @param[in] b Second sequence number.
 *
 * @retval true @p a is before @p b.
 * @retval false @p a is not before @p b.
 */
static inline bool esy_seq32_before(uint32_t a, uint32_t b) {
  return ((int32_t)(a - b) < 0);
}

/**
 * @brief Check whether sequence number @p now has reached @p target.
 *
 * This helper is suitable for wrap-safe deadline checks on monotonic uint32
 * counters, including millisecond ticks. The comparison is valid when the
 * maximum meaningful distance is less than 2^31.
 *
 * @param[in] now Current sequence/tick value.
 * @param[in] target Target sequence/tick value.
 *
 * @retval true @p now is equal to or after @p target.
 * @retval false @p now is before @p target.
 */
static inline bool esy_seq32_reached(uint32_t now, uint32_t target) {
  return !esy_seq32_before(now, target);
}

/**
 * @brief Check whether @p x is inside the closed interval [start, end].
 *
 * The interval comparison is wrap-safe.
 *
 * @param[in] x Sequence number to check.
 * @param[in] start Inclusive interval start.
 * @param[in] end Inclusive interval end.
 *
 * @retval true @p x is inside the interval.
 * @retval false @p x is outside the interval.
 */
static inline bool esy_seq32_in_closed_range(uint32_t x, uint32_t start,
                                             uint32_t end) {
  return ((uint32_t)(x - start) <= (uint32_t)(end - start));
}

#ifdef __cplusplus
}
#endif

#endif /* ESY_COMMON_UTILS_ESY_SEQ_H */
