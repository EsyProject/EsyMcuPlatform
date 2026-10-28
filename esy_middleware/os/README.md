<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# OS Middleware

## Purpose

`esy_os` is the portable RTOS boundary for tasks, synchronization, timers,
work queues, streams and messages. The FreeRTOS adapter owns native calls;
public headers expose opaque handles and caller-owned storage, without native
RTOS or vendor headers. The module does not allocate from the heap.

## Public Surface

Headers under `include/esy_middleware/os/` provide core initialization and
tickless callbacks, task/notification/snapshot operations, optional queue,
mutex, semaphore, timer, work, stream-buffer and message-buffer operations.
`esy_os_storage.h` defines the common lifecycle contract.
`esy_os_fatal.h` provides the immutable optional fatal-handler binding and
`esy_os_assert_failed()` for the consumer's native assertion macro.
`esy_os_context.h` exposes the ESP-IDF ISR/core/local-IRQ boundary used by the
system backend. It contains no native RTOS types and owns no task or memory.

## Integration

The consumer's `esy_feature_import.cmake` supplies `ESY_RTOS=FreeRTOS`,
`ENABLE_ESY_SYSTEM`, and the optional `ENABLE_ESY_OS_*` switches. Link `esy_os`;
its public interface propagates common types. The consumer supplies the native
provider through `FREE_RTOS_KERNEL_LIB`; native include paths and compilation
options remain private. The ESP32 port directly includes native ESP-IDF headers
under `freertos/`; other ports directly include their native FreeRTOS headers.
No generated forwarding headers or additional provider include directories are
used. A missing native header fails compilation.

ESP32 system integration also links `esy_os_context`, assembled before the
backend by the platform root (or supplied explicitly by a direct backend
consumer). This small native-port target depends on the FreeRTOS provider and
has no HAL dependency, so backend context queries cannot form an OS/HAL cycle.
It requires `ESY_RTOS=FreeRTOS`; local IRQ tokens nest only on the calling core and
provide no cross-core exclusion. ESP32 HAL millisecond delays are busy hardware
delays; applications that need scheduler waits use the task delay API.

For STM32F4 with FreeRTOS, the port supplies `SysTick_Handler` through the
`esy_os_stm32f4_interrupts` object target, included in the platform's
`esy_interrupts` interface target. Link `esy_interrupts` into the final firmware
so startup weak aliases cannot prevent extraction of the strong handler. The
consumer must not define a competing SysTick wrapper. The port first calls
`esy_hal_system_tick_from_isr()` on every tick and forwards to `esy_os_systick()`
only after the scheduler has started. Native scheduler suspension still counts
as started. This calls the native `xPortSysTickHandler()` function directly;
the provider need not define a same-named preprocessor macro. The hook is a
no-op on Pico and ESP32, whose native ports already own their tick interrupts.
Keep the native `xPortSysTickHandler` symbol distinct from `SysTick_Handler` in
the consumer's `FreeRTOSConfig.h`; a self-mapping macro is allowed. Aliasing it
to `SysTick_Handler` conflicts with the platform's strong wrapper and can cause
duplicate definitions or recursive forwarding.
Hardware access uses public HAL only; neither board nor application
code owns this entry. The configured kernel tick period must remain compatible
with the STM32 HAL timebase and the tickless settlement contract. Other
platforms use their native tick owners.

`ENABLE_ESY_OS_WORK` requires both `ENABLE_ESY_OS_QUEUE` and
`ENABLE_ESY_OS_TIMER`; invalid combinations fail at configure time. Core/task
and task-snapshot sources remain available when optional wrappers are off.
Native configuration must enable static allocation and the capabilities used
by selected wrappers. Timer cleanup requires `INCLUDE_xTimerPendFunctionCall=1`.
Work requires `INCLUDE_vTaskSuspend=1` and `INCLUDE_eTaskGetState=1`. Task identity
queries require the native current-task API. Indefinite waits require native
`INCLUDE_vTaskSuspend=1`; do not configure them as finite timeouts.

### Usage Example

Select `ENABLE_ESY_OS_QUEUE`, link `esy_os`, and call this example from task
context after OS initialization. A private one-item queue copies a value out
again; zero timeouts keep both operations nonblocking.

