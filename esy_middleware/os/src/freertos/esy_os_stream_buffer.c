/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/os/esy_os_stream_buffer.h"

#include <string.h>
#include <stdint.h>

#include "esy_os_freertos_internal.h"
#ifdef ESY_PLATFORM_ESP32
#include "freertos/stream_buffer.h"
#else
#include "stream_buffer.h"
#endif

/* Opaque handle real definition (private) */
struct esy_os_stream_buffer_s {
  StreamBufferHandle_t handle;
  size_t capacity;   /* Exposed via query API */
};

/* Internal layout stored inside opaque static storage */
struct esy_os_stream_buffer_static_impl_s {
  StaticStreamBuffer_t native;               /* FreeRTOS static stream buffer object */
  struct esy_os_stream_buffer_s wrapper;     /* Public opaque wrapper */
};

_Static_assert(_Alignof(esy_os_stream_buffer_static_storage_t) >=
                   _Alignof(struct esy_os_stream_buffer_static_impl_s),
               "Native control block alignment exceeds public storage");

/* Compile-time guard: public storage must be large enough */
_Static_assert(ESY_OS_STREAM_BUFFER_STATIC_STORAGE_BYTES >=
                   sizeof(struct esy_os_stream_buffer_static_impl_s),
               "ESY_OS_STREAM_BUFFER_STATIC_STORAGE_BYTES too small");

esy_err_t esy_os_stream_buffer_create_static(size_t capacity_bytes,
                                             void *buffer_mem,
                                             esy_os_stream_buffer_static_storage_t *storage,
                                             esy_os_stream_buffer_t **out_sbuf) {
  if (out_sbuf) *out_sbuf = NULL;
#if (configSUPPORT_STATIC_ALLOCATION == 1)
  if (!out_sbuf || !buffer_mem || !storage || capacity_bytes < 2u) {
    return ESY_ERR_INVALID_ARG;
  }

  /* Alignment guard for opaque storage */
  if (((uintptr_t)storage->bytes % ESY_OS_STREAM_BUFFER_STATIC_STORAGE_ALIGN) != 0) {
    return ESY_ERR_INVALID_ARG;
  }

  struct esy_os_stream_buffer_static_impl_s *impl =
      (struct esy_os_stream_buffer_static_impl_s *)(void *)storage->bytes;

  memset(impl, 0, sizeof(*impl));

  /* Default trigger level = 1 (can be changed later) */
  StreamBufferHandle_t h = xStreamBufferCreateStatic(capacity_bytes,
                                                     1,
                                                     (uint8_t *)buffer_mem,
                                                     &impl->native);
  if (!h) return ESY_ERR_FAIL;

  impl->wrapper.handle = h;
  impl->wrapper.capacity = capacity_bytes - 1u;

  *out_sbuf = &impl->wrapper;
  return ESY_OK;
#else
  (void)capacity_bytes; (void)buffer_mem; (void)storage; (void)out_sbuf;
  return ESY_ERR_NOT_SUPPORTED;
#endif
}

void esy_os_stream_buffer_delete(esy_os_stream_buffer_t *sbuf) {
  if (!sbuf || !sbuf->handle) return;

  vStreamBufferDelete(sbuf->handle);
  sbuf->handle = NULL;
}

esy_err_t esy_os_stream_buffer_reset(esy_os_stream_buffer_t *sbuf) {
  if (!sbuf || !sbuf->handle) return ESY_ERR_INVALID_ARG;
  return xStreamBufferReset(sbuf->handle) == pdPASS ? ESY_OK : ESY_ERR_BUSY;
}

esy_err_t esy_os_stream_buffer_send(esy_os_stream_buffer_t *sbuf,
                                   const void *data,
                                   size_t size,
                                   size_t *out_bytes_sent,
                                   uint32_t timeout_ms) {
  if (!sbuf || !sbuf->handle || !data || size == 0) return ESY_ERR_INVALID_ARG;

  size_t sent = xStreamBufferSend(sbuf->handle,
                                  data,
                                  size,
                                  esy_os_timeout_ticks(timeout_ms));
  if (out_bytes_sent) *out_bytes_sent = sent;

  /* Stream buffer may legitimately send partially -> still ESY_OK */
  return ESY_OK;
}

esy_err_t esy_os_stream_buffer_receive(esy_os_stream_buffer_t *sbuf,
                                      void *data,
                                      size_t max_size,
                                      size_t *out_bytes_received,
                                      uint32_t timeout_ms) {
  if (!sbuf || !sbuf->handle || !data || max_size == 0) return ESY_ERR_INVALID_ARG;

  size_t recvd = xStreamBufferReceive(sbuf->handle,
                                      data,
                                      max_size,
                                      esy_os_timeout_ticks(timeout_ms));
  if (out_bytes_received) *out_bytes_received = recvd;

  /* Partial receive is normal -> ESY_OK */
  return ESY_OK;
}

esy_err_t esy_os_stream_buffer_send_from_isr(esy_os_stream_buffer_t *sbuf,
                                            const void *data,
                                            size_t size,
                                            size_t *out_bytes_sent,
                                            bool *need_yield) {
  if (!sbuf || !sbuf->handle || !data || size == 0) return ESY_ERR_INVALID_ARG;

  BaseType_t hp_task_woken = pdFALSE;
  size_t sent = xStreamBufferSendFromISR(sbuf->handle,
                                         data,
                                         size,
                                         &hp_task_woken);
  if (out_bytes_sent) *out_bytes_sent = sent;
  if (need_yield) *need_yield = (hp_task_woken == pdTRUE);

  return ESY_OK;
}

esy_err_t esy_os_stream_buffer_receive_from_isr(esy_os_stream_buffer_t *sbuf,
                                               void *data,
                                               size_t max_size,
                                               size_t *out_bytes_received,
                                               bool *need_yield) {
  if (!sbuf || !sbuf->handle || !data || max_size == 0) return ESY_ERR_INVALID_ARG;

  BaseType_t hp_task_woken = pdFALSE;
  size_t recvd = xStreamBufferReceiveFromISR(sbuf->handle,
                                             data,
                                             max_size,
                                             &hp_task_woken);
  if (out_bytes_received) *out_bytes_received = recvd;
  if (need_yield) *need_yield = (hp_task_woken == pdTRUE);

  return ESY_OK;
}

esy_err_t esy_os_stream_buffer_set_trigger_level(esy_os_stream_buffer_t *sbuf,
                                                 size_t trigger_level) {
  if (!sbuf || !sbuf->handle) return ESY_ERR_INVALID_ARG;

  BaseType_t ok = xStreamBufferSetTriggerLevel(sbuf->handle, trigger_level);
  return (ok == pdPASS) ? ESY_OK : ESY_ERR_FAIL;
}

size_t esy_os_stream_buffer_bytes_used(const esy_os_stream_buffer_t *sbuf) {
  if (!sbuf || !sbuf->handle) return 0;
  return xStreamBufferBytesAvailable(sbuf->handle);
}

size_t esy_os_stream_buffer_space_free(const esy_os_stream_buffer_t *sbuf) {
  if (!sbuf || !sbuf->handle) return 0;
  return xStreamBufferSpacesAvailable(sbuf->handle);
}

size_t esy_os_stream_buffer_capacity(const esy_os_stream_buffer_t *sbuf) {
  if (!sbuf) return 0;
  return sbuf->capacity;
}
