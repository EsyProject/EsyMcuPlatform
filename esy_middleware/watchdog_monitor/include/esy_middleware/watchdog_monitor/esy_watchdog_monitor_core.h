/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file esy_watchdog_monitor_core.h
 * @brief Heartbeat supervision shared by caller-driven and OS execution.
 *
 * One system owner controls the singleton and its hardware watchdog. In
 * caller-driven mode all ordinary APIs run serially on one non-ISR context;
 * no other task/core/ISR may access the instance. The bounded atomic deadline
 * query remains permitted in an interrupt-masked power window, excluding
 * lifecycle. In OS mode live report/control operations retain MPMC locking.
 * Caller-driven participants need no OS tasks. Alert callbacks run outside
 * synchronization in the executor; they may report/control slots, but must
 * not recursively process or perform lifecycle. No allocation is performed.
 */
#ifndef ESY_WATCHDOG_MONITOR_CORE_H
#define ESY_WATCHDOG_MONITOR_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/system/esy_power_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Task report handle token.
 *
 * The handle is a value token returned by registration. It does not point to
 * internal storage. Applications should store it in the owning module context
 * and pass its address to subsequent APIs. Its fields are public for ABI
 * transparency but must be treated as read-only by applications.
 */
typedef struct esy_watchdog_report_s {
  uint32_t slot_index; /**< Internal slot index token. */
  uint32_t
      generation; /**< Slot generation token used to reject stale handles. */
  uint32_t magic; /**< Handle integrity token. */
} esy_watchdog_report_t;

/* -------------------------------------------------------------------------- */
/* Storage model */
/* -------------------------------------------------------------------------- */

/**
 * @brief Default maximum number of monitored tasks.
 *
 * Recommended slot count for applications that do not need a custom capacity.
 * Pass this value to @ref ESY_WATCHDOG_MONITOR_CORE_STATIC_STORAGE when declaring
 * caller-owned storage.
 */
#define ESY_WATCHDOG_MONITOR_DEFAULT_MAX_TASKS        8u

/** @brief Copied task-name capacity including the terminating NUL byte. */
#define ESY_WATCHDOG_MONITOR_TASK_NAME_BYTES 32u

/**
 * @brief Storage size for one monitored task slot.
 *
 * This is an ABI contract for caller-owned static storage. The contents are
 * private to the watchdog monitor implementation and must not be inspected by
 * applications.
 */
#define ESY_WATCHDOG_MONITOR_TASK_SLOT_BYTES          64u

/**
 * @brief Required alignment for task slot storage.
 */
#define ESY_WATCHDOG_MONITOR_TASK_SLOT_ALIGN          8u

/**
 * @brief Caller-owned opaque storage for one monitored task.
 *
 * The array supplied through
 * @ref esy_watchdog_monitor_core_resources_t::task_slots must remain valid
 * from init until deinit.
 */
typedef struct esy_watchdog_monitor_task_slot_s {
#if defined(__cplusplus)
  alignas(ESY_WATCHDOG_MONITOR_TASK_SLOT_ALIGN)
#else
  _Alignas(ESY_WATCHDOG_MONITOR_TASK_SLOT_ALIGN)
#endif
      uint8_t bytes[ESY_WATCHDOG_MONITOR_TASK_SLOT_BYTES]; /**< Opaque slot bytes. */
} esy_watchdog_monitor_task_slot_t;

/* -------------------------------------------------------------------------- */
/* Callback types */
/* -------------------------------------------------------------------------- */

/**
 * @brief Watchdog alert reason.
 */
typedef enum esy_watchdog_alert_reason_e {
  ESY_WATCHDOG_ALERT_REASON_MISSED_HEARTBEAT = 0, /**< Task missed heartbeat. */
} esy_watchdog_alert_reason_t;

/**
 * @brief Watchdog alert event.
 *
 * The event is valid only for the duration of the callback. Applications that
 * need to retain event data must copy both the event and its task_name string
 * in the callback. Unregister cannot invalidate this snapshot.
 */
typedef struct esy_watchdog_alert_event_s {
  esy_watchdog_alert_reason_t reason; /**< Alert reason. */
  const char *task_name; /**< Monitor-owned name snapshot; callback lifetime only. */
  esy_watchdog_report_t report;       /**< Report token for the alerted task. */
  uint32_t registered_task_count; /**< Number of registered tasks at alert. */
  uint32_t check_period_ms;       /**< Monitor check period in milliseconds. */
} esy_watchdog_alert_event_t;

