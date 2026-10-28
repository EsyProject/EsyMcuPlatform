/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/os/esy_os_queue.h"

#include <string.h>
#include <stdint.h>

#include "esy_os_freertos_internal.h"
#ifdef ESY_PLATFORM_ESP32
#include "freertos/queue.h"
#else
#include "queue.h"
#endif

/* Opaque handle real definition (private) */
struct esy_os_queue_s {
  QueueHandle_t handle;
};

/* Internal layout stored inside opaque static storage */
struct esy_os_queue_static_impl_s {
  StaticQueue_t native;          /* FreeRTOS static queue control block */
  struct esy_os_queue_s wrapper; /* Public opaque wrapper */
};

_Static_assert(_Alignof(esy_os_queue_static_storage_t) >=
                   _Alignof(struct esy_os_queue_static_impl_s),
               "Native control block alignment exceeds public storage");

/* Compile-time guard: public storage must be large enough */
_Static_assert(ESY_OS_QUEUE_STATIC_STORAGE_BYTES >= sizeof(struct esy_os_queue_static_impl_s),
               "ESY_OS_QUEUE_STATIC_STORAGE_BYTES too small");

esy_err_t esy_os_queue_create_static(size_t item_size,
                                     size_t max_items,
                                     void *queue_buffer,
                                     esy_os_queue_static_storage_t *storage,
                                     esy_os_queue_t **out_queue) {
  if (out_queue) *out_queue = NULL;
#if (configSUPPORT_STATIC_ALLOCATION == 1)
  if (!storage || !out_queue || !queue_buffer || item_size == 0 || max_items == 0) {
    return ESY_ERR_INVALID_ARG;
  }

  /* Optional: multiplication overflow guard */
  if (item_size > (SIZE_MAX / max_items)) {
    return ESY_ERR_INVALID_ARG;
  }

  /* Storage alignment guard */
  if (((uintptr_t)storage->bytes % ESY_OS_QUEUE_STATIC_STORAGE_ALIGN) != 0) {
    return ESY_ERR_INVALID_ARG;
  }

#if defined(configQUEUE_STORAGE_ALIGNED)
  /* FreeRTOS optional requirement for queue storage alignment */
  if (((uintptr_t)queue_buffer % configQUEUE_STORAGE_ALIGNED) != 0) {
    return ESY_ERR_INVALID_ARG;
  }
#endif

  /* Reinterpret opaque bytes as internal static object */
  struct esy_os_queue_static_impl_s *impl =
      (struct esy_os_queue_static_impl_s *)(void *)storage->bytes;

  memset(impl, 0, sizeof(*impl));

  QueueHandle_t h = xQueueCreateStatic((UBaseType_t)max_items,
                                       (UBaseType_t)item_size,
                                       (uint8_t *)queue_buffer,
                                       &impl->native);
  if (!h) return ESY_ERR_FAIL;

  impl->wrapper.handle = h;

  *out_queue = &impl->wrapper;
  return ESY_OK;
#else
  (void)item_size; (void)max_items; (void)queue_buffer; (void)storage; (void)out_queue;
  return ESY_ERR_NOT_SUPPORTED;
#endif
}

void esy_os_queue_delete(esy_os_queue_t *queue) {
  if (!queue || !queue->handle) return;

  vQueueDelete(queue->handle);
  queue->handle = NULL;
}

esy_err_t esy_os_queue_send(esy_os_queue_t *queue,
                            const void *item,
                            uint32_t timeout_ms) {
  if (!queue || !queue->handle || !item) return ESY_ERR_INVALID_ARG;

  BaseType_t r = xQueueSend(queue->handle, item, esy_os_timeout_ticks(timeout_ms));
  return (r == pdPASS) ? ESY_OK : ESY_ERR_TIMEOUT;
}

esy_err_t esy_os_queue_receive(esy_os_queue_t *queue,
                               void *item,
                               uint32_t timeout_ms) {
  if (!queue || !queue->handle || !item) return ESY_ERR_INVALID_ARG;

  BaseType_t r = xQueueReceive(queue->handle, item, esy_os_timeout_ticks(timeout_ms));
  return (r == pdPASS) ? ESY_OK : ESY_ERR_TIMEOUT;
}

esy_err_t esy_os_queue_send_from_isr(esy_os_queue_t *queue,
                                    const void *item,
                                    bool *need_yield) {
  if (!queue || !queue->handle || !item) return ESY_ERR_INVALID_ARG;

  BaseType_t hp_task_woken = pdFALSE;
  BaseType_t r = xQueueSendFromISR(queue->handle, item, &hp_task_woken);

  if (need_yield) {
    *need_yield = (hp_task_woken == pdTRUE);
  }

  return (r == pdPASS) ? ESY_OK : ESY_ERR_FAIL;
}

esy_err_t esy_os_queue_receive_from_isr(esy_os_queue_t *queue,
                                       void *item,
                                       bool *need_yield) {
  if (!queue || !queue->handle || !item) return ESY_ERR_INVALID_ARG;

  BaseType_t hp_task_woken = pdFALSE;
  BaseType_t r = xQueueReceiveFromISR(queue->handle, item, &hp_task_woken);

  if (need_yield) {
    *need_yield = (hp_task_woken == pdTRUE);
  }

  return (r == pdPASS) ? ESY_OK : ESY_ERR_FAIL;
}
