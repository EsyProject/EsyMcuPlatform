/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_DEVICE_H
#define ESY_DEVICE_H

#include <stdbool.h>
#include <stdint.h>

#if !defined(__cplusplus)
#include <stdatomic.h>
#endif

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/system/esy_power_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_device.h
 * @brief Static device graph, usage-reference, and PM lifecycle core.
 *
 * The device core is independent of the OS, board, HAL, HAL backend, and
 * middleware.
 * It owns no memory and performs no dynamic allocation. Registry lifecycle and
 * graph transitions are single-owner task-context operations. Device get/put
 * operations support concurrent task callers. Only the explicitly named
 * @ref esy_device_put_from_isr release path is available to ISR callers.
 */

/** @brief Device PM actions issued by the device core. */
typedef enum esy_device_pm_action_e {
  ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND = 0,
  ESY_DEVICE_PM_ACTION_RUNTIME_RESUME,
  ESY_DEVICE_PM_ACTION_SYSTEM_SUSPEND,
  ESY_DEVICE_PM_ACTION_SYSTEM_RESUME
} esy_device_pm_action_t;

/** @brief Observable device lifecycle state. */
typedef enum esy_device_state_e {
  ESY_DEVICE_STATE_UNINITIALIZED = 0,
  ESY_DEVICE_STATE_ACTIVE,
  ESY_DEVICE_STATE_RUNTIME_SUSPENDED,
  ESY_DEVICE_STATE_SYSTEM_SUSPENDED,
  ESY_DEVICE_STATE_TRANSITIONING,
  ESY_DEVICE_STATE_FAULTED
} esy_device_state_t;

struct esy_device_s;
struct esy_device_registry_s;

/** @brief Device object type embedded by stateful driver and logical handles.
 */
typedef struct esy_device_s esy_device_t;

/** @brief Explicit registry type owned by the system integrator. */
typedef struct esy_device_registry_s esy_device_registry_t;

/**
 * @brief Driver-owned PM action callback.
 *
 * @param[in,out] device Device being transitioned.
 * @param[in] action Runtime or system transition action.
 * @param[in] system_state System target/source state. Runtime actions receive
 * @ref ESY_POWER_STATE_ACTIVE and must ignore this argument.
 * @retval ESY_OK Transition completed.
 * @retval <0 Transition failed.
 * @retval ESY_ERR_STATE Irrecoverable suspend/rollback failure: hardware may
 * differ from its previous state. The registry and device become faulted and
 * require system recovery. Other suspend errors must leave hardware unchanged.
 *
 * @note The callback runs synchronously in the transition caller's task
 * context. It must be bounded, non-reentrant, and leave hardware unchanged
 * when returning an error from a suspend action. It must not call a public
 * operation that acquires a device usage reference, because the registry is
 * transitioning; use a private driver transition primitive instead.
 * SYSTEM_RESUME runs with interrupts and the OS tick enabled, but the RTOS
 * scheduler may still be suspended by a tickless-idle port. It may use bounded
 * polling or interrupt-driven hardware completion, but must not wait on an RTOS
 * object, delay, or scheduling action.
 */
typedef esy_err_t (*esy_device_pm_action_fn_t)(esy_device_t *device,
                                               esy_device_pm_action_t action,
                                               esy_power_state_t system_state);

/**
 * @brief Perform the interrupt-masked part of system resume.
 *
 * @param[in,out] device Device being recovered.
 * @param[in] system_state System state being exited or aborted.
 * @retval ESY_OK Early recovery completed.
 * @retval <0 Early recovery failed and the graph is faulted.
 *
 * @note This optional callback runs synchronously with global interrupts
 * masked. It may restore only always-on clocks, registers, or local IRQ masks
 * needed to make later interrupt delivery safe. It must be bounded,
 * non-blocking, allocation-free, logging-free, and independent of RTOS time.
 * A NULL callback declares that the suspended device cannot receive or service
 * an IRQ before its task-context SYSTEM_RESUME callback runs.
 */
typedef esy_err_t (*esy_device_pm_resume_atomic_fn_t)(
    esy_device_t *device, esy_power_state_t system_state);

/**
 * @brief Report whether persistent device activity permits runtime suspend.
 *
 * This callback is evaluated only after the usage reference count reaches
 * zero. It must be bounded, non-blocking, and safe in the transition caller's
 * task context. A NULL callback means usage references fully describe runtime
 * activity.
 */
