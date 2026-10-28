/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_multicore.h"

#include <stddef.h>
#include <string.h>

#include "esy_hal_backend/esy_hb_multicore.h"

/* ============ vt_* functions bound to queue->ops ============ */

static void vt_queue_send_blocking(esy_multicore_queue_t *queue,
                                   const void *data) {
  esy_hb_multicore_queue_send_blocking(queue, data);
}

static bool vt_queue_try_send(esy_multicore_queue_t *queue,
                              const void *data) {
  return esy_hb_multicore_queue_try_send(queue, data);
}

static void vt_queue_receive_blocking(esy_multicore_queue_t *queue,
                                      void *data) {
  esy_hb_multicore_queue_receive_blocking(queue, data);
}

static bool vt_queue_try_receive(esy_multicore_queue_t *queue,
                                 void *data) {
  return esy_hb_multicore_queue_try_receive(queue, data);
}

static const esy_multicore_queue_vtable_t k_queue_ops = {
    .vt_queue_send_blocking = vt_queue_send_blocking,
    .vt_queue_try_send = vt_queue_try_send,
    .vt_queue_receive_blocking = vt_queue_receive_blocking,
    .vt_queue_try_receive = vt_queue_try_receive,
};

/* ================= core control ================= */

esy_err_t esy_hal_multicore_start_core(uint8_t core_id, void (*entry)(void)) {
  if (!entry) return ESY_ERR_INVALID_ARG;
  return esy_hb_multicore_start_core(core_id, entry);
}

/* ================= queue APIs ================= */

esy_err_t esy_hal_multicore_queue_init(esy_multicore_queue_t *queue,
                                            uint32_t item_size,
                                            uint32_t depth,
                                            uint8_t *buf,
                                            size_t buf_size) {
  if (!queue || !buf || item_size == 0 || depth == 0)
    return ESY_ERR_INVALID_ARG;
  if (item_size > UINT16_MAX || depth > UINT16_MAX)
    return ESY_ERR_INVALID_ARG;
  if (buf_size / ((size_t)depth + 1u) < item_size) return ESY_ERR_NO_MEM;

  /* Forward directly to the HAL backend. */
  esy_err_t rc = esy_hb_multicore_queue_init(queue, item_size, depth, buf,
                                           buf_size);
  if (rc != ESY_OK) return rc;

  /* Publish the shared provider only after backend initialization succeeds. */
  queue->ops = &k_queue_ops;
  return ESY_OK;
}

void esy_hal_multicore_queue_deinit(esy_multicore_queue_t *queue) {
  if (!queue) return;
  /* Forward directly to the HAL backend. */
  esy_hb_multicore_queue_deinit(queue);
  memset(queue, 0, sizeof(*queue));
}

/* Public queue operations dispatch through the bound provider. */

void esy_hal_multicore_queue_send_blocking(esy_multicore_queue_t *queue,
                                                const void *data) {
  if (!queue || !data) return;
  if (queue->ops == NULL || !queue->ops->vt_queue_send_blocking) return;
  queue->ops->vt_queue_send_blocking(queue, data);
}

bool esy_hal_multicore_queue_try_send(esy_multicore_queue_t *queue,
                                           const void *data) {
  if (!queue || !data) return false;
  if (queue->ops == NULL || !queue->ops->vt_queue_try_send) return false;
  return queue->ops->vt_queue_try_send(queue, data);
}

void esy_hal_multicore_queue_receive_blocking(esy_multicore_queue_t *queue,
                                                   void *data) {
  if (!queue || !data) return;
  if (queue->ops == NULL || !queue->ops->vt_queue_receive_blocking) return;
  queue->ops->vt_queue_receive_blocking(queue, data);
}

bool esy_hal_multicore_queue_try_receive(esy_multicore_queue_t *queue,
                                              void *data) {
  if (!queue || !data) return false;
  if (queue->ops == NULL || !queue->ops->vt_queue_try_receive) return false;
  return queue->ops->vt_queue_try_receive(queue, data);
}
