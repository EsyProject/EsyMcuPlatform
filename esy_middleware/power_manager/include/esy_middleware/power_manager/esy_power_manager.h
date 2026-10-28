/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_POWER_MANAGER_H
#define ESY_POWER_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#if !defined(__cplusplus)
#include <stdatomic.h>
#endif

#include "esy_common/device/esy_device.h"
#include "esy_common/defs/coordination/esy_activity_guard_defs.h"
#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/system/esy_power_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_power_manager.h
 * @brief Transactional resumable-idle controller with caller-owned storage.
 *
 * Ownership: one task-context owner serializes manager lifecycle, registration,
 * queries and transactions with device-registry transitions. All borrowed names,
 * vtables, contexts, resource descriptors and arrays remain valid until manager
 * deinit. A registry/backend/storage set belongs to one manager; registration
 * handles belong to one registration only. Fields are implementation-owned after
 * init/register; never copy or re-zero live objects, including faulted objects.
 *
 * Initialize manager and registration handles with {0} before first use. The
 * reset function is an alternative for fresh manager storage only. Backing
 * arrays need not be zeroed. Transactions require transaction_init before first
 * prepare (or {0}); it accepts uninitialized storage without reading it.
 *
 * Only lock acquire/release support concurrent task callers. They never block;
 * acquire may return BUSY during a transition. No lock API is ISR-facing.
 * Lifecycle/unregister/deinit require all callers and bound guard consumers to
 * stop first. Atomic transaction operations execute on the transaction owner
 * with interrupts masked and, when configured, managed peers held by the
 * coordinator. No other public entry point is callable from an ISR.
 *
 * Configure -> register -> start -> transact -> deinit. There is no stop API;
 * registrations freeze at start. Successful deinit invalidates registrations
 * and permits init again. Init/registration failures leave objects unchanged.
 * Failed cleanup retains outstanding ownership and requires system recovery;
 * a faulted manager cannot be recycled via reset, init or deinit.
 */

/** @brief Sentinel for an unconstrained absolute deadline or latency. */
#define ESY_POWER_TIME_INFINITE UINT64_MAX

struct esy_power_manager_s;
typedef struct esy_power_manager_s esy_power_manager_t;

/**
 * @brief Producer quiesce operations. Errors must leave hardware unchanged.
 *
 * quiesce and resume run in task context with interrupts enabled. resume may
 * use interrupt-driven or bounded polling services, but the tickless caller's
 * scheduler may remain suspended. It must not wait on RTOS scheduling or
 * release visible work before the device graph completes late recovery.
 */
typedef struct esy_power_quiesce_ops_s {
  esy_err_t (*quiesce)(void *ctx, esy_power_state_t state);
  esy_err_t (*resume)(void *ctx, esy_power_state_t state);
} esy_power_quiesce_ops_t;

/** @brief Zero-initialized registration handle for one producer. */
typedef struct esy_power_quiesce_participant_s {
  esy_power_manager_t *manager;
  const char *name;
  const esy_power_quiesce_ops_t *ops;
  void *ctx;
  uint32_t manager_index;
  uint32_t magic;
  bool quiesced;
} esy_power_quiesce_participant_t;

/** @brief Borrowed producer name, operations and context. */
typedef struct esy_power_quiesce_config_s {
  const char *name;
  const esy_power_quiesce_ops_t *ops;
  void *ctx;
} esy_power_quiesce_config_t;

/** @brief Wake-reason observation with no timekeeping responsibility. */
typedef struct esy_wake_source_result_s {
  bool triggered;
} esy_wake_source_result_t;

/**
 * @brief Operations for a non-deadline wake reason such as GPIO or radio IRQ.
 *
 * prepare, finish_and_unmask, and abort run in task context with interrupts
 * enabled, although a tickless caller's scheduler may remain suspended.
 * A failing prepare must leave hardware unchanged.
 * capture_and_mask_atomic runs with global interrupts masked and must
 * also mask the source locally before returning. The source remains locally
 * masked until late graph and participant recovery finishes. Atomic capture
 * must be bounded, allocation-free, logging-free, and non-blocking.
 */
