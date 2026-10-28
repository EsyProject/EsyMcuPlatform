/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_os_stream_buffer.h
 * @brief OS abstraction layer - byte stream buffer (Stream Buffer).
 *
 * This module provides an OS-agnostic wrapper over a byte-oriented stream buffer,
 * aligning with the semantics of FreeRTOS StreamBuffer.
 *
 * Key properties:
 * - Byte stream semantics (no message boundaries).
 * - Supports static creation.
 * - Task-context APIs may block with timeout.
 * - ISR-context APIs are non-blocking and provide *_from_isr() variants.
 * - Supports a trigger level (wake-up threshold) to reduce spurious wake-ups.
 *
 * Contracts:
 * - Functions returning esy_err_t: ESY_OK on success; negative values on failure.
 * - Send/receive operations may complete partially (unlike message buffers).
 * - The implementation defines the exact mapping to the underlying OS.
 *
 * Concurrency notes (typical FreeRTOS guidance):
 * - Stream buffers are commonly used with one writer and one reader.
 * - If you need multi-producer / multi-consumer access, protect calls with
 *   a mutex at a higher layer.
 *
 * Static allocation:
 * - Static stream buffer creation uses caller-provided data buffer and opaque
 *   storage, allowing the implementation to remain fully opaque while avoiding
 *   dynamic allocation for the control block.
 */

#ifndef ESY_OS_STREAM_BUFFER_H
#define ESY_OS_STREAM_BUFFER_H

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
 * @brief Static stream buffer storage size in bytes.
 *
 * This is an ABI contract for the OS port layer.
 * Increase this value if the internal implementation grows.
 *
 * The storage is used by @ref esy_os_stream_buffer_create_static and must persist
 * for the lifetime of the stream buffer.
 */
#define ESY_OS_STREAM_BUFFER_STATIC_STORAGE_BYTES 256u

/**
 * @brief Static stream buffer storage alignment (bytes).
 *
 * Choose 8 for ESP32 / common 32-bit MCUs, generally safe.
 * If your platform needs stronger alignment, bump this value.
 */
#define ESY_OS_STREAM_BUFFER_STATIC_STORAGE_ALIGN 8u

/* -------------------------------------------------------------------------- */
/* Opaque types                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief Stream buffer handle (opaque).
 *
 * The internal representation is OS-specific and hidden from callers.
 */
typedef struct esy_os_stream_buffer_s esy_os_stream_buffer_t;

/**
 * @brief Static stream buffer storage object (opaque-bytes).
 *
 * The caller owns this object and must keep it alive for the lifetime of the
 * stream buffer. The internal layout is implementation-defined.
 */
typedef struct esy_os_stream_buffer_static_storage_s {
#ifdef __cplusplus
  alignas(ESY_OS_STREAM_BUFFER_STATIC_STORAGE_ALIGN)
#else
  _Alignas(ESY_OS_STREAM_BUFFER_STATIC_STORAGE_ALIGN)
#endif
  uint8_t bytes[ESY_OS_STREAM_BUFFER_STATIC_STORAGE_BYTES];
} esy_os_stream_buffer_static_storage_t;

/* -------------------------------------------------------------------------- */
/* Creation / destruction                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief Create a statically allocated stream buffer (no dynamic allocation for control block).
 *
 * Uses caller-provided data buffer memory and caller-provided opaque storage for
 * the internal static control block.
 *
 * @param[in]  capacity_bytes  Backing allocation in bytes (at least 2); usable capacity is one less.
 * @param[in]  buffer_mem      Caller-provided data buffer. Size must be at least
 *                             @p capacity_bytes bytes.
 * @param[in]  storage         Caller-provided opaque static storage object.
 *                             Must persist for the lifetime of the stream buffer.
 * @param[out] out_sbuf        Output: created stream buffer handle.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure (invalid arguments, unsupported, OS error, etc.).
 */
extern esy_err_t esy_os_stream_buffer_create_static(size_t capacity_bytes,
                                                   void *buffer_mem,
                                                   esy_os_stream_buffer_static_storage_t *storage,
                                                   esy_os_stream_buffer_t **out_sbuf);

/**
 * @brief Delete / deinitialize a stream buffer.
 *
 * Only internal state is deinitialized; caller-provided storage is not freed.
 *
 * @param[in] sbuf Stream buffer handle.
 */
extern void esy_os_stream_buffer_delete(esy_os_stream_buffer_t *sbuf);

/**
 * @brief Reset the stream buffer (discard all buffered data).
 *
 * Resets internal read/write indices and drops all currently buffered bytes.
 *
 * @param[in] sbuf Stream buffer handle.
 * @retval ESY_OK Reset completed.
 * @retval ESY_ERR_BUSY A task is blocked on the buffer; contents are unchanged.
 * @retval ESY_ERR_INVALID_ARG Invalid handle.
 */
extern esy_err_t esy_os_stream_buffer_reset(esy_os_stream_buffer_t *sbuf);

/* -------------------------------------------------------------------------- */
/* Task-context APIs                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief Send bytes into the stream buffer (task context).
 *
 * This call may block up to @p timeout_ms waiting for space. The operation may
 * succeed partially (i.e., fewer than @p size bytes may be written).
 *
 * @param[in]  sbuf            Stream buffer handle.
 * @param[in]  data            Pointer to the bytes to send.
 * @param[in]  size            Number of bytes to send.
 * @param[out] out_bytes_sent  Output: number of bytes actually written.
 *                             May be NULL if not needed.
 * @param[in]  timeout_ms      Timeout in milliseconds:
 *                             - 0: return immediately
 *                             - UINT32_MAX: wait indefinitely
 *
 * @retval ESY_OK Success (possibly partial; see @p out_bytes_sent).
 * @retval <0     Failure (invalid arguments, OS error, etc.).
 */
