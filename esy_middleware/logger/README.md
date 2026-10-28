<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# esy_logger

## Purpose

`esy_logger` formats severity-tagged lines and submits them to a UART, SEGGER
RTT, printf, or caller-provided backend. Production code uses no dynamic memory.
The frontend is process-wide and can fan out one formatted line to several
backends, including multiple independent UART instances. Printf and RTT retain
one live instance each. UART and printf use buffered engines with either
caller-driven processing or optional OS workers; RTT output is synchronous in
both modes.

## Public Surface

Public headers live under `include/esy_middleware/logger/`:

- `esy_logger.h`: configuration-based bind (`esy_logger_init`) for one or more
  backends, detach (`esy_logger_detach`), filter, format
  (`esy_log`), and `ESY_LOG_*` convenience macros.
- `esy_logger_backend.h`: paired buffer acquire/submit interface.
- `esy_logger_backend_uart_core.h` / `esy_logger_backend_printf_core.h`: caller-driven
  configuration, buffer resources, initialization and bounded processing.
- `esy_logger_backend_uart.h`: OS adapter configuration, optional DMA transfers,
  and reversible sender quiesce/resume.
- `esy_logger_backend_printf.h`: buffered stdio output with per-core pools.
- `esy_logger_backend_rtt.h`: synchronous output through a shared scratch buffer.

`src/esy_logger_backend_uart_internal.h` is private test support, not a public
integration API. Consumers must not include it.

## Integration

The consumer's `esy_feature_import.cmake` selects `ENABLE_ESY_LOGGER` and
`ENABLE_ESY_LOGGER_BACKEND_UART`, `ENABLE_ESY_LOGGER_BACKEND_PRINTF`, or
`ENABLE_ESY_LOGGER_BACKEND_RTT`. Disabled backends contribute no source files.
Link `esy_logger` for the selected runtime assembly, or link the concrete target
whose public API the consumer uses:

| Target | Responsibility and dependencies |
| --- | --- |
| `esy_logger_core` | Formatting, binding, filtering and detach only; public common types, no HAL, OS, container or RTT dependency. |
| `esy_logger_backend_uart_core` | Caller-driven UART engine; links the frontend, privately requires HAL/system, UART/DMA, common DS and utilities. |
| `esy_logger_backend_printf_core` | Caller-driven printf engine; links the frontend, privately requires HAL/system, common DS and utilities. |
| `esy_logger_backend_rtt_core` | Synchronous RTT engine; links the frontend, privately requires HAL/system and its SEGGER provider. |
| `esy_logger_backend_uart`, `esy_logger_backend_printf` | FreeRTOS adapters, each linking only its own core; OS headers propagate because their public resource types use OS task storage. |
| `esy_logger_backend_rtt` | FreeRTOS RTT adapter linking its core; its OS dependency remains private because its public header has no OS resource types. |
| `esy_logger` | INTERFACE aggregate of the frontend and every enabled backend, choosing core targets for `ESY_RTOS=None` and their OS adapters for `ESY_RTOS=FreeRTOS`. |

Enabled backends each create their own archive. Linking one concrete backend
target does not link another enabled backend, its worker adapter or its RTT
provider. Each backend core depends on the frontend because backend deinit
calls detach; the frontend has no reverse backend dependency. Private compiler
options, HAL/DS headers and provider include paths do not propagate to frontend
consumers. UART's public handle types come from common types, so its HAL header
dependency also remains private. Archive-copy consumers must collect the chosen
concrete backend targets and their link dependencies rather than assuming one
archive contains all engines or all OS adapters.

Built-in engines require `ENABLE_ESY_SYSTEM` and an `esy_hal` target.
`ESY_RTOS=None` builds their cores without OS headers, task resources or an OS target.
The OS adapters require `ESY_RTOS=FreeRTOS` and `esy_os`. UART/printf also require
`ENABLE_ESY_COMMON_DS`, `esy_common_ds` and `esy_common_utils`. UART requires both
`ENABLE_ESY_UART` and `ENABLE_ESY_DMA`, even with a short-send-only runtime
configuration. These dependency checks apply only to enabled backends; the
frontend alone needs only common types and the library's private build options.