```c
#include "esy_middleware/os/esy_os_queue.h"

esy_err_t queue_round_trip(uint32_t value, uint32_t *received) {
    if (received == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    esy_os_queue_static_storage_t storage;
    uint32_t queue_buffer;
    esy_os_queue_t *queue = NULL;
    esy_err_t result = esy_os_queue_create_static(
        sizeof(value), 1u, &queue_buffer, &storage, &queue);
    if (result != ESY_OK) {
        return result;
    }
    result = esy_os_queue_send(queue, &value, 0u);
    if (result == ESY_OK) {
        result = esy_os_queue_receive(queue, received, 0u);
    }
    esy_os_queue_delete(queue);
    return result;
}
```

The queue stays private and is deleted before its local backing storage expires.
For a queue shared by tasks or ISR producers, retain its handle, control storage
and buffer in the lifecycle owner's persistent state, stop every user before
deletion, and use the explicit ISR APIs where required. Consume `received` only
when the function returns `ESY_OK`.

### Public API requirements

Opaque storage size and alignment are ABI requirements. C++17 and C11 storage
have matching alignment and aggregate layout. Manual source lists must include
`esy_os_task_snapshot.c`.

- Timer stop/delete return `esy_err_t`. Retry `ESY_ERR_BUSY` from a running
  task while retaining storage and callback context; only `ESY_OK` completes
  cleanup. Ignoring the return and reclaiming memory is invalid. Finish a pending
  stop before requesting delete (`ESY_ERR_STATE` otherwise).
- `esy_os_task_delay_until()` takes `esy_os_tick_t *` and returns status. Initialize
  it with `esy_os_task_get_tick()` and retain the public tick type.
- Notification actions use `esy_os_notify_action_t` and named public constants.
  Raw native action numbers are not a public contract; invalid actions are rejected.
  Value-based notify/wait and semaphore-style give/take share the same default
  notification value and pending state. They are alternate views of one slot,
  so consumers must coordinate their use. Clearing notification state preserves
  its value; take clears or decrements that value. Shared host fakes preserve
  this interaction, including ISR sends, rather than providing separate channels.
- Work queue creation takes `esy_os_work_queue_resources_t`, including buffer
  capacities. Allocate `ESY_OS_WORK_QUEUE_BUFFER_BYTES(depth)` queue bytes and
  an aligned stack; all resources stay borrowed until destruction finishes.
- Work destroy, cancel_sync, flush and delayed cancellation return status.
  Delayed reschedule can return `BUSY` while retiring the old timer, including
  reschedule-to-zero. Retry or cancel to completion before releasing resources.
- Message/stream reset return status; `BUSY` preserves contents when native
  reset fails. Message receive reports `TIMEOUT` for no message and `NO_MEM` for
  an undersized destination; it never truncates or consumes an oversized message.
- The snapshot API requires caller-owned scratch as its fifth argument. Names
  are inline arrays, not borrowed pointers; copy rather than assign names.
  Native runtime counters are explicitly narrowed modulo 2^32 in public results.

### Tickless adapter integration

Custom `esy_os_tickless_ops_t` providers must implement `recover_atomic`,
`check_prepare_recovery` and `read_clock_atomic`; missing callbacks are rejected
at registration. A failed prepare is safe only if `check_prepare_recovery`
returns `ESY_OK`; otherwise the port masks interrupts and requests reset. Keep failed
transaction storage alive for system recovery.

The OS samples the provider's shared monotonic clock after stopping the kernel
tick and after masked early recovery. Tick settlement includes the partial tick
and this interval, including a canceled entry's recovery time. Entry-result
elapsed time is diagnostic. An overrun does not truncate elapsed ticks or discard
the fractional remainder. The port steps strictly before the earliest task
timeout and publishes subsequent ticks with `xTaskIncrementTick()` while the
idle owner still holds scheduler suspension. Native `xTaskResumeAll()` processes
the pending ticks and unblocks expired tasks after the port hook returns. A
hardware-pending tick is deducted once and remains owned by its ISR.

