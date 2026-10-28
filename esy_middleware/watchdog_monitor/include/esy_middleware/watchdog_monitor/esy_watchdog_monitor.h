/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_WATCHDOG_MONITOR_H
#define ESY_WATCHDOG_MONITOR_H

#include <stdint.h>
#include <stdbool.h>

#include "esy_watchdog_monitor_core.h"
#include "esy_middleware/os/esy_os_task.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_watchdog_monitor.h
 * @brief Task-level watchdog monitoring middleware.
 *
 * ## Overview
 * This module provides a centralized watchdog monitoring service for
 * task-level "heartbeat" supervision:
 *
 * - Each task registers itself and periodically reports a heartbeat.
 * - A dedicated monitor task drives the same time-gated core as caller mode.
 * - Early notifications do not shorten the heartbeat period or trigger a feed.
 * - If a task fails to report in the current monitor window, an alert callback
 *   may be triggered.
 * - The monitor task may optionally feed or stop a hardware watchdog.
 *
 * ## Design principles
 * - Task-centric: each monitored task owns a lightweight report handle.
 * - Centralized policy: heartbeat checks and alert handling are consolidated.
 * - OS-aware but platform-agnostic: actual task/timer/watchdog operations are
 *   delegated to the OS middleware and public HAL.
 *
 * ## Concurrency model
 * - Live report/control operations are task-context MPMC operations serialized
 *   by short OS critical sections. No ISR caller is supported, except the
 *   explicitly bounded deadline query in an interrupt-masked power window.
 * - A report value may be copied. Do not mutate the same report storage while
 *   another caller reads it; unregister clears its argument on success.
 * - One system owner serializes init/start/stop/deinit and excludes other public
 *   callers during init/deinit. The internal monitor may run during stop/deinit;
 *   cooperative parking confirms its completion before storage is reclaimed.
 * - Callbacks run outside critical sections on the monitor task. They must be
 *   short and nonblocking. They may report/control tasks; lifecycle operations
 *   from that task return ESY_ERR_STATE. Callback replacement while an invocation
 *   is active returns ESY_ERR_BUSY without changing the registered pair.
 * - Only one missed-task alert is delivered per scan; any miss skips that feed.
 * - No OS heap allocation is used. Caller-driven consumers instead include
 *   esy_watchdog_monitor_core.h and provide slots without task resources.
 *
 * ## Error handling
 * - All APIs return @ref esy_err_t unless otherwise noted.
 * - ESY_OK indicates success; negative values indicate failure.
 */

/* -------------------------------------------------------------------------- */
/* Configuration */
/* -------------------------------------------------------------------------- */

/** @brief Default OS monitor stack size in bytes. */
#define ESY_WATCHDOG_MONITOR_DEFAULT_STACK_SIZE_BYTES 2048u

/**
 * @brief Caller-owned static resources for the watchdog monitor.
 *
 * The resources must remain valid from successful init until deinit. The task
 * control storage is distinct from the task stack buffer and must point to an
 * @ref esy_os_task_static_storage_t object. Backing bytes may be uninitialized;
 * init/create overwrite them before reading. Storage must be unused, distinct,
 * and unmoved. Do not reclaim or re-zero it after a failed/BUSY deinit.
 */
typedef struct {
  void *task_stack_buffer; /**< Monitor task stack buffer (must not be NULL). */
  uint32_t task_stack_buffer_size; /**< Monitor task stack size in bytes (>0). */
  esy_os_task_static_storage_t
      *task_control_storage; /**< Monitor task control storage. */
  esy_watchdog_monitor_task_slot_t
      *task_slots;          /**< Static slots for registered task reports. */
  uint32_t task_slot_count; /**< Number of entries in @ref task_slots (>0). */
} esy_watchdog_monitor_static_resources_t;

/**
 * @brief Declare caller-owned watchdog monitor storage as one app-owned object.
 *
 * The object declared by this macro must remain valid from successful init
 * until deinit. Use @ref ESY_WATCHDOG_MONITOR_BIND_STATIC_RESOURCES to bind the
 * object to an @ref esy_watchdog_monitor_static_resources_t instance.
 *
 * @param name            Object name to declare.
 * @param task_slot_count Number of monitor task slots.
 * @param task_stack_size Monitor task stack bytes.
 */
#define ESY_WATCHDOG_MONITOR_STATIC_STORAGE(name, task_slot_count,            \
                                            task_stack_size)                  \
  struct {                                                                    \
    ESY_ALIGNED_STORAGE(task_stack_buffer, (task_stack_size));                \
    esy_os_task_static_storage_t task_control_storage;                        \
    esy_watchdog_monitor_task_slot_t task_slots[(task_slot_count)];           \
  } name

/**
 * @brief Bind app-owned watchdog monitor storage to a resource descriptor.
 *
 * @param resources Pointer to @ref esy_watchdog_monitor_static_resources_t.
 * @param storage   Object declared by
 *                  @ref ESY_WATCHDOG_MONITOR_STATIC_STORAGE.
 */