For UART, initialize the HAL system and UART first. The caller owns the UART,
its configuration, and TX arbitration; logger init neither initializes nor
reconfigures the UART. Keep it valid until backend deinit returns `ESY_OK`.
Give the logger exclusive TX use, or provide external arbitration. Configure
caller-owned resources through `*_STATIC_STORAGE()` and
`*_BIND_STATIC_RESOURCES()`, initialize a `{0}` backend handle, then bind it with
`esy_logger_init()`. Static storage declarations may be members of an
application context. Defaults must be populated using `*_default_cfg()` and
then overridden; init does not substitute defaults for zero parameters.

RTT requires a consumer-provided SEGGER implementation: set
`ESY_SEGGER_RTT_TARGET`, provide a target named `SEGGER_RTT`, or set
`ESY_SEGGER_RTT_ROOT` containing `RTT/SEGGER_RTT.c`, `RTT/SEGGER_RTT.h`, and
`Config/SEGGER_RTT_Conf.h`. Only `esy_logger_backend_rtt_core` links the provider or compiles its source
form and includes the SEGGER header. No provider source or include path is added
to the frontend, UART or printf targets. No vendor type appears in the logger
public headers.

### Usage Example: Multiple backends and two UART outputs

Enable every required backend independently in the consumer feature import;
UART instances share the UART backend feature, not a compile-time instance
limit. Initialize a separate HAL UART and backend object for each output. Each
UART backend needs its own context storage, pool, ready ring, and, in OS mode,
task stack and task control storage. Its DMA callback and sender worker refer
only to that instance. Distinct instances must use distinct UART TX resources
and non-overlapping backing storage. The caller is responsible for physical
resource arbitration; the logger does not discover aliases between HAL handles.

Declare a separate `ESY_LOGGER_BACKEND_UART_STATIC_STORAGE()` object per OS UART,
or `ESY_LOGGER_BACKEND_UART_CORE_STATIC_STORAGE()` per caller-driven UART, and
use the matching `*_BIND_STATIC_RESOURCES()` macro. Zero-initialize the storage
object before first use, in addition to the backend handle. The macros include
the opaque context storage needed by each instance. Manually assembled resource
descriptors must also supply `context_storage`. Pool bytes need not be cleared.

After both backend initializations succeed, the application can bind them as
follows. Here `outputs` is a caller-owned, zero-initialized object that remains
alive for the complete binding lifetime; `uart_a` and `uart_b` are pointers to
the initialized backend objects.

```c
#include "esy_middleware/logger/esy_logger.h"

typedef struct {
  esy_logger_backend_binding_t bindings[2];
  esy_logger_static_resources_t resources;
} consumer_logger_outputs_t;

static esy_err_t bind_outputs(consumer_logger_outputs_t *outputs,
                              esy_logger_backend_t *uart_a,
                              esy_logger_backend_t *uart_b) {
  if (outputs == NULL || uart_a == NULL || uart_b == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  outputs->bindings[0] = (esy_logger_backend_binding_t){
      .backend = uart_a, .filter_level = ESY_LOG_LEVEL_INFO};
  outputs->bindings[1] = (esy_logger_backend_binding_t){
      .backend = uart_b, .filter_level = ESY_LOG_LEVEL_DEBUG};
  outputs->resources = (esy_logger_static_resources_t){
      .bindings = outputs->bindings, .binding_count = 2u};
  const esy_logger_config_t config = {
      .static_resources = &outputs->resources,
      .filter_level = ESY_LOG_LEVEL_DEBUG};
  esy_err_t status = esy_logger_init(&config);
  if (status == ESY_OK) {
    ESY_LOG_I("app", "Logger ready (%u outputs)", 2u);
  }
  return status;
}
```