/**
 * @brief Watchdog alert callback.
 *
 * Invoked when the monitor detects that a task failed to report a heartbeat
 * within the allowed period. The callback runs in the owning executor context and
 * must remain short and non-blocking.
 *
 * At most one callback is emitted per monitor check window. If several tasks
 * miss the same window, @p event describes the first missed task observed by
 * the monitor scan. Hardware watchdog feeding is skipped for that cycle.
 *
 * @param[in] event    Alert event. Valid only during this callback.
 * @param[in] user_ctx User context supplied during callback registration.
 */
typedef void (*esy_watchdog_alert_cb_t)(const esy_watchdog_alert_event_t *event,
                                        void *user_ctx);

/** @brief Borrowed supervision slots; no task resources are required.
 * Config/descriptor values are copied by init. The backing slots must be unused,
 * distinct, aligned and unmoved until successful deinit. Arbitrary initial bytes
 * are allowed. Failed cleanup never permits clearing/reusing the slots. */
typedef struct {
  esy_watchdog_monitor_task_slot_t *task_slots; /**< Caller-owned slot array. */
  uint32_t task_slot_count; /**< Number of slots, in [1, INT32_MAX]. */
} esy_watchdog_monitor_core_resources_t;

/** @brief Declare one caller-owned aggregate of slots with no OS storage.
 * @param name Object name.
 * @param slot_count Positive slot capacity. */
#define ESY_WATCHDOG_MONITOR_CORE_STATIC_STORAGE(name, slot_count) \
  struct { esy_watchdog_monitor_task_slot_t task_slots[(slot_count)]; } name

/** @brief Bind a slot aggregate; backing storage outlives successful deinit.
 * @param resources Pointer to a core resource descriptor.
 * @param storage Object declared with ESY_WATCHDOG_MONITOR_CORE_STATIC_STORAGE. */
#define ESY_WATCHDOG_MONITOR_CORE_BIND_STATIC_RESOURCES(resources, storage) \
  do { \
    (resources)->task_slots = (storage).task_slots; \
    (resources)->task_slot_count = \
        sizeof((storage).task_slots) / sizeof((storage).task_slots[0]); \
  } while (0)

/** @brief Caller-driven timing and resource configuration. */
typedef struct {
  uint32_t check_period_ms; /**< Positive period, less than 2^31 milliseconds. */
  uint32_t hw_timeout_ms; /**< Hardware timeout, greater than check_period_ms. */
  esy_monotonic_clock_t clock; /**< Optional borrowed clock for the atomic hard
    deadline query. Ops/context outlive successful deinit; not the check timer. */
  esy_watchdog_monitor_core_resources_t *static_resources; /**< Required slots. */
} esy_watchdog_monitor_core_config_t;

/** @brief Fill timing defaults; resources and optional clock remain NULL.
 * @param cfg Output, or NULL for a no-op. */
void esy_watchdog_monitor_core_default_cfg(esy_watchdog_monitor_core_config_t *cfg);

/** @brief Initialize caller-driven supervision and immediately start hardware.
 * @param cfg Copied configuration; slot backing storage/clock remain borrowed.
 * @retval ESY_OK Initialized; call core_start promptly before hardware timeout.
 * @retval ESY_ERR_INVALID_ARG Invalid resources, timing or clock operation.
 * @retval ESY_ERR_STATE Singleton already initialized, unchanged.
 * @retval ESY_ERR_NOT_SUPPORTED Configured deadline atomics are not lock-free.
 * @retval <0 HAL init failure; uninitialized, port may require recovery.
 * No worker is created. Backing bytes are overwritten before use. A missing
 * certified timeout bound disables deadline queries, not supervision. */
esy_err_t esy_watchdog_monitor_core_init(const esy_watchdog_monitor_core_config_t *cfg);

/** @brief Start caller-driven execution; first check is due after one period.
 * @retval ESY_OK Started without creating a task; restart after stop is allowed.
 * @retval ESY_ERR_STATE Uninitialized, OS-owned, already running or hardware stopped.
 * Hardware already counts from init; start does not restart/feed it. */