typedef struct esy_wake_source_ops_s {
  esy_err_t (*get_supported_state_mask)(void *ctx, uint32_t *out_state_mask);
  esy_err_t (*prepare)(void *ctx, esy_power_state_t state);
  esy_err_t (*capture_and_mask_atomic)(void *ctx,
                                       esy_wake_source_result_t *out_result);
  esy_err_t (*finish_and_unmask)(void *ctx);
  esy_err_t (*abort)(void *ctx);
} esy_wake_source_ops_t;

/** @brief Zero-initialized wake registration; manager_index is stable while registered. */
typedef struct esy_wake_source_s {
  esy_power_manager_t *manager;
  const char *name;
  const esy_wake_source_ops_t *ops;
  void *ctx;
  uint32_t manager_index;
  uint32_t magic;
  bool prepared;
  bool captured_and_masked;
} esy_wake_source_t;

/** @brief Borrowed non-deadline wake-source configuration. */
typedef struct esy_wake_source_config_s {
  const char *name;
  const esy_wake_source_ops_t *ops;
  void *ctx;
} esy_wake_source_config_t;

/**
 * @brief Exact finite-deadline timer, separate from wake attribution.
 *
 * prepare, finish, and abort run in task context with interrupts enabled; late
 * finish or abort must not wait on RTOS scheduling.
 * arm_absolute_atomic and capture_and_cancel_atomic run with global interrupts
 * masked and must be bounded, allocation-free, logging-free, and non-blocking.
 * The absolute wake time uses the manager clock domain.
 * A failing prepare or arm must leave hardware unchanged.
 * capture_and_cancel_atomic must stop or acknowledge the alarm and report
 * whether it expired before global interrupts are restored.
 */
typedef struct esy_power_deadline_timer_ops_s {
  esy_err_t (*get_supported_state_mask)(void *ctx, uint32_t *out_state_mask);
  esy_err_t (*prepare)(void *ctx, esy_power_state_t state);
  esy_err_t (*arm_absolute_atomic)(void *ctx, esy_power_state_t state,
                                   uint64_t wake_time_us);
  esy_err_t (*capture_and_cancel_atomic)(void *ctx, bool *out_expired);
  esy_err_t (*finish)(void *ctx);
  esy_err_t (*abort)(void *ctx);
} esy_power_deadline_timer_ops_t;

/** @brief State support added to a shared monotonic clock instance. */
typedef struct esy_power_clocksource_ops_s {
  esy_err_t (*get_supported_state_mask)(void *ctx, uint32_t *out_state_mask);
} esy_power_clocksource_ops_t;

/**
 * @brief Dynamic idle constraint such as a watchdog window.
 *
 * All returned deadlines use the manager clock domain. UINT64_MAX means
 * unconstrained. prepare runs with interrupts enabled. revalidate_atomic runs
 * in the final interrupt-masked window. resume runs in task context with
 * interrupts enabled after early graph recovery, but must not wait on RTOS
 * scheduling. revalidate_atomic must be
 * bounded, allocation-free, logging-free, and non-blocking. A failing prepare
 * must leave hardware unchanged.
 */
typedef struct esy_power_constraint_ops_s {
  esy_err_t (*get_hard_deadline)(void *ctx, uint64_t now_us,
                                 esy_power_state_t state,
                                 uint64_t *out_deadline_us);
  esy_err_t (*prepare)(void *ctx, esy_power_state_t state,
                       uint64_t wake_time_us);
  esy_err_t (*revalidate_atomic)(void *ctx, uint64_t now_us,
                                 esy_power_state_t state,
                                 uint64_t *out_deadline_us);
  esy_err_t (*resume)(void *ctx, esy_power_state_t state,
                      uint64_t resumed_at_us);
} esy_power_constraint_ops_t;

/** @brief Zero-initialized dynamic constraint registration. */
typedef struct esy_power_constraint_s {
  esy_power_manager_t *manager;
  const char *name;
  const esy_power_constraint_ops_t *ops;
  void *ctx;
  uint32_t manager_index;
  uint32_t magic;
  bool prepared;
} esy_power_constraint_t;