Call this startup helper only while its table is unbound. Configuration and
resource descriptors are copied; the mutable bindings table remains borrowed
and must not be accessed directly while bound. Logging still uses `ESY_LOG_*`:
INFO reaches both UARTs, while DEBUG reaches only the second. Add RTT or a
custom backend by adding another binding and supplying its own resources.
Initialization failures leave the previous frontend binding unchanged. The
lifecycle owner cleans up any newly initialized backends, retaining resources
and retrying if cleanup reports BUSY.

### Caller-driven integration

Link `esy_logger_backend_uart_core` or `esy_logger_backend_printf_core`, include
the matching `_core.h` header, and initialize
with `*_core_default_cfg()` / `*_core_init()`. Supply only pool and ready-ring
storage through `*_core_resources_t`; no stack or task control object is needed.
Resource descriptors are copied during init, while all backing buffers and the
backend object remain borrowed. Printf's caller-driven mode needs one pool,
regardless of the platform core count. Bind the chosen backend through the same
`esy_logger_init()` frontend API. Regularly call `*_core_process(backend, budget)`
with a positive block budget from the owner loop. RTT uses
`esy_logger_backend_rtt_core_init()` and synchronous submission, with no process
step; link `esy_logger_backend_rtt_core` and include `esy_logger_backend_rtt.h`.
`esy_logger_backend_rtt_init()` belongs to the FreeRTOS adapter; caller-driven
RTT uses `core_init` in every build configuration.

The caller-driven owner pins all logging, lifecycle, processing and raw callbacks
to one core. UART/printf permit raw ISR acquire/submit on that core, and UART DMA
completion must be delivered there. Local IRQ masking protects pool and lease
updates; it is not a cross-core lock. Other cores must not access these instances.
RTT raw callbacks remain non-ISR operations. FreeRTOS init uses
multi-producer OS critical sections, task creation and notifications, with
cooperative parking before deletion.
Do not mix core init/process with OS init on one live backend; core processing
rejects worker-owned instances. Successful deinit permits either mode on reuse.

Each core process attempts at most its block budget. Blocking UART short sends,
stdio writes/flushes and provider callbacks still determine elapsed time. UART
retains short-write tails and retries BUSY/STATE no sooner than 10 ms using the
HAL tick, including unsigned wrap; keep gaps below half the tick range.
Queues can fill if polling is too infrequent. Stdio and asynchronous UART output
remain best-effort. RTT must use a nonblocking provider mode for caller-driven
integration. The same detach, borrowed-buffer and DMA leases protect shutdown:
BUSY retains storage, pending queued output is discarded only on successful
cleanup, and timeout never permits force-clearing a live instance.

UART processing polls active DMA retirement before attempting another block.
The OS worker retries active DMA at 10 ms intervals rather than waiting solely
for a completion notification. A pending shutdown continues retirement and
starts no queued output. Caller-driven cleanup failures return their UART error
and retain storage; continue processing after deinit returns BUSY, then retry
deinit. A failed submission that retains a HAL borrow is retired through polling
or a later successful abort before the best-effort log block is released.
Provider and callback duration remain part of each processing step.

## Contracts

### Frontend and concurrency

Formatting and lifecycle operations run outside ISR context in the owning loop
or an ordinary OS task. Do not log in
ISRs. A C11 atomic transaction guard protects the binding across an entire line,
including every acquire/submit pair. It does not mask interrupts or wait for
another producer: concurrent/reentrant calls drop their whole line if the guard
is occupied. Filter updates are atomic. A bind/detach attempt returns
`ESY_ERR_BUSY` while a frontend transaction is active; retry under an
application-defined deadline. Successful initialization replaces the complete
binding and sets its configured global filter. Single-output and multi-output
consumers use the same configuration interface and binding table.

The multi-backend table is caller-owned mutable storage. A NULL backend marks
an inactive slot; initialization rejects an empty table, an entirely inactive
table, duplicate backend objects, invalid filters and incomplete callbacks.
Reinitialization with a table overlapping the currently borrowed table is
rejected; detach every member or replace the binding before reusing that table.
The global and per-backend filters both apply. A line is formatted once and
then submitted to each eligible backend in table order. A backend failure stops
only that backend's remaining chunks; later backends are still attempted.
There is no cross-backend delivery transaction or guaranteed delivery.

