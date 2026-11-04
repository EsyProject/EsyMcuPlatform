<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# I/O schedulers

## Purpose

Provide independently linked SPI, UART TX, generic DMA and PWM DMA schedulers. These
components do not interpret protocol frames or own protocol transactions.
Protocol-specific channels live in [protocol](../protocol/README.md) and may
borrow a scheduler through its public API. The application owns physical
handles, feature selection, execution contexts, startup, shutdown and OTA policy.
Linking a target does not initialize hardware or create a worker.

SPI queues complete bus transactions, including prepared command/payload
transactions used by LCD controllers. UART schedules DMA TX from an existing
owner context; RX remains the HAL byte stream and can run independently.
ProtoBin UART and Modbus UART reuse the same TX implementation in independent
channel-owned instances. Sharing the implementation does not multiplex protocols
or permit multiple owners of the same physical UART.

The generic DMA core serializes transfer descriptors on a borrowed DMA controller.
The PWM core serializes waveform DMA requests on a borrowed PWM handle. Both are
caller-driven and expose processing, queueing, cancellation and relative wait hints without
creating tasks. Direct HAL use remains available when queueing is unnecessary.

## Public Surface

Public headers are flat under `include/esy_middleware/io_scheduler/`:

- `esy_spi_scheduler_core.h`: caller-driven creation and processing, request
  descriptors, buffer reservations, prepared transactions, quiesce and destruction.
- `esy_spi_scheduler.h`: optional FreeRTOS worker configuration and creation.
- `esy_uart_tx_scheduler.h`: owner-driven initialization, queued TX, processing,
  stop and deinitialization.
- `esy_dma_scheduler_core.h`: owner-driven descriptor queue and controller retirement.
- `esy_pwm_scheduler_core.h`: owner-driven waveform DMA queue and output retirement.

The scheduler name describes ordered transfer execution. Only the explicit SPI
OS adapter create operation creates a task. Serialization is per instance;
producers still obey the single-producer or serialized-owner contracts below.

## Integration

Select modules only in the consumer's `esy_feature_import.cmake`:

```cmake
set(ENABLE_ESY_IO_SCHEDULER ON)
set(ENABLE_ESY_SPI_SCHEDULER ON)
set(ENABLE_ESY_SPI ON)
set(ENABLE_ESY_SYSTEM ON)
set(ENABLE_ESY_COMMON_DS ON)
```

| Target | Consumer selection | Dependencies |
| --- | --- | --- |
| `esy_spi_scheduler_core` | `ENABLE_ESY_SPI_SCHEDULER` | SPI, system and common data structures; no OS |
| `esy_spi_scheduler` | Same gate and `ESY_RTOS=FreeRTOS` | SPI core and OS worker support |
| `esy_uart_tx_scheduler` | `ENABLE_ESY_UART_TX_SCHEDULER` | UART and common data structures; no OS/system requirement |
| `esy_dma_scheduler_core` | `ENABLE_ESY_DMA_SCHEDULER` | Generic DMA and common data structures; no OS/system requirement |
| `esy_pwm_scheduler_core` | `ENABLE_ESY_PWM_SCHEDULER` | PWM and common data structures; no OS/system/generic DMA requirement |

HAL support alone assembles no scheduler. An empty selection produces no targets.
A protocol consumer explicitly selects its scheduler dependency; this module
never selects or links a protocol family. With `ESY_RTOS=None`, link the SPI core
or any other core target directly. No initialization or worker creation occurs at link.
SPI/UART/PWM native DMA does not require the generic `esy_hal_dma_*` controller
facade or its `ENABLE_ESY_DMA` feature. Only the generic DMA core requires it.

LCD and touch drivers accept generic SPI transaction callbacks instead of linking
the worker adapter. Their owner injects one provider for the shared bus and
chooses caller-driven core processing or an OS worker. Synchronous initialization
and register access may use HAL directly under exclusive, idle bus ownership.

### Usage Example