/** @brief Borrowed dynamic constraint configuration. */
typedef struct esy_power_constraint_config_s {
  const char *name;
  const esy_power_constraint_ops_t *ops;
  void *ctx;
} esy_power_constraint_config_t;

/** @brief Result produced by the atomic hardware-entry backend. */
typedef struct esy_power_backend_result_s {
  bool entered;
  esy_err_t entry_status;
  esy_err_t fatal_error;
} esy_power_backend_result_t;

/**
 * @brief Resumable hardware backend. Begin failures must be fail-atomic.
 *
 * get_capabilities may run with interrupts masked and must be bounded,
 * non-blocking, allocation-free and logging-free. Capabilities stay stable for
 * the transaction. begin runs with interrupts enabled. complete and abort perform
 * backend early recovery and may run with interrupts masked after a successful begin.
 * enter_atomic, complete, and masked abort must be bounded, allocation-free,
 * logging-free, independent of RTOS time, and non-blocking. A fatal enter
 * result requires immediate fail-stop/reset.
 */
typedef struct esy_power_backend_ops_s {
  esy_err_t (*get_capabilities)(void *backend_ctx,
                                esy_power_capabilities_t *out_capabilities);
  esy_err_t (*begin)(void *backend_ctx, esy_power_state_t state);
  esy_err_t (*enter_atomic)(void *backend_ctx,
                            esy_power_backend_result_t *out_result);
  esy_err_t (*complete)(void *backend_ctx);
  esy_err_t (*abort)(void *backend_ctx);
} esy_power_backend_ops_t;

/**
 * @brief Optional all-core rendezvous required when managed_core_count > 1.
 *
 * prepare, release_peers, complete, and abort run in task context with
 * interrupts enabled. Late callbacks must not wait on RTOS scheduling.
 * park_peers_atomic runs with global
 * interrupts masked and must be bounded, allocation-free, logging-free, and
 * non-blocking. prepare and park failures must leave peers unchanged.
 * After successful parking, peers stay parked until graph, participants,
 * constraints and wake sources recover. Late abort releases parked peers as
 * its last action, including when entry failed before graph commit.
 */
typedef struct esy_power_core_coordinator_ops_s {
  esy_err_t (*prepare)(void *ctx, esy_power_state_t state);
  esy_err_t (*park_peers_atomic)(void *ctx, esy_power_state_t state);
  esy_err_t (*release_peers)(void *ctx, esy_power_state_t state);
  esy_err_t (*complete)(void *ctx, esy_power_state_t state);
  esy_err_t (*abort)(void *ctx, esy_power_state_t state);
} esy_power_core_coordinator_ops_t;

#if defined(__cplusplus)
/** @brief C++ layout view of the C11 atomic field; access only through APIs. */
typedef uint32_t esy_power_atomic_u32_t;
#else
/** @brief C11 atomic counter/state field, managed only by the module. */
typedef _Atomic uint32_t esy_power_atomic_u32_t;
#endif

/** @brief Zero-initialized reference-counted task activity lock. */
typedef struct esy_power_lock_s {
  esy_power_manager_t *manager;
  const char *name;
  esy_power_state_t deepest_allowed_state;
  esy_power_atomic_u32_t usage_count;
  uint32_t manager_index;
  uint32_t magic;
} esy_power_lock_t;

/** @brief Named lock limiting sleep to a state below STATE_COUNT. */
typedef struct esy_power_lock_config_s {
  const char *name;
  esy_power_state_t deepest_allowed_state;
} esy_power_lock_config_t;

/** @brief Automatic idle request expressed in the manager clock domain. */
typedef struct esy_power_request_s {
  uint64_t latest_resume_time_us;
  uint64_t maximum_exit_latency_us;
  uint64_t late_resume_guard_us;
  uint32_t allowed_state_mask;
  uint32_t required_wake_source_mask;
} esy_power_request_t;

