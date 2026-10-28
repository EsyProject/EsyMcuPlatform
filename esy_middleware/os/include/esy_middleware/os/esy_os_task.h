/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_os_task.h
 * @brief OS abstraction layer - task management interface.
 *
 * This module wraps common RTOS task primitives, including task
 * creation/deletion, delays, task notifications, and critical-section helpers.
 * Task creation uses caller-provided static storage.
 *
 * Conventions:
 * - Task entry function type: @ref esy_os_task_func_t
 * - All functions returning esy_err_t: ESY_OK on success; negative values on
 * failure.
 * - Some APIs provide *_from_isr() variants for interrupt context.
 *
 * Notes:
 * - The exact semantics of "priority", "core affinity", and "critical section"
 *   are implementation-defined and mapped to the underlying OS (e.g.,
 * FreeRTOS).
 * - Static task creation uses caller-provided stack and opaque storage,
 * allowing the implementation to remain fully opaque while avoiding heap
 * allocation.
 */

#ifndef ESY_OS_TASK_H
#define ESY_OS_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_middleware/os/esy_os_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Compile-time constants                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief Maximum number of task status entries supported by the system snapshot
 * API.
 *
 * Used by @ref esy_os_task_get_system_state.
 */
#define ESY_OS_TASK_SYSTEM_STATE_MAX     32u

/** @brief Snapshot name capacity, including the terminating NUL byte. */
#define ESY_OS_TASK_NAME_BYTES 32u

/** @brief Opaque native snapshot budget; the port checks size and alignment. */
#define ESY_OS_TASK_SNAPSHOT_STORAGE_BYTES (ESY_OS_TASK_SYSTEM_STATE_MAX * 64u)

/**
 * @brief Caller-owned scratch space for one synchronous task snapshot.
 *
 * Declare a complete object of this type. Its native layout is private to the
 * OS port. Concurrent calls require separate storage or external serialization.
 * The storage may be reused immediately after the snapshot call returns.
 */
typedef struct esy_os_task_snapshot_storage_s {
  ESY_ALIGNED_STORAGE(native, ESY_OS_TASK_SNAPSHOT_STORAGE_BYTES); /**< Opaque bytes. */
} esy_os_task_snapshot_storage_t;

/**
 * @brief Static task storage size in bytes.
 *
 * This is an ABI contract for the OS port layer.
 * - If internal needs grow, increase this value (old values still work if large
 * enough).
 * - FreeRTOS ports may grow StaticTask_t significantly when options such as
 *   configUSE_NEWLIB_REENTRANT are enabled.
 *
 * The storage is used by @ref esy_os_task_create_static and must persist for
 * the lifetime of the task.
 */
#ifndef ESY_OS_TASK_STATIC_STORAGE_BYTES
#define ESY_OS_TASK_STATIC_STORAGE_BYTES 1536u
#endif

/**
 * @brief Static task storage alignment (bytes).
 *
 * Choose 8 for ESP32 / common 32-bit MCUs, generally safe.
 * If your platform needs stronger alignment, bump this value.
 */
#define ESY_OS_TASK_STATIC_STORAGE_ALIGN 8u

/**
 * @brief Static task stack buffer alignment in bytes.
 *
 * This is the minimum alignment accepted by @ref esy_os_task_create_static for
 * caller-owned task stack buffers. Keep this value at least as strict as the
 * underlying OS stack word alignment.
 */
#define ESY_OS_TASK_STACK_ALIGN         8u

/* -------------------------------------------------------------------------- */
/* Task state                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief Task state enumeration (OS-abstracted).
 *
 * Mapped from the underlying OS task state (e.g., FreeRTOS eTaskState).
 */
typedef enum esy_os_task_state_e {
  ESY_OS_TASK_STATE_RUNNING = 0, /**< Currently running on a CPU. */
  ESY_OS_TASK_STATE_READY,       /**< Ready to run. */
  ESY_OS_TASK_STATE_BLOCKED,     /**< Blocked (waiting for an event/time). */
  ESY_OS_TASK_STATE_SUSPENDED,   /**< Suspended (not scheduled). */
  ESY_OS_TASK_STATE_DELETED,     /**< Deleted/terminated. */
  ESY_OS_TASK_STATE_INVALID      /**< Invalid/unknown state. */
} esy_os_task_state_t;