For UART TX, select `ENABLE_ESY_UART_TX_SCHEDULER` and its dependencies, then link
`esy_uart_tx_scheduler`. The owner has already initialized the UART and scheduler
with persistent handle/request storage. Submit immutable bytes as follows:

```c
#include "esy_middleware/io_scheduler/esy_uart_tx_scheduler.h"

esy_err_t queue_ready(esy_uart_tx_scheduler_t *scheduler,
                      esy_uart_tx_scheduler_tx_callback_t completed,
                      void *context) {
    static const uint8_t k_message[] = "ready\r\n";
    const esy_uart_tx_scheduler_tx_request_t request = {
        .data = k_message,
        .length = sizeof(k_message) - 1u,
        .on_complete = completed,
        .user_data = context,
    };
    return esy_uart_tx_scheduler_submit(scheduler, &request);
}
```

The descriptor is copied; supply a non-NULL completion callback and keep its
context alive through callback return. Check submission status: an error means
no request was accepted. The same serialized owner repeatedly calls
`esy_uart_tx_scheduler_process()` and checks its result; a processing error can
retain an accepted request, so retry cleanup rather than resubmitting it. Stop
producers and drain/cancel through processing before successful deinit releases
the persistent resources. No worker is created by this example.

### Current assembly

Public includes use `esy_middleware/io_scheduler/`. Select
`ENABLE_ESY_IO_SCHEDULER` and the required component features,
`ENABLE_ESY_SPI_SCHEDULER`, `ENABLE_ESY_UART_TX_SCHEDULER`,
`ENABLE_ESY_DMA_SCHEDULER` or `ENABLE_ESY_PWM_SCHEDULER`, in the consumer's
feature file. Link the concrete targets listed above; there is no aggregate
`esy_io_scheduler` target. SPI APIs, types and storage macros use
`esy_spi_scheduler`; UART TX uses `esy_uart_tx_scheduler`.

The [protocol module](../protocol/README.md) composes these schedulers into
caller-driven channels and optional OS workers.

### SPI execution and submission

Create caller-driven instances with zeroed `esy_spi_scheduler_storage_t` and
`esy_spi_scheduler_core_config_t::static_resources`. Supply request slots and
optional TX/RX byte storage. Link `esy_spi_scheduler_core` and call
`esy_spi_scheduler_core_process()` from the same serialized, non-ISR owner.
Worker instances instead use `esy_spi_scheduler_create()` with persistent task
stack/control storage and `esy_spi_scheduler`.

`esy_spi_scheduler_get_write_block()` reserves a descriptor until submit and
exposes any configured TX/RX payload buffers. TX-only and RX-only requests use
this descriptor API with the unused direction set to NULL. Fill the selected
buffers, set the request length and submit through `esy_spi_scheduler_submit()`.
A second reservation returns STATE or NULL as specified by the entry point.
Rejected submission preserves the caller's reservation for retry; a convenience
submission that reserved internally releases its reservation on rejection.

Full-duplex clients that write a fixed command size before submit should use
`esy_spi_scheduler_get_read_write_buffer_with_arg()`. It takes a nonzero minimum
size and checks both per-request allocations before reserving a slot. An
undersized TX or RX allocation returns `NO_MEM`, clears the output pointers and
leaves storage and existing reservations unchanged. This is the sized callback
adapter used by XPT2046. It is the public buffer-reservation callback for
full-duplex clients; single-direction clients use the descriptor API.

An ordinary convenience submission requires a terminal callback. A descriptor
may omit its callback when no completion notification is needed. Every accepted
request still has to finish before quiesce or destroy succeeds. All queues have
one serialized producer and one execution owner; callback submissions share the
producer contract. Ring capacity is `block_count - 1`, with a power-of-two block
count of at least two. DMA callbacks only publish completion state.

