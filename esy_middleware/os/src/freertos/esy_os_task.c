/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/os/esy_os_task.h"

#include <string.h>
#include <stdint.h>

#include "esy_os_freertos_internal.h"
#ifdef ESY_PLATFORM_ESP32
#include "freertos/task.h"
#else
#include "task.h"
#endif

#include "esy_hal/esy_hal_system.h"

/* Opaque handle actual definition (private to this translation unit) */
struct esy_os_task_s {
  TaskHandle_t handle;
};

/* Internal layout used by static storage bytes */
struct esy_os_task_static_impl_s {
  StaticTask_t native;          /* FreeRTOS static TCB */
  struct esy_os_task_s wrapper; /* Our public opaque wrapper */
};

_Static_assert(_Alignof(esy_os_task_static_storage_t) >=
                   _Alignof(struct esy_os_task_static_impl_s),
               "Native control block alignment exceeds public storage");

/* Compile-time guard: public storage must be large enough */
_Static_assert(ESY_OS_TASK_STATIC_STORAGE_BYTES >=
                   sizeof(struct esy_os_task_static_impl_s),
               "ESY_OS_TASK_STATIC_STORAGE_BYTES too small");

#ifdef ESY_PLATFORM_ESP32
#include "freertos/portmacro.h"
/* Task and ISR callers on every core share this lock for compound state.
 * Callers must keep critical sections short and never block within them. */
typedef struct { portMUX_TYPE mux; } esy_os_task_ctx_t;
static esy_os_task_ctx_t s_ctx = {.mux = portMUX_INITIALIZER_UNLOCKED};
#endif

esy_err_t esy_os_task_create_static(const char *name, esy_os_task_func_t func,
                                    void *arg, uint32_t stack_size_bytes,
                                    uint32_t priority, void *stack_buffer,
                                    esy_os_task_static_storage_t *storage,
                                    esy_os_task_t **out_task) {
  if (out_task) *out_task = NULL;
#if (configSUPPORT_STATIC_ALLOCATION == 1)
  if (!func || !stack_buffer || !storage || !out_task)
    return ESY_ERR_INVALID_ARG;

  /* Stack size must be multiple of StackType_t (FreeRTOS requirement) */
  if (stack_size_bytes == 0 || (stack_size_bytes % sizeof(StackType_t)) != 0) {
    return ESY_ERR_INVALID_ARG;
  }

  /* Stack buffer must be properly aligned */
  if (((uintptr_t)stack_buffer % ESY_OS_TASK_STACK_ALIGN) != 0) {
    return ESY_ERR_INVALID_ARG;
  }

  /* Storage alignment guard (ABI contract) */
  if (((uintptr_t)storage->bytes % ESY_OS_TASK_STATIC_STORAGE_ALIGN) != 0) {
    return ESY_ERR_INVALID_ARG;
  }

  /* Reinterpret opaque bytes as our internal static object */
  struct esy_os_task_static_impl_s *impl =
      (struct esy_os_task_static_impl_s *)(void *)storage->bytes;

  memset(impl, 0, sizeof(*impl));

  /* Convert bytes -> words for FreeRTOS */
  uint32_t depth_words = stack_size_bytes / sizeof(StackType_t);

  TaskHandle_t h = xTaskCreateStatic(
      func, name ? name : "esy_task", (configSTACK_DEPTH_TYPE)depth_words, arg,
      (UBaseType_t)priority, (StackType_t *)stack_buffer, &impl->native);
  if (!h) return ESY_ERR_FAIL;

  /* Initialize wrapper that becomes the public opaque handle */
  impl->wrapper.handle = h;

  *out_task = &impl->wrapper;
  return ESY_OK;
#else
  (void)name;
  (void)func;
  (void)arg;
  (void)stack_size_bytes;
  (void)priority;
  (void)stack_buffer;
  (void)storage;
  (void)out_task;
  return ESY_ERR_NOT_SUPPORTED;
#endif
}

void esy_os_task_delete(esy_os_task_t *task) {
  if (!task || !task->handle) return;

  TaskHandle_t native = task->handle;
  task->handle = NULL;
  vTaskDelete(native);
}