At most 1024 synthetic pending ticks may be published per transaction. Replay
requires one native tick operation per synthetic tick, including any configured
native tick hook in IRQ-masked idle context. Those hooks retain their bounded,
non-blocking ISR restrictions. Provider recovery and replay together must fit the
consumer's IRQ latency and watchdog budgets. More replay, unrepresentable elapsed
time, clock failure or regression requests masked reset before partial settlement
or tick/IRQ restoration, preserving fail-stop behavior instead of silently losing
time. Reads must be bounded and valid through the selected power state. Measure
stop/read, settlement and read/restart overhead on the target.
Late callbacks run with interrupts enabled but the scheduler suspended and must
not block on RTOS objects. Registration and removal require a stopped scheduler.

The native sleep-admission check must reject pre-existing kernel pending ticks.
The inspected provider reports `V11.1.0+` and implements this contract; merely
exposing similarly named functions does not qualify older or custom providers. Native SMP
also requires global scheduler suspension that retains the TASK lock, a declared
`configTICK_CORE`, and IRQ-preserving recursive `portGET_ISR_LOCK()` /
`portRELEASE_ISR_LOCK()` operations. Only the configured tick core enters this
adapter; each replay publication briefly takes ISR lock in TASK-to-ISR order.
ESP-IDF SMP has different per-core suspension semantics, so tickless registration
returns `ESY_ERR_NOT_SUPPORTED` for it and for SMP providers missing the required
declarations. Single-core ports retain their local IRQ-masked path.

## Contracts

### Fatal events

The serialized startup owner may call `esy_os_set_fatal_handler()` once after
`esy_os_init()`. Cortex ports require registration before scheduler startup.
ESP32's native `app_main()` already runs under the SDK scheduler; its app startup
owner registers before creating application-owned tasks. Registration after
application startup is unsupported. An atomic release/acquire publication lets
concurrent fatal readers observe either an absent or complete immutable binding,
without a kernel lock or compare/exchange. The RAM-resident callback and its
context remain valid until reset; no runtime unbind is provided. The OS stack
overflow hook and `esy_os_assert_failed()` dispatch reason and opaque source/task
details without logging or dereferencing those addresses. The handler must never
return, block, allocate or call the OS; kernel locks may already be held. An
absent or returned callback preserves the default disable-interrupts-and-halt
behavior. The optional HAL fatal entry is one compatible consumer choice; the
OS module does not depend on HAL fault capture or coredump storage. Consumer
linker scripts must copy the documented RAM/IRAM function sections when using
these fatal paths while Flash is unavailable.


### Storage and ownership

Create accepts unused, aligned storage containing arbitrary bytes. It never
reads an uninitialized flag or magic. Do not recreate a live object: callers
must serialize lifecycle and keep output-pointer objects distinct from storage.
Failed creation clears a valid output handle, retains no resources and permits
retry. Successful creation borrows backing memory and callback context.

Before delete/reset, stop users, ISR producers and blocked callers; unlock
mutexes. Successful synchronous deletion invalidates aliases. Repeated delete
is harmless only while the same wrapper storage remains resident and has not
been reused. Self-deleting tasks have deferred native TCB cleanup: retain their
resources for system lifetime unless native reclamation is independently proven.
Deletion of another task requires it to be parked and not executing on a core.

### Time, callbacks and concurrency

Finite positive millisecond waits round up to a tick and saturate below the
native forever token. Timeout `UINT32_MAX` maps directly to `portMAX_DELAY`.
Delay-until copies through a native-width temporary, including at tick wrap;
it never casts a pointer to a differently sized integer. ESP32 task and ISR
critical sections share the same cross-core spinlock. Other ports use their
native task/ISR critical APIs. Native interrupt-priority restrictions apply.

Timers have one serialized control owner. Start is a nonblocking command enqueue;
queue saturation returns `BUSY`. Stop/delete enqueue a native operation and a
daemon fence with zero wait. `BUSY` means cleanup is incomplete, including before
the scheduler starts. A callback may make a nonblocking cleanup attempt, but
must return and allow the daemon to progress; it must not spin on `BUSY`.
Do not rearm a timer or race lifecycle owners while cleanup is pending.

Work submissions support multiple task/ISR producers and one FIFO worker.
Short critical sections protect admission and completion; handlers and blocking
calls run outside them. ISR submission reports `need_yield` and never yields;
the ISR owner aggregates notifications and yields once. Idle cancellation does
not affect the next submission. Cancel/flush pin the observed activity against
resubmission until completion, without sequence-number wrap or lost wakeups.
Waiting on one's own worker returns `STATE`. Synchronous waits require a running
scheduler, no critical section, and must not run in timer callbacks.