Each core processing call observes automatic HAL completion, retires at most one prior
request, then starts at most one next request, which may complete synchronously.
Call regularly enough to meet transfer retirement and queue latency requirements.
Provider calls, including hardware cleanup/abort, blocking fallback, request
length and callbacks determine elapsed time;
the request budget is not a time deadline. Process returns OK for a valid step,
including idle/quiesced steps; transfer errors go to terminal callbacks/results.
Process rejects recursive calls and OS-owned instances. The existing OS worker
processes active transfers at one-millisecond intervals, then sleeps while idle.
Internal notifications wake it for submissions, startup and control; callers do
not configure a second DMA callback. Caller-driven owners use
`esy_spi_scheduler_next_wait_ms()` to pace the same retirement path. Direct HAL
users receive automatic native DMA callbacks without a HAL progress call.
See [asynchronous I/O events](../../esy_hal/docs/async_io_events.md).

### Prepared SPI transactions

`esy_spi_scheduler_submit_prepared_with_arg()` implements
`esy_spi_submit_prepared_t`: one borrowed TX payload, a synchronous prepare
callback and exactly one terminal result callback. Prepare runs under exclusive
bus ownership and can send bounded window/setup commands. Its failure skips
payload I/O. Completion runs after payload I/O stops while ownership is retained
for CS cleanup. Completion may enqueue another request. Neither callback may
process, quiesce or destroy its provider; prepare must not enqueue onto it.

The payload and callback context remain borrowed until completion starts.
Ordinary transfer buffers/context remain borrowed through callback return.
Providers may complete inline before submission returns. A rejected submission
retains nothing and invokes no callbacks. The ring commit is acceptance; a later
worker notification error cannot revoke an accepted request.

### UART TX execution

Use `init/submit/process/request_stop/deinit` from one serialized non-ISR owner.
Submission copies descriptors; immutable data and callback contexts remain
borrowed through the exactly-once terminal callback. Rejection retains nothing.
Process retires at most one request and starts at most one transfer, calling
UART completion state once. Synchronous fallback/provider calls and callbacks add to elapsed time.
Callbacks may enqueue but cannot recursively process or perform lifecycle work.

Use `esy_uart_tx_scheduler_next_wait_ms()` after each processing turn: zero
means queued work ready to start, 1 ms requests active DMA/wire/cleanup processing,
and `UINT32_MAX` means idle. The caller arranges submission scheduling. No
peripheral worker or wake callback is created.

Success requires DMA completion and physical wire idle. Cleanup errors retain
resources for owner retry. A rejected hardware submission that remains busy
retains its slot and buffer until terminal retirement or successful abort,
then produces one error completion without resubmission. Stop either drains
queued requests or cancels unsent requests with CANCELLED completion. Accepted
in-flight DMA drains. Deinit returns BUSY until requests, callbacks and physical
TX retire. Initialization and deinitialization leave RX unchanged.

`request_abort()` upgrades shutdown to cancel queued and active TX. The request
only closes admission and records intent; it performs no I/O or callback. Each
following process slice makes at most one abort attempt and one terminal callback.
Abort failure or a still-busy wire retains all borrowed storage for retry. A
terminal transfer already observed idle keeps its original success/error result;
otherwise safely retired active requests complete as CANCELLED. Failed retained
submissions always report their original error. Shutdown cannot be downgraded.
Bytes already transmitted cannot be withdrawn. The owner bounds cleanup retries
and keeps resources alive until deinit succeeds.

### Generic DMA and PWM DMA execution

Zero-initialize `esy_dma_scheduler_core_t` or `esy_pwm_scheduler_core_t`, then
initialize it with a previously initialized, quiescent HAL handle and persistent
request slots. Initialization borrows the handle; successful deinitialization
releases the borrow without initializing or destroying hardware. Exclude other
users of that controller/output throughout this lifetime. Each FIFO uses a
power-of-two slot count of at least two, with usable capacity `count - 1`;
the active request occupies one slot.

Use `submit/process/request_stop/request_abort/is_idle/deinit` on one serialized,
non-ISR owner. Submit copies a request and borrows its transfer buffers and
callback arguments. Rejection retains nothing. Process performs a bounded
request step, not an entire queue drain. Hardware providers and application
callbacks determine elapsed time; these cores promise no wall-clock deadline.
Application callbacks may not reenter these cores. Wake callbacks only signal
readiness and may run from IRQ; they never deliver the terminal outcome.

