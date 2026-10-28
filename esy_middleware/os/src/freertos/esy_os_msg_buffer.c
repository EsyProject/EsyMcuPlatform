/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/os/esy_os_msg_buffer.h"

#include <string.h>
#include <stdint.h>

#include "esy_os_freertos_internal.h"
#ifdef ESY_PLATFORM_ESP32
#include "freertos/message_buffer.h"
#else
#include "message_buffer.h"
#endif

/* Opaque handle real definition (private) */
struct esy_os_msg_buffer_s {
  MessageBufferHandle_t handle;
  size_t capacity_bytes;
};

/* Internal layout stored inside opaque static storage */
struct esy_os_msg_buffer_static_impl_s {
  StaticMessageBuffer_t native;      /* FreeRTOS static message buffer object */
  struct esy_os_msg_buffer_s wrapper;/* Public opaque wrapper */
};

_Static_assert(_Alignof(esy_os_msg_buffer_static_storage_t) >=
                   _Alignof(struct esy_os_msg_buffer_static_impl_s),
               "Native control block alignment exceeds public storage");

/* Compile-time guard: public storage must be large enough */
_Static_assert(ESY_OS_MSG_BUFFER_STATIC_STORAGE_BYTES >= sizeof(struct esy_os_msg_buffer_static_impl_s),
               "ESY_OS_MSG_BUFFER_STATIC_STORAGE_BYTES too small");

static TickType_t esy_os_msg_buffer_ms_to_ticks_(uint32_t timeout_ms) {
  if (timeout_ms == UINT32_MAX) return portMAX_DELAY;
  return (TickType_t)esy_os_timeout_ticks(timeout_ms);
}

esy_err_t esy_os_msg_buffer_create_static(size_t capacity_bytes,
                                          void *buffer_mem,
                                          esy_os_msg_buffer_static_storage_t *storage,
                                          esy_os_msg_buffer_t **out_mbuf) {
  if (out_mbuf) *out_mbuf = NULL;
#if (configSUPPORT_STATIC_ALLOCATION == 1)
  if (!out_mbuf || !buffer_mem || !storage || capacity_bytes <= sizeof(configMESSAGE_BUFFER_LENGTH_TYPE) + 1u) {
    return ESY_ERR_INVALID_ARG;
  }

  /* Storage alignment guard */
  if (((uintptr_t)storage->bytes % ESY_OS_MSG_BUFFER_STATIC_STORAGE_ALIGN) != 0) {
    return ESY_ERR_INVALID_ARG;
  }

  struct esy_os_msg_buffer_static_impl_s *impl =
      (struct esy_os_msg_buffer_static_impl_s *)(void *)storage->bytes;

  memset(impl, 0, sizeof(*impl));

  MessageBufferHandle_t h = xMessageBufferCreateStatic(capacity_bytes,
                                                       (uint8_t *)buffer_mem,
                                                       &impl->native);
  if (!h) return ESY_ERR_NO_MEM;

  impl->wrapper.handle = h;
  impl->wrapper.capacity_bytes = capacity_bytes - 1u;

  *out_mbuf = &impl->wrapper;
  return ESY_OK;
#else
  (void)capacity_bytes; (void)buffer_mem; (void)storage; (void)out_mbuf;
  return ESY_ERR_NOT_SUPPORTED;
#endif
}

void esy_os_msg_buffer_delete(esy_os_msg_buffer_t *mbuf) {
  if (!mbuf || !mbuf->handle) return;

  vMessageBufferDelete(mbuf->handle);
  mbuf->handle = NULL;
}

esy_err_t esy_os_msg_buffer_reset(esy_os_msg_buffer_t *mbuf) {
  if (!mbuf || !mbuf->handle) return ESY_ERR_INVALID_ARG;
  return xMessageBufferReset(mbuf->handle) == pdPASS ? ESY_OK : ESY_ERR_BUSY;
}