/** @brief Primary entry status, recovery status and independent wake diagnostics. */
typedef struct esy_power_result_s {
  esy_power_state_t selected_state;
  uint64_t hard_deadline_us;
  uint64_t wake_time_us;
  bool entered;
  bool elapsed_valid;
  bool deadline_expired;
  bool fatal;
  uint64_t elapsed_us;
  uint32_t triggered_wake_source_mask;
  esy_err_t primary_error;
  esy_err_t recovery_error;
  esy_err_t fatal_error;
} esy_power_result_t;

/** @brief Caller-owned transaction. Initialize before first prepare; do not copy. */
typedef struct esy_power_transaction_s {
  esy_power_manager_t *manager;
  esy_power_request_t request;
  esy_power_result_t result;
  uint32_t armed_wake_source_mask;
  uint32_t phase;
  bool constraints_prepared;
  bool wake_sources_prepared;
  bool deadline_prepared;
  bool deadline_armed;
  bool backend_begun;
  bool backend_enter_attempted;
  bool graph_prepared;
  bool device_graph_committed;
  bool cores_prepared;
  bool peers_parked;
} esy_power_transaction_t;

/** @brief Exclusive typed pointer arrays with nonzero capacities; wake capacity <= 32. */
typedef struct esy_power_manager_static_resources_s {
  esy_power_lock_t **locks;
  uint32_t lock_capacity;
  esy_power_quiesce_participant_t **participants;
  uint32_t participant_capacity;
  esy_wake_source_t **wake_sources;
  uint32_t wake_source_capacity;
  esy_power_constraint_t **constraints;
  uint32_t constraint_capacity;
} esy_power_manager_static_resources_t;

/** @brief Borrowed providers and exclusive resources. Optional clock/timer pairs may be absent. */
typedef struct esy_power_manager_config_s {
  esy_device_registry_t *device_registry;
  const esy_power_backend_ops_t *backend_ops;
  void *backend_ctx;
  const esy_power_deadline_timer_ops_t *deadline_timer_ops;
  void *deadline_timer_ctx;
  const esy_power_clocksource_ops_t *clocksource_ops;
  esy_monotonic_clock_t clock;
  uint8_t managed_core_count;
  const esy_power_core_coordinator_ops_t *core_coordinator_ops;
  void *core_coordinator_ctx;
  esy_power_manager_static_resources_t *static_resources;
} esy_power_manager_config_t;

/** @brief Zero-initialized manager handle; one externally serialized lifecycle owner. */
struct esy_power_manager_s {
  esy_device_registry_t *device_registry;
  const esy_power_backend_ops_t *backend_ops;
  void *backend_ctx;
  const esy_power_deadline_timer_ops_t *deadline_timer_ops;
  void *deadline_timer_ctx;
  const esy_power_clocksource_ops_t *clocksource_ops;
  esy_monotonic_clock_t clock;
  uint8_t managed_core_count;
  const esy_power_core_coordinator_ops_t *core_coordinator_ops;
  void *core_coordinator_ctx;
  esy_power_manager_static_resources_t *static_resources;
  uint32_t lock_count;
  uint32_t participant_count;
  uint32_t wake_source_count;
  uint32_t constraint_count;
  esy_power_atomic_u32_t phase;
  uint32_t magic;
};

/** @brief Declare typed storage; capacity must be in [1, 32]. */
#define ESY_POWER_MANAGER_STATIC_STORAGE(name, capacity)       \
  struct {                                                     \
    esy_power_lock_t *locks[(capacity)];                       \
    esy_power_quiesce_participant_t *participants[(capacity)]; \
    esy_wake_source_t *wake_sources[(capacity)];               \
    esy_power_constraint_t *constraints[(capacity)];           \
  } name

/** @brief Bind caller-owned arrays; evaluate only before manager init. */
#define ESY_POWER_MANAGER_BIND_STATIC_RESOURCES(resources, storage)         \
  do {                                                                      \
    (resources)->locks = (storage).locks;                                   \
    (resources)->lock_capacity =                                            \
        sizeof((storage).locks) / sizeof((storage).locks[0]);               \
    (resources)->participants = (storage).participants;                     \
    (resources)->participant_capacity =                                     \
        sizeof((storage).participants) / sizeof((storage).participants[0]); \
    (resources)->wake_sources = (storage).wake_sources;                     \
    (resources)->wake_source_capacity =                                     \
        sizeof((storage).wake_sources) / sizeof((storage).wake_sources[0]); \
    (resources)->constraints = (storage).constraints;                       \
    (resources)->constraint_capacity =                                      \
        sizeof((storage).constraints) / sizeof((storage).constraints[0]);   \
  } while (0)

