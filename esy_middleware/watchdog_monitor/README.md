# Watchdog Monitor

## Purpose

`esy_watchdog_monitor` supervises task heartbeats and feeds the HAL watchdog
only when all enabled slots reported during the preceding global window.
It uses caller-owned static storage and one shared supervision core. Execution
is either caller-driven or performed by an optional system-owned OS worker.

## Public Surface

The public header is `include/esy_middleware/watchdog_monitor/esy_watchdog_monitor.h`.
It exposes the OS lifecycle `default_cfg`, `init`, `start`, `stop`, and `deinit`.
The OS-free `esy_watchdog_monitor_core.h` exposes `core_default_cfg`, `core_init`,
`core_start` and `core_process`, plus the same report/control/stop/deinit APIs.
The singleton supports one execution mode at a time. Caller-driven participants
do not require OS tasks.
Tasks register value report tokens, report heartbeats, enable/disable checking,
query the last completed check, and unregister their own tokens.
`set_alert_cb` installs one callback/context pair. `get_hard_deadline_atomic`
provides a conservative optional low-power constraint.

## Integration

Enable `ENABLE_ESY_WATCHDOG_MONITOR`, `ENABLE_ESY_WATCHDOG`, and
`ENABLE_ESY_SYSTEM` in the consumer feature import. `esy_watchdog_monitor_core`
contains the engine and has no OS header, resource or target requirement.
With `ESY_RTOS=FreeRTOS`, `esy_watchdog_monitor` contains the worker adapter and links
the shared core. Link that adapter for OS execution. With `ESY_RTOS=None`, only
`esy_watchdog_monitor_core` is assembled. Link the core and include `_core.h`
for caller-driven execution, including from an existing owner task. Feature selection does not initialize or start supervision.

For the OS path, native FreeRTOS must enable `INCLUDE_vTaskSuspend` and `INCLUDE_eTaskGetState`; unsupported parking
is rejected before hardware initialization. OS/HAL dependencies are public
abstractions; this module does not include a hardware SDK or native RTOS header.

Declare caller-owned stack, task control storage and slots with
`ESY_WATCHDOG_MONITOR_STATIC_STORAGE`, bind them with
`ESY_WATCHDOG_MONITOR_BIND_STATIC_RESOURCES`, then supply the descriptor through
`config.static_resources`. Backing bytes may be uninitialized. The descriptor
and config are copied; backing storage and an optional clock remain borrowed.
Initialize before registering tasks. Successful init starts hardware immediately:
call start promptly, before hardware expires. A failed start retains ownership;
retry start or inspect the status from deinit.

For caller-driven execution, declare slot-only storage using
`ESY_WATCHDOG_MONITOR_CORE_STATIC_STORAGE` and bind it with
`ESY_WATCHDOG_MONITOR_CORE_BIND_STATIC_RESOURCES` to
`esy_watchdog_monitor_core_resources_t`. Supply this through
`esy_watchdog_monitor_core_config_t.static_resources`, then call `core_init` and
`core_start`. The system owner owns lifecycle; each monitored participant owns
and registers its own report token. Call `core_process` from the owner loop and
use the shared report/control APIs. No stack, task control storage or OS worker
is required. Backing slots need not be zero-filled; descriptors are copied.
Stop/deinit are the shared APIs in either mode. Hardware starts during init in
both modes, so start promptly. Stop does not stop hardware or invalidate reports;
restart promptly or deinitialize, checking whether the port permits hardware stop.

For OTA, the system owner must keep supervision/feed execution available, or
explicitly coordinate affected task checks and a bounded Flash/interrupt-masking
budget smaller than the hardware timeout. This reusable module neither detects
OTA nor stops consumer tasks. Suspending the monitor also suspends feeding;
HAL Flash lockout and watchdog-clock behavior depend on the selected target.

### Usage Example

For caller-driven supervision, initialize the system/HAL first and prepare a core
configuration with persistent slots and valid timing. The system owner starts
the monitor and calls `poll_monitor()` at least once per
check period with bounded latency. Each participant owns its report token and
registers, reports after successful work, and unregisters on that same serialized
execution context. For example:

