# Power Manager

## Purpose

The power manager is a statically allocated controller for resumable idle. It
selects `CPU_IDLE` or `RETENTION` from device, backend, wake, timing, lock, and
dynamic constraint data, then executes the selection as a rollback-capable
transaction. Reset-class shutdown is deliberately outside automatic policy.

## Public Surface

- `esy_power_manager.h` defines the platform-independent manager core, power
  locks, quiesce participants, wake reasons, deadline timers, clocksources,
  dynamic constraints, multicore coordination, requests, and transactions.
- `esy_power_hal_backend.h` adapts `esy_hal` power entry and its
  always-on clocksource to the manager contracts.
- `esy_power_tickless_adapter.h` adapts a manager transaction to the FreeRTOS
  tickless contract exposed by `esy_os`.
- `esy_power_wakeup_timer_adapter.h` exposes one HAL hardware alarm as the
  manager deadline timer and monotonic clock domain.
- `esy_power_watchdog_constraint.h` optionally composes the watchdog monitor's
  hardware deadline into the manager without reversing module dependencies.
- `esy_hal_low_power.h` exposes resumable HAL transactions and the
  separate explicit shutdown API.

The build keeps these dependency directions explicit:

```text
esy_power_manager                 common/device only
esy_power_hal_backend       manager -> HAL
esy_power_freertos_adapter       manager -> esy_os
esy_power_wakeup_timer_adapter   manager -> HAL wakeup timer
esy_power_watchdog_constraint    manager + watchdog monitor
```

Every context, transaction, registration object, and backing array is
caller-owned. No power path allocates memory.

## Integration

Enable `ENABLE_ESY_POWER_MANAGER` and `ENABLE_ESY_LOW_POWER`. Initialize a device
registry, register all devices, and start the registry before starting a
manager that borrows it. The registry owner must keep it started until after
manager deinitialization. Register locks, quiesce participants, wake sources,
and constraints while the manager is still configuring.

Finite idle requires two separate timing services:

- one deadline timer that can be prepared in task context and armed with an
  absolute wake timestamp in the manager clock domain, then captured and
  cancelled before interrupts are restored;
- one monotonic 64-bit clocksource that remains valid through the selected
  state and is read immediately around hardware entry.

Wake sources only identify non-deadline wake reasons such as GPIO or radio
interrupts. They do not own deadline programming or elapsed-time accounting.
A required wake-source bit refers to its stable registration index.

Dynamic constraints return absolute hard deadlines in that same clock domain.
The manager samples them during selection and revalidates them in the final
interrupt-masked window. A running watchdog can use the optional watchdog
constraint target, which reads the monitor's last-feed deadline without making
the monitor depend on the power manager.

The FreeRTOS path is:

```text
sample current kernel-tick phase              interrupts enabled
  -> remaining window = full tick window - elapsed current-period time
prepare(remaining relative kernel window)     interrupts enabled
  -> convert to absolute latest_resume_time_us
  -> quiesce, constraints, wake setup, backend/device prepare
mask interrupts and confirm kernel idle
stop kernel tick, measure partial period, sample shared monotonic clock
enter_atomic()                                interrupts masked
  -> park peers, re-read clock, revalidate absolute constraints
  -> arm at hard deadline - exit latency - guard, graph commit
  -> backend entry and HAL backend early clock restore
  -> capture/mask wake reasons, cancel deadline, clocksource end
recover_atomic()                              interrupts masked
  -> restore backend and graph IRQ-safe minimum state
  -> capture/mask prepared wake reasons on an uncommitted abort
sample shared clock after recovery
settle partial period + stopped-clock interval, restart tick, restore interrupts
complete committed transaction                interrupts enabled
  -> deadline finish, late graph resume, constraints, participants, wake unmask
  -> release peer cores last
abort uncommitted transaction                 interrupts enabled
  -> graph rollback, backend/deadline cleanup, constraints, participants, wake
  -> abort coordinator and release parked peers last
```

### Lifecycle and recovery