The DMA core accepts single-route descriptors, retaining the public HAL's width,
direction and circular-transfer contracts. It does not infer a batch or graph
schedule. Original descriptor half/completion/error callbacks run from owner
processing. Half events and circular completions retain the request. The optional
`done` callback receives one terminal result after retirement, including prepare
or start errors and cancellation. A successful prepare followed by failed start
retains the route even if hardware is not busy: a later processing step explicitly
aborts it before returning buffers. Abort failure retains the slot for retry.

The PWM core accepts finite or circular waveforms through its own terminal and
optional coalesced progress callbacks. The embedded DMA descriptor's callback
and notification fields must be empty because the core owns those bindings.
Finite completion retires DMA and disables output before terminal delivery or
starting the next waveform. A disable/abort failure retains the active request
for cleanup retry. DMA source completion does not guarantee that the final PWM
sample has occupied a full output period. Frequency, polarity and DMA mapping
remain the borrowed HAL's configuration; the core does not retune them.

`request_stop(false)` closes admission and drains queued work. With `true`, it
cancels queued work after the active request retires. An active finite transfer
continues to completion; an active circular transfer requires `request_abort()`.
Abort closes admission and requests cancellation of active and queued work.
Control calls perform no hardware cleanup or terminal callbacks: keep calling
process until idle, then deinitialize. Cancellation cannot undo physical effects.
Never clear storage or release a waveform after a failed cleanup/deinit.

Both cores expose `next_wait_ms()`: `0` requests a turn for queued work, `1`
requests active transfer or cleanup processing, and `UINT32_MAX` means idle.
Requery after each process turn and keep processing finite and circular work
until retirement. These are relative hints, with no internal clock/deadline or
timeout-driven cancellation. Callbacks deliver completion/progress from owner
processing; there is no separate DMA wake callback.

For example, this finite PWM helper borrows an already initialized, idle PWM
handle. Select `ENABLE_ESY_IO_SCHEDULER`, `ENABLE_ESY_PWM_SCHEDULER`,
`ENABLE_ESY_PWM` and `ENABLE_ESY_COMMON_DS`, then link `esy_pwm_scheduler_core`.
The caller owns zeroed core storage, slots and samples. It supplies a terminal
callback, checks each returned status and repeatedly invokes `pwm_step()` from
the same owner. On shutdown, call `request_stop` or `request_abort`, keep stepping
until idle, then call deinit before releasing storage. No task is created.

```c
#include "esy_middleware/io_scheduler/esy_pwm_scheduler_core.h"

esy_err_t pwm_begin(esy_pwm_scheduler_core_t *core, esy_pwm_t *pwm,
                    esy_pwm_scheduler_request_t slots[2],
                    const uint16_t *samples, uint32_t sample_count,
                    esy_pwm_scheduler_complete_cb_t completed, void *arg) {
    if (sample_count == 0u || sample_count > UINT32_MAX / sizeof(*samples))
        return ESY_ERR_INVALID_ARG;
    const esy_pwm_scheduler_core_config_t config = {
        .static_resources = {.requests = slots, .request_count = 2u},
    };
    esy_err_t result = esy_pwm_scheduler_core_init(core, pwm, &config);
    if (result != ESY_OK) return result;
    const esy_pwm_scheduler_request_t request = {
        .transfer = {.src = samples, .length = sample_count * sizeof(*samples),
                     .transfer_size = ESY_DMA_TRANSFER_SIZE_16},
        .on_complete = completed,
        .user_data = arg,
    };
    result = esy_pwm_scheduler_core_submit(core, &request);
    if (result != ESY_OK) (void)esy_pwm_scheduler_core_deinit(core);
    return result;
}

esy_err_t pwm_step(esy_pwm_scheduler_core_t *core) {
    return esy_pwm_scheduler_core_process(core);
}

esy_err_t pwm_close_if_idle(esy_pwm_scheduler_core_t *core) {
    esy_err_t result = esy_pwm_scheduler_core_request_stop(core, true);
    if (result != ESY_OK) return result;
    if (!esy_pwm_scheduler_core_is_idle(core)) return ESY_ERR_BUSY;
    return esy_pwm_scheduler_core_deinit(core);
}
```

