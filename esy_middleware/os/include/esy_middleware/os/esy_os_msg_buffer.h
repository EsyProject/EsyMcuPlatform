/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_os_msg_buffer.h
 * @brief OS abstraction - message buffer (MessageBuffer-like).
 *
 * This module provides an OS-agnostic wrapper around a MessageBuffer primitive
 * (aligned with FreeRTOS MessageBuffer semantics), including:
 * - Static creation
 * - Task-context and ISR-context send/receive
 * - Reset and query APIs (capacity, free space, used bytes, next message size)
 *
 * Semantics:
 * - Data is sent/received as discrete *messages*; message boundaries are preserved.
 * - Task-context send can block waiting for space; ISR send must not block.
 * - A message is written only when the whole message (including internal length header)
 *   can fit. Otherwise:
 *   - Task-context send waits up to timeout
 *   - ISR send fails immediately
 *
 * Concurrency constraints (consistent with FreeRTOS recommendations):
 * - Supports single-producer + single-consumer concurrency.
 * - For multi-producer or multi-consumer usage, protect with a mutex at upper layer.
 *
 * Static allocation:
 * - Static message buffer creation uses caller-provided data buffer and opaque
 *   storage, allowing the implementation to remain fully opaque while avoiding
 *   dynamic allocation for the control block.
 */

#ifndef ESY_OS_MSG_BUFFER_H
#define ESY_OS_MSG_BUFFER_H

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
 * @brief Static message buffer storage size in bytes.
 *
 * This is an ABI contract for the OS port layer.
 * Increase this value if the internal implementation grows.
 *
 * The storage is used by @ref esy_os_msg_buffer_create_static and must persist
 * for the lifetime of the message buffer.
 */
#define ESY_OS_MSG_BUFFER_STATIC_STORAGE_BYTES 256u

/**
 * @brief Static message buffer storage alignment (bytes).
 *
 * Choose 8 for ESP32 / common 32-bit MCUs, generally safe.
 * If your platform needs stronger alignment, bump this value.
 */
#define ESY_OS_MSG_BUFFER_STATIC_STORAGE_ALIGN 8u

/* -------------------------------------------------------------------------- */
/* Opaque types                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief Message buffer handle (opaque).
 *
 * The concrete definition is OS/port-specific and hidden in the implementation.
 */
typedef struct esy_os_msg_buffer_s esy_os_msg_buffer_t;

/**
 * @brief Static message buffer storage object (opaque-bytes).
 *
 * The caller owns this object and must keep it alive for the lifetime of the
 * message buffer. The internal layout is implementation-defined.
 */
typedef struct esy_os_msg_buffer_static_storage_s {
#ifdef __cplusplus
  alignas(ESY_OS_MSG_BUFFER_STATIC_STORAGE_ALIGN)
#else
  _Alignas(ESY_OS_MSG_BUFFER_STATIC_STORAGE_ALIGN)
#endif
  uint8_t bytes[ESY_OS_MSG_BUFFER_STATIC_STORAGE_BYTES];
} esy_os_msg_buffer_static_storage_t;

/* -------------------------------------------------------------------------- */
/* Creation / destruction                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief Create a static message buffer (no dynamic allocation for control block).
 *
 * Uses caller-provided:
 * - A data storage area @p buffer_mem of at least @p capacity_bytes
 * - An opaque static storage object @p storage
 *
 * @param[in]  capacity_bytes Backing allocation in bytes; must exceed native message-length overhead + 1.
 * @param[in]  buffer_mem     Caller-provided data buffer (size >= capacity_bytes).
 * @param[in]  storage        Caller-provided opaque static storage object.
 *                            Must persist for the lifetime of the message buffer.
 * @param[out] out_mbuf       Output: created message buffer handle.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure (invalid arguments, unsupported, etc.).
 */
extern esy_err_t esy_os_msg_buffer_create_static(size_t capacity_bytes,
                                                void *buffer_mem,
                                                esy_os_msg_buffer_static_storage_t *storage,
                                                esy_os_msg_buffer_t **out_mbuf);

/**
 * @brief Delete / deinitialize a message buffer.
 *
 * This only deinitializes internal state and does not free caller-provided
 * memory.
 *
 * @param[in] mbuf Message buffer handle (may be NULL).
 */
extern void esy_os_msg_buffer_delete(esy_os_msg_buffer_t *mbuf);

/**
 * @brief Reset the message buffer (drop all pending messages).
 *
 * After a successful reset, the buffer becomes empty.
 *
 * @param[in] mbuf Message buffer handle.
 * @retval ESY_OK Reset completed.
 * @retval ESY_ERR_BUSY A task is blocked on the buffer; contents are unchanged.
 * @retval ESY_ERR_INVALID_ARG Invalid handle.
 */
extern esy_err_t esy_os_msg_buffer_reset(esy_os_msg_buffer_t *mbuf);

/* -------------------------------------------------------------------------- */
/* Task-context APIs                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief Send a single message (task context, may block).
 *
 * A send succeeds only if the *entire* message can be written into the buffer.
 *
 * Timeout convention:
 * - timeout_ms = 0         : return immediately
 * - timeout_ms = UINT32_MAX: wait forever (if the underlying OS supports it)
 *
 * @param[in] mbuf       Message buffer handle.
 * @param[in] msg        Pointer to message bytes to send.
 * @param[in] msg_len    Message length in bytes.
 * @param[in] timeout_ms Timeout in milliseconds.
 *
 * @retval ESY_OK          Success (message fully written).
 * @retval ESY_ERR_TIMEOUT Timed out without sending (implementation-defined).
 * @retval ESY_ERR_NO_MEM  Not enough space (timeout_ms == 0 case, implementation-defined).
 * @retval <0              Other failures (invalid arguments, OS error, etc.).
 */