typedef bool (*esy_device_is_runtime_idle_fn_t)(const esy_device_t *device);

/** @brief Immutable operations implemented by a device owner. */
typedef struct esy_device_ops_s {
  esy_device_pm_action_fn_t pm_action;
  esy_device_is_runtime_idle_fn_t is_runtime_idle;
  esy_device_pm_resume_atomic_fn_t system_resume_atomic;
} esy_device_ops_t;

/** @brief Registration configuration for one device object. */
typedef struct esy_device_config_s {
  const char *name;
  const esy_device_ops_t *ops;
  void *driver_ctx;
  esy_device_t *const *dependencies;
  uint32_t dependency_count;
  uint32_t supported_state_mask;
} esy_device_config_t;

/** @brief Owner-independent fields used by typed device adapters. */
typedef struct esy_device_registration_config_s {
  const char *name;                  /**< Stable diagnostic name. */
  esy_device_t *const *dependencies; /**< Parent/bus/domain devices. */
  uint32_t dependency_count;         /**< Dependency array length. */
  uint32_t supported_state_mask;     /**< Supported system states. */
} esy_device_registration_config_t;

#if defined(__cplusplus)
typedef uint32_t esy_device_atomic_u32_t;
#else
/** @brief C11 atomic field type used by public concrete device objects. */
typedef _Atomic uint32_t esy_device_atomic_u32_t;
#endif

/**
 * @brief Concrete device object embedded in typed peripheral handles.
 *
 * Initialize caller-owned storage with {0} before its first registration.
 * Uninitialized automatic storage is not supported. After registration, treat
 * all fields as read-only; never clear a registered object to bypass cleanup.
 * Successful unregister or registry deinit restores the object to the zero
 * state, ready for registration again. Failed registration leaves it unchanged.
 * The dependency array, name, operations, and driver context remain caller-owned
 * for the complete registration lifetime.
 */
struct esy_device_s {
  const char *name;
  const esy_device_ops_t *ops;
  void *driver_ctx;
  esy_device_t *const *dependencies;
  uint32_t dependency_count;
  uint32_t supported_state_mask;
  esy_device_registry_t *registry;
  esy_device_atomic_u32_t usage_count;
  esy_device_atomic_u32_t state;
  uint32_t registry_index;
  uint32_t magic;
};

/**
 * @brief Caller-owned registry backing arrays.
 *
 * The arrays need not be initialized before registry init; successful init
 * initializes their complete capacity. Keep these arrays and this resource
 * descriptor alive and exclusively assigned to the registry until deinit
 * succeeds.
 */
typedef struct esy_device_registry_static_resources_s {
  esy_device_t **devices;
  uint32_t *topology;
  uint32_t *work;
  uint32_t capacity;
} esy_device_registry_static_resources_t;

/** @brief Registry initialization configuration. */
typedef struct esy_device_registry_config_s {
  esy_device_registry_static_resources_t *static_resources;
} esy_device_registry_config_t;

/**
 * @brief Concrete device registry instance.
 *
 * Allocate one instance per independent device graph and initialize its storage
 * with {0} before its first init. Uninitialized automatic storage is not
 * supported. Fields are public for static allocation and ABI transparency but
 * are read-only to callers after init. Never clear a live registry to bypass
 * deinit. Successful deinit restores the zero state and permits init again.
 */
struct esy_device_registry_s {
  esy_device_registry_static_resources_t *static_resources;
  uint32_t device_count;
  uint32_t topology_count;
  esy_device_atomic_u32_t phase;
  esy_power_state_t suspended_state;
  uint32_t magic;
};

/** @brief Declare fixed backing storage for a device registry. */
#define ESY_DEVICE_REGISTRY_STATIC_STORAGE(name, device_capacity) \
  struct {                                                        \
    esy_device_t *devices[(device_capacity)];                     \
    uint32_t topology[(device_capacity)];                         \
    uint32_t work[(device_capacity)];                             \
  } name

/** @brief Bind declared storage to registry static resources. */
#define ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(resources, storage) \
  do {                                                                \
    (resources)->devices = (storage).devices;                         \
    (resources)->topology = (storage).topology;                       \
    (resources)->work = (storage).work;                               \
    (resources)->capacity =                                           \
        sizeof((storage).devices) / sizeof((storage).devices[0]);     \
  } while (0)