/** @brief Initialize fresh, exclusively owned manager storage without reading it.
 * @param manager Fresh storage (may be uninitialized), or NULL for a no-op.
 * This is not cleanup: never call on a live/faulted manager or during callbacks. */
void esy_power_manager_reset(esy_power_manager_t *manager);
/** @brief Initialize fresh transaction storage without reading it.
 * @param transaction Fresh storage or a successfully completed/aborted transaction;
 * NULL is a no-op. Never erase a prepared, active or failed-recovery transaction. */
void esy_power_transaction_init(esy_power_transaction_t *transaction);
/** @brief Initialize a manager in configuring state, in task context.
 * @param manager Exclusive {0} or successfully deinitialized/reset storage.
 * @param config Providers and exclusive static resources; all required operations
 * and arrays must exist. Optional clock/clocksource must be supplied together.
 * @retval ESY_OK Initialized; registry lifecycle remains owned by its caller.
 * @retval ESY_ERR_INVALID_ARG Invalid pointer, provider, core count or capacity.
 * @retval ESY_ERR_STATE Already initialized; no resources are overwritten.
 * @retval ESY_ERR_NOT_SUPPORTED Multicore configuration lacks a coordinator.
 * All failures leave the manager and arrays unchanged; fix config and retry. */
esy_err_t esy_power_manager_init(esy_power_manager_t *manager,
                                 const esy_power_manager_config_t *config);
/** @brief Freeze registration once the borrowed registry is ready.
 * @param manager Initialized configuring manager, exclusively owned.
 * @retval ESY_OK Ready for transactions and lock acquisition.
 * @retval ESY_ERR_INVALID_ARG Invalid handle.
 * @retval ESY_ERR_STATE Wrong phase (including repeated start) or registry not ready.
 * Failures leave the manager configuring/current phase unchanged. */
esy_err_t esy_power_manager_start(esy_power_manager_t *manager);
/** @brief Invalidate unused registrations and clear the manager.
 * @param manager Exclusive initialized handle; all guard/lock/callback callers
 * must be stopped. Does not deinitialize providers or the device registry.
 * @retval ESY_OK Storage zeroed; manager and registrations may be initialized again.
 * @retval ESY_ERR_INVALID_ARG Invalid handle, including repeated deinit.
 * @retval ESY_ERR_BUSY Outstanding references or prepared resources; unchanged.
 * @retval ESY_ERR_STATE Active transaction or faulted manager; unchanged.
 * Release references before retrying BUSY; a recovery fault requires system recovery. */
esy_err_t esy_power_manager_deinit(esy_power_manager_t *manager);

/** @brief Register a zero-initialized lock while configuring, in owner task context.
 * @param manager Exclusive configuring manager.
 * @param lock Exclusive {0} storage, unregistered from any manager.
 * @param config Valid depth policy and a name that outlives registration.
 * @retval ESY_OK Registered. @retval ESY_ERR_INVALID_ARG Invalid configuration.
 * @retval ESY_ERR_STATE Frozen manager or already registered handle.
 * @retval ESY_ERR_NO_MEM Full capacity. All failures leave inputs unchanged. */
esy_err_t esy_power_lock_register(esy_power_manager_t *manager,
                                  esy_power_lock_t *lock,
                                  const esy_power_lock_config_t *config);
/** @brief Unregister an unused lock before manager start, in owner task context.
 * @param lock Registered handle; all external users must have stopped.
 * @retval ESY_OK Cleared and reusable. @retval ESY_ERR_INVALID_ARG Invalid handle.
 * @retval ESY_ERR_STATE Manager already started. @retval ESY_ERR_BUSY Still in use.
 * Failures preserve ownership. */