#define ESY_WATCHDOG_MONITOR_BIND_STATIC_RESOURCES(resources, storage)        \
  do {                                                                        \
    (resources)->task_stack_buffer = (storage).task_stack_buffer.bytes;       \
    (resources)->task_stack_buffer_size =                                     \
        sizeof((storage).task_stack_buffer.bytes);                            \
    (resources)->task_control_storage = &(storage).task_control_storage;      \
    (resources)->task_slots = (storage).task_slots;                           \
    (resources)->task_slot_count =                                            \
        sizeof((storage).task_slots) / sizeof((storage).task_slots[0]);       \
  } while (0)

/**
 * @brief Watchdog monitor configuration.
 *
 * Defines timing and behavior parameters used by the monitor. Caller-owned
 * memory is grouped in @ref static_resources and must be supplied by the
 * caller before init.
 */
typedef struct {
  uint32_t check_period_ms;   /**< Monitor check period in milliseconds (>0). */
  uint32_t hw_timeout_ms;     /**< Hardware watchdog timeout in milliseconds.
                                   Must be > check_period_ms; no software-only mode. */
  uint32_t task_priority;     /**< Monitor task priority. */
  uint8_t task_core_affinity; /**< CPU core affinity (platform-defined
                                 semantics, e.g. 0xFF for no affinity). */
  esy_monotonic_clock_t clock; /**< Optional borrowed clock; ops/context must
                                  outlive successful deinit. */
  esy_watchdog_monitor_static_resources_t
      *static_resources; /**< Caller-owned static resources. */
} esy_watchdog_monitor_config_t;

/* -------------------------------------------------------------------------- */
/* Lifecycle control */
/* -------------------------------------------------------------------------- */

/**
 * @brief Fill a configuration structure with default values.
 *
 * The default configuration sets behavior parameters only. Applications must
 * bind caller-owned storage to @ref esy_watchdog_monitor_config_t::static_resources
 * before calling @ref esy_watchdog_monitor_init.
 *
 * @param[out] cfg Configuration structure to fill (must not be NULL).
 */
extern void esy_watchdog_monitor_default_cfg(
    esy_watchdog_monitor_config_t *cfg);

/**
 * @brief Initialize the watchdog monitor module.
 *
 * This starts the hardware watchdog immediately, then prepares supervision.
 * Call start promptly, before the hardware timeout. Initial static resources
 * may contain arbitrary bytes. Config/descriptors are copied, backing storage
 * and clock dependencies are borrowed. A hardware init failure leaves the
 * module uninitialized, but the port may require reset after a hardware fault.
 * Unsupported timeout bounds disable deadline queries, not supervision.
 * Reinitializing an already initialized module returns @ref ESY_ERR_STATE.
 *
 * @param[in] cfg Configuration structure.
 * @retval ESY_OK              Success.
 * @retval ESY_ERR_INVALID_ARG Invalid configuration.
 * @retval ESY_ERR_STATE       Module is already initialized.
 * @retval ESY_ERR_NOT_SUPPORTED Cooperative parking or required lock-free atomics unavailable.
 * @retval <0                  HAL watchdog initialization failure.
 */
extern esy_err_t esy_watchdog_monitor_init(
    const esy_watchdog_monitor_config_t *cfg);

/**
 * @brief Start the watchdog monitor task.
 *
 * @retval ESY_OK        Success.
 * @retval ESY_ERR_STATE Module is not initialized or already running.
 * @retval <0            Task creation failure; initialized hardware/resources
 *                       remain owned. Retry start, or check deinit's result.
 * Starting after successful stop is permitted while hardware remains active.
 * No restart is allowed during partially completed deinit.
 */
extern esy_err_t esy_watchdog_monitor_start(void);

/**
 * @brief Request cooperative stop and poll for safe task deletion.
 * @retval ESY_OK No task remains; its stack/control storage is reusable by start.
 * @retval ESY_ERR_BUSY Stop requested, task has not parked; retain all resources.
 * @retval ESY_ERR_STATE Called from the monitor task/callback.
 * @retval <0 OS failure; retain resources and retry or recover.
 * Nonblocking, single-owner task-context API. On BUSY yield to the monitor and
 * retry within an application-chosen deadline. Never re-zero storage on timeout.
 * Hardware keeps counting; restart promptly or deinit. An already admitted feed
 * may finish before parking. Repeated successful stop is harmless.
 */
extern esy_err_t esy_watchdog_monitor_stop(void);

/**
 * @brief Stop hardware, then cooperatively reclaim the monitor task/resources.
 * @retval ESY_OK Cleanup complete (also when already deinitialized); reinit allowed.
 * @retval ESY_ERR_BUSY Feed/callback is active or task is not parked; retain
 *                     resources, yield and retry with an application deadline.
 * @retval ESY_ERR_NOT_SUPPORTED Hardware cannot stop; its ownership and running
 *                              supervision are retained. Keep feeding or reset.
 * @retval ESY_ERR_STATE Called from the monitor task/callback.
 * @retval <0 Hardware/OS cleanup error; retain storage and recover or retry.
 * One lifecycle owner must exclude all other public callers throughout cleanup.
 * A BUSY result after hardware shutdown leaves a stopping task; start is invalid.
 * Successful cleanup invalidates all report aliases, including across reinit.
 */
extern esy_err_t esy_watchdog_monitor_deinit(void);

#ifdef __cplusplus
}
#endif
#endif /* ESY_WATCHDOG_MONITOR_H */
