/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/os/esy_os_mutex.h"

#include <string.h>
#include <stdint.h>

#ifdef ESY_PLATFORM_ESP32
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#else
#include "FreeRTOS.h"
#include "semphr.h"
#endif

/* Opaque handle real definition (private) */
struct esy_os_mutex_s {
  SemaphoreHandle_t handle;
};

/* Internal layout stored inside opaque static storage */
struct esy_os_mutex_static_impl_s {
  StaticSemaphore_t native;      /* FreeRTOS static semaphore object */
  struct esy_os_mutex_s wrapper; /* Public opaque wrapper */
};

_Static_assert(_Alignof(esy_os_mutex_static_storage_t) >=
                   _Alignof(struct esy_os_mutex_static_impl_s),
               "Native control block alignment exceeds public storage");

/* Compile-time guard: public storage must be large enough */
_Static_assert(ESY_OS_MUTEX_STATIC_STORAGE_BYTES >= sizeof(struct esy_os_mutex_static_impl_s),
               "ESY_OS_MUTEX_STATIC_STORAGE_BYTES too small");

esy_err_t esy_os_mutex_create_static(esy_os_mutex_static_storage_t *storage,
                                     esy_os_mutex_t **out_mutex) {
  if (out_mutex) *out_mutex = NULL;
#if (configSUPPORT_STATIC_ALLOCATION == 1)
  if (!storage || !out_mutex) return ESY_ERR_INVALID_ARG;

  /* Alignment guard for opaque storage */
  if (((uintptr_t)storage->bytes % ESY_OS_MUTEX_STATIC_STORAGE_ALIGN) != 0) {
    return ESY_ERR_INVALID_ARG;
  }

  /* Reinterpret opaque bytes as internal static object */
  struct esy_os_mutex_static_impl_s *impl =
      (struct esy_os_mutex_static_impl_s *)(void *)storage->bytes;

  memset(impl, 0, sizeof(*impl));

  impl->wrapper.handle = xSemaphoreCreateMutexStatic(&impl->native);
  if (!impl->wrapper.handle) return ESY_ERR_FAIL;

  *out_mutex = &impl->wrapper;
  return ESY_OK;
#else
  (void)storage;
  (void)out_mutex;
  return ESY_ERR_NOT_SUPPORTED;
#endif
}

void esy_os_mutex_delete(esy_os_mutex_t *mutex) {
  if (!mutex || !mutex->handle) return;

  vSemaphoreDelete(mutex->handle);
  mutex->handle = NULL;
}

void esy_os_mutex_lock(esy_os_mutex_t *mutex) {
  if (!mutex || !mutex->handle) return;

  /* Must be called from task context */
  xSemaphoreTake(mutex->handle, portMAX_DELAY);
}

void esy_os_mutex_unlock(esy_os_mutex_t *mutex) {
  if (!mutex || !mutex->handle) return;

  xSemaphoreGive(mutex->handle);
}
