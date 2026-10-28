/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_os_semaphore.h
 * @brief OS abstraction layer - counting semaphore interface.
 *
 * This module provides an OS-agnostic counting semaphore abstraction.
 *
 * Features:
 * - Supports static creation
 * - Task-context take/give operations
 * - ISR-context take/give variants with optional context-switch request
 * - Internal implementation remains fully opaque
 * - Creation uses caller-provided opaque storage (no heap required)
 *
 * Semantics:
 * - This is a counting semaphore with range [0 .. max_count].
 * - give() increments the count by 1, but must not exceed max_count.
 * - take() decrements the count by 1 if count > 0.
 *
 * Context rules:
 * - Task-context API may block.
 * - ISR-context API must not block and uses *_from_isr() variants only.
 *
 * Return value contract:
 * - All functions returning esy_err_t use ESY_OK on success and a negative
 *   error code on failure (see esy_common_defs.h / project error codes).
 */

#ifndef ESY_OS_SEMAPHORE_H
#define ESY_OS_SEMAPHORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_middleware/os/esy_os_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Compile-time constants                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief Static semaphore storage size in bytes.
 *
 * This is an ABI contract for the OS port layer.
 * Increase this value if the internal implementation grows.
 *
 * The storage is used by @ref esy_os_semaphore_create_static and must persist for
 * the lifetime of the semaphore.
 */
#define ESY_OS_SEMAPHORE_STATIC_STORAGE_BYTES 128u

/**
 * @brief Static semaphore storage alignment (bytes).
 *
 * Choose 8 for ESP32 / common 32-bit MCUs, generally safe.
 * If your platform needs stronger alignment, bump this value.
 */
#define ESY_OS_SEMAPHORE_STATIC_STORAGE_ALIGN 8u

/* -------------------------------------------------------------------------- */
/* Opaque handle types                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Semaphore handle (opaque).
 *
 * The internal representation and OS-specific objects are hidden from callers.
 */
typedef struct esy_os_semaphore_s esy_os_semaphore_t;

/**
 * @brief Static semaphore storage object (opaque-bytes).
 *
 * The caller owns this object and must keep it alive for the lifetime of the semaphore.
 * The internal layout is implementation-defined.
 */
typedef struct esy_os_semaphore_static_storage_s {
#ifdef __cplusplus
  alignas(ESY_OS_SEMAPHORE_STATIC_STORAGE_ALIGN)
#else
  _Alignas(ESY_OS_SEMAPHORE_STATIC_STORAGE_ALIGN)
#endif
  uint8_t bytes[ESY_OS_SEMAPHORE_STATIC_STORAGE_BYTES];
} esy_os_semaphore_static_storage_t;

/* -------------------------------------------------------------------------- */
/* Creation / destruction                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief Create a statically allocated counting semaphore (no dynamic allocation).
 *
 * Uses caller-provided opaque storage and performs no dynamic memory allocation.
 *
 * @param[in]  max_count  Maximum semaphore count (must be > 0).
 * @param[in]  init_count Initial semaphore count (must be <= max_count).
 * @param[in]  storage    Caller-provided opaque static storage object (must not be NULL).
 *                        Must persist for the lifetime of the semaphore.
 * @param[out] out_sem    Output: created semaphore handle.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure (invalid arguments, unsupported, OS error, etc.).
 */
extern esy_err_t esy_os_semaphore_create_static(uint32_t max_count,
                                               uint32_t init_count,
                                               esy_os_semaphore_static_storage_t *storage,
                                               esy_os_semaphore_t **out_sem);

/**
 * @brief Delete / deinitialize a semaphore.
 *
 * Only internal state is deinitialized; caller-provided storage is not freed.
 *
 * @param[in] sem Semaphore handle.
 */
extern void esy_os_semaphore_delete(esy_os_semaphore_t *sem);

/* -------------------------------------------------------------------------- */
/* Task-context APIs                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief Take the semaphore (task context).
 *
 * This function blocks until the semaphore count becomes available and the
 * take operation succeeds.
 *
 * @note This API does not provide timeout control. If you need a timeout,
 *       consider adding a separate take-with-timeout API in the OS layer.
 *
 * @param[in] sem Semaphore handle.
 *
 * @retval ESY_OK Success (count decremented).
 * @retval <0     Failure (invalid arguments, OS error, etc.).
 */
extern esy_err_t esy_os_semaphore_take(esy_os_semaphore_t *sem);

/**
 * @brief Give the semaphore (task context).
 *
 * Increments the semaphore count by 1, without exceeding max_count.
 *
 * @param[in] sem Semaphore handle.
 *
 * @retval ESY_OK Success (count incremented).
 * @retval <0     Failure (invalid arguments, count overflow, OS error, etc.).
 */
extern esy_err_t esy_os_semaphore_give(esy_os_semaphore_t *sem);

/* -------------------------------------------------------------------------- */
/* ISR-context APIs                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief Take the semaphore from ISR context (non-blocking).
 *
 * This function must only be called from interrupt context. It never blocks.
 *
 * @param[in]  sem        Semaphore handle.
 * @param[out] need_yield Output: set to true if a context switch should be requested
 *                        when exiting the interrupt (may be NULL).
 *
 * @retval ESY_OK Success (count decremented).
 * @retval <0     Failure (count is zero, invalid arguments, OS error, etc.).
 */
extern esy_err_t esy_os_semaphore_take_from_isr(esy_os_semaphore_t *sem,
                                               bool *need_yield);

/**
 * @brief Give the semaphore from ISR context (non-blocking).
 *
 * This function must only be called from interrupt context. It never blocks.
 *
 * @param[in]  sem        Semaphore handle.
 * @param[out] need_yield Output: set to true if a context switch should be requested
 *                        when exiting the interrupt (may be NULL).
 *
 * @retval ESY_OK Success (count incremented).
 * @retval <0     Failure (count overflow, invalid arguments, OS error, etc.).
 */
extern esy_err_t esy_os_semaphore_give_from_isr(esy_os_semaphore_t *sem,
                                               bool *need_yield);

#ifdef __cplusplus
}
#endif

#endif /* ESY_OS_SEMAPHORE_H */