## Contracts

Zero-initialize opaque handles before first creation. Backing buffers and OS
storage need no additional clearing. Repeated create on owned storage returns
STATE. Successful destroy allows recreation. Failed cleanup retains resources;
retry rather than clearing live storage. Create requires idle physical I/O;
worker adapters additionally require cooperative task parking support. Serialize
all lifecycle operations with producers, processing and callbacks. No public
operation is ISR-callable. All application callbacks execute in the owner.

SPI accepts aligned whole-unit DMA requests with lengths fitting INT32_MAX and
any selected allocated buffer. An idle DMA rejection completes once; only an
idle unsupported rejection falls back to matching blocking read/write/full-duplex
byte I/O. A failed submit that stays busy retains the request slot, buffers,
prepared transaction and callbacks. Subsequent process calls process terminal
retirement, then make at most one abort attempt if no terminal callback arrived.
Abort failure retains everything for another process call; success cancels the
provider callback and completes the request once with the submission error.
No blocking fallback is attempted while that borrow remains active.
Unsupported wider transfers report NOT_SUPPORTED. Short transfers report
IO and the known completed count; unknown DMA counts are zero. A failed transfer
may have partial physical effects.

Stop SPI producers and drain every accepted request before quiesce/destroy.
BUSY leaves admission and processing active so the owner can drain, including
callbacks and physical wire activity. Successful quiesce blocks submissions and
new starts; resume permits work and wakes the worker. A failed quiesce restores
worker notification. Once shutdown is accepted, new work is rejected. Keep all
resources alive until the worker has parked and destruction succeeds. Normally
accepted DMA waits for its terminal notification and physical idle. A retained
failed submission may instead retire through successful task abort, whose HAL
contract cancels all pending callback access. Quiesce/destroy remain BUSY until
the scheduler has delivered the one terminal completion; continue processing.

## Resource Model

Queues, payload buffers, opaque storage and optional SPI worker stack/control
objects are caller-owned, persistent and nonoverlapping. SPI storage follows
block-ring alignment. UART TX, SPI, generic DMA and PWM DMA cores create no
tasks. No runtime heap allocation is used.

The consumer determines the SPI worker's OTA policy. Keep it running only with
application Flash/concurrency guarantees, or stop producers and confirm bounded
quiesce/drain before operations requiring silence. Timeout retains ownership and
fails or defers that transition. Resume after rollback or restore handles before
recreating a destroyed scheduler. IRQ/DMA must be idle before handle release.
For DMA/PWM owners, stop producers and retain processing until drain or explicit
abort completes before Flash operations that require silence. Circular requests
cannot naturally drain. The application bounds cleanup retries and decides
whether to defer OTA, recover or reset after a timeout; a timeout never releases
live request storage. No application task or OTA policy is installed by a core.

## Verification

From the platform root:

```sh
python3 -B esy_middleware/io_scheduler/tests/test_cmake_consumer.py
ruby tools/quality/run_module_tests.rb esy_middleware/io_scheduler/tests /tmp/esy-io-scheduler-tests test:all
ruby tools/quality/run_module_tests.rb esy_middleware/io_scheduler/tests /tmp/esy-io-scheduler-gcov gcov:all
python3 -B esy_middleware/io_scheduler/tests/check_coverage.py /tmp/esy-io-scheduler-gcov/artifacts/gcov/gcovr/coverage.txt
ruby tools/quality/run_module_tests.rb esy_middleware/io_scheduler/tests /tmp/esy-io-scheduler-sanitizer test:all --sanitizer
```

