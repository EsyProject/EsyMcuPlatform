/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_os_mutex.h
 * @brief OS abstraction layer - mutex (mutual exclusion) interface.
 *
 * This module defines an OS-agnostic mutex abstraction used to synchronize
 * access to shared resources between tasks.
 *
 * Features:
 * - Supports static creation
 * - Internal implementation remains fully opaque
 * - Creation uses caller-provided opaque storage (no heap required)
 *
 * Usage notes:
 * - Mutex APIs must NOT be called from interrupt context.
 * - A mutex must always be unlocked by the same task that locked it.
 * - Prefer pairing @ref esy_os_mutex_lock and @ref esy_os_mutex_unlock
 *   in the same logical scope to avoid deadlocks.
 */

#ifndef ESY_OS_MUTEX_H
#define ESY_OS_MUTEX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_middleware/os/esy_os_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Compile-time constants                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief Static mutex storage size in bytes.
 *
 * This is an ABI contract for the OS port layer.
 * Increase this value if the internal implementation grows.
 *
 * The storage is used by @ref esy_os_mutex_create_static and must persist for
 * the lifetime of the mutex.
 */
#define ESY_OS_MUTEX_STATIC_STORAGE_BYTES 128u

/**
 * @brief Static mutex storage alignment (bytes).
 *
 * Choose 8 for ESP32 / common 32-bit MCUs, generally safe.
 * If your platform needs stronger alignment, bump this value.
 */
#define ESY_OS_MUTEX_STATIC_STORAGE_ALIGN 8u

/* -------------------------------------------------------------------------- */
/* Opaque handle types                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Mutex handle (opaque).
 *
 * The internal structure and OS-specific representation are hidden from users.
 */
typedef struct esy_os_mutex_s esy_os_mutex_t;

/**
 * @brief Static mutex storage object.
 *
 * The caller owns this object and must keep it alive for the lifetime of the mutex.
 * The internal layout is implementation-defined.
 */
typedef struct esy_os_mutex_static_storage_s {
#ifdef __cplusplus
  alignas(ESY_OS_MUTEX_STATIC_STORAGE_ALIGN)
#else
  _Alignas(ESY_OS_MUTEX_STATIC_STORAGE_ALIGN)
#endif
  uint8_t bytes[ESY_OS_MUTEX_STATIC_STORAGE_BYTES];
} esy_os_mutex_static_storage_t;

/* -------------------------------------------------------------------------- */
/* Creation / destruction                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief Create a statically allocated mutex.
 *
 * Uses caller-provided opaque storage and performs no dynamic memory allocation.
 *
 * @param[in]  storage   Caller-provided static storage object (must not be NULL).
 *                       Must persist for the lifetime of the mutex.
 * @param[out] out_mutex Output: created mutex handle.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure (invalid arguments, unsupported, OS error, etc.).
 */
extern esy_err_t esy_os_mutex_create_static(esy_os_mutex_static_storage_t *storage,
                                            esy_os_mutex_t **out_mutex);

/**
 * @brief Delete / deinitialize a mutex.
 *
 * This only deinitializes internal state and does not free caller-provided
 * storage.
 *
 * @param[in] mutex Mutex handle.
 */
extern void esy_os_mutex_delete(esy_os_mutex_t *mutex);

/* -------------------------------------------------------------------------- */
/* Lock / unlock operations                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief Lock a mutex (block until acquired).
 *
 * If the mutex is already held by another task, the calling task will block
 * until the mutex becomes available.
 *
 * Recursive locking behavior depends on the underlying OS implementation.
 *
 * @warning Must not be called from interrupt context.
 *
 * @param[in] mutex Mutex handle.
 */
extern void esy_os_mutex_lock(esy_os_mutex_t *mutex);

/**
 * @brief Unlock a mutex.
 *
 * The mutex must be unlocked by the same task that successfully locked it.
 * Unlocking a mutex not owned by the calling task results in undefined behavior
 * (OS-dependent).
 *
 * @warning Must not be called from interrupt context.
 *
 * @param[in] mutex Mutex handle.
 */
extern void esy_os_mutex_unlock(esy_os_mutex_t *mutex);

#ifdef __cplusplus
}
#endif

#endif /* ESY_OS_MUTEX_H */