extern esy_err_t esy_os_msg_buffer_send(esy_os_msg_buffer_t *mbuf,
                                       const void *msg,
                                       size_t msg_len,
                                       uint32_t timeout_ms);

/**
 * @brief Receive a single message (task context, may block).
 *
 * Receives one full message from the buffer, preserving message boundaries.
 *
 * Message preservation:
 * - For FreeRTOS MessageBuffer: if @p max_size is smaller than the message length,
 *   it leaves the entire message queued and returns ESY_ERR_NO_MEM.
 *
 * @param[in]  mbuf       Message buffer handle.
 * @param[out] out_buf    Output buffer to copy message bytes into.
 * @param[in]  max_size   Size of @p out_buf in bytes.
 * @param[out] out_copied Optional: actual number of bytes copied to @p out_buf.
 * @param[in]  timeout_ms Timeout in milliseconds:
 *                        - 0          : return immediately
 *                        - UINT32_MAX : wait indefinitely
 *
 * @retval ESY_OK          Success (one complete message received).
 * @retval ESY_ERR_TIMEOUT No message received before timeout, including an empty non-blocking read.
 * @retval <0              Other failures (invalid arguments, OS error, etc.).
 */
extern esy_err_t esy_os_msg_buffer_receive(esy_os_msg_buffer_t *mbuf,
                                          void *out_buf,
                                          size_t max_size,
                                          size_t *out_copied,
                                          uint32_t timeout_ms);

/**
 * @brief Peek the size of the next message (task context, non-destructive).
 *
 * Does not remove the message from the buffer.
 *
 * @param[in] mbuf Message buffer handle.
 *
 * @return Length in bytes of the next message; returns 0 if the buffer is empty.
 */
extern size_t esy_os_msg_buffer_next_msg_size(const esy_os_msg_buffer_t *mbuf);

/* -------------------------------------------------------------------------- */
/* ISR-context APIs                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief Send a single message from ISR (non-blocking).
 *
 * Succeeds only if the entire message can fit into the buffer at the time of call.
 * If not enough space is available, returns ESY_ERR_NO_MEM immediately.
 *
 * @param[in]  mbuf       Message buffer handle.
 * @param[in]  msg        Pointer to message bytes to send.
 * @param[in]  msg_len    Message length in bytes.
 * @param[out] need_yield Optional: set to true if a higher priority task should be
 *                        woken after ISR exit. May be NULL.
 *
 * @retval ESY_OK         Success.
 * @retval ESY_ERR_NO_MEM Not enough space to write the entire message.
 * @retval <0             Other failures (invalid arguments, unsupported, etc.).
 */
extern esy_err_t esy_os_msg_buffer_send_from_isr(esy_os_msg_buffer_t *mbuf,
                                                const void *msg,
                                                size_t msg_len,
                                                bool *need_yield);

/**
 * @brief Receive a single message from ISR (non-blocking).
 *
 * Behavior matches task-context receive regarding truncation.
 *
 * @param[in]  mbuf       Message buffer handle.
 * @param[out] out_buf    Output buffer.
 * @param[in]  max_size   Size of @p out_buf.
 * @param[out] out_copied Optional: actual number of bytes copied.
 * @param[out] need_yield Optional: set to true if a higher priority task should be
 *                        woken after ISR exit. May be NULL.
 *
 * @retval ESY_OK        Success (one message received).
 * @retval ESY_ERR_NO_MEM Empty buffer or destination too small; no message consumed.
 * @retval <0            Other failures (invalid arguments, unsupported, etc.).
 */
extern esy_err_t esy_os_msg_buffer_receive_from_isr(esy_os_msg_buffer_t *mbuf,
                                                   void *out_buf,
                                                   size_t max_size,
                                                   size_t *out_copied,
                                                   bool *need_yield);

/* -------------------------------------------------------------------------- */
/* Query APIs                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief Get the number of bytes currently used in the buffer.
 *
 * This includes all queued messages and any internal bookkeeping overhead
 * (e.g., per-message length headers), consistent with the underlying OS semantics.
 *
 * @param[in] mbuf Message buffer handle.
 *
 * @return Used bytes in the buffer.
 */
extern size_t esy_os_msg_buffer_bytes_used(const esy_os_msg_buffer_t *mbuf);

/**
 * @brief Get the number of free bytes currently available for writing.
 *
 * Note: Because message buffers require space for internal headers, the returned
 * free space does not necessarily imply that an arbitrary message of that size
 * can be written. A send succeeds only if the entire message plus overhead fits.
 *
 * @param[in] mbuf Message buffer handle.
 *
 * @return Free bytes in the buffer.
 */
extern size_t esy_os_msg_buffer_space_free(const esy_os_msg_buffer_t *mbuf);

/**
 * @brief Get the total capacity of the message buffer in bytes.
 *
 * @param[in] mbuf Message buffer handle.
 *
 * @return Usable capacity including message headers (allocation minus one).
 */
extern size_t esy_os_msg_buffer_capacity(const esy_os_msg_buffer_t *mbuf);

#ifdef __cplusplus
}
#endif

#endif /* ESY_OS_MSG_BUFFER_H */