esy_err_t esy_power_lock_unregister(esy_power_lock_t *lock);
/** @brief Acquire a task-context reference without blocking.
 * @param lock Registered lock; concurrent task callers are allowed, not ISR callers.
 * @retval ESY_OK Reference acquired; caller may begin protected activity.
 * @retval ESY_ERR_BUSY Manager transitioning/not ready; no reference retained.
 * @retval ESY_ERR_STATE Reference count overflow.
 * @retval ESY_ERR_INVALID_ARG Invalid lock. Lifecycle must be externally serialized. */
esy_err_t esy_power_lock_acquire(esy_power_lock_t *lock);
/** @brief Release one task-context reference after protected activity stops.
 * @param lock Registered lock; supports concurrent task callers, never ISR callers.
 * @retval ESY_OK Released. @retval ESY_ERR_STATE Underflow; count unchanged.
 * @retval ESY_ERR_INVALID_ARG Invalid lock. Release is allowed during transitions. */
esy_err_t esy_power_lock_release(esy_power_lock_t *lock);
/** @brief Borrow a registered lock through a task-only activity guard.
 * @param lock Registered lock, valid until every guard consumer stops.
 * @param guard Caller-owned output; copied guards also borrow the same lock.
 * @retval ESY_OK Bound without acquiring a reference.
 * @retval ESY_ERR_INVALID_ARG Invalid lock or output; output unchanged.
 * Bind before publishing the guard; serialize binding with all users. */
esy_err_t esy_power_lock_bind_activity_guard(esy_power_lock_t *lock,
                                             esy_activity_guard_t *guard);

/** @brief Register a zero-initialized participant while configuring, in owner task context.
 * @param manager Exclusive configuring manager.
 * @param participant Exclusive {0} storage, unregistered from any manager.
 * @param config Borrowed valid name and operations/policy. Context may be NULL
 * only when its callbacks permit it. All borrowed data outlives registration.
 * @retval ESY_OK Registered. @retval ESY_ERR_INVALID_ARG Invalid configuration.
 * @retval ESY_ERR_STATE Frozen manager or already registered handle.
 * @retval ESY_ERR_NO_MEM Full capacity. All failures leave inputs unchanged. */
esy_err_t esy_power_quiesce_register(
    esy_power_manager_t *manager, esy_power_quiesce_participant_t *participant,
    const esy_power_quiesce_config_t *config);
/** @brief Unregister an unused participant before manager start, in owner task context.
 * @param participant Registered handle; all external users must have stopped.
 * @retval ESY_OK Cleared and reusable. @retval ESY_ERR_INVALID_ARG Invalid handle.
 * @retval ESY_ERR_STATE Manager already started. @retval ESY_ERR_BUSY Still in use.
 * Failures preserve ownership. */
esy_err_t esy_power_quiesce_unregister(
    esy_power_quiesce_participant_t *participant);

/** @brief Register a zero-initialized source while configuring, in owner task context.
 * @param manager Exclusive configuring manager.
 * @param source Exclusive {0} storage, unregistered from any manager.
 * @param config Borrowed valid name and operations/policy. Context may be NULL
 * only when its callbacks permit it. All borrowed data outlives registration.
 * @retval ESY_OK Registered. @retval ESY_ERR_INVALID_ARG Invalid configuration.
 * @retval ESY_ERR_STATE Frozen manager or already registered handle.
 * @retval ESY_ERR_NO_MEM Full capacity. All failures leave inputs unchanged. */
esy_err_t esy_wake_source_register(esy_power_manager_t *manager,
                                   esy_wake_source_t *source,
                                   const esy_wake_source_config_t *config);
/** @brief Unregister an unused source before manager start, in owner task context.
 * @param source Registered handle; all external users must have stopped.
 * @retval ESY_OK Cleared and reusable. @retval ESY_ERR_INVALID_ARG Invalid handle.
 * @retval ESY_ERR_STATE Manager already started. @retval ESY_ERR_BUSY Still in use.
 * Failures preserve ownership. Surviving wake-source indices do not change;
 * discard masks containing a removed source before that slot is reused. */