esy_err_t esy_watchdog_monitor_core_start(void);

/** @brief Advance at most one complete supervision scan when its period is due.
 * @retval ESY_OK Not due, or one scan completed (including a reported miss).
 * @retval ESY_ERR_STATE Uninitialized, OS-owned, stopped or recursive invocation.
 * Uses the HAL millisecond tick with unsigned wrap. Early calls neither
 * consume heartbeats nor feed hardware. Late calls scan once, never replay
 * missed periods, and anchor the next period at that scan's beginning.
 * Work is bounded by configured slot count, one callback and one hardware feed.
 * Callback/provider cost is additional elapsed time; this is no wall-time bound.
 * Call at least once per period with a bounded scheduling latency, and ensure
 * check interval plus latency/provider cost is below the actual hardware timeout.
 * Gaps must stay below 2^31 ms. The boolean window contains reports since the
 * preceding scan; it cannot prove liveness within skipped nominal windows.
 * Non-ISR owner only. A stalled owner relies on hardware reset, not this call. */
esy_err_t esy_watchdog_monitor_core_process(void);

/** @brief Stop execution while hardware continues counting.
 * @retval ESY_OK Caller-driven execution stopped, or OS worker safely deleted.
 * @retval ESY_ERR_BUSY OS worker has not parked; retain resources and retry.
 * @retval ESY_ERR_STATE Lifecycle from a callback/worker or recursive processing.
 * @retval <0 OS cleanup failure; retain resources.
 * Single lifecycle owner, non-ISR only. Restart promptly or deinit. Reports
 * retain their current window across stop/start. Repeated stop is harmless. */
esy_err_t esy_watchdog_monitor_stop(void);

/** @brief Stop hardware and reclaim supervision resources on successful cleanup.
 * @retval ESY_OK Fully cleaned up or already idle; reinit allowed, reports invalid.
 * @retval ESY_ERR_BUSY Feed/callback active or OS worker not parked; retain/retry.
 * @retval ESY_ERR_STATE Lifecycle called recursively from executor/provider.
 * @retval ESY_ERR_NOT_SUPPORTED Hardware cannot stop; ownership and supervision
 * remain intact, continue processing/reporting or recover through system reset.
 * @retval <0 Hardware/OS failure; retain storage and inspect actual state.
 * The owner excludes every public caller and deadline reader during cleanup.
 * Failed hardware stop reopens feed admission; successful hardware stop followed
 * by BUSY requires cleanup retry and forbids restart. Generations remain unique
 * across successful reinitialization within the boot. */
esy_err_t esy_watchdog_monitor_deinit(void);

/* -------------------------------------------------------------------------- */
/* Task registration and control */
/* -------------------------------------------------------------------------- */

/**
 * @brief Register a task for watchdog monitoring.
 *
 * The module reserves static bookkeeping for the task and returns a report
 * token used for future interactions. Storage comes from the static task slot
 * array configured during init.
 *
 * @param[in]  task_name  Task name valid during this call only. It is copied;
 *                        names exceeding 31 bytes are truncated and NUL-terminated.
 * @param[out] out_report Returned task report handle token; initial bytes may be arbitrary.
 *                         On failure the output is unchanged. Do not overwrite
 *                         a live report without unregistering it first.
 * @retval ESY_OK              Success.
 * @retval ESY_ERR_INVALID_ARG Invalid arguments.
 * @retval ESY_ERR_STATE       Module is not initialized.
 * @retval ESY_ERR_NO_MEM      No free slots or boot-wide generation space exhausted.
 *                             Generations never wrap/repeat within one boot.
 */
extern esy_err_t esy_watchdog_monitor_register(
    const char *task_name, esy_watchdog_report_t *out_report);

/**
 * @brief Unregister a previously registered task.
 *
 * Releases all monitoring resources associated with the task.
 * The caller should discard the handle after this call. On success, the passed
 * handle is cleared to an invalid token.
 *
 * @param[in] report Task report handle.
 */
extern void esy_watchdog_monitor_unregister(esy_watchdog_report_t *report);

