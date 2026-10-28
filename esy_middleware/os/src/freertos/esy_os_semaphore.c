/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/os/esy_os_semaphore.h"

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
struct esy_os_semaphore_s {
  SemaphoreHandle_t handle;
};

/* Internal layout stored inside opaque static storage */
struct esy_os_semaphore_static_impl_s {
  StaticSemaphore_t native;          /* FreeRTOS static semaphore object */
  struct esy_os_semaphore_s wrapper; /* Public opaque wrapper */
};

_Static_assert(_Alignof(esy_os_semaphore_static_storage_t) >=
                   _Alignof(struct esy_os_semaphore_static_impl_s),
               "Native control block alignment exceeds public storage");

/* Compile-time guard: public storage must be large enough */
_Static_assert(ESY_OS_SEMAPHORE_STATIC_STORAGE_BYTES >= sizeof(struct esy_os_semaphore_static_impl_s),
               "ESY_OS_SEMAPHORE_STATIC_STORAGE_BYTES too small");

esy_err_t esy_os_semaphore_create_static(uint32_t max_count,
                                         uint32_t init_count,
                                         esy_os_semaphore_static_storage_t *storage,
                                         esy_os_semaphore_t **out_sem) {
  if (out_sem) *out_sem = NULL;
#if (configSUPPORT_STATIC_ALLOCATION == 1)
  if (!storage || !out_sem || max_count == 0 || init_count > max_count) {
    return ESY_ERR_INVALID_ARG;
  }

  /* Alignment guard for opaque storage */
  if (((uintptr_t)storage->bytes % ESY_OS_SEMAPHORE_STATIC_STORAGE_ALIGN) != 0) {
    return ESY_ERR_INVALID_ARG;
  }

  /* Reinterpret opaque bytes as internal static object */
  struct esy_os_semaphore_static_impl_s *impl =
      (struct esy_os_semaphore_static_impl_s *)(void *)storage->bytes;

  memset(impl, 0, sizeof(*impl));

  SemaphoreHandle_t h = xSemaphoreCreateCountingStatic(
      (UBaseType_t)max_count,
      (UBaseType_t)init_count,
      &impl->native);
  if (!h) return ESY_ERR_FAIL;

  impl->wrapper.handle = h;

  *out_sem = &impl->wrapper;
  return ESY_OK;
#else
  (void)max_count; (void)init_count; (void)storage; (void)out_sem;
  return ESY_ERR_NOT_SUPPORTED;
#endif
}

void esy_os_semaphore_delete(esy_os_semaphore_t *sem) {
  if (!sem || !sem->handle) return;

  vSemaphoreDelete(sem->handle);
  sem->handle = NULL;
}

esy_err_t esy_os_semaphore_take(esy_os_semaphore_t *sem) {
  if (!sem || !sem->handle) return ESY_ERR_INVALID_ARG;

  if (xSemaphoreTake(sem->handle, portMAX_DELAY) != pdPASS) {
    return ESY_ERR_FAIL;
  }
  return ESY_OK;
}

esy_err_t esy_os_semaphore_give(esy_os_semaphore_t *sem) {
  if (!sem || !sem->handle) return ESY_ERR_INVALID_ARG;

  if (xSemaphoreGive(sem->handle) != pdPASS) {
    return ESY_ERR_FAIL;
  }
  return ESY_OK;
}

esy_err_t esy_os_semaphore_take_from_isr(esy_os_semaphore_t *sem,
                                         bool *need_yield) {
  if (!sem || !sem->handle) return ESY_ERR_INVALID_ARG;

  BaseType_t hp_task_woken = pdFALSE;
  BaseType_t r = xSemaphoreTakeFromISR(sem->handle, &hp_task_woken);

  if (need_yield) {
    *need_yield = (hp_task_woken == pdTRUE);
  }

  return (r == pdPASS) ? ESY_OK : ESY_ERR_FAIL;
}

esy_err_t esy_os_semaphore_give_from_isr(esy_os_semaphore_t *sem,
                                         bool *need_yield) {
  if (!sem || !sem->handle) return ESY_ERR_INVALID_ARG;

  BaseType_t hp_task_woken = pdFALSE;
  BaseType_t r = xSemaphoreGiveFromISR(sem->handle, &hp_task_woken);

  if (need_yield) {
    *need_yield = (hp_task_woken == pdTRUE);
  }

  return (r == pdPASS) ? ESY_OK : ESY_ERR_FAIL;
}