Fan-out remains synchronous at the frontend callback boundary: a blocking RTT
provider delays the complete logging transaction and increases contention
drops, even when UART delivery uses independent workers. Select a nonblocking
provider mode when bounded logging latency is required; fan-out alone does not
isolate provider latency.

The lifecycle owner must externally serialize bind, detach, backend init/deinit,
and UART quiesce/resume. OS-mode task logging may run concurrently. A bound object and
its context must stay alive until successful detach or deinit. Detach removes
only the requested backend and leaves all other bindings active; detaching an
absent backend is a successful no-op. The final detach releases the frontend's
reference to the table. A successful detach prevents new frontend submissions,
but backend queues, DMA and physical output still need their own shutdown.
Custom backends must also detach successfully before destroying their context.

The frontend formats into a 1024-byte stack buffer, including the terminator,
and appends a newline. Invalid/filter-excluded levels or an absent backend are
silently dropped. Formatting truncation keeps the terminating newline; backend
exhaustion/failure can drop a suffix, including that newline. These are
best-effort diagnostics, not a reliable record transport.

### Buffer ownership

Call the function pointers with the **backend object's address** as `self`,
not `backend.ctx`. Acquire returns a writable buffer and capacity. Write at most
that capacity, then submit once; length zero cancels the acquired block.
Submission of a valid acquired buffer returns ownership even when it reports an
error. Do not access it afterward. A foreign pointer or double submission is
rejected without consuming another producer's lease.

In OS mode, UART and printf raw buffer callbacks support task/ISR producers
through short OS critical sections; this does not permit ISR formatting or logging. RTT calls
are task-only because the SEGGER provider may block. Direct producers must not
start new calls once shutdown begins, but must return already borrowed buffers.
Do not mix direct submissions with frontend logging on one backend without
external serialization: direct writes are not part of a frontend line guard.

### Initialization and shutdown

Every concrete backend handle must be initialized with `{0}` before first use.
UART opaque context storage must also be zero-initialized before first use;
init reads its documented ownership state to reject live storage reuse. Each
UART instance owns separate caller-provided storage and can be stopped or
reinitialized independently.
Do not copy a live handle. Repeated init returns `ESY_ERR_STATE` without changing
it. Configuration or creation failure leaves an inactive handle unchanged and
safe to deinit/retry. Backing payload buffers need not be zero-filled. Printf
and RTT each permit one live instance; UART instance count is bounded by the
caller's memory, transport and optional OS task resources.

All three `*_deinit()` functions return `esy_err_t`:

- `ESY_OK`: the frontend is detached, resources are released, and the handle is
  zeroed. Repeated deinit is a successful no-op; reinitialization is allowed.
- `ESY_ERR_BUSY`: keep the handle, UART/provider, and all storage alive, and retry
  later. A frontend transaction leaves the backend running. Otherwise the
  backend enters stopping state, rejects new acquisitions, and waits for borrowed
  buffers, output calls, callbacks, and the OS worker to park. Returning borrowed buffers in
  this state releases them and reports `ESY_ERR_STATE`.
- `ESY_ERR_INVALID_ARG`: NULL/foreign handle or an ISR lifecycle call.

Completion of I/O does **not** automatically finish shutdown. Only a subsequent
successful deinit permits releasing/reusing storage. The owner must use a bounded
retry policy and handle timeout without re-zeroing an active handle. Shutdown
can remain BUSY if a producer never returns a buffer or a transport never
completes. Queued but unstarted logs are discarded at successful shutdown.

UART/printf OS adapters require cooperative task parking. Initialization returns
`ESY_ERR_NOT_SUPPORTED` without creating a task if the port lacks it. With the
scheduler running, even an idle worker may initially yield BUSY until it parks;
no task or context is deleted before that acknowledgement. Before scheduler
startup workers cannot run, so deletion needs no acknowledgement. A worker may
not deinitialize itself (`ESY_ERR_STATE`).