Destroy closes admission, waits for in-flight submitters, drains accepted work,
then confirms the worker is suspended before deleting task and queue. Stop
external producers before reclaiming the queue. Handlers must finish in bounded
time; destroy and synchronous waits have no timeout. A retained delayed-work
binding returns `BUSY` before closing the queue. Cancel delayed objects to
completion first, even after timer expiry or failed start. Delayed expiry makes
one enqueue attempt; `esy_os_delayed_work_last_result()` exposes dispatch errors.
`is_active()` becomes false after expiry or start failure.

Stream/message buffers use the native single-writer/single-reader contract.
Static capacity is the supplied allocation minus one reserved byte; messages
also consume a native length field per message. Stream I/O may return `OK` with
partial or zero bytes; inspect the count. Message receive is all-or-nothing.
ISR receive returns `NO_MEM` when empty or the destination is too small.

### Snapshots and platform policy

Snapshots are synchronous and task-only. Outputs and scratch are non-overlapping,
exclusive to the call and not retained. Capacity is 1 through
`ESY_OS_TASK_SYSTEM_STATE_MAX`; insufficient native capacity returns `NO_MEM`,
invalid arguments return `INVALID_ARG`, and unavailable tracing returns
`NOT_SUPPORTED`. Optional counts are cleared on entry and items stay unchanged
on error. Each entry owns a name truncated to 31 bytes plus NUL. Native handles
are borrowed identifiers, not references that keep tasks alive.

An outer scheduler suspension protects native enumeration and name copying.
ESP32 without `CONFIG_FREERTOS_UNICORE=1` returns `NOT_SUPPORTED` until equivalent
cross-core name-lifetime protection is established. Core affinity is applied
only where native SMP/core-affinity support exists, otherwise it is a no-op.

OTA policy belongs to the consumer. Before a Flash transition requiring worker
silence, stop producers, finish delayed cancellation, and drain/destroy queues.
The consumer must bound its handlers and define timeout, recovery, watchdog,
ISR/DMA and output-safety policy. The library cannot establish those properties.

## Resource Model

All control blocks, task stacks, queue data and stream/message backing buffers
are caller-owned. Work queue resources are aggregated in one public descriptor;
queue storage contains native queue and worker control blocks. Delayed storage
contains base work and timer control blocks. Native size/alignment assertions
reject configurations exceeding the public budgets. Snapshot scratch avoids a
maximum-sized temporary array on task stacks. No runtime heap ownership is hidden.

## Verification

`tests/test_esy_os_fatal.c` verifies registration lifecycle and assertion/stack
overflow forwarding in the native-port fixture. The `fatal_esp32.yml` variant
checks successful one-time binding with the native scheduler already running;
the default and `tickless_smp.yml` variants retain Cortex pre-scheduler admission.
Fatal forwarding makes no scheduler or HAL call. Native object inspection must
also confirm callback loads/stores remain inline on the selected ISA, including
RP2040 without a library-backed atomic read. Host tests do not establish
emergency-stack, IRQ, watchdog-reset or retained-memory behavior on hardware.


From `tests/`:

```sh
ceedling test:all
ceedling gcov:all
python3 -B check_coverage_sources.py
python3 -B test_coverage_sources.py
python3 -B test_cmake_consumer.py
ruby ../../../tools/quality/run_module_tests.rb . /tmp/esy-os-tick16 test:test_os_contracts --mixin=tick16.yml
ruby ../../../tools/quality/run_module_tests.rb . /tmp/esy-os-tick64 test:test_os_contracts --mixin=tick64.yml
ruby ../../../tools/quality/run_module_tests.rb . /tmp/esy-os-tickless-smp test:test_esy_os --mixin=tickless_smp.yml
ruby ../../../tools/quality/run_module_tests.rb . /tmp/esy-os-fatal-smp test:test_esy_os_fatal --mixin=tickless_smp.yml
ruby ../../../tools/quality/run_module_tests.rb . /tmp/esy-os-fatal-esp32 test:test_esy_os_fatal --mixin=fatal_esp32.yml
ruby ../../../tools/quality/run_module_tests.rb . /tmp/esy-os-tickless-tick16 test:test_esy_os --mixin=tickless_tick16.yml
ruby ../../../tools/quality/run_module_tests.rb . /tmp/esy-os-tickless-tick64 test:test_esy_os --mixin=tick64.yml
ruby ../../../tools/quality/run_module_tests.rb . /tmp/esy-os-esp32-critical test:test_os_contracts --mixin=esp32_critical.yml
ruby ../../../tools/quality/run_module_tests.rb . /tmp/esy-os-trace-disabled test:test_esy_os_task_snapshot --mixin=trace_disabled.yml
ruby ../../../tools/quality/run_module_tests.rb . /tmp/esy-os-esp32-snapshot test:test_esy_os_task_snapshot --mixin=esp32_snapshot_unsupported.yml
```

