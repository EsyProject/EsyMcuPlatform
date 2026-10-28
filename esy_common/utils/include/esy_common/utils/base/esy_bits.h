/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_COMMON_UTILS_ESY_BITS_H
#define ESY_COMMON_UTILS_ESY_BITS_H

/**
 * @file esy_bits.h
 * @brief Common bit and integer helper functions.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check whether a 32-bit unsigned integer is a power of two.
 *
 * @param[in] value Value to check.
 *
 * @retval true Value is a power of two.
 * @retval false Value is zero or not a power of two.
 */
static inline bool esy_is_power_of_two_u32(uint32_t value) {
  if (value == 0u) return false;
  return (value & (value - 1u)) == 0u;
}

/**
 * @brief Return the larger of two values.
 *
 * @note Arguments may be evaluated more than once.
 */
#ifndef ESY_MAX
#define ESY_MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/**
 * @brief Return the smaller of two values.
 *
 * @note Arguments may be evaluated more than once.
 */
#ifndef ESY_MIN
#define ESY_MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/**
 * @brief Compute the absolute value.
 *
 * @warning For signed integers, ESY_ABS(INT_MIN) overflows.
 */
#ifndef ESY_ABS
#define ESY_ABS(x) ((x) < 0 ? -(x) : (x))
#endif

#ifdef __cplusplus
}
#endif

#endif /* ESY_COMMON_UTILS_ESY_BITS_H */