Handle BUSY before reusing resources; ignoring the result is unsafe for
resource reclamation.

### Backend output behavior

UART reserves a block until blocking send or DMA completion/error finishes.
Both success and error callbacks release the active transfer and notify in the
actual task/ISR context. DMA BUSY/STATE leaves the descriptor queued and the
sender delays 10 ms before retrying, without requiring another log notification.
Short blocking sends also retry BUSY/STATE, zero writes, or a partial suffix.
Other send errors discard the block; asynchronous errors are not reported to
an already-completed submit call. There is no implicit DMA-to-blocking fallback;
configure the short threshold appropriately for a port without DMA support.

`quiesce()` succeeds only without active I/O/callbacks or physical UART TX and
then prevents new sends. DMA source completion can return the log block while
bytes remain in a FIFO or shift register. Quiesce and successful deinit require
that physical output to drain too, without retaining an already retired source
block. The busy query runs outside critical sections under a sender admission
gate; it never waits for drain or forces an abort. BUSY leaves quiesce running;
retry before treating TX as quiet. Exclusive TX use or external arbitration
must remain in effect until resume or successful deinit. Producers may
still queue blocks while quiesced. `resume()` restarts sending and notifies the
task. Repeated quiesce/resume in the same state returns `ESY_ERR_STATE`.

Printf drains descriptors in global submission order across all per-core rings,
including sequence wrap and producer migration. Thus frontend chunks preserve
line order. `fwrite` and `fflush` run outside critical sections; output failures
remain best-effort and are not reported to the original producer.

RTT holds its scratch-buffer lease until `SEGGER_RTT_Write()` returns. A competing
acquire returns `ESY_ERR_BUSY`; a short provider write returns `ESY_ERR_IO`.
Provider configuration determines blocking and drop behavior.

### OTA execution policy

Fan-out adds no dispatcher task. Each OS-mode UART instance creates one sender
worker; two UART outputs therefore require two independently owned workers.
Printf also has its optional worker; RTT and caller-driven UART create none.
The consumer owns OTA admission and must explicitly decide for every worker
whether diagnostics are required and safe during OTA, considering Flash timing,
ISR/DMA, shared transports and watchdog effects. For quiescence, use
detach/quiesce/deinit leases and wait for in-flight
I/O and borrowed buffers before releasing storage. BUSY or an application deadline retains resources and must block any transition that depends on
successful quiescence. UART resume is available after reversible quiesce; after successful
deinit, initialize and bind the backend again. ISR/DMA,
output, shared transport and watchdog effects are consumer-specific; these
modules create no watchdog registrations. Caller-driven baremetal validation
has OTA disabled and introduces no Flash mutation or OTA stop orchestration.

## Resource Model

| Backend | Pool bytes | Index/ready entries | Limits |
| --- | --- | --- | --- |
| UART | `pool_block_count * block_size` | `pool_block_count` each, shared across cores | Block 1–128 bytes; power-of-two pool depth 2–32 |
| printf | `core_count * pool_block_count * block_size` | `core_count * pool_block_count` each | Block 1–128 bytes; power-of-two pool depth 2–8 per core |
| RTT | One scratch buffer | None | 1–256 bytes |

UART and printf require at least two pool blocks because their ready rings
reserve one slot to distinguish full from empty. A depth of one is rejected
before touching caller storage or creating a worker, in both execution modes.

Printf uses the runtime core count (zero is treated as one), which must not
exceed `ESY_MAX_CORE_COUNT`; allocate for the largest supported runtime count.
Each UART instance's pool storage is shared across cores; its macro accepts the
pool depth, block size and sender stack size without multiplying capacities by
core count. Each also reserves one
`esy_logger_backend_uart_context_storage_t`; budget its `sizeof` and alignment
alongside the buffers, and never share it between live instances.

