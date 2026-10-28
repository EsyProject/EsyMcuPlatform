# Core lockout

## Purpose

Run a synchronous operation inside a CPU safe window. The engine acquires an
optional activity guard and masks local interrupts. The FreeRTOS adapter also
parks the scheduled peer core in RAM with its interrupts masked. The module
coordinates CPU execution; it does not initialize Flash, own a Flash transaction,
or register a Flash hook. Application-level OTA quiescence belongs to the consumer.

## Public Surface

- `esy_core_lockout_core_init()` initializes single-active-core execution without OS resources.
- `esy_core_lockout_init()` initializes the optional FreeRTOS peer workers.
- `esy_core_lockout_execute(operation, arg)` synchronously executes one callback in the safe window.
- `esy_core_lockout_set_activity_guard(guard)` configures an optional borrowed guard before successful init.

The callback is `void (*)(void *)`. It must not block, depend on interrupts or
peer execution, or reenter this module. Communicate operation-specific results
through its argument. A successful execute result describes coordination and
completion of the callback; it does not establish the callback's business result.

## Integration

Select `ENABLE_ESY_CORE_LOCKOUT` in the consumer's `esy_feature_import.cmake` and
enable `ENABLE_ESY_SYSTEM`. Flash support is not required. Link
`esy_core_lockout_core` for the OS-independent engine. With `ESY_RTOS=FreeRTOS`,
`esy_core_lockout` adds the OS worker adapter and requires `esy_os`. With
`ESY_RTOS=None`, link `esy_core_lockout_core` directly; the OS adapter target is not
assembled. Unsupported RTOS/dependency combinations fail configuration.

For Flash consumers, explicitly bind the execution API after physical Flash and
OS initialization, before any task can mutate Flash:

```c
ret = esy_hal_flash_set_safe_execute(esy_core_lockout_execute);
if (ret != ESY_OK) return ret;
ret = esy_core_lockout_init();
if (ret != ESY_OK) return ret;
```

Binding before initialization makes the installed hook reject execution while
workers are unready, including partial initialization failure. Stop startup on
binding or initialization failure. For a single active core, use
`esy_core_lockout_core_init()` instead. Hook support is a separate Flash-port
contract: Pico accepts custom hooks; STM32F4 and ESP32 currently reject a
non-NULL hook. Core initialization itself performs no hook registration.

Worker priority defaults to 31 and stack capacity to 1024 bytes. Supply a
compatible priority range or override `ESY_CORE_LOCKOUT_TASK_PRIORITY` for the
adapter target. The port must support pinned workers and RAM execution of the
IRQ, event and atomic primitives used while a peer is parked.

Manual archive links include `libesy_core_lockout_core.a`; with `ESY_RTOS=None`
there is no adapter archive. The consumer explicitly binds the Flash hook before
initialization and checks both operations before permitting Flash access.

### Usage Example

For caller-driven single-active-core execution, keep every peer inactive until
reset and call from the initializing core, outside ISR context. The caller
supplies a real operation and its argument; all code/data needed by that operation
must remain accessible while local interrupts are masked. The callback must not
block, reenter lockout or depend on interrupts/peers. This example installs no
Flash hook and creates no OS worker.

```c
#include "esy_middleware/core_lockout/esy_core_lockout_core.h"

esy_err_t run_single_core_operation(esy_core_lockout_operation_t operation,
                                    void *argument) {
    if (operation == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    esy_err_t result = esy_core_lockout_core_init();
    if (result != ESY_OK) {
        return result;
    }
    return esy_core_lockout_execute(operation, argument);
}
```

The argument is borrowed only until execute returns. Inspect the callback's
operation-specific result separately from coordination status. Initialization
fixes the mode/owner until reset; there is no stop/deinit API. An execution error
can follow an operation effect, so do not replay it blindly.

## Contracts

Initialization, guard configuration and execution are externally serialized.
The module adds no internal mutex. Calls are non-reentrant and forbidden in ISR
context. Once the scheduler runs, execute from task context with interrupts
enabled. Before scheduler start, all peers must be inactive. The consumer owns
operation serialization, DMA and other XIP users, and watchdog/IRQ latency budgets.
Provider and callback durations determine latency; there is no hard time bound.

Caller mode fixes the initializing core until reset. Every peer must remain
inactive for that entire lifetime, even on a device reporting multiple physical
cores. Wrong-core init/execution returns STATE before acquiring the guard or
masking IRQs. Core count cannot detect an independently launched peer. A physical
core outside OS scheduling also needs consumer-enforced inactivity in OS mode.

Mode selection precedes worker preparation and cannot change until reset.
Preparation failure leaves execute returning STATE. Retrying the same init path
creates only missing workers, preserving partial allocation. Successful init
freezes the guard. Repeated init creates no duplicate resources, but never clears
a guard release fault. No deinit or runtime mode switch is provided.

