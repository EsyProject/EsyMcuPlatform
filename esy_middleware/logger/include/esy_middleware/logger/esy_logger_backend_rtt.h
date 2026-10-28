/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_LOGGER_BACKEND_RTT_H
#define ESY_LOGGER_BACKEND_RTT_H

/**
 * @file esy_logger_backend_rtt.h
 * @brief Logger backend: RTT-style synchronous output implementation.
 *
 * Characteristics:
 * - Conforms to @ref esy_logger_backend_t via acquire_buffer()/submit_buffer().
 * - Uses caller-owned scratch buffer storage and no heap allocation.
 * - Performs synchronous writes during submit through the linked SEGGER RTT
 *   implementation.
 */

#include "esy_logger_backend.h"

#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Caller-owned static resources for the RTT logger backend.
 *
 * The buffer must remain valid from successful init until deinit returns ESY_OK. It is used as
 * the logger front-end formatting block returned by acquire_buffer().
 */
typedef struct {
  uint8_t *buffer;    /**< Scratch buffer returned by acquire_buffer(). */
  size_t buffer_size; /**< Size of @ref buffer in bytes. */
} esy_logger_backend_rtt_static_resources_t;

/**
 * @brief Declare caller-owned RTT backend storage as one app-owned object.
 *
 * @param name        Object name to declare.
 * @param buffer_size Scratch buffer size in bytes.
 */
#define ESY_LOGGER_BACKEND_RTT_STATIC_STORAGE(name, buffer_size) \
  struct {                                                       \
    uint8_t buffer[(buffer_size)];                               \
  } name

/**
 * @brief Bind app-owned RTT backend storage to a resource descriptor.
 *
 * @param resources Pointer to @ref esy_logger_backend_rtt_static_resources_t.
 * @param storage   Object declared by
 *                  @ref ESY_LOGGER_BACKEND_RTT_STATIC_STORAGE.
 */
#define ESY_LOGGER_BACKEND_RTT_BIND_STATIC_RESOURCES(resources, storage) \
  do {                                                                   \
    (resources)->buffer = (storage).buffer;                              \
    (resources)->buffer_size = sizeof((storage).buffer);                 \
  } while (0)

/**
 * @brief RTT logger backend configuration.
 */
typedef struct {
  uint32_t up_buffer_index; /**< RTT up-buffer/channel index. */
  esy_logger_backend_rtt_static_resources_t
      *static_resources; /**< Caller-owned scratch buffer resources. */
} esy_logger_backend_rtt_config_t;

/**
 * @brief Populate RTT logger backend configuration with recommended defaults.
 *
 * Current implementation defaults:
 * - up_buffer_index = 0
 * - static_resources = NULL
 *
 * @param[out] cfg Configuration pointer (must not be NULL).
 */
extern void esy_logger_backend_rtt_default_cfg(
    esy_logger_backend_rtt_config_t *cfg);

/**
 * @brief Initialize RTT output with FreeRTOS synchronization.
 *
 * @param[in,out] logger_backend Output: backend object to be initialized.
 * @param[in]     config         Input: initialization configuration.
 *
 * @note Available from the FreeRTOS adapter target. Caller-driven integrations
 * use @ref esy_logger_backend_rtt_core_init. Task/boot context only, with an
 * externally serialized lifecycle owner. The
 * handle must be `{0}` before first use or successfully deinitialized. Invalid
 * configuration/creation failure leaves it unchanged and safe to deinit/retry.
 * Repeated init returns ESY_ERR_STATE and preserves the active instance.
 * Storage and the backend object must survive until deinit returns ESY_OK.
 * The returned raw acquire/submit callbacks are task-only because the RTT
 * provider may block. Pass the backend object's address as self. The scratch
 * buffer stays acquired until the synchronous provider write has returned.
 *
 * @retval ESY_OK              Success.
 * @retval ESY_ERR_INVALID_ARG Invalid configuration.
 * @retval ESY_ERR_STATE       Backend singleton is already initialized.
 *
 * @note This function performs no heap allocation. The supplied static
 *       resources must remain valid until deinit returns ESY_OK.
 */
extern esy_err_t esy_logger_backend_rtt_init(
    esy_logger_backend_t *logger_backend,
    const esy_logger_backend_rtt_config_t *config);

/**
 * @brief Stop the RTT backend and release its resources when idle (task only).
 *
 * The lifecycle owner must serialize init/bind/deinit and stop new direct
 * callback calls. This first detaches the frontend; if a frontend transaction
 * is active it returns BUSY without stopping that transaction. Once stopping,
 * new acquire calls fail; submit returns any outstanding buffer without output.
 * Pending queued output is discarded on success. A BUSY result retains the
 * handle and every resource; retry from the owner task with an application
 * deadline. Timeout does not authorize clearing or releasing storage.
 *
 * @param[in,out] logger_backend Zero-initialized or live backend handle.
 * @retval ESY_OK Fully released; the zero handle may be reused immediately.
 * @retval ESY_ERR_BUSY A frontend transaction, borrowed buffer, I/O or callback
 * is still active. Cleanup is not automatic: retain resources and retry.
 * @retval ESY_ERR_INVALID_ARG NULL/foreign handle or ISR call.
 */
extern esy_err_t esy_logger_backend_rtt_deinit(
    esy_logger_backend_t *logger_backend);

/**
 * @brief Initialize synchronous RTT output with local IRQ lease protection.
 * @note One owner core serializes logging, raw callbacks and lifecycle outside ISR
 * context. Other cores must not access the instance. The provider
 * must use a nonblocking RTT mode; submit calls SEGGER_RTT_Write synchronously
 * and reports partial output as IO. No task or timer is created. The same storage
 * and BUSY deinit ownership contract applies as for the OS adapter.
 * @param backend Zero-initialized backend object, borrowed until deinit succeeds.
 * @param config Borrowed scratch buffer and RTT up-buffer index.
 * @retval ESY_OK Ready for frontend binding.
 * @retval ESY_ERR_INVALID_ARG Invalid handle/configuration or ISR call.
 * @retval ESY_ERR_STATE Instance or backend is already live.
 */
esy_err_t esy_logger_backend_rtt_core_init(esy_logger_backend_t *backend,
    const esy_logger_backend_rtt_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* ESY_LOGGER_BACKEND_RTT_H */