/**
 * @brief Enable monitoring checks for a task.
 *
 * Once enabled, missing heartbeats from this task will trigger alerts.
 *
 * @param[in] report Task report handle.
 * @retval ESY_OK              Success.
 * @retval ESY_ERR_INVALID_ARG Invalid or unknown handle.
 * @retval ESY_ERR_STATE       Module is not initialized.
 */
extern esy_err_t esy_watchdog_monitor_enable_task_check(
    const esy_watchdog_report_t *report);

/**
 * @brief Disable monitoring checks for a task.
 *
 * When disabled, missing heartbeats from this task will be ignored.
 *
 * @param[in] report Task report handle.
 */
extern void esy_watchdog_monitor_disable_task_check(
    const esy_watchdog_report_t *report);

/**
 * @brief Query the last known activity status of a task.
 *
 * @param[in]  report    Task report handle.
 * @param[out] out_alive Output flag: true if task was alive at last enabled check.
 *                         False before its first check; unchanged on errors.
 *                         Disabled checks preserve the previous result.
 * @retval ESY_OK              Success.
 * @retval ESY_ERR_INVALID_ARG Invalid arguments or unknown handle.
 * @retval ESY_ERR_STATE       Module is not initialized.
 */
extern esy_err_t esy_watchdog_monitor_get_task_status(
    const esy_watchdog_report_t *report, bool *out_alive);

/**
 * @brief Report a heartbeat for the current task.
 *
 * The monitored task should call this periodically to indicate liveness.
 *
 * @param[in] report Task report handle.
 */
extern void esy_watchdog_monitor_report(const esy_watchdog_report_t *report);

/**
 * @brief Get the current number of registered tasks.
 *
 * @return >=0 Number of registered tasks; negative value on failure.
 */
extern int32_t esy_watchdog_monitor_get_task_count(void);

/* -------------------------------------------------------------------------- */
/* Hardware watchdog and alerts */
/* -------------------------------------------------------------------------- */

/**
 * @brief Disable hardware watchdog feeding.
 *
 * Prevents the monitor executor from feeding the hardware watchdog.
 * Intended for debugging or fault reproduction. A previously admitted feed
 * may complete; no future check admits another feed. Reset by successful reinit.
 */
extern void esy_watchdog_monitor_disable_hw_feed(void);

/**
 * @brief Read the absolute hardware-watchdog deadline in the supplied domain.
 *
 * The clock must identify the same operations and context configured at init.
 * The bounded query is safe in the power manager's interrupt-masked window;
 * it must not overlap lifecycle operations. No callback or OS lock is acquired.
 * @param clock Same borrowed ops/context as configured at init.
 * @param out_deadline_us Written only on ESY_OK.
 * @retval ESY_OK Conservative absolute deadline: pre-feed time + certified bound.
 * @retval ESY_ERR_INVALID_ARG NULL pointer.
 * @retval ESY_ERR_STATE Uninitialized, mismatched/missing clock or failed read.
 * @retval ESY_ERR_NOT_SUPPORTED Port supplies no certified timeout lower bound.
 * @retval ESY_ERR_BUSY Concurrent publication prevented a snapshot in 3 attempts.
 * The caller must reject/defer sleep on any error; requested nominal timeout is
 * never used as a substitute for a guaranteed bound.
 */
extern esy_err_t esy_watchdog_monitor_get_hard_deadline_atomic(
    const esy_monotonic_clock_t *clock, uint64_t *out_deadline_us);

/**
 * @brief Set or clear the watchdog alert callback.
 *
 * @retval ESY_OK Pair updated; previous context can be reclaimed when replaced.
 * @retval ESY_ERR_BUSY An invocation still uses the previous pair; unchanged.
 * @retval ESY_ERR_STATE Module is uninitialized.
 * Non-ISR owner/task context, nonblocking. Serialize replacement with destruction of user_ctx;
 * keep the installed pair alive until successful replacement/clear or deinit.
 * Clearing from inside a callback returns BUSY; retry after the callback exits.
 * @param[in] cb       Alert callback function. Pass NULL to clear.
 * @param[in] user_ctx User context passed back to @p cb (may be NULL).
 */
extern esy_err_t esy_watchdog_monitor_set_alert_cb(esy_watchdog_alert_cb_t cb,
                                              void *user_ctx);

#ifdef __cplusplus
}
#endif
#endif /* ESY_WATCHDOG_MONITOR_CORE_H */