/**
 * @brief A snapshot entry describing a task in the system.
 *
 * Notes:
 * - @ref name is an owned, NUL-terminated copy, truncated to
 * ESY_OS_TASK_NAME_BYTES - 1 bytes. Missing native names become empty strings.
 * It remains valid with this entry after task deletion or scratch reuse.
 * - @ref handle is a borrowed native identifier, not a lifetime reference.
 * A snapshot does not authorize using it after task deletion.
 * - @ref stack_high_water_mark is expressed in words (same as FreeRTOS
 *   uxTaskGetStackHighWaterMark()).
 * - @ref runtime_counter is meaningful only if runtime stats are enabled in the
 * OS; otherwise it may be 0. Counters are reported modulo 2^32, including
 * ports whose native runtime counter is wider.
 */
typedef struct esy_os_task_status_s {
  char name[ESY_OS_TASK_NAME_BYTES]; /**< Caller-owned task name copy. */
  void *handle;     /**< Native OS task handle. */

  esy_os_task_state_t state; /**< Current task state. */

  uint32_t
      current_priority;   /**< Current priority (may be boosted/inherited). */
  uint32_t base_priority; /**< Base priority (without inheritance/boost). */

  uint32_t stack_high_water_mark; /**< Minimum ever remaining stack (words). */
  uint32_t task_number; /**< OS-assigned task number/ID (if available). */

  uint32_t runtime_counter; /**< Runtime stats counter (OS-defined units). */
} esy_os_task_status_t;

/* -------------------------------------------------------------------------- */
/* Opaque handle types                                                        */
/* -------------------------------------------------------------------------- */

/** @brief Task handle (opaque). */
typedef struct esy_os_task_s esy_os_task_t;

/**
 * @brief Static task storage object.
 *
 * The caller owns this object and must keep it alive for the lifetime of the
 * task. The internal layout is implementation-defined.
 *
 * This enables fully opaque static allocation without exposing internal
 * structs.
 */
typedef struct esy_os_task_static_storage_s {
#ifdef __cplusplus
  alignas(ESY_OS_TASK_STATIC_STORAGE_ALIGN)
#else
  _Alignas(ESY_OS_TASK_STATIC_STORAGE_ALIGN)
#endif
      uint8_t bytes[ESY_OS_TASK_STATIC_STORAGE_BYTES];
} esy_os_task_static_storage_t;

/* -------------------------------------------------------------------------- */
/* Task entry                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief Task entry function type.
 *
 * @param[in] arg User-defined argument passed to the task at creation time.
 */
typedef void (*esy_os_task_func_t)(void *arg);

/* -------------------------------------------------------------------------- */
/* Creation / deletion                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Create a statically allocated task (no dynamic allocation).
 *
 * Uses caller-provided stack buffer and opaque storage.
 *
 * @param[in]  name              Task name (may be NULL).
 * @param[in]  func              Task entry function (must not be NULL).
 * @param[in]  arg               Argument passed to @p func when the task
 * starts.
 * @param[in]  stack_size_bytes  Stack size in bytes. Must be a multiple of the
 *                               underlying OS stack word size (e.g.,
 * StackType_t).
 * @param[in]  priority          Task priority (OS-defined range/meaning).
 * @param[in]  stack_buffer      Caller-provided stack memory buffer.
 *                               Must have at least @p stack_size_bytes bytes
 * and must satisfy OS-required alignment.
 * @param[in]  storage           Caller-provided opaque static storage object.
 *                               Must persist for the lifetime of the task.
 * @param[out] out_task          Output: created task handle.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure (invalid arguments, unsupported, OS error, etc.).
 */
extern esy_err_t esy_os_task_create_static(
    const char *name, esy_os_task_func_t func, void *arg,
    uint32_t stack_size_bytes, uint32_t priority, void *stack_buffer,
    esy_os_task_static_storage_t *storage, esy_os_task_t **out_task);

/**
 * @brief Delete a task.
 *
 * Behavior depends on the underlying OS:
 * - Some OSes allow deleting another task.
 * - NULL is a no-op. Self deletion requires the explicit handle.
 * - Follow esy_os_storage.h for deferred native task reclamation.
 *
 * @param[in] task Task handle.
 */
extern void esy_os_task_delete(esy_os_task_t *task);