esy_err_t esy_wake_source_unregister(esy_wake_source_t *source);

/** @brief Register a zero-initialized constraint while configuring, in owner task context.
 * @param manager Exclusive configuring manager.
 * @param constraint Exclusive {0} storage, unregistered from any manager.
 * @param config Borrowed valid name and operations/policy. Context may be NULL
 * only when its callbacks permit it. All borrowed data outlives registration.
 * @retval ESY_OK Registered. @retval ESY_ERR_INVALID_ARG Invalid configuration.
 * @retval ESY_ERR_STATE Frozen manager or already registered handle.
 * @retval ESY_ERR_NO_MEM Full capacity. All failures leave inputs unchanged. */
esy_err_t esy_power_constraint_register(
    esy_power_manager_t *manager, esy_power_constraint_t *constraint,
    const esy_power_constraint_config_t *config);
/** @brief Unregister an unused constraint before manager start, in owner task context.
 * @param constraint Registered handle; all external users must have stopped.
 * @retval ESY_OK Cleared and reusable. @retval ESY_ERR_INVALID_ARG Invalid handle.
 * @retval ESY_ERR_STATE Manager already started. @retval ESY_ERR_BUSY Still in use.
 * Failures preserve ownership. */
esy_err_t esy_power_constraint_unregister(esy_power_constraint_t *constraint);

/** @brief Fill a request with all resumable states and infinite limits.
 * @param request Exclusive output; NULL is a no-op. Set a finite deadline or
 * a nonzero required wake mask before prepare. */
void esy_power_request_default(esy_power_request_t *request);
/** @brief Query currently eligible idle state without acquiring resources.
 * @param manager Ready manager; call from its serialized task owner.
 * @param out_state Written only on success.
 * @retval ESY_OK Eligible state written. @retval ESY_ERR_INVALID_ARG Invalid inputs.
 * @retval ESY_ERR_STATE Not ready or clock read failed.
 * @retval ESY_ERR_NOT_SUPPORTED No eligible idle state, including ACTIVE-only.
 * Provider query errors propagate. This snapshot does not reserve a state. */
esy_err_t esy_power_manager_get_deepest_allowed_state(
    const esy_power_manager_t *manager, esy_power_state_t *out_state);
/** @brief Runtime-suspend unused devices via the borrowed registry.
 * @param manager Ready manager; serialize with all registry transitions.
 * @retval ESY_ERR_INVALID_ARG Invalid manager. @retval ESY_ERR_STATE Not ready.
 * Other results follow esy_device_registry_runtime_suspend_idle. Task context only. */
esy_err_t esy_power_manager_runtime_suspend_idle(esy_power_manager_t *manager);

/** @brief Read the shared monotonic domain; task or owner atomic-window context.
 * @param manager Initialized manager with a clock, serialized against lifecycle.
 * @param out_time_us Valid output; written according to the clock provider contract.
 * @retval ESY_ERR_INVALID_ARG Invalid handle, output or absent clock.
 * Other errors propagate from read_atomic. This API does not establish readiness. */
esy_err_t esy_power_manager_now_atomic(const esy_power_manager_t *manager,
                                       uint64_t *out_time_us);

/** @brief Prepare one transaction with interrupts enabled on the owner task.
 * @param manager Ready manager, exclusively owning providers and device graph.
 * @param transaction Fresh initialized transaction or one with successful recovery.
 * @param request Absolute limits in the configured clock domain; required wake bits
 * refer to currently registered slots. No fields may alias the transaction output.
 * @retval ESY_OK Prepared; enter/recover then complete or abort is required.
 * @retval ESY_ERR_INVALID_ARG Invalid input, unregistered wake bit or transaction phase.
 * @retval ESY_ERR_BUSY Another transition is active or a constraint expired.
 * @retval ESY_ERR_NOT_SUPPORTED No compatible state or finite timing provider.
 * Provider errors propagate. After preparation starts, primary_error records the
 * cause; recovery_error != ESY_OK means retained ownership and mandatory system
 * recovery. Earlier validation/selection failures leave transaction unchanged.
 * A deadline becoming finite after prepare cancels entry with BUSY; retry a new
 * transaction after successful recovery so the timer can be prepared safely. */