Initialize manager, registration objects and adapters with `{0}` before first
use; initialize transactions with `esy_power_transaction_init()`. Repeated init returns `ESY_ERR_STATE` without
erasing live state. Only successful deinit permits reuse. Reset/initializer
helpers are for fresh unused storage and must never erase a live or faulted
transaction.

`esy_power_hal_backend_init()` returns `esy_err_t`; check the result
and pair it with `esy_power_hal_backend_deinit()` after manager deinit.
For the watchdog constraint, unregister its constraint before adapter deinit.
Clear the OS tickless registration before destroying the tickless adapter.
Keep all callback contexts and backing arrays alive until their owner has
successfully released them.

Custom `esy_os_tickless_ops_t` adapters must provide `recover_atomic`,
`check_prepare_recovery` and `read_clock_atomic`; registration rejects missing
callbacks. After failed prepare, `check_prepare_recovery` returns `ESY_OK` only
when cleanup is complete and normal execution is safe. Any error requests reset
with interrupts masked. The OS uses its own monotonic samples around the stopped
tick interval, including early recovery; the entry result's elapsed time remains
diagnostic. Both reads must use the same clock, valid across the chosen state.

Coordinators must release successfully parked peers in late `abort`, after graph
and producer recovery. A failed park must undo its own partial rendezvous
before returning. Capabilities remain stable during a transaction. Wake-source indices remain stable until that source is removed;
its empty slot can be reused by a later registration, so masks referring to the
removed source must be retired by their owner.

### Usage Example

This example suspends unused devices through the runtime PM path; it does not
enter CPU sleep or mask interrupts. One task owner supplies a `{0}` manager and a
prepared configuration with a started device registry, real backend operations
and persistent backing arrays. Keep the registry, providers and storage alive
until manager deinit succeeds, including after an error.

```c
#include "esy_middleware/power_manager/esy_power_manager.h"

esy_err_t suspend_unused_devices(esy_power_manager_t *manager,
                                 const esy_power_manager_config_t *config) {
    esy_err_t result = esy_power_manager_init(manager, config);
    if (result != ESY_OK) {
        return result;
    }
    result = esy_power_manager_start(manager);
    if (result == ESY_OK) {
        result = esy_power_manager_runtime_suspend_idle(manager);
    }
    esy_err_t cleanup = esy_power_manager_deinit(manager);
    return cleanup != ESY_OK ? cleanup : result;
}
```

Manager deinit detaches its registrations; it does not deinitialize the registry
or resume devices. Their owner manages subsequent usage references and lifecycle.
A faulted graph or manager requires the documented system recovery path instead
of clearing storage or repeating init. Use the tickless adapter and its complete
transaction/recovery sequence above for system idle.

## Contracts

- State selection considers only resumable states. Standby is available only
  through `esy_hal_shutdown()` and never passes through the device graph.
- A state is eligible only when the absolute window is at least entry latency +
  minimum residency + exit latency + late-resume guard, and its exit latency
  satisfies the request limit.
- Device, backend, required-wake, deadline, clocksource, lock, and request masks
  are intersected exactly; masks need not be monotonic.
- A finite request fails closed without a compatible deadline timer and
  clocksource. An infinite request requires at least one explicit wake source;
  an installed clocksource still restricts eligible states. If final constraint
  revalidation introduces a finite deadline without a prepared timer, entry
  returns `ESY_ERR_BUSY` and must be recovered/aborted before a later retry.
- The OS excludes the elapsed current-tick fraction before the adapter converts
  the remaining window into an absolute manager-clock deadline. A failed phase
  query skips tickless entry rather than risking a late wake.
- Deadline programming uses an absolute manager-clock timestamp and therefore
  accounts for subsequent prepare and SysTick-suspension time without
  subtracting mixed clocks.
- Early clock restoration is implemented by the selected low-power HAL backend and
  sequenced by the generic HAL transaction. A restore or final-clock read
  error is fatal: the OS keeps interrupts masked and requests a reset.