bool esy_os_task_park_supported(void) {
#if INCLUDE_vTaskSuspend == 1 && INCLUDE_eTaskGetState == 1
  return true;
#else
  return false;
#endif
}

esy_err_t esy_os_task_park(void) {
#if INCLUDE_vTaskSuspend == 1 && INCLUDE_eTaskGetState == 1
  vTaskSuspend(NULL);
  return ESY_OK;
#else
  return ESY_ERR_NOT_SUPPORTED;
#endif
}

esy_err_t esy_os_task_delete_parked(esy_os_task_t *task) {
  if (task == NULL) return ESY_ERR_INVALID_ARG;
#if INCLUDE_vTaskSuspend == 1 && INCLUDE_eTaskGetState == 1
  if (task->handle == NULL) return ESY_OK;
  if (esy_os_task_is_current(task)) return ESY_ERR_STATE;
  /* The exclusive lifecycle owner forbids resume after cooperative parking.
   * Native suspended state confirms the task has left its execution context. */
  if (eTaskGetState(task->handle) != eSuspended) return ESY_ERR_BUSY;
  esy_os_task_delete(task);
  return ESY_OK;
#else
  return ESY_ERR_NOT_SUPPORTED;
#endif
}

void esy_os_task_delay_ms(uint32_t ms) { vTaskDelay(esy_os_finite_ticks(ms)); }

esy_os_tick_t esy_os_task_get_tick(void) { return (esy_os_tick_t)xTaskGetTickCount(); }

bool esy_os_task_is_current(const esy_os_task_t *task) {
  return task && task->handle && task->handle == xTaskGetCurrentTaskHandle();
}

esy_err_t esy_os_task_delay_until(esy_os_tick_t *prev_tick, uint32_t period_ms) {
  if (!prev_tick || period_ms == 0u || *prev_tick > (uint64_t)portMAX_DELAY)
    return ESY_ERR_INVALID_ARG;
  TickType_t native_tick = (TickType_t)*prev_tick;
  vTaskDelayUntil(&native_tick, esy_os_finite_ticks(period_ms));
  *prev_tick = (esy_os_tick_t)native_tick;
  return ESY_OK;
}

int32_t esy_os_task_get_stack_remaining(esy_os_task_t *task) {
  if (!task || !task->handle) return ESY_ERR_INVALID_ARG;
#if (INCLUDE_uxTaskGetStackHighWaterMark == 1)
  return (int32_t)uxTaskGetStackHighWaterMark(task->handle);
#else
  return ESY_ERR_NOT_SUPPORTED;
#endif
}

void *esy_os_task_get_handle(esy_os_task_t *task) {
  return task ? (void *)task->handle : NULL;
}

void esy_os_task_set_core_affinity(esy_os_task_t *task, uint8_t core_affinity) {
#if defined(FREE_RTOS_KERNEL_SMP) && (configUSE_CORE_AFFINITY == 1)
  if (task && task->handle) {
    vTaskCoreAffinitySet(task->handle, core_affinity);
  }
#else
  (void)task;
  (void)core_affinity;
#endif
}

void esy_os_task_yield_core(uint8_t core_id) {
#if defined(FREE_RTOS_KERNEL_SMP) && (configNUMBER_OF_CORES > 1)
  portYIELD_CORE((int)core_id);
#else
  (void)core_id;
  taskYIELD();
#endif
}

esy_err_t esy_os_task_notify(esy_os_task_t *task, uint32_t value,
                             esy_os_notify_action_t action) {
  if (!task || !task->handle ||
      (unsigned int)action > (unsigned int)ESY_OS_NOTIFY_SET_WITHOUT_OVERWRITE)
    return ESY_ERR_INVALID_ARG;
  static const eNotifyAction k_actions[] = {
      eNoAction, eSetBits, eIncrement, eSetValueWithOverwrite,
      eSetValueWithoutOverwrite};
  BaseType_t result = xTaskNotify(task->handle, value, k_actions[action]);
  return (result == pdPASS) ? ESY_OK : ESY_ERR_FAIL;
}

