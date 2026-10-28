/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_LOGGER_BACKEND_H
#define ESY_LOGGER_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_logger_backend.h
 * @brief Logger backend interface (zero dynamic-memory requirement is
 *        implementation-specific).
 *
 * Contract:
 * - acquire/submit are synchronous calls; whether they are allowed in
 *   interrupt context is backend-dependent.
 * - acquire_buffer() provides a writable buffer for formatting log output;
 *   submit_buffer() commits that buffer for output.
 * - acquire → write → submit is a single paired transaction; buffer ownership
 *   is returned to the backend after submit.
 * - If variable-sized buffers are not supported, acquire may return a fixed
 *   buffer length; upper layers must respect the returned @p buf_len.
 */

/**
 * @brief Logger backend interface.
 *
 * Initialize caller-owned handles with `{0}` before the first concrete backend
 * init. Do not copy a live handle or re-zero it to bypass cleanup. Successful
 * deinit restores this initial state; failed init leaves an inactive handle
 * unchanged. Repeated init of a live handle is rejected without modifying it.
 * The binding/lifecycle owner serializes all init, bind, detach and deinit calls.
 * Direct users must stop starting new callback calls before deinit; already
 * acquired buffers must still be submitted (length zero cancels a buffer).
 * After successful deinit, it may be initialized again or deinitialized again
 * (an idempotent no-op); its cleared callbacks must not be invoked.
 * Concrete backend documentation specifies task/ISR and stop behavior.
 */
typedef struct {
    /**
     * @brief Acquire a writable buffer.
     *
     * @param[in]  self    Pointer to this esy_logger_backend_t object, not its ctx field; non-NULL.
     * @param[out] buf     Output pointer to a writable buffer (must not be NULL).
     * @param[out] buf_len Output writable buffer size in bytes (>0 indicates success).
     *
     * @retval ESY_OK              Success.
     * @retval <0 (negative esy_err_t) Failure (e.g. resource busy, invalid arguments).
     *
     * @note Upper layers must not write beyond @p buf_len and must not
     *       acquire another buffer in the same producer transaction before submit.
     *       Different producers may hold separate buffers if the backend permits it.
     */
    esy_err_t (*acquire_buffer)(void *self, uint8_t **buf, size_t *buf_len);

    /**
     * @brief Submit a previously acquired buffer for output.
     *
     * @param[in] self Pointer to this esy_logger_backend_t object, not its ctx field; non-NULL.
     * @param[in] buf  Pointer to data to be submitted (must be the buffer returned
     *                 by the most recent acquire_buffer() call).
     * @param[in] len  Number of bytes actually written (<= buf_len returned by acquire).
     *
     * @retval ESY_OK              Success.
     * @retval <0 (negative esy_err_t) Failure.
     *
     * @warning After submission, buffer ownership returns to the backend.
     *          Do not access the buffer after calling submit_buffer().
     */
    esy_err_t (*submit_buffer)(void *self, const uint8_t *buf, size_t len);

    /**
     * @brief Backend-specific context pointer.
     *
     * Owned and interpreted by the backend implementation.
     */
    void *ctx;
} esy_logger_backend_t;

#ifdef __cplusplus
}
#endif

#endif /* ESY_LOGGER_BACKEND_H */
