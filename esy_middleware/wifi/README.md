# Wi-Fi Manager

## Purpose

`esy_wifi_manager` drives a station connection without creating a task. One
caller-owned manager exclusively reserves the single Wi-Fi HAL backend.
Product scheduling, retry pacing, network clients and power policy remain with
the consumer.

## Public Surface

- `esy_wifi_manager.h`: configuration, init, persistent connection requests,
  processing, stop, deinit and snapshot queries.
- `esy_wifi_manager_device.h`: optional caller-owned device-graph binding.
- `esy_common/defs/peripheral/esy_wifi_defs.h`: authentication modes, credential limits
  and IPv4 representation.

## Integration

The consumer defines `ENABLE_ESY_WIFI_MANAGER` and `ENABLE_ESY_WIFI` in its feature
import file. Link `esy_wifi_manager`; its public dependencies supply common
headers. Wi-Fi currently supports Pico W and Pico 2 W. Other ports reject the
feature during configuration.

The optional [build integration](../../docs/integration/build_integration.md),
enabled by early import, supplies a default `esy_wifi_provider` on Pico W/Pico 2 W
when `ENABLE_ESY_WIFI` is enabled. The target is created when adding the library
and links the SDK's polling CYW43/lwIP target and `esy_project_options`, plus the
selected native kernel target for FreeRTOS.
Supply `lwipopts.h` through the configured include paths, conventionally
`${ESY_SPECS_DIR}/include/specs`. No target named `specs` is required.

Explicit provider targets defined before adding the library take precedence,
including those declared after an explicit project import. Direct consumers
without the integration helpers provide the target before adding EsyMcuPlatform;
for the current polling backend:

```cmake
add_library(esy_wifi_provider INTERFACE)
target_link_libraries(esy_wifi_provider INTERFACE
    pico_cyw43_arch_lwip_poll my_network_configuration)
```

`my_network_configuration` supplies `lwipopts.h` and any selected SDK/RTOS
configuration requirements transitively. Bare-metal configurations need no
FreeRTOS dependency; RTOS consumers provide their selected native target. No
consumer directory or native port path is assumed. Use the polling CYW43/lwIP
provider compatible with the consumer's Pico SDK; threaded/background provider
variants are outside this integration contract. The SDK comes from the consumer,
not this module. Keep backend operations on an SDK-supported execution core and
service the poll loop at an appropriate interval.

### Usage Example

The consumer owns a persistent `esy_wifi_manager_t manager = {0}`. Populate
`config` with `esy_wifi_manager_default_cfg()` and valid station credentials.
The following helpers run on its single SDK-supported task/main-loop owner;
the manager is not registered in a device graph in this minimal example.

```c
#include "esy_middleware/wifi/esy_wifi_manager.h"

esy_err_t example_wifi_start(esy_wifi_manager_t *manager,
                             const esy_wifi_manager_config_t *config) {
  esy_err_t status = esy_wifi_manager_init(manager, config);
  if (status != ESY_OK) return status;
  status = esy_wifi_manager_request_connect(manager);
  if (status != ESY_OK) {
    esy_err_t cleanup = esy_wifi_manager_deinit(manager);
    return cleanup == ESY_OK ? status : cleanup;
  }
  return ESY_OK;
}

esy_err_t example_wifi_step(esy_wifi_manager_t *manager,
                            esy_wifi_ipv4_addr_t *address) {
  esy_err_t status = esy_wifi_manager_process(manager);
  if (status != ESY_OK) return status;
  return esy_wifi_manager_get_ipv4_addr(manager, address);
}

esy_err_t example_wifi_stop(esy_wifi_manager_t *manager) {
  return esy_wifi_manager_deinit(manager);
}
```

After successful start, call `example_wifi_step()` regularly. An unavailable
address returns `STATE`, including while association or DHCP is pending; inspect
`esy_wifi_manager_get_state()` and `esy_wifi_manager_get_last_error()` to decide
whether to continue or recover. Do not repeat start to retry a connection.
Before stop, quiesce network clients and all manager callers. Successful stop
releases the backend reservation; failed cleanup retains the manager and its
storage for owner recovery. To stop hardware while retaining the initialized
manager for later requests, use `esy_wifi_manager_stop()` instead.

## Contracts

### Lifecycle and ownership

Lifecycle calls (init/deinit/register/unregister) are externally serialized across
instances, with all manager and graph endpoints quiescent. A fresh manager may
contain uninitialized storage; successful init reads no previous fields. `{0}`
is recommended and permits pre-init queries. Failed init leaves storage and any
existing owner unchanged. Never query a never-initialized, nonzero object after a
failed init; retry init instead.

| Operation/state | Result and retained resources |
| --- | --- |
| First valid init | Copies credentials and reserves the backend; no hardware I/O. |
| Init current owner again | `STATE`; original configuration and binding retained. |
| Init another instance | `BUSY`; original owner retained, new object untouched. |
| Failed connection | Manager remains initialized and owns its backend resources; retry process, stop or cleanly deinit. |
| Stop | Cancels requests published before cancellation and shuts down the backend; manager and binding remain usable. |
| Deinit while bound | `BUSY`; caller must detach first. |
| Successful deinit | Stops backend, clears storage and releases reservation; init may run again. |
| Repeated deinit | `STATE`; no backend operation. |