```c
#include "esy_middleware/watchdog_monitor/esy_watchdog_monitor_core.h"

esy_err_t start_monitor(const esy_watchdog_monitor_core_config_t *config) {
    esy_err_t result = esy_watchdog_monitor_core_init(config);
    if (result != ESY_OK) return result;
    result = esy_watchdog_monitor_core_start();
    if (result != ESY_OK) {
        esy_err_t cleanup = esy_watchdog_monitor_deinit();
        return cleanup != ESY_OK ? cleanup : result;
    }
    return ESY_OK;
}

esy_err_t poll_monitor(void) {
    return esy_watchdog_monitor_core_process();
}

esy_err_t start_participant(esy_watchdog_report_t *report) {
    return esy_watchdog_monitor_register("worker", report);
}

void report_participant(const esy_watchdog_report_t *report) {
    esy_watchdog_monitor_report(report);
}

void stop_participant(esy_watchdog_report_t *report) {
    esy_watchdog_monitor_unregister(report);
}

esy_err_t stop_monitor(void) {
    return esy_watchdog_monitor_deinit();
}
```

Check process and lifecycle results. After all participants stop and unregister,
the system owner calls `stop_monitor()` while excluding other public callers.
Retain slots and any borrowed clock on cleanup errors. If hardware stop returns
`ESY_ERR_NOT_SUPPORTED`, keep supervision/reporting active or use system reset;
do not reclaim storage. Hardware starts during init, before `start_monitor()`
returns, so startup work must fit its timeout.

## Contracts

All ordinary APIs are non-ISR operations. In caller-driven mode one execution
context serializes processing, report/control and lifecycle; other task/core/ISR
callers must not enter these operations. No locking or IRQ masking is needed.
The explicitly atomic deadline query remains available in an interrupt-masked
power window, excluding lifecycle. In OS mode live slot operations remain MPMC,
serialized by short cross-core OS critical sections, one fixed-size slot at a
time. No callback, hardware call, clock read or blocking operation runs inside
these sections. Slot generation validation and heartbeat mutation are committed
together. Copies of a report token are allowed; callers must not modify the same
report storage while another caller reads it. `unregister` clears its argument.
Names are copied during registration, truncated to 31 bytes plus NUL. Alert names
are copied again into the synchronous callback snapshot, so unregister/reuse
cannot invalidate them. Copy both event and string to retain an alert.

Registration starts checking immediately, without a startup grace window.
`report` sets a boolean heartbeat; an enabled check consumes it exactly once and
stores `last_alive`. `get_task_status` returns that last completed result (false
before the first check), not pending heartbeat state. Disabled slots preserve
the previous result. At most the first missed slot is reported per scan; every
miss prevents that cycle's feed. Boot-wide generations survive deinit/reinit;
exhaustion returns `ESY_ERR_NO_MEM` rather than reusing an old token.

Callback and context are published as one pair. Replacement/clear returns
`ESY_ERR_BUSY` while an invocation is active and leaves the old pair unchanged.
Only successful replacement/clear permits reclaiming the previous context.
Callbacks must be short and nonblocking. Reporting and slot control are allowed;
lifecycle calls from the monitor task, or recursively from caller-driven
processing/providers/callbacks, return `ESY_ERR_STATE`. Recursive core processing
is also rejected; reporting and slot control do not recursively run a scan.

Both executors use the HAL millisecond tick to gate supervision. Start
anchors the first check one period later. Early polling or early OS notifications
neither consume heartbeats nor feed hardware; the OS worker waits for the
remaining interval. A late call performs one scan, without replaying skipped
windows, and anchors the next period at that scan's beginning. Unsigned tick wrap
is supported; check periods and gaps must stay below 2^31 milliseconds.

One process call handles at most the configured slot count, one alert callback
and one hardware feed. Provider/clock/callback execution time remains additional
elapsed time; this is a work bound, not a wall-time or nonblocking guarantee.
Call at least once per period and bound scheduling latency. The real check gap,
including latency and provider/callback cost, must stay below the actual hardware
timeout, accounting for clock tolerance. A boolean window records reports since
the preceding scan and cannot certify liveness inside skipped nominal windows.
A stalled owner relies on hardware reset; additional catch-up scans or feeds
would not prove health.

One system owner serializes lifecycle calls and excludes other public callers
throughout init/deinit (including deadline readers). Stop requests wake the
monitor, which finishes admitted work and cooperatively parks. Stop returns
`ESY_ERR_BUSY` until the OS confirms suspended state; then deletion is synchronous
and task storage is reusable. On BUSY the owner must yield and retry with its own
bounded deadline. In caller-driven mode stop completes synchronously outside
processing and retains the current report window. Failure/timeout never
authorizes re-zeroing or reclaiming storage. Hardware keeps counting during stop; restart promptly or deinit.