extern esy_err_t esy_os_stream_buffer_send(esy_os_stream_buffer_t *sbuf,
                                          const void *data,
                                          size_t size,
                                          size_t *out_bytes_sent,
                                          uint32_t timeout_ms);

/**
 * @brief Receive bytes from the stream buffer (task context).
 *
 * This call may block up to @p timeout_ms waiting for data. The operation may
 * succeed partially (i.e., fewer than @p max_size bytes may be read).
 *
 * @param[in]  sbuf                Stream buffer handle.
 * @param[out] data                Destination buffer for received bytes.
 * @param[in]  max_size            Maximum number of bytes to receive.
 * @param[out] out_bytes_received  Output: number of bytes actually read.
 *                                May be NULL if not needed.
 * @param[in]  timeout_ms          Timeout in milliseconds:
 *                                - 0: return immediately
 *                                - UINT32_MAX: wait indefinitely
 *
 * @retval ESY_OK Success (possibly partial; see @p out_bytes_received).
 * @retval <0     Failure (invalid arguments, OS error, etc.).
 */
extern esy_err_t esy_os_stream_buffer_receive(esy_os_stream_buffer_t *sbuf,
                                             void *data,
                                             size_t max_size,
                                             size_t *out_bytes_received,
                                             uint32_t timeout_ms);

/* -------------------------------------------------------------------------- */
/* ISR-context APIs                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief Send bytes into the stream buffer from ISR context (non-blocking).
 *
 * This function must only be called from interrupt context. It never blocks.
 * The operation may succeed partially.
 *
 * @param[in]  sbuf            Stream buffer handle.
 * @param[in]  data            Pointer to the bytes to send.
 * @param[in]  size            Number of bytes to send.
 * @param[out] out_bytes_sent  Output: number of bytes actually written.
 *                             May be NULL if not needed.
 * @param[out] need_yield      Output: set to true if a context switch should be
 *                             requested when exiting the ISR. May be NULL.
 *
 * @retval ESY_OK Success (possibly partial; see @p out_bytes_sent).
 * @retval <0     Failure (invalid arguments, OS error, etc.).
 */
extern esy_err_t esy_os_stream_buffer_send_from_isr(esy_os_stream_buffer_t *sbuf,
                                                   const void *data,
                                                   size_t size,
                                                   size_t *out_bytes_sent,
                                                   bool *need_yield);

/**
 * @brief Receive bytes from the stream buffer from ISR context (non-blocking).
 *
 * This function must only be called from interrupt context. It never blocks.
 * The operation may succeed partially.
 *
 * @param[in]  sbuf                Stream buffer handle.
 * @param[out] data                Destination buffer for received bytes.
 * @param[in]  max_size            Maximum number of bytes to receive.
 * @param[out] out_bytes_received  Output: number of bytes actually read.
 *                                May be NULL if not needed.
 * @param[out] need_yield          Output: set to true if a context switch should
 *                                be requested when exiting the ISR. May be NULL.
 *
 * @retval ESY_OK Success (possibly partial; see @p out_bytes_received).
 * @retval <0     Failure (invalid arguments, OS error, etc.).
 */
extern esy_err_t esy_os_stream_buffer_receive_from_isr(esy_os_stream_buffer_t *sbuf,
                                                      void *data,
                                                      size_t max_size,
                                                      size_t *out_bytes_received,
                                                      bool *need_yield);

/* -------------------------------------------------------------------------- */
/* Configuration                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief Set the trigger level (wake-up threshold) for receive operations.
 *
 * When the number of bytes available to read reaches @p trigger_level, tasks
 * blocked on receive may be woken up. Exact behavior depends on the underlying
 * OS (mirrors FreeRTOS xStreamBufferSetTriggerLevel()).
 *
 * @param[in] sbuf           Stream buffer handle.
 * @param[in] trigger_level  Trigger level in bytes. A value of 0 means using an
 *                           implementation-defined default.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure (invalid arguments, OS error, unsupported, etc.).
 */
extern esy_err_t esy_os_stream_buffer_set_trigger_level(esy_os_stream_buffer_t *sbuf,
                                                       size_t trigger_level);

/* -------------------------------------------------------------------------- */
/* Queries                                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief Get the number of bytes currently stored (readable) in the buffer.
 *
 * @param[in] sbuf Stream buffer handle.
 * @return Number of bytes available to read.
 */
extern size_t esy_os_stream_buffer_bytes_used(const esy_os_stream_buffer_t *sbuf);

/**
 * @brief Get the remaining free space (writable bytes) in the buffer.
 *
 * @param[in] sbuf Stream buffer handle.
 * @return Number of bytes that can still be written.
 */
extern size_t esy_os_stream_buffer_space_free(const esy_os_stream_buffer_t *sbuf);

/**
 * @brief Get the total capacity of the stream buffer in bytes.
 *
 * @param[in] sbuf Stream buffer handle.
 * @return Usable capacity in bytes (backing allocation minus one).
 */
extern size_t esy_os_stream_buffer_capacity(const esy_os_stream_buffer_t *sbuf);

#ifdef __cplusplus
}
#endif

#endif /* ESY_OS_STREAM_BUFFER_H */
