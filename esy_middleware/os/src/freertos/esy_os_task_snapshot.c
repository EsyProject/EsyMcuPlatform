/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/os/esy_os_task.h"

#ifdef ESY_PLATFORM_ESP32
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include "FreeRTOS.h"
#include "task.h"
#endif

/* Map FreeRTOS state -> abstract state */
#if (configUSE_TRACE_FACILITY == 1) && \
    (!defined(INCLUDE_uxTaskGetSystemState) || INCLUDE_uxTaskGetSystemState == 1) && \
    !(defined(ESY_PLATFORM_ESP32) && \
      (!defined(CONFIG_FREERTOS_UNICORE) || CONFIG_FREERTOS_UNICORE != 1))
static esy_os_task_state_t esy_os_task_map_state(eTaskState s) {
  switch (s) {
    case eRunning:
      return ESY_OS_TASK_STATE_RUNNING;
    case eReady:
      return ESY_OS_TASK_STATE_READY;
    case eBlocked:
      return ESY_OS_TASK_STATE_BLOCKED;
    case eSuspended:
      return ESY_OS_TASK_STATE_SUSPENDED;
    case eDeleted:
      return ESY_OS_TASK_STATE_DELETED;
    default:
      return ESY_OS_TASK_STATE_INVALID;
  }
}

#endif

esy_err_t esy_os_task_get_system_state(esy_os_task_status_t *items,
                                       uint32_t items_capacity,
                                       uint32_t *out_count,
                                       uint32_t *out_total_run_time,
                                       esy_os_task_snapshot_storage_t *storage) {
  if (out_count) *out_count = 0u;
  if (out_total_run_time) *out_total_run_time = 0u;
  if (!items || items_capacity == 0u ||
      items_capacity > ESY_OS_TASK_SYSTEM_STATE_MAX || !storage ||
      (uintptr_t)items % _Alignof(esy_os_task_status_t) != 0u ||
      (uintptr_t)storage % _Alignof(esy_os_task_snapshot_storage_t) != 0u) {
    return ESY_ERR_INVALID_ARG;
  }

#if (configUSE_TRACE_FACILITY == 1) &&         \
    (defined(INCLUDE_uxTaskGetSystemState)     \
         ? (INCLUDE_uxTaskGetSystemState == 1) \
         : 1) && \
    !(defined(ESY_PLATFORM_ESP32) && \
      (!defined(CONFIG_FREERTOS_UNICORE) || CONFIG_FREERTOS_UNICORE != 1))

  _Static_assert(sizeof(TaskStatus_t) * ESY_OS_TASK_SYSTEM_STATE_MAX <=
                     ESY_OS_TASK_SNAPSHOT_STORAGE_BYTES,
                 "Native task snapshot exceeds public storage budget");
  _Static_assert(_Alignof(esy_os_task_snapshot_storage_t) >= _Alignof(TaskStatus_t),
                 "Native task snapshot requires stronger alignment");
  TaskStatus_t *tmp = (TaskStatus_t *)(void *)storage->native.bytes;

  configRUN_TIME_COUNTER_TYPE total = 0;
  /* Task-only lifecycle protection: native names live in TCB storage. Keep
   * scheduler suspension nested across enumeration and copying, so deletion
   * cannot invalidate them. Upstream FreeRTOS SMP holds its recursive task
   * lock here (including the RP2040/RP2350 port); ESP32 multicore is excluded
   * above until equivalent lifetime protection is established. No formatting,
   * logging, allocation or blocking operations belong inside this scope. */
  vTaskSuspendAll();
  UBaseType_t n =
      uxTaskGetSystemState(tmp, (UBaseType_t)items_capacity, &total);

  if (n == 0u) {
    (void)xTaskResumeAll();
    return ESY_ERR_NO_MEM;
  }

  for (UBaseType_t i = 0; i < n; i++) {
    size_t length = 0u;
    const char *name = tmp[i].pcTaskName;
    while (name && length < sizeof(items[i].name) - 1u && name[length]) {
      items[i].name[length] = name[length];
      ++length;
    }
    items[i].name[length] = '\0';
    items[i].handle = (void *)tmp[i].xHandle;
    items[i].state = esy_os_task_map_state(tmp[i].eCurrentState);

    items[i].current_priority = (uint32_t)tmp[i].uxCurrentPriority;
    items[i].base_priority = (uint32_t)tmp[i].uxBasePriority;

    items[i].stack_high_water_mark = (uint32_t)tmp[i].usStackHighWaterMark;
    items[i].task_number = (uint32_t)tmp[i].xTaskNumber;

    items[i].runtime_counter = (uint32_t)tmp[i].ulRunTimeCounter;
  }
  (void)xTaskResumeAll();

  if (out_count) *out_count = (uint32_t)n;
  if (out_total_run_time) *out_total_run_time = (uint32_t)total;

  return ESY_OK;

#else
  (void)out_count;
  (void)out_total_run_time;
  return ESY_ERR_NOT_SUPPORTED;
#endif
}