/**
 * @brief Initialize an explicit device registry.
 *
 * The registry must be zero-initialized before first use, or previously
 * deinitialized successfully. Backing arrays need no prior initialization.
 * Call from the single lifecycle owner in task context; no callbacks run.
 * On failure, the registry and backing arrays remain unchanged.
 *
 * @param[in,out] registry Caller-owned registry storage.
 * @param[in] config Configuration with exclusively owned backing arrays.
 * @retval ESY_OK Registry initialized and accepting registrations.
 * @retval ESY_ERR_INVALID_ARG Invalid pointers or zero capacity.
 * @retval ESY_ERR_STATE Registry is already initialized; it remains live.
 */
esy_err_t esy_device_registry_init(esy_device_registry_t *registry,
                                   const esy_device_registry_config_t *config);

/**
 * @brief Validate dependencies, reject cycles, and freeze registration.
 *
 * Registration order is unrestricted. The generated topology is deterministic
 * for a fixed registration order.
 */
esy_err_t esy_device_registry_start(esy_device_registry_t *registry);

/**
 * @brief Report whether registration is frozen and the graph is ready.
 *
 * This bounded query lets graph consumers borrow an already-started registry
 * without taking ownership of its lifecycle.
 */
bool esy_device_registry_is_ready(const esy_device_registry_t *registry);

/**
 * @brief Deinitialize an idle registry and invalidate registered devices.
 *
 * The single lifecycle owner must stop graph users and asynchronous activity
 * first. Only configuring or ready registries with no usage references can be
 * deinitialized. No driver callbacks run and no hardware is released. Success
 * clears registered device objects and backing arrays, and restores the
 * registry to the zero state. Registry and devices can then be initialized or
 * registered again without caller clearing. On failure, retain all storage and
 * ownership; release references or finish the current transition before retry.
 *
 * @param[in,out] registry Initialized caller-owned registry.
 * @retval ESY_OK Registry and registered devices invalidated.
 * @retval ESY_ERR_INVALID_ARG Registry is NULL or not initialized.
 * @retval ESY_ERR_BUSY A usage reference is still held; the registry stays live.
 * @retval ESY_ERR_STATE Transitioning, suspended, or faulted registry.
 */
esy_err_t esy_device_registry_deinit(esy_device_registry_t *registry);

/**
 * @brief Register a caller-owned device before registry start.
 *
 * Initialize device storage with {0} before its first registration, or reuse it
 * after successful unregister or registry deinit. The single lifecycle owner
 * calls this function in task context; no callbacks run. Configuration values
 * are copied, but their referenced objects remain borrowed until successful
 * unregister or registry deinit. On failure, the device and registry remain
 * unchanged; a failed first registration can be retried without clearing.
 *
 * @param[in,out] registry Initialized registry accepting registrations.
 * @param[in,out] device Caller-owned, unregistered device storage.
 * @param[in] config Registration values and borrowed dependency references.
 * @retval ESY_OK Device registered in the active state with zero usage count.
 * @retval ESY_ERR_INVALID_ARG Invalid configuration, device, or registry.
 * @retval ESY_ERR_STATE Registry is frozen or device is already registered.
 * @retval ESY_ERR_NO_MEM Registry capacity reached.
 */
esy_err_t esy_device_register(esy_device_registry_t *registry,
                              esy_device_t *device,
                              const esy_device_config_t *config);

/**
 * @brief Unregister an unused device before registry start.
 *
 * Call from the single lifecycle owner in task context. No callbacks run.
 * Success removes and clears the device, permitting registration again. On
 * failure, keep the device and all borrowed resources alive and unchanged.
 *
 * @param[in,out] device Registered caller-owned device.
 * @retval ESY_OK Device removed and restored to the zero state.
 * @retval ESY_ERR_INVALID_ARG Device is NULL or not registered.
 * @retval ESY_ERR_STATE Registry registration is frozen.
 * @retval ESY_ERR_BUSY Device has usage references or registered dependents.
 */
esy_err_t esy_device_unregister(esy_device_t *device);

/**
 * @brief Acquire one usage reference on a device and its dependencies.
 *
 * Dependencies are acquired first. A runtime-suspended device is resumed
 * synchronously before the reference is granted.
 */
esy_err_t esy_device_get(esy_device_t *device);

/** @brief Release one usage reference from a device and its dependencies. */
esy_err_t esy_device_put(esy_device_t *device);