esy_err_t esy_power_transaction_prepare(esy_power_manager_t *manager,
                                        esy_power_transaction_t *transaction,
                                        const esy_power_request_t *request);

/**
 * @brief Commit and enter with interrupts masked.
 *
 * A fatal result requires immediate fail-stop/reset without restoring
 * interrupts. On success, peer cores and source IRQs remain held until
 * late task-context recovery finishes. A failure after successful peer parking
 * also holds peers until late abort; do not restore peer execution directly.
 * @param transaction Prepared transaction owned by this caller.
 * @retval ESY_OK Entry attempted; inspect result.entered.
 * @retval ESY_ERR_INVALID_ARG Invalid handle. @retval ESY_ERR_STATE Invalid phase.
 * Other provider errors propagate; inspect result.fatal before any recovery.
 */
esy_err_t esy_power_transaction_enter_atomic(
    esy_power_transaction_t *transaction);

/**
 * @brief Perform bounded early recovery with global interrupts masked.
 *
 * Call after a successful prepare, whether atomic entry was attempted or
 * cancelled. This restores the backend and the IRQ-safe subset of the device
 * graph, and locally masks prepared wake sources. On success, restore the OS
 * tick and global interrupts, then call complete or abort to finish recovery.
 * @param transaction Prepared/entered transaction with no fatal result.
 * @retval ESY_OK Early recovery completed, late recovery still required.
 * @retval ESY_ERR_INVALID_ARG Invalid handle.
 * @retval ESY_ERR_STATE Invalid phase or fatal early recovery; keep interrupts
 * masked and reset when result.fatal is set. Failed ownership is retained.
 */
esy_err_t esy_power_transaction_recover_atomic(
    esy_power_transaction_t *transaction);

/**
 * @brief Finish a committed transaction in task context.
 *
 * Global interrupts and the OS tick must be enabled. The FreeRTOS scheduler may
 * remain suspended until its tickless port hook returns, so ordinary callbacks
 * may use bounded polling or interrupt-driven completion but must not block on
 * RTOS scheduling. Wake sources and peers are released after graph recovery.
 * The return value reports only completion/recovery status. A recoverable
 * hardware-entry error is written to @p out_result as `primary_error` and does
 * not make successful recovery fail.
 *
 * @param[in,out] transaction Prepared transaction after successful early
 * recovery.
 * @param[out] out_result Final entry and recovery result.
 * @retval ESY_OK Recovery completed; inspect `out_result->primary_error` for
 * the hardware-entry outcome.
 * @retval ESY_ERR_INVALID_ARG Invalid transaction or output pointer.
 * @retval ESY_ERR_STATE Invalid phase, fatal transaction, or incomplete
 * recovery.
 */
esy_err_t esy_power_transaction_complete(esy_power_transaction_t *transaction,
                                         esy_power_result_t *out_result);

/**
 * @brief Finish an uncommitted transaction abort in task context.
 *
 * Call after @ref esy_power_transaction_recover_atomic with global interrupts
 * and the OS tick enabled. The scheduler may remain suspended, so callbacks
 * must not wait on RTOS scheduling. Prepared wake sources remain locally
 * masked until graph and participant recovery finishes.
 *
 * A task-context caller that cancels before atomic entry may call this function
 * directly without recover_atomic to perform the full rollback with
 * interrupts enabled.
 * @param transaction Prepared (before entry) or atomically recovered uncommitted
 * transaction owned by the caller. Do not call directly after an entry attempt.
 * @param out_result Valid final output; untouched on invalid arguments/phase.
 * @retval ESY_OK Recovery completed. @retval ESY_ERR_INVALID_ARG Invalid inputs.
 * @retval ESY_ERR_STATE Wrong phase or failed recovery. A nonzero recovery_error
 * retains ownership and requires system recovery, not reinitialization.
 */
esy_err_t esy_power_transaction_abort(esy_power_transaction_t *transaction,
                                      esy_power_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif /* ESY_POWER_MANAGER_H */
