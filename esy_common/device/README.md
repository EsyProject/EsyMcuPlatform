# Device Core

## Purpose

`esy_common_device` provides a statically allocated device graph, usage
references, runtime suspend/resume, and ordered system power transitions. It is
independent of the OS, board, HAL, HAL backend, and middleware layers.

## Public Surface

The public API is declared in `include/esy_common/device/esy_device.h`.
`esy_device_t` is embedded in caller-owned typed adapters for peripheral or
logical handles. `esy_device_registry_t` owns one explicit dependency graph.

The graph classifies resources by lifecycle rather than treating every API as
a device:

| Resource class | Representation | Integrations or owner policy |
| --- | --- | --- |
| Stateful hardware controller | Typed HAL adapter | DMA, SPI, XIO SPI, ADC, I2C, UART, XIO UART, PWM, timer capture, USB device |
| Independently suspendable logical peripheral | Middleware adapter | ADF4351/ADF4372 frequency synthesizer, ILI9488/LCD, XPT2046/touch, Wi-Fi manager |
| Child resource with no independent controller lifecycle | Owned handle plus adapter-specific GPIO PM profile | Auxiliary GPIO pins own the normal configuration; typed PM adapters claim one physical owner and select optional idle/sleep overrides |
| System policy or indivisible operation | Power lock | Flash/OTA transactions, protocol sessions, watchdog policy, and product critical work |
| Scheduler/time infrastructure | System power backend or wake policy | RTOS tick, wake timer, clock restore, reset, and watchdog hardware |

HAL backends do not register themselves. The system owner registers typed HAL
adapters after hardware initialization, and middleware registers a logical node
only when it owns additional independently suspendable state. This keeps graph
assembly, topology, and system policy outside interrupt and vendor-driver code.

## Integration

Link `esy_common_device` directly or consume it through `esy_common`. Allocate
registry backing arrays with `ESY_DEVICE_REGISTRY_STATIC_STORAGE`, register
caller-owned devices in any order, and call `esy_device_registry_start()` after
static assembly is complete. Graph consumers such as the power manager borrow
that started registry and never start, stop, or deinitialize it.

Initialize registry and device objects explicitly before their first use. The
backing arrays do not need to be cleared by the caller:

```c
static esy_device_registry_t registry = {0};
static esy_device_t device = {0};
static ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
static esy_device_registry_static_resources_t resources;
ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
const esy_device_registry_config_t registry_config = {
    .static_resources = &resources,
};
const esy_device_config_t device_config = {
    .name = "logical-resource",
    .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE),
};

esy_err_t result = esy_device_registry_init(&registry, &registry_config);
if (result != ESY_OK) return result;
result = esy_device_register(&registry, &device, &device_config);
if (result == ESY_OK) result = esy_device_registry_start(&registry);
if (result != ESY_OK) {
  esy_err_t cleanup_result = esy_device_registry_deinit(&registry);
  return cleanup_result == ESY_OK ? result : cleanup_result;
}
/* Keep all objects alive until graph users stop and registry deinit succeeds. */
```

## Contracts

- Registry lifecycle and graph transitions are single-owner task operations.
- Both `esy_device_registry_t` and `esy_device_t` require `{0}` before their
  first init or registration. Uninitialized automatic storage is unsupported;
  a magic field does not make it safe to pass such storage. Registry init
  initializes the backing arrays, so those arrays need no prior clearing.
- Repeated registry init or device registration returns `ESY_ERR_STATE` without
  replacing the live object. Failed first init or registration leaves the
  objects unchanged and permits retry after correcting the configuration.
- Successful unregister clears the device. Successful registry deinit clears
  every registered device, backing array, and the registry's lifecycle state.
  These objects can be reused without manual clearing. Deinit runs no driver
  callbacks and does not release hardware; the integrator still owns adapter
  detachment and physical cleanup.
- To retire a started graph, first stop producers and finish or cancel all
  asynchronous work, then deinitialize the registry, unbind each typed adapter,
  and deinitialize its hardware/backend. Registry deinit clears embedded graph
  nodes but leaves typed handle links and adapter-owned resources attached.
  Retain each binding until its own unbind succeeds. A later cleanup failure
  preserves the remaining ownership for retry; do not clear the binding to
  bypass it. Do not resume I/O or register a cleared node again before typed
  cleanup succeeds. Restore GPIO profiles to their default state before release.
- Failed unregister or registry deinit preserves ownership. Retain the objects,
  graph arrays, and borrowed references until cleanup succeeds. In particular,
  `ESY_ERR_BUSY` requires releasing usage references or removing dependents;
  never clear a live object or its arrays to bypass cleanup.
- `esy_device_registry_is_ready()` is a bounded lifecycle query; it transfers
  no ownership.
- Device get/put supports concurrent task callers. Only
  `esy_device_put_from_isr()` is safe for maskable ISR callers. Native ports use
  always-lock-free 32-bit atomics; RP2040 uses SDK IRQ-masked hardware spinlocks
  and can briefly contend with another core without an RTOS wait. NMI/fault
  handlers and core reset/halt while a lock is owned are excluded. Qualify ISR
  latency and graph depth on the target; see
  [atomic backends](../../docs/integration/atomic_backends.md).
