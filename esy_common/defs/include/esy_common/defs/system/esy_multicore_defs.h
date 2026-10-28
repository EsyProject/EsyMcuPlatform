/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_MULTICORE_DEFS_H
#define ESY_MULTICORE_DEFS_H

/**
 * @file esy_multicore_defs.h
 * @brief Inter-core (multi-core) queue types and backend vtable definitions.
 *
 * This header provides a platform-independent abstraction for inter-core queues.
 * The HAL backend supplies the concrete queue operations; the HAL wrapper
 * binds them to this interface via a vtable (function table).
 *
 * Design goals:
 * - Zero dynamic allocation: backend state can be stored in an embedded buffer.
 * - Simple semantics: blocking APIs for task/thread context, try_* for non-blocking use.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"

/**
 * @brief Size (in bytes) of the reserved private backend storage inside the queue handle.
 *
 * HAL backend implementations may use this space to store internal state without
 * dynamic memory allocation.
 */
#define ESY_MULTICORE_QUEUE_PRIV_BYTES 32

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Forward Declarations and Handle Types                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief Forward declaration of the inter-core queue handle.
 */
struct esy_multicore_queue_s;

/**
 * @brief Inter-core queue handle type.
 */
typedef struct esy_multicore_queue_s esy_multicore_queue_t;

/* -------------------------------------------------------------------------- */
/* Backend Vtable                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief Inter-core queue function table (bound by the HAL wrapper).
 *
 * The HAL wrapper implements the functions in this vtable by forwarding to the
 * HAL backend for the actual inter-core queue behavior. All functions take
 * @ref esy_multicore_queue_t* as the first parameter.
 *
 * @note
 * - "blocking" functions: may block the current execution context until the
 *   operation can complete.
 * - "try_*" functions: non-blocking; return immediately with a boolean result.
 *
 * @warning Blocking functions are generally not suitable for ISR context.
 */
typedef struct {
  /**
   * @brief Send one item to the queue (blocking).
   *
   * If the queue is full, this function blocks until space becomes available.
   *
   * @param[in] queue Queue handle
   * @param[in] data  Pointer to the item to send (item size is defined by init/implementation)
   */
  void (*vt_queue_send_blocking)(esy_multicore_queue_t *queue, const void *data);

  /**
   * @brief Try to send one item to the queue (non-blocking).
   *
   * @param[in] queue Queue handle
   * @param[in] data  Pointer to the item to send
   * @retval true  Send succeeded
   * @retval false Queue is full; nothing was sent
   */
  bool (*vt_queue_try_send)(esy_multicore_queue_t *queue, const void *data);

  /**
   * @brief Receive one item from the queue (blocking).
   *
   * If the queue is empty, this function blocks until an item becomes available.
   *
   * @param[in]  queue Queue handle
   * @param[out] data  Output buffer for the received item (size defined by init/implementation)
   */
  void (*vt_queue_receive_blocking)(esy_multicore_queue_t *queue, void *data);

  /**
   * @brief Try to receive one item from the queue (non-blocking).
   *
   * @param[in]  queue Queue handle
   * @param[out] data  Output buffer for the received item
   * @retval true  Receive succeeded and @p data was filled
   * @retval false Queue is empty; nothing was received
   */
  bool (*vt_queue_try_receive)(esy_multicore_queue_t *queue, void *data);
} esy_multicore_queue_vtable_t;

/* -------------------------------------------------------------------------- */
/* Handle Definition                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief Inter-core queue control block (opaque handle).
 *
 * Minimal fields:
 * - @ref ops: shared immutable provider table bound by the HAL wrapper
 * - @ref ctx: backend private context pointer (opaque to upper layers)
 * - @ref backend: fixed-size aligned private storage to avoid dynamic allocation
 */
typedef struct esy_multicore_queue_s {
  const esy_multicore_queue_vtable_t *ops; /**< HAL-owned immutable provider table. */
  void *ctx;                        /**< Backend private context pointer */

  union {
    max_align_t _a;                                     /**< Alignment helper */
    uint8_t     bytes[ESY_MULTICORE_QUEUE_PRIV_BYTES];  /**< Backend private storage */
  } backend;
} esy_multicore_queue_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_MULTICORE_DEFS_H */