UART/printf each require one caller-owned `esy_os_task_static_storage_t` and a
stack buffer of 1–2048 bytes aligned to `ESY_OS_TASK_STACK_ALIGN`; the selected
OS port may impose a larger minimum. Pool buffers, index stacks, typed ready
slots, task storage, and stack must remain valid until deinit returns `ESY_OK`.
Resource descriptors themselves are read during init. No runtime heap is used.
Budget the frontend's stack buffer in every producer task, in addition to the
backend task stack and libc formatting/output stack requirements.

## Verification

Pool-depth regressions cover rejection of one without changing the index
storage, acceptance of the minimum depth two, repeated-init state precedence,
and reuse after successful deinit for both buffered backends.

Core tests cover work budgets, timed short-write retries, borrowed-buffer stop
failure and DMA retention. Core and OS UART regressions separate successful and
failed DMA source retirement from physical drain, verify immediate pool reuse,
BUSY shutdown/retry and sender wakeup after a rejected quiesce. Blocking source
return with a busy wire and critical-section exclusion are checked separately.
C/C++ consumer tests build each backend with no OS
target or OS include path as well as the existing FreeRTOS combinations.
Dual-UART tests cover simultaneous DMA, per-output filtering, independent pool
leases, stopping one output while the other remains bound, and reuse after
shutdown or creation failure. Frontend tests cover fan-out, partial failures,
atomic replacement, duplicate/invalid bindings, table lifetime, and reentrancy.
The C/C++ matrix also initializes, logs through and independently stops two
caller-driven UART instances without any OS target.

From the EsyMcuPlatform repository root:

```sh
(cd esy_middleware/logger/tests && ceedling test:all)
(cd esy_middleware/logger/tests && ceedling gcov:all)
python3 -B esy_middleware/logger/tests/test_cmake_consumer.py
./tools/checks/check_source_contracts.sh esy_middleware/logger esy_test
```

Unit tests execute the sender tasks with reusable port fakes, including backoff,
DMA completion/error, synchronous callbacks, output/notification shutdown
interleavings, borrowed-buffer shutdown, initialization failure/retry, frontend
reentrancy, and multicore FIFO ordering. Host stdio capture uses glibc
`fopencookie`; it does not change production I/O. The CMake matrix builds/links
C and C++ consumers for all eight backend combinations in caller-driven and OS
modes, three provider forms and missing-dependency diagnostics. A frontend-only
consumer formats, filters and submits through an injected backend without any
HAL, OS, DS or SEGGER target. With all three backends enabled, separate builds
link only the frontend or one concrete backend and inspect executable symbols,
archive links and compile-header paths for unrelated dependencies. It uses host fakes,
not a vendor SDK or actual FreeRTOS scheduling. Both source checks and the
CMake matrix are registered in `tools/quality/ci_quality_gate.sh`.

Run the real consumer firmware build with its selected toolchain and features.
On the target, test UART DMA completion/error and shutdown, SMP task/ISR
scheduling, and the selected RTT provider's behavior. At a slow
UART rate, capture the final stop bit after a source-complete callback: quiesce
and deinit must remain BUSY until the HAL reports physical drain. Repeat after
an error and with the OS worker parked; a fake busy flag is not wire evidence.

## Known Limits

- The global frontend and its nonblocking contention drops are intentional.
  Printf and RTT remain single-instance. A line whose backend runs out of
  buffers may be truncated even without contention.
- Backend shutdown discards queued logs and has no forced abort or internal deadline.
  A missing DMA callback or abandoned buffer requires application recovery.
- UART retry cadence is fixed at 10 ms; permanent BUSY/STATE cannot be repaired
  by the logger. Port DMA failure must obey the public completion contract.
- Printf is intended for host/simulation output, with libc-dependent latency.
- RTT still directly depends on the SEGGER provider in its implementation.
- Host tests model ordered interleavings; they do not prove hardware SMP/ISR
  timing, vendor DMA behavior, or target stack margins.