/** @brief Whether this port supports cooperative parking and checked deletion.
 * May be queried before task creation. No state is changed. */
extern bool esy_os_task_park_supported(void);

/** @brief Park the calling task until its owner deletes it.
 * Task context only, outside critical sections. No public resume is permitted.
 * The caller must finish all callbacks and release resources first.
 * @retval ESY_ERR_NOT_SUPPORTED Port lacks suspend/state support.
 * @retval ESY_OK Returned only if an external native user resumed the task;
 * callers must immediately park again and must not resume normal processing.
 */
extern esy_err_t esy_os_task_park(void);

/** @brief Nonblocking deletion of an exclusively owned, cooperatively parked task.
 * No other user may resume/delete this task. Task context only.
 * @retval ESY_OK Parked task deleted; storage is reusable, or already deleted.
 * @retval ESY_ERR_BUSY Task is not yet suspended; retain storage and retry.
 * @retval ESY_ERR_STATE Attempt to delete the calling task.
 * @retval ESY_ERR_INVALID_ARG NULL handle.
 * @retval ESY_ERR_NOT_SUPPORTED Port lacks suspend/state support.
 */
extern esy_err_t esy_os_task_delete_parked(esy_os_task_t *task);

/* -------------------------------------------------------------------------- */
/* Delays                                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief Delay the calling task for a given number of milliseconds.
 *
 * This function must be called from task context.
 *
 * @param[in] ms Delay duration in milliseconds.
 */
extern void esy_os_task_delay_ms(uint32_t ms);

/** @brief Portable tick storage; represents the native counter modulo its width. */
typedef uint64_t esy_os_tick_t;
/** @brief Read the current native tick counter from task context. */
extern esy_os_tick_t esy_os_task_get_tick(void);
/** @brief Return whether the live task is the calling task (not ISR-safe). */
extern bool esy_os_task_is_current(const esy_os_task_t *task);
/**
 * @brief Delay periodically; use esy_os_task_get_tick() for the initial value.
 * @retval ESY_OK Native delay completed and previous tick updated.
 * @retval ESY_ERR_INVALID_ARG NULL counter, zero period, or value outside native width.
 * Positive periods round up to a tick and saturate below the forever sentinel.
 */
extern esy_err_t esy_os_task_delay_until(esy_os_tick_t *prev_tick, uint32_t period_ms);

/* -------------------------------------------------------------------------- */
/* Introspection                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief Get remaining stack space for a task.
 *
 * Units are OS-defined; for FreeRTOS this is returned in words and typically
 * maps to uxTaskGetStackHighWaterMark() semantics.
 *
 * @param[in] task Task handle.
 *
 * @return >= 0: remaining stack (words); < 0: failure (invalid
 * args/unsupported).
 */
extern int32_t esy_os_task_get_stack_remaining(esy_os_task_t *task);

/**
 * @brief Get the native OS task handle.
 *
 * @param[in] task Task handle.
 *
 * @return Native RTOS task handle pointer (OS-defined).
 */
extern void *esy_os_task_get_handle(esy_os_task_t *task);

/**
 * @brief Set task core affinity (platform-specific).
 *
 * The meaning of @p core_affinity is platform/OS-specific. On some platforms it
 * may be ignored or treated as a bitmask, CPU index, or policy.
 *
 * @param[in] task          Task handle.
 * @param[in] core_affinity Core affinity parameter (platform-defined
 * semantics).
 */
extern void esy_os_task_set_core_affinity(esy_os_task_t *task,
                                          uint8_t core_affinity);

/**
 * @brief Request a scheduler yield on a specific core.
 *
 * On SMP ports this requests that @p core_id re-evaluates scheduling. On
 * single-core or unsupported ports the implementation may fall back to yielding
 * the current core or become a no-op.
 *
 * @param[in] core_id Target core index.
 */
extern void esy_os_task_yield_core(uint8_t core_id);

/* -------------------------------------------------------------------------- */
/* Task notifications (value-based)                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief Portable notification update actions; no native enum is required.
 *
 * Notify/wait and give/take, including their ISR send variants, share the same
 * default notification slot. Give increments the value and marks it pending;
 * take clears or decrements that value and clears pending state. Wait observes
 * pending state and applies its documented masks. Clearing notification state
 * does not clear the value. Coordinate mixed API styles on each task; they do
 * not provide independent notification channels.
 */