The serialized caller publishes request/release sequence numbers; each worker
publishes locked/completed sequence numbers. Acquire/release atomic loads and
stores establish these separate ownership domains. Cancellation cannot be
lost to a late lock acknowledgement. Sequence wrap is safe because only one
request per worker may be outstanding.

A peer lock timeout prevents callback execution, publishes release and waits up
to one additional one-second timeout for completion. An undrained canceled
request blocks new execution with BUSY. Retries notify the outstanding worker
again, even after the caller moves to another core. A peer release timeout can
occur after the callback ran; inspect the operation state or abort its transaction
instead of blindly replaying it.

Each successful guard acquire receives exactly one release attempt, including
when peer locking failed. Acquire errors return unchanged without release; the
provider must leave no ownership behind on an unsuccessful acquire. Guard
release failure overrides coordination errors with STATE and permanently latches
a fault. Subsequent execute/init calls reject without guard callbacks or another
lockout. Pending workers may still drain, but cannot clear this fault. Retain
provider resources and use the consumer's fatal/reset path; there is no automatic
cleanup retry, reset of live storage or recovery API. The operation may already
have executed when cleanup fails.

### OTA worker policy

Both peer workers remain available throughout OTA because every coordinated
Flash erase/program needs them. They consume notifications and execute only the
RAM handshake while parked. They own no DMA, hardware output, timer, application
queue or watchdog report. IRQ masking prevents peer ISR execution during the safe
window; it does not stop independent DMA or application protocols. Consumers
must establish their OTA policies and watchdog margins separately.

Workers return to notification waits after success or acknowledged cancellation;
OTA failure requires no worker teardown. Undrained cancellation blocks subsequent
operations; reset is a product recovery option if the peer cannot recover.
Single-active-core mode creates no workers and provides only local IRQ/guard
protection. It has no independent-peer stop handshake.

## Resource Model

The engine owns one static guard/owner/mode/fault context. The adapter owns a
separate static context with two task-control objects, two aligned stacks and
handshake state. Caller mode allocates none of those OS resources. No runtime
heap allocation is used. Resources remain valid until reset; never clear live
worker storage after timeout or partial allocation failure.

The guard descriptor is borrowed, not copied. Keep its fields unchanged and its
storage, callback code and pointed-to context valid. Before successful init, a
successful replacement or NULL configuration ends the previous borrow; invalid
replacement or init failure does not. After successful init configuration is
frozen until reset, including after a release fault. Do not use a descriptor or
context local to a function that returns.

Guard callbacks run synchronously in the caller context outside local IRQ
masking. The provider owns and synchronizes its context. If execution occurs
before scheduler start, callbacks must support that context. Callback code/data
placement and all other accesses while XIP is unavailable remain the operation
provider's responsibility.

## Verification

Run from this directory:

```sh
(cd tests && ceedling test:all)
(cd tests && ceedling gcov:all)
python3 -B tests/test_cmake_consumer.py
```

| Risk | Test |
| --- | --- |
| Cancellation and late worker | `test_core_lockout_timeout_drains_late_worker_before_retry` |
| Cancellation during request observation | `test_core_lockout_cancellation_between_request_read_and_lock_is_not_lost` |
| Missing notification, cross-core retry | `test_core_lockout_unacknowledged_cancel_blocks_both_cores_until_drained` |
| Executed operation with undrained release | `test_core_lockout_release_timeout_preserves_executed_operation_and_pending_worker` |
| Sequence/clock wrap | `test_core_lockout_sequences_wrap_and_survive_repeated_timeouts` |
| Partial creation, retry, duplicate notification | `test_core_lockout_init_fails_closed_retries_partial_creation_and_is_idempotent` |
| Single scheduled core, guard ordering | `test_single_core_init_guards_operations_without_workers_or_flash_dependency` |
| Single active owner, ISR/reentry, fault latch | `test_core_lockout_core_guards_one_owner_without_os_and_latches_uncertain_release` |
| Borrowed guard replacement/fault lifetime | `test_core_lockout_guard_replacement_validation_and_fault_lifetime` |
| Public C/C++ headers, no OS or Flash, feature closure | `tests/test_cmake_consumer.py` |

Guard failure tests also cover acquisition rejection, cleanup precedence before
and after callback execution, and a permanent fault surviving late completion.
Host fixtures deterministically drive requester/worker endpoints. They do not
emulate FreeRTOS SMP or establish target timing. Consumer startup tests cover
explicit Flash binding and reject binding/init failure before business startup.

## Known Limits

The adapter supports at most two schedulable cores. The coordinated path has
been integrated with the Pico port; a build on another port does not establish
correct runtime coordination. Caller mode requires all peers inactive for its
entire lifetime. This is an execution-window facility, not arbitrary CPU power,
reset, scheduling or task-lifecycle control.

Target measurements cover RAM placement (including generated atomic instructions),
maximum IRQ-off duration, stack usage, peer scheduling, DMA/XIP interactions and
watchdog timing on hardware. Guard release faults require reset. Host tests and
embedded compilation do not establish these hardware properties.