esy_err_t esy_os_task_notify_wait(uint32_t entry_clear_bits,
                                  uint32_t exit_clear_bits, uint32_t *value,
                                  uint32_t timeout_ms) {
  uint32_t out = 0;
  BaseType_t result = xTaskNotifyWait(entry_clear_bits, exit_clear_bits, &out,
                                      esy_os_timeout_ticks(timeout_ms));
  if (result == pdPASS) {
    if (value) *value = out;
    return ESY_OK;
  }
  return ESY_ERR_FAIL;
}

void esy_os_task_notify_clear(void) {
  /* Clear notification state of current task */
  xTaskNotifyStateClear(NULL);
}

esy_err_t esy_os_task_notify_give(esy_os_task_t *task) {
  if (!task || !task->handle) return ESY_ERR_INVALID_ARG;
  BaseType_t result = xTaskNotifyGive(task->handle);
  return (result == pdPASS) ? ESY_OK : ESY_ERR_FAIL;
}

esy_err_t esy_os_task_notify_take(bool clear_count, uint32_t timeout_ms) {
  uint32_t result = ulTaskNotifyTake(clear_count ? pdTRUE : pdFALSE,
                                     esy_os_timeout_ticks(timeout_ms));
  return (result > 0) ? ESY_OK : ESY_ERR_FAIL;
}

esy_err_t esy_os_task_notify_give_from_isr(esy_os_task_t *task,
                                           bool *need_yield) {
  if (!task || !task->handle) return ESY_ERR_INVALID_ARG;
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  vTaskNotifyGiveFromISR(task->handle, &xHigherPriorityTaskWoken);
  if (need_yield) *need_yield = (xHigherPriorityTaskWoken == pdTRUE);
  return ESY_OK;
}

esy_err_t esy_os_task_notify_from_isr(esy_os_task_t *task, uint32_t value,
                                      esy_os_notify_action_t action, bool *need_yield) {
  if (need_yield) *need_yield = false;
  if (!task || !task->handle ||
      (unsigned int)action > (unsigned int)ESY_OS_NOTIFY_SET_WITHOUT_OVERWRITE)
    return ESY_ERR_INVALID_ARG;
  static const eNotifyAction k_actions[] = {
      eNoAction, eSetBits, eIncrement, eSetValueWithOverwrite,
      eSetValueWithoutOverwrite};
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  BaseType_t result = xTaskNotifyFromISR(task->handle, value, k_actions[action],
                                         &xHigherPriorityTaskWoken);
  if (need_yield) *need_yield = (xHigherPriorityTaskWoken == pdTRUE);
  return (result == pdPASS) ? ESY_OK : ESY_ERR_FAIL;
}

void esy_os_task_enter_critical(void) {
#ifdef ESY_PLATFORM_ESP32
  taskENTER_CRITICAL(&s_ctx.mux);
#else
  taskENTER_CRITICAL();
#endif
}

void esy_os_task_exit_critical(void) {
#ifdef ESY_PLATFORM_ESP32
  taskEXIT_CRITICAL(&s_ctx.mux);
#else
  taskEXIT_CRITICAL();
#endif
}

uint32_t esy_os_task_enter_critical_from_isr(void) {
#ifdef ESY_PLATFORM_ESP32
  taskENTER_CRITICAL_ISR(&s_ctx.mux);
  return 0u;
#else
  return (uint32_t)taskENTER_CRITICAL_FROM_ISR();
#endif
}

void esy_os_task_exit_critical_from_isr(uint32_t saved_status) {
#ifdef ESY_PLATFORM_ESP32
  (void)saved_status;
  taskEXIT_CRITICAL_ISR(&s_ctx.mux);
#else
  taskEXIT_CRITICAL_FROM_ISR((UBaseType_t)saved_status);
#endif
}

uint32_t esy_os_task_enter_crit_auto(void) {
  /* Auto-select ISR-safe or task-context critical section */
  if (esy_hal_system_is_in_isr()) {
    return esy_os_task_enter_critical_from_isr();
  }
  esy_os_task_enter_critical();
  return 0;
}

void esy_os_task_exit_crit_auto(uint32_t key) {
  if (esy_hal_system_is_in_isr()) {
    esy_os_task_exit_critical_from_isr(key);
  } else {
    (void)key;
    esy_os_task_exit_critical();
  }
}