typedef enum {
  ESY_OS_NOTIFY_NO_ACTION = 0, /**< Wake without changing the value. */
  ESY_OS_NOTIFY_SET_BITS, /**< Bitwise OR into the value. */
  ESY_OS_NOTIFY_INCREMENT, /**< Increment the value. */
  ESY_OS_NOTIFY_SET_WITH_OVERWRITE, /**< Replace the value even when pending. */
  ESY_OS_NOTIFY_SET_WITHOUT_OVERWRITE /**< Fail if a notification is pending. */
} esy_os_notify_action_t;

/**
 * @brief Send a task notification (value-based).
 *
 * This maps to the underlying OS task-notification mechanism (e.g., FreeRTOS
 * xTaskNotify()).
 *
 * @param[in] task   Target task.
 * @param[in] value  Notification value.
 * @param[in] action Portable update action; values outside esy_os_notify_action_t are rejected.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure (invalid args/unsupported/OS error).
 */
extern esy_err_t esy_os_task_notify(esy_os_task_t *task, uint32_t value,
                                    esy_os_notify_action_t action);

/**
 * @brief Wait for a task notification (value-based).
 *
 * This maps to the underlying OS wait primitive (e.g., FreeRTOS
 * xTaskNotifyWait()).
 *
 * @param[in]  entry_clear_bits Bits to clear on entry before waiting, only when
 *                             no notification is already pending.
 * @param[in]  exit_clear_bits  Bits to clear on exit (after unblocking).
 * @param[out] value            Output: received notification value (may be
 * NULL).
 * @param[in]  timeout_ms       Timeout in milliseconds:
 *                              - 0: return immediately
 *                              - UINT32_MAX: wait indefinitely
 *
 * @retval ESY_OK Success (notification received).
 * @retval <0     Failure or timeout (implementation-defined error codes).
 */
extern esy_err_t esy_os_task_notify_wait(uint32_t entry_clear_bits,
                                         uint32_t exit_clear_bits,
                                         uint32_t *value, uint32_t timeout_ms);

/**
 * @brief Clear the notification state of the current task.
 *
 * This is typically used to reset any pending notification before waiting
 * again.
 */
extern void esy_os_task_notify_clear(void);

/* -------------------------------------------------------------------------- */
/* Task notifications (counting semaphore style)                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief Send a "give" notification to a task (counting-semaphore style).
 *
 * This maps to the underlying OS "notify give" primitive (e.g., FreeRTOS
 * xTaskNotifyGive()).
 *
 * @param[in] task Target task.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure (invalid args/unsupported/OS error).
 */
extern esy_err_t esy_os_task_notify_give(esy_os_task_t *task);

/**
 * @brief Wait for a "take" notification (counting-semaphore style).
 *
 * This maps to the underlying OS "notify take" primitive (e.g., FreeRTOS
 * ulTaskNotifyTake()).
 *
 * @param[in] clear_count Whether to clear the accumulated count on exit.
 * @param[in] timeout_ms  Timeout in milliseconds:
 *                        - 0: return immediately
 *                        - UINT32_MAX: wait indefinitely
 *
 * @retval ESY_OK Success (count was available).
 * @retval <0     Failure or timeout (implementation-defined error codes).
 */
extern esy_err_t esy_os_task_notify_take(bool clear_count, uint32_t timeout_ms);

/* -------------------------------------------------------------------------- */
/* ISR variants                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief Send a "give" notification from ISR context.
 *
 * @param[in]  task       Target task.
 * @param[out] need_yield Output: set to true if a context switch should be
 * requested when exiting the ISR. May be NULL if not needed.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure (invalid args/unsupported/OS error).
 */
extern esy_err_t esy_os_task_notify_give_from_isr(esy_os_task_t *task,
                                                  bool *need_yield);

/**
 * @brief Send a task notification (value-based) from ISR context.
 *
 * @param[in]  task       Target task.
 * @param[in]  value      Notification value.
 * @param[in]  action     Portable update action from esy_os_notify_action_t.
 * @param[out] need_yield Output: set to true if a context switch should be
 * requested when exiting the ISR. May be NULL if not needed.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure (invalid args/unsupported/OS error).
 */