Deinit first stops hardware while feed admission is closed. If hardware cannot
stop, it returns `ESY_ERR_NOT_SUPPORTED`, preserves ownership and permits existing
supervision to continue. STM32F4 IWDG has this limitation: keep feeding or reset.
After successful hardware stop, deinit polls cooperative task deletion and may
return BUSY; start is then invalid until cleanup completes. ESY_OK is the only
result that releases all borrowed storage. Repeated successful stop/deinit are
harmless; repeated init/start return `ESY_ERR_STATE`.

A deadline uses a timestamp sampled *before* init/feed plus a platform-certified
minimum timeout, including clock tolerance. Requested nominal timeouts are never
substituted. A missing bound returns `ESY_ERR_NOT_SUPPORTED` and a failed clock
read invalidates the deadline. Current Pico, STM32F4 and ESP32 ports do not certify
a lower bound in an arbitrary supplied monotonic domain, so low-power callers
must reject/defer sleep; normal watchdog supervision is unaffected. A future
certified port must account for its actual watchdog settings and clock domain.
The bounded query uses lock-free sequentially consistent atomic snapshot fields,
with at most three attempts. It invokes no callback and takes no OS lock, including
in the power manager's interrupt-masked window. Init rejects a configured clock
on a target where those atomics are not lock-free.

## Resource Model

All selected task/slot storage belongs to the caller and must remain unmoved until
successful deinit. Production code allocates no heap memory. Runtime singleton
state resides in one context. Each opaque slot uses a 64-byte,
8-byte-aligned budget. The callback event uses a
32-byte stack name snapshot. Generation state is retained for the system boot.

Stop/deinit/callback-setting return `esy_err_t`. Callers must handle BUSY or
unsupported cleanup before reclaiming resources. Core config/resources omit OS
storage. Public report and callback types are shared through `_core.h`, which
the OS header includes. Static-archive users link
`libesy_watchdog_monitor_core.a` alongside the OS adapter archive; CMake target
users receive it transitively. Caller-driven builds link the core directly.
The power manager's watchdog constraint depends only on the shared core and its
atomic query, so it does not pull in the OS adapter.

## Verification

Host tests are module-owned and use shared OS/HAL fakes. They cover poisoned
first-use storage, failure/retry, real task-entry parking, cleanup refusal,
callback reentrancy, slot interleavings, name snapshots, generation epochs,
last-check status and conservative deadline/error paths. Caller-driven tests
cover early calls, one-scan late calls, tick wrap, restart anchors, callback
recursion, mode exclusion and irreversible hardware stop. OS notification tests
verify that early wakeups do not shorten the check period. Native OS fixture tests
exercise parking/state-checked deletion. HAL backends use public contract checks and
consumer builds. HIL procedures exercise hardware enable/reset/stop behavior.

```sh
cd esy_middleware/watchdog_monitor/tests
ceedling test:all
ceedling gcov:all
cd ../../..
python3 -B esy_middleware/watchdog_monitor/tests/test_cmake_consumer.py
tools/checks/check_source_contracts.sh esy_middleware/watchdog_monitor
```

The CMake regression includes a six-thread stress probe under ThreadSanitizer,
target-only C11/C++17 consumers, feature-off source isolation, OS-free
caller-driven C/C++ link/run consumers,
core compile-interface isolation inside an OS build, and rejection of missing
watchdog/system or unsupported RTOS combinations. Repository CI
runs this regression, source contracts and the module Ceedling suite. Coverage
mode requires every production source exactly once (no fixtures), with at least
85% line and 85% branch coverage; missing or empty evidence fails the gate.
Report paths must resolve to local production sources using the platform,
module or test directory as an explicit root. Foreign namesakes and duplicate
aliases are rejected.

## Known Limits

There are no per-task deadlines, grace windows or miss counters. A completed
scan is not a simultaneous snapshot of every slot; concurrent registration is
observed in the current or next scan. An already admitted feed may finish after
`disable_hw_feed` returns. Stop/deinit are pollable, not blocking joins; owners
must schedule retries and recovery. Cross-core native parking/reclamation,
actual hardware timeouts, main-loop scheduling latency and Flash/OTA behavior
depend on the target and consumer configuration. Both execution modes use HAL
watchdog APIs without vendor headers; enabling baremetal support does not launch
a peer core or establish Flash lockout safety. An irreversible hardware init
failure can require system reset even though module init failed.