- Early committed recovery and post-prepare abort run before global interrupts
  are restored. Only backend recovery, wake-source capture/masking, and optional
  device `system_resume_atomic` hooks run there; they must be bounded,
  allocation-free, logging-free, independent of RTOS time, and non-blocking.
- Ordinary device, participant, constraint, deadline, wake-source finish, and
  peer callbacks run after interrupts and the OS tick are restored. FreeRTOS
  still has the scheduler suspended inside its tickless hook, so these callbacks
  may use bounded polling or interrupt completion but must not wait on RTOS
  objects, task delays, or scheduling.
- Prepare callbacks must be fail-atomic. Rollback and normal completion respect
  dependency order. A failed stage faults the manager and retains its ownership;
  dependent producers, wake unmasking and peer release do not proceed. A faulted
  registry during prepare is also a manager recovery failure. Retain all storage
  and use system recovery/reset; do not retry init to clear the fault.
- Transaction completion returns only the late recovery status. A non-fatal
  backend entry error remains available as `esy_power_result_t.primary_error`
  and does not request a system reset after recovery succeeds.
- Power locks can be acquired only while the manager is ready. Atomic entry
  revalidates their counts to close the final transition race.
- Wake sources use prepare, atomic capture-and-mask, and finish-and-unmask
  phases. Non-wake device IRQs remain the responsibility of their device PM
  callbacks.
- `managed_core_count > 1` requires a coordinator. It parks every managed peer
  before commit and holds them until graph, participants, constraints, and wake
  sources are fully restored.

## Resource Model

Use `ESY_DEVICE_REGISTRY_STATIC_STORAGE` for device graph arrays and
`ESY_POWER_MANAGER_STATIC_STORAGE` for lock, participant, wake-source, and
constraint pointer arrays. Registered names, operations, contexts, dependency
arrays, backend objects, and coordinator objects must remain valid until
deinitialization.

## Verification

Run the focused host suites:

```sh
cd esy_common/device/tests && ceedling test:all
cd esy_hal/tests && ceedling test:all
cd esy_middleware/power_manager/tests && ceedling test:all
ceedling gcov:all
python3 -B check_coverage.py build/artifacts/gcov/gcovr/coverage.json
python3 -B test_coverage_gate.py
python3 -B test_cmake_consumer.py
cd esy_middleware/os/tests && ceedling test:all
```

The regression set covers capability-based selection, absolute watchdog
deadlines and atomic revalidation, nonzero tick-phase deadline compensation,
exit-latency wake advance, separate deadline attribution, fatal clock
restore/final reads, IRQ-safe resume ordering, transaction rollback, FreeRTOS
settlement, and held multicore rendezvous.
The power-specific JSON coverage gate requires all five production sources,
at least 85% line coverage in each, and at least 70% aggregate branch coverage.
Its self-tests reject missing, empty, duplicate and below-threshold reports.
`tests/gcovr.cfg` limits object discovery to this suite and filters reports to
the module's production sources; dependency and test-framework coverage is
owned by their respective suites.
CMake checks compile/link C11 and C++17 consumers of each exported target and
verify feature-off, missing low-power, optional watchdog and RTOS-less assembly.
These checks run in the repository quality gate.
Hardware procedures are in `esy_hal/tests/hil/low_power_hil_plan.md`.

## Known Limits

- STM32F4 has no generic RTC/LPTIM board contract, so its standard always-on
  clocksource reports only `ACTIVE`. Timed retention remains unavailable until
  a consumer supplies a board-owned deadline timer and clocksource.
- Pico exposes only `CPU_IDLE` and requires a consumer-selected hardware alarm
  index; EsyMcuPlatform does not claim one implicitly.
- EsyMcuPlatform defines the multicore coordination contract but no generic
  coordinator. Products must supply one matching their scheduler and core
  ownership model.
- A fatal early-restore failure intentionally leaves the manager transaction
  unfinished because normal execution is no longer safe; reset is mandatory.

- Host tests do not prove multicore rendezvous, IRQ latency or clock accuracy.
  Validate stop/read and read/restart overhead, early recovery duration, timer
  resolution, late-resume guard, watchdog margin and reset behavior on hardware.