- Dependencies are acquired before children and released after children.
- Runtime and system suspend are child-first; resume is parent-first.
- Runtime suspension skips devices that report persistent activity and
  protects every parent/bus/domain dependency reachable from those devices.
- Device acquisition reserves its usage reference before runtime resume, so
  concurrent autosuspend also protects an in-flight resume callback. A failed
  acquisition releases both that reservation and acquired dependency references.
- Cycles and dependencies outside the registry fail during registry start.
- Suspend callbacks must restore their entry hardware state before returning a
  recoverable error. If restoration is impossible, return `ESY_ERR_STATE`: the
  core faults that device and the registry and stops further graph transitions.
  This is a fatal contract, including runtime suspend; system recovery/reset
  belongs to the product. Existing adapters must reserve this result for an
  unrecoverable state. Other suspend errors retain the rollback contract.
- System recovery has two phases. Optional `system_resume_atomic` hooks run
  parent-first with global interrupts masked and restore only IRQ-safe minimum
  state. Ordinary `SYSTEM_RESUME` callbacks run parent-first after interrupts
  and the OS tick are enabled. They may use bounded polling or interrupt-driven
  I/O, but not RTOS waits because a tickless port may still have its scheduler
  suspended. Prepare-time rollback calls ordinary resume with interrupts
  enabled.
- Custom `esy_device_ops_t` implementations may leave `system_resume_atomic`
  NULL only when the suspended device cannot deliver or service an IRQ before
  late resume. ISR-facing devices must restore minimum hardware state or keep
  their source IRQ locally masked in that hook.
- Transition callbacks must not call public operations that acquire device
  references. The registry rejects new references while transitioning, so a
  callback uses private driver transition primitives for its own hardware.
- A handle with an optional `device` field must be zero-initialized before its
  HAL or middleware initializer is called, unless that initializer explicitly
  permits uninitialized storage (as Wi-Fi manager init does). It must be detached
  before its public deinitializer can release hardware. Wi-Fi bindings can also
  be detached after successful registry deinit; keep the typed binding and its
  manager alive until that operation succeeds.
- The device core stores dependency and power-state policy only. It has no
  generic pin-control field. Typed logical-device adapters may consume an
  adapter-specific caller-owned GPIO PM profile. Each profile describes only
  auxiliary GPIO idle/sleep differences; controller pads are selected by the
  UART/SPI/I2C adapter through the board port. The registry never manipulates
  pins directly.

## Resource Model

The registry, devices, dependency arrays, graph arrays, names, operations, and
driver contexts are caller-owned. They remain valid until unregister or registry
deinitialization. The module performs no dynamic allocation.

Middleware follows these integration rules:

| Module | PM integration |
| --- | --- |
| `freq_synth`, `lcd`, `touch`, `wifi` | Register a logical device only for an instance with independently suspendable state. |
| `logger`, `transport`, `protocol` | Use registered UART/SPI handles; their transfers or persistent UART RX streams hold the hardware references. They do not duplicate the hardware node. |
| `lvgl` | Uses the registered LCD instance and does not add a second display node. |
| `usb` | The product registers the USB controller adapter. CDC and protocol instances remain children of that product-owned lifecycle. |
| `ota`, `core_lockout` | Inject an `esy_activity_guard_t` backed by a product power lock; flash transactions are not modeled as logical devices. |
| `watchdog_monitor`, `os` | System policy/backend integration owns watchdog, wake timer, tick suppression, and clock restoration. |
| `numerics` | No hardware node by default; the product may hold a policy lock for a synchronous numerical operation that must not be interrupted by deep sleep. |

## Verification

Run the host tests from `esy_common/device/tests` with:

```sh
ceedling test:all
```

The host suite uses pthread synchronization to hold a runtime resume callback
while autosuspend scans the graph. It verifies that the scan succeeds without
another PM callback, the acquisition completes, and suspension remains possible
after release. Resume-failure tests check that device and dependency references
are released while the registry retains its fault state.
Idle-hook regressions verify no inspection while a device or its dependencies
hold usage references, inspection after release, and dependency protection for
persistent zero-reference activity.
Explicit `{0}` lifecycle cases also cover first init/registration with dirty
backing arrays, repeated init/registration, failed initialization and cleanup,
and successful unregister/deinit followed by reuse.

Run common public-header checks from the repository root with:

```sh
tools/checks/check_esy_common_contracts.sh
```

## Known Limits

- Runtime autosuspend scheduling policy belongs to the power-manager layer;
  this module only provides deterministic explicit idle suspension.
- Graph capacities are fixed at initialization.
- Transition callbacks are synchronous and task-context only.
- One-shot DMA adapters require the backend to dispatch either the completion
  or error callback exactly once. Continuous/circular DMA without a public stop
  contract is protected by the backend busy check; it may cause a requested
  deep transition to return `ESY_ERR_BUSY` instead of being selected out in
  advance.
- Persistent hardware activity that is not represented by a usage reference
  must implement `is_runtime_idle`; omitting it means reference count zero is
  the complete runtime-idle contract. The core checks usage references before
  calling this hook. A held reference skips the hook and protects the complete
  dependency closure; after release, the hook may veto suspension for persistent
  activity without a usage reference.