Initialize every binding with `{0}` before first registration. Live bindings must
never be cleared, copied or rebound. Registration failure preserves its inputs;
unregister failure preserves the association. For a started graph, quiesce
network clients and all graph/manager endpoints, successfully deinit the
registry, unregister the surviving typed binding, then deinit the manager.
Before registry start, unregister directly. Registry backing arrays and bindings
must remain alive through this sequence. Failed cleanup never authorizes freeing
storage. Do not use direct HAL/backend Wi-Fi APIs while a manager owns them.

### Concurrency and power

One task owns process and stop; these calls must not overlap. Requests are MPSC
atomic intent and may run concurrently with processing and PM transitions.
Repeated requests coalesce and do not restart an in-progress timeout. Stop
cancels requests at its atomic cancellation point; a later concurrent request is
retained for the next process call. Queries are atomic snapshots from any task;
an address/state may become stale immediately after return. Lifecycle changes
must not overlap any of these endpoints. No API is ISR-safe.

A registered manager acquires a device reference before backend work. The device
core excludes PM actions while references are held. The runtime-idle callback
reads only atomic intent, so it can overlap process and requests. Suspend stops
the backend and invalidates the address but preserves intent. Resume performs no
connection I/O; the next process call reconnects, including a request arriving
while suspended. Network clients need their own product quiescence/dependency
policy before system suspend; a Wi-Fi binding alone does not represent active
TCP transactions or OTA work.

### Errors and polling

An in-progress connection is reported as process success; inspect the state.
Transient backend-start contention also keeps the state CONNECTING and retries
on a later process call without losing connection intent or marking FAILED.
Device acquisition errors are returned and recorded by `get_last_error`.
IPv4 is refreshed on every connected poll. Pending address assignment (`STATE`)
keeps process successful but leaves address unavailable and records that error.
Other address errors are returned. A subsequent successful poll clears errors.
Output address storage is unchanged when the query fails.

Initialization and backend polling can perform synchronous SDK I/O. Incremental
processing avoids waiting for a whole connection attempt; it is not a hard
real-time bound. The consumer owns poll frequency and error retry pacing.

## Resource Model

The manager stores fixed arrays of 33 SSID bytes and 65 password bytes, including
terminators; it does not borrow configuration strings after init returns. The
common named limits are the single source for these sizes. Strings supplied to
the direct HAL/backend API remain borrowed until backend deinit instead.

There is no manager heap allocation, task, timer, ISR or queue. A module context
holds only the explicit exclusive-owner reservation. Runtime data remains in
the caller-owned manager and optional binding. The Pico SDK may own hardware,
network and allocation resources; those side effects depend on the selected port
and provider configuration. Public concrete storage uses 32-bit C11 atomics and
C++ layout representations; callers access fields only through library APIs.

## Verification

From `esy_middleware/wifi/tests`:

```sh
ceedling test:all
ceedling gcov:all
python3 -B test_cmake_consumer.py
python3 -B test_coverage_gate.py
python3 -B check_coverage.py build/artifacts/gcov/gcovr/coverage.json
```

Unit tests cover initialization, exclusive ownership, binding failure/cleanup,
credential copies and bounds, request coalescing, runtime/system suspend/resume,
rollback, stop/reinit, delayed/changed IPv4 and error recording. CMake regressions
exercise real module target interfaces with a reusable fake HAL backend facade and
the real HAL forwarding source, C11/C++17 clients, feature-off isolation,
missing providers and unsupported ports. Concurrent request/query producers,
process/stop and runtime PM also run under ThreadSanitizer; memory checks run
under AddressSanitizer/UndefinedBehaviorSanitizer. These are host regressions,
not vendor-driver unit tests.

CI runs source contracts, these regressions and a coverage gate requiring the
complete manager production-source population and at least 85% line and branch
coverage per file. Report paths must resolve to local production sources using
the platform, module or test directory as an explicit root. Foreign namesakes
and duplicate aliases are rejected. See
[hardware qualification](tests/hil/wifi_hil_plan.md) for target test procedures.

## Known Limits

- One station backend and one initialized manager; no multi-radio support.
- Direct consumers provide `esy_wifi_provider`; optional build integration
  supplies a polling default on Pico W/Pico 2 W. Network configuration remains
  consumer-owned; no fixed `specs` target or RP2350 RTOS include path is injected.
- Hardware connection timing, SDK initialization-failure cleanup, power/IRQ/DMA
  shutdown and Flash/OTA coexistence depend on the selected target and SDK.
- No AP mode, scanning, IPv6, event callbacks or automatic background task.

The atomic implementation follows the [selected backend](../../docs/integration/atomic_backends.md).
RP2040 uses Pico SDK IRQ-masked hardware spinlocks; requests and snapshots remain
task-only APIs. Native ports retain their always-lock-free atomic requirement.
