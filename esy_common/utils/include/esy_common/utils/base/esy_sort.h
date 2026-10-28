/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_COMMON_UTILS_ESY_SORT_H
#define ESY_COMMON_UTILS_ESY_SORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_sort.h
 * @brief Common sorting utility algorithms.
 *
 * ## Overview
 * This header provides simple, lightweight sorting helpers intended for
 * embedded and resource-constrained environments.
 *
 * Current scope:
 * - In-place sorting for `uint16_t` arrays.
 *
 * Design notes:
 * - No dynamic memory allocation.
 * - Operates directly on caller-provided arrays.
 * - Uses a simple insertion-sort implementation.
 * - Intended for small arrays typically found in MCU control paths.
 * - For large data sets, provide a domain-specific sorter instead of using
 *   this helper as a general-purpose high-throughput algorithm.
 *
 * Thread safety:
 * - This module performs no internal synchronization.
 * - Concurrent access to the same array must be protected by the caller.
 */

/**
 * @brief Sort a `uint16_t` array in ascending order.
 *
 * @param[in,out] arr Pointer to the array to be sorted.
 * @param[in] count Number of elements in @p arr.
 */
void esy_sort_u16(uint16_t *arr, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* ESY_COMMON_UTILS_ESY_SORT_H */