C/C++ consumers verify concrete targets, OS-free cores, feature trimming and
missing dependencies. The SPI HAL fixture exercises the real public facade with
injected operation tables. Unit tests cover reservations, retained DMA failures,
prepared transactions, exactly-once results, shutdown, cooperative parking and
UART TX lifecycle, DMA prepared-route cleanup, waveform output retirement,
finite/circular cancellation and silent IRQ progression through regular processing. Each production C
source must reach 85% line coverage.
The PWM consumer injects completion/error IRQs between terminal observation and
progress clearing, including finite output-disable retries and circular progress.
LCD/touch integration tests exercise these schedulers through public APIs.
The [DMA/PWM hardware plan](tests/hil/dma_pwm_scheduler_hil_plan.md) covers target
IRQ, physical output and shutdown scenarios.

## Known Limits

HAL providers must report accurate busy state and exactly one terminal DMA
notification per accepted transfer. Provider and callback execution can delay
drain/parking; BUSY retains ownership.
The DMA/PWM cores do not emulate missing hardware DMA capabilities or mappings;
unsupported operations report through their documented submission/terminal path.
Pico DMA has no half-transfer notification. Unrequested half callbacks remain
NULL; requesting one returns `NOT_SUPPORTED`. For finite Pico PWM output with
only terminal notification, set `on_progress = NULL`.
Classic ESP32 currently has neither generic DMA nor PWM DMA support. UART RX,
I2C/CAN scheduling and additional OS workers remain outside these cores.

Target verification exercises separate SPI TX/RX DMA errors, delayed IRQs,
physical wire retirement, CS ownership, supported DMA mappings and the same
stop/drain sequence before OTA Flash operations requiring bus silence.


### UART TX OS adapter

Link `esy_uart_tx_scheduler_os` with `ESY_RTOS=FreeRTOS`,
`ENABLE_ESY_SYSTEM` and `ENABLE_ESY_UART_TX_SCHEDULER`. The adapter borrows an
initialized, idle UART and caller-owned queue/task/stack storage. It uses a
bounded MPSC OS queue; its worker alone drives the existing TX core and delivers
result callbacks. Direct HAL DMA requires no processing. Native DMA is selected
when supported; otherwise `send_timeout` uses the request budget (0 selects
1000 ms). SPI blocking fallback also supports uint16_t-buffer APIs and reports
byte counts, retaining each provider's documented wire representation.

```c
#include "esy_middleware/io_scheduler/esy_uart_tx_scheduler_os.h"

/* adapter starts at {0}; cfg binds persistent resources; UART is idle. */
esy_err_t scheduled_tx_start(esy_uart_tx_scheduler_os_t *adapter,
    esy_uart_t *uart, const esy_uart_tx_scheduler_os_config_t *cfg,
    const esy_uart_tx_scheduler_tx_request_t *request) {
    esy_err_t result = esy_uart_tx_scheduler_os_create(adapter, uart, cfg);
    if (result != ESY_OK) return result;
    result = esy_uart_tx_scheduler_os_submit(adapter, request);
    if (result != ESY_OK) {
        (void)esy_uart_tx_scheduler_os_request_stop(adapter, true);
        esy_err_t cleanup = esy_uart_tx_scheduler_os_destroy(adapter);
        return cleanup == ESY_OK ? result : cleanup;
    }
    return ESY_OK;
}

/* Stop/join producers first. Keep storage alive and retry BUSY cleanup. */
esy_err_t scheduled_tx_stop(esy_uart_tx_scheduler_os_t *adapter) {
    esy_err_t result = esy_uart_tx_scheduler_os_request_stop(adapter, false);
    return result == ESY_OK ? esy_uart_tx_scheduler_os_destroy(adapter) : result;
}
```

A synchronous fallback already executing cannot be interrupted by a stop request.
Successful TX results wait for physical wire idle; timeout/error can leave an
already copied prefix on the wire. Stop/destroy retains the UART until physical
TX and all callback uses retire. No backend includes or links the OS adapter.