Coverage population is read from JSON, including the ESP32 execution-context
bridge. Missing production sources, duplicates and test-fixture entries fail;
human-readable report wrapping cannot hide a source.

All ten portable production translation units participate in the tests. Native
fixtures model command queuing, timer fences, queue capacity, worker scheduling boundaries,
message preservation and reset rejection. Tests cover poisoned first storage,
creation failure/retry, delayed cleanup, cancellation completion interleavings,
ISR lock selection, notification actions, low-rate/16-bit/64-bit ticks and native
64-bit runtime counters. CMake tests build all wrappers and the optional-off
configuration, reject missing dependencies, exercise ESP32 prefixed providers,
reject unqualified SMP tickless registration, and compare C/C++ storage
size/alignment/aggregate offsets using only `esy_os`. Tickless regressions model
the native next-unblock boundary and pending-tick processing on scheduler resume,
including committed/canceled early-recovery overruns, fractional carry, one
hardware-pending tick, native tick wrap, bounded replay and tick-owner SMP locks.
The repository quality gate runs these checks and verifies that coverage includes
every portable production C file and excludes fixtures. Shared OS fake tests run
separately.
The JSON inventory resolves canonical local source identities and rejects foreign
namesakes and duplicate aliases. Optional local internal-header rows cannot
replace a required C source. See the [quality tools guide](../../tools/README.md)
for accepted report path roots. This population check does not impose a separate
per-source line or branch percentage threshold.
The CMake consumer suite links the real STM32F4 interrupt translation unit and
OS wrapper to an ordinary native handler function without a same-named macro.
It also accepts the self-mapping macro form.
It checks pre-scheduler HAL progress, HAL-before-native ordering, exactly one
native tick per interrupt after startup and during scheduler suspension,
conditional target assembly, and the object's restricted dependencies. Pico
and ESP32 consumers also link and run the real wrapper without supplying a
Cortex-M SysTick symbol. These checks require no hardware SDK headers.

On a qualified native tickless target, arrange a task timeout inside a canceled
entry's bounded early-recovery overrun. Compare the native tick count and task
wakeup against the always-on clock, repeat with a pending physical tick and
fractional intervals, and verify expiry processing after scheduler resume.
On SMP, verify only the configured tick core enters the adapter. Measure masked
latency with the configured tick hook and the maximum permitted replay; confirm
the watchdog budget and the reset path for excessive replay on a recovery fixture.

## Known Limits

Host fixtures do not prove native scheduling, priority inversion, IRQ ceilings,
SMP deletion/reclamation, timer-daemon fairness or hardware timing. The active
consumer build checks its selected features and toolchain; it does not validate
other vendor toolchains or disabled wrappers on hardware. In target tests, exercise
ESP32 lock contention, 16/64-bit native ports, task stack margins, OTA quiescence
and timer/work stress. No hardware acceptance is implied by host coverage.

### Cooperative task shutdown

`esy_os_task_park_supported()` reports native suspend/state-query availability.
A task finishes callbacks and releases shared resources, then calls
`esy_os_task_park()`; it must never be resumed. Its exclusive owner polls
`esy_os_task_delete_parked()`, which returns BUSY until native suspended state is
observed and only then deletes the task. Successful deletion permits reusing its
static stack/TCB; BUSY/errors do not. This is distinct from self-deletion, whose
storage reclamation may be deferred. Required native options are
`INCLUDE_vTaskSuspend=1` and `INCLUDE_eTaskGetState=1`. The owner supplies its own
bounded retry and recovery policy. The native fixture suite checks state/error
transitions; exercise cross-core scheduler behavior on the selected target.