extern esy_err_t esy_os_task_notify_from_isr(esy_os_task_t *task,
                                             uint32_t value, esy_os_notify_action_t action,
                                             bool *need_yield);

/* -------------------------------------------------------------------------- */
/* Critical sections                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief Enter a critical section (task context).
 *
 * This typically disables interrupts up to a configured priority level or
 * enters an OS-managed critical section. Must not be used from ISR context.
 */
extern void esy_os_task_enter_critical(void);

/**
 * @brief Exit a critical section (task context).
 *
 * Pairs with @ref esy_os_task_enter_critical.
 */
extern void esy_os_task_exit_critical(void);

/**
 * @brief Enter a critical section from ISR context.
 *
 * Returns a saved status value that must be passed to
 * @ref esy_os_task_exit_critical_from_isr.
 *
 * @return Saved interrupt/critical-section status key.
 */
extern uint32_t esy_os_task_enter_critical_from_isr(void);

/**
 * @brief Exit a critical section from ISR context.
 *
 * @param[in] saved_status Status key returned by @ref
 * esy_os_task_enter_critical_from_isr.
 */
extern void esy_os_task_exit_critical_from_isr(uint32_t saved_status);

/**
 * @brief Enter a critical section (auto mode) and return a key for exit.
 *
 * This is an alternative to explicit enter/exit calls, providing a key that
 * must be passed back on exit. The implementation decides whether this maps
 * to task-context or ISR-safe behavior.
 *
 * @return Key used to exit the critical section.
 */
extern uint32_t esy_os_task_enter_crit_auto(void);

/**
 * @brief Exit a critical section entered by @ref esy_os_task_enter_crit_auto.
 *
 * @param[in] key Key returned by @ref esy_os_task_enter_crit_auto.
 */
extern void esy_os_task_exit_crit_auto(uint32_t key);

/* -------------------------------------------------------------------------- */
/* System state snapshot                                                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief Get a snapshot of all tasks in the system.
 *
 * - The caller provides the @p items output buffer.
 * - Capacity must be in [1, ESY_OS_TASK_SYSTEM_STATE_MAX]. The API returns a
 *   complete snapshot or ESY_ERR_NO_MEM; it does not return a partial list.
 * - The caller supplies a complete, naturally aligned scratch storage object.
 *   Items and scratch must not overlap and must be exclusive during this call.
 * - Running task context only, outside critical sections. The FreeRTOS port
 *   suspends scheduling across enumeration and name copying. Names are owned
 *   by items after return; native handles still require external lifetime control.
 * - ESP32 ports without CONFIG_FREERTOS_UNICORE == 1 return
 *   ESY_ERR_NOT_SUPPORTED: cross-core lifetime protection is not established.
 * - Optional output counters are reset to zero before validation; items are
 *   unchanged on error. No buffers are retained after return.
 * - The actual number of written entries is returned via @p out_count
 * (optional).
 * - If runtime stats are enabled, total runtime is returned via @p
 * out_total_run_time (optional).
 *
 * FreeRTOS preconditions (typical):
 * - configUSE_TRACE_FACILITY == 1
 * - INCLUDE_uxTaskGetSystemState == 1 (if supported/used by the port)
 *
 * @param[out] items              Output array of task status entries.
 * @param[in]  items_capacity     Capacity of @p items in number of entries.
 * @param[out] out_count          Output: number of entries actually written.
 *                                May be NULL if not needed.
 * @param[out] out_total_run_time Output: total runtime counter across all
 * tasks. May be NULL if not needed.
 * @param[in,out] storage        Caller-owned native snapshot scratch storage.
 *
 * @retval ESY_OK                Success.
 * @retval ESY_ERR_INVALID_ARG   Invalid arguments.
 * @retval ESY_ERR_NO_MEM        Capacity cannot hold a complete snapshot.
 * @retval ESY_ERR_NOT_SUPPORTED Required OS features or port lifetime protection
 *                              are unavailable.
 */
extern esy_err_t esy_os_task_get_system_state(esy_os_task_status_t *items,
                                              uint32_t items_capacity,
                                              uint32_t *out_count,
                                              uint32_t *out_total_run_time,
                                              esy_os_task_snapshot_storage_t *storage);

#ifdef __cplusplus
}
#endif

#endif /* ESY_OS_TASK_H */