esy_err_t esy_os_msg_buffer_send(esy_os_msg_buffer_t *mbuf,
                                 const void *msg,
                                 size_t msg_len,
                                 uint32_t timeout_ms) {
  if (!mbuf || !mbuf->handle || (!msg || msg_len == 0)) return ESY_ERR_INVALID_ARG;

  size_t sent = xMessageBufferSend(mbuf->handle,
                                   msg,
                                   msg_len,
                                   esy_os_msg_buffer_ms_to_ticks_(timeout_ms));

  if (sent == msg_len) return ESY_OK;

  /* MessageBuffer send is all-or-nothing: 0 or full length */
  if (timeout_ms == 0) return ESY_ERR_NO_MEM;
  return ESY_ERR_TIMEOUT;
}

esy_err_t esy_os_msg_buffer_receive(esy_os_msg_buffer_t *mbuf,
                                    void *out_buf,
                                    size_t max_size,
                                    size_t *out_copied,
                                    uint32_t timeout_ms) {
  if (!mbuf || !mbuf->handle || (!out_buf || max_size == 0)) return ESY_ERR_INVALID_ARG;

  size_t copied = xMessageBufferReceive(mbuf->handle,
                                        out_buf,
                                        max_size,
                                        esy_os_msg_buffer_ms_to_ticks_(timeout_ms));
  if (out_copied) *out_copied = copied;

  if (copied != 0u) return ESY_OK;
  if (xStreamBufferNextMessageLengthBytes(mbuf->handle) > max_size)
    return ESY_ERR_NO_MEM;
  return ESY_ERR_TIMEOUT;
}

size_t esy_os_msg_buffer_next_msg_size(const esy_os_msg_buffer_t *mbuf) {
  if (!mbuf || !mbuf->handle) return 0;
  return xStreamBufferNextMessageLengthBytes(mbuf->handle);
}

esy_err_t esy_os_msg_buffer_send_from_isr(esy_os_msg_buffer_t *mbuf,
                                          const void *msg,
                                          size_t msg_len,
                                          bool *need_yield) {
  if (!mbuf || !mbuf->handle || (!msg || msg_len == 0)) return ESY_ERR_INVALID_ARG;

  BaseType_t hp_task_woken = pdFALSE;
  size_t sent = xMessageBufferSendFromISR(mbuf->handle,
                                          msg,
                                          msg_len,
                                          &hp_task_woken);
  if (need_yield) *need_yield = (hp_task_woken == pdTRUE);

  return (sent == msg_len) ? ESY_OK : ESY_ERR_NO_MEM;
}

esy_err_t esy_os_msg_buffer_receive_from_isr(esy_os_msg_buffer_t *mbuf,
                                             void *out_buf,
                                             size_t max_size,
                                             size_t *out_copied,
                                             bool *need_yield) {
  if (!mbuf || !mbuf->handle || (!out_buf || max_size == 0)) return ESY_ERR_INVALID_ARG;

  BaseType_t hp_task_woken = pdFALSE;
  size_t copied = xMessageBufferReceiveFromISR(mbuf->handle,
                                               out_buf,
                                               max_size,
                                               &hp_task_woken);
  if (out_copied) *out_copied = copied;
  if (need_yield) *need_yield = (hp_task_woken == pdTRUE);

  return copied > 0u ? ESY_OK : ESY_ERR_NO_MEM;
}

size_t esy_os_msg_buffer_bytes_used(const esy_os_msg_buffer_t *mbuf) {
  if (!mbuf || !mbuf->handle) return 0;

  size_t free_bytes = xMessageBufferSpaceAvailable(mbuf->handle);
  if (free_bytes > mbuf->capacity_bytes) return 0; /* Defensive */
  return mbuf->capacity_bytes - free_bytes;
}

size_t esy_os_msg_buffer_space_free(const esy_os_msg_buffer_t *mbuf) {
  if (!mbuf || !mbuf->handle) return 0;
  return xMessageBufferSpaceAvailable(mbuf->handle);
}

size_t esy_os_msg_buffer_capacity(const esy_os_msg_buffer_t *mbuf) {
  return mbuf ? mbuf->capacity_bytes : 0;
}