/**
 * @brief Release one usage reference from ISR context.
 *
 * This path traverses the immutable dependency graph and performs 32-bit C11
 * atomic decrements. Native ports use always-lock-free atomics. The RP2040 SDK
 * backend masks local IRQs for each short atomic operation and may spin while
 * another core finishes an operation; it performs no RTOS wait. It is safe for
 * maskable ISR callers, not NMI/fault handlers. Do not halt/reset a participating
 * core while it owns the SDK atomic lock. ISR latency depends on graph depth
 * and contention and must be qualified on the target.
 */
esy_err_t esy_device_put_from_isr(esy_device_t *device);

/** @brief Query a device usage count. */
esy_err_t esy_device_get_usage_count(const esy_device_t *device,
                                     uint32_t *out_usage_count);

/** @brief Query a device lifecycle state. */
esy_err_t esy_device_get_state(const esy_device_t *device,
                               esy_device_state_t *out_state);

/**
 * @brief Runtime-suspend all currently idle devices child-first.
 *
 * On recoverable failure, devices suspended by this call resume parent-first.
 * A suspend callback returning ESY_ERR_STATE faults the device and registry;
 * no further graph callbacks run because hardware state is no longer reliable.
 */
esy_err_t esy_device_registry_runtime_suspend_idle(
    esy_device_registry_t *registry);

/** @brief Compute the deepest state permitted by device refs/capabilities. */
esy_err_t esy_device_registry_get_deepest_allowed_state(
    const esy_device_registry_t *registry, esy_power_state_t *out_state);

/**
 * @brief Return the exact automatic-idle capability intersection.
 *
 * Reset-class states require an explicit shutdown request and are not modeled
 * by this resumable device graph.
 */
esy_err_t esy_device_registry_get_allowed_state_mask(
    const esy_device_registry_t *registry, uint32_t *out_state_mask);

/** @brief Report whether an unrecoverable graph transition fault occurred. */
bool esy_device_registry_is_faulted(const esy_device_registry_t *registry);

/**
 * @brief Prepare the device graph for a system power state.
 *
 * This task-context phase runs with interrupts enabled. Active devices are
 * suspended child-first. Devices that were already runtime-suspended remain
 * suspended without an artificial runtime resume.
 */
esy_err_t esy_device_registry_system_prepare(esy_device_registry_t *registry,
                                             esy_power_state_t state);

/**
 * @brief Atomically commit a prepared device graph to system suspend.
 *
 * This bounded function performs no driver callbacks and may be called with
 * interrupts disabled immediately before hardware power-state entry.
 */
esy_err_t esy_device_registry_system_commit_atomic(
    esy_device_registry_t *registry);

/**
 * @brief Perform interrupt-masked early recovery of a prepared graph.
 *
 * Optional system_resume_atomic callbacks run parent-first. On success the
 * graph remains unavailable until @ref esy_device_registry_system_abort
 * completes task-context recovery.
 */
esy_err_t esy_device_registry_system_abort_atomic(
    esy_device_registry_t *registry);

/**
 * @brief Finish abort recovery and restore pre-prepare runtime states.
 *
 * Call after @ref esy_device_registry_system_abort_atomic with global
 * interrupts enabled. Ordinary SYSTEM_RESUME callbacks run parent-first. The
 * RTOS scheduler may still be suspended, so callbacks must not wait on RTOS
 * scheduling even though interrupt-driven or bounded polling I/O is allowed.
 * A task-context caller that has not entered the masked phase may call this
 * function directly to perform the complete abort without the atomic hook.
 */
esy_err_t esy_device_registry_system_abort(esy_device_registry_t *registry);

/**
 * @brief Perform interrupt-masked early recovery of a suspended graph.
 *
 * Optional system_resume_atomic callbacks run parent-first. On success the
 * graph remains unavailable until @ref esy_device_registry_system_resume
 * completes task-context recovery.
 */
esy_err_t esy_device_registry_system_resume_atomic(
    esy_device_registry_t *registry);

/**
 * @brief Finish task-context recovery of a system-suspended graph.
 *
 * Call after @ref esy_device_registry_system_resume_atomic with global
 * interrupts enabled. Ordinary SYSTEM_RESUME callbacks run parent-first. The
 * RTOS scheduler may still be suspended, so callbacks must not wait on RTOS
 * scheduling even though interrupt-driven or bounded polling I/O is allowed.
 */
esy_err_t esy_device_registry_system_resume(esy_device_registry_t *registry);

#ifdef __cplusplus
}
#endif

#endif /* ESY_DEVICE_H */
