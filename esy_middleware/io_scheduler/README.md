<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# I/O schedulers

## Purpose

Provide independently linked SPI transaction and UART TX schedulers. These
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

## Public Surface

Public headers are flat under `include/esy_middleware/io_scheduler/`:

- `esy_spi_scheduler_core.h`: caller-driven creation and processing, request
  descriptors, buffer reservations, prepared transactions, quiesce and destruction.
- `esy_spi_scheduler.h`: optional FreeRTOS worker configuration and creation.
- `esy_uart_tx_scheduler.h`: owner-driven initialization, queued TX, processing,
  stop and deinitialization.

The scheduler name describes ordered transfer execution. Only the explicit SPI
OS adapter create operation creates a task. Serialization is per instance;
producers still obey the single-producer or serialized-owner contracts below.

## Integration

Select modules only in the consumer's `esy_feature_import.cmake`:

```cmake
set(ENABLE_ESY_IO_SCHEDULER ON)
set(ENABLE_ESY_SPI_SCHEDULER ON)
set(ENABLE_ESY_SPI ON)
set(ENABLE_ESY_DMA ON)
set(ENABLE_ESY_SYSTEM ON)
set(ENABLE_ESY_COMMON_DS ON)
```

| Target | Consumer selection | Dependencies |
| --- | --- | --- |
| `esy_spi_scheduler_core` | `ENABLE_ESY_SPI_SCHEDULER` | SPI, DMA/system and common data structures; no OS |
| `esy_spi_scheduler` | Same gate and `ESY_RTOS=FreeRTOS` | SPI core and OS worker support |
| `esy_uart_tx_scheduler` | `ENABLE_ESY_UART_TX_SCHEDULER` | UART, DMA and common data structures; no OS/system requirement |

HAL support alone assembles no scheduler. An empty selection produces no targets.
A protocol consumer explicitly selects its scheduler dependency; this module
never selects or links a protocol family. With `ESY_RTOS=None`, link the SPI core
or UART TX target directly. No initialization or worker creation occurs at link.

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
`ENABLE_ESY_SPI_SCHEDULER` or `ENABLE_ESY_UART_TX_SCHEDULER`, in the consumer's
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

`esy_spi_scheduler_get_write_block()` reserves a descriptor until submit.
`esy_spi_scheduler_get_write_buffer_with_arg()` reserves the same resource while
exposing preallocated payload bytes. Fill those bytes and submit them through
`esy_spi_scheduler_submit_result_with_arg()`. A second reservation returns STATE.
Buffers supplied after a reservation must match its allocated buffers. Rejected
submission preserves a caller's prior reservation for retry; a convenience
submission that reserved internally releases its reservation on rejection.

Full-duplex clients that write a fixed command size before submit should use
`esy_spi_scheduler_get_read_write_buffer_with_arg()`. It takes a nonzero minimum
size and checks both per-request allocations before reserving a slot. An
undersized TX or RX allocation returns `NO_MEM`, clears the output pointers and
leaves storage and existing reservations unchanged. This is the sized callback
adapter used by XPT2046; the original unsized getter remains available.

An ordinary convenience submission requires a terminal callback. A descriptor
may omit its callback when no completion notification is needed. Every accepted
request still has to finish before quiesce or destroy succeeds. All queues have
one serialized producer and one execution owner; callback submissions share the
producer contract. Ring capacity is `block_count - 1`, with a power-of-two block
count of at least two. DMA callbacks only publish completion state.

Each core processing call polls the provider once, retires at most one prior
request, then starts at most one next request, which may complete synchronously.
Call regularly enough to meet transfer retirement and queue latency requirements.
Provider calls, including hardware cleanup/abort, blocking fallback, request
length and callbacks determine elapsed time;
the request budget is not a time deadline. Process returns OK for a valid step,
including idle/quiesced steps; transfer errors go to terminal callbacks/results.
Process rejects recursive calls and OS-owned instances. Worker adapters perform
this processing themselves, waiting on notifications when idle and polling at
1 ms while active. Direct users of built-in Pico/STM32 SPI DMA must similarly
call `esy_hal_spi_poll()` until terminal notification.

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
Process retires at most one request and starts at most one transfer, polling
UART retirement once. Provider calls and callbacks add to elapsed time.
Callbacks may enqueue but cannot recursively process or perform lifecycle work.

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
prepared transaction and callbacks. Subsequent process calls poll for terminal
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
block-ring alignment. UART TX and the SPI core create no tasks. No runtime heap
allocation is used.

The consumer determines the SPI worker's OTA policy. Keep it running only with
application Flash/concurrency guarantees, or stop producers and confirm bounded
quiesce/drain before operations requiring silence. Timeout retains ownership and
fails or defers that transition. Resume after rollback or restore handles before
recreating a destroyed scheduler. IRQ/DMA must be idle before handle release.

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
UART TX lifecycle. Each production C source must reach 85% line coverage.
LCD/touch integration tests exercise these schedulers through public APIs.

## Known Limits

HAL providers must report accurate busy state and exactly one terminal DMA
notification per accepted transfer. Host tests do not prove IRQ ordering, SMP
scheduling, wire/CS timing, DMA width support or SRAM/DMA accessibility. Provider
and callback execution can delay drain/parking; BUSY retains ownership.

Target verification exercises separate SPI TX/RX DMA errors, delayed IRQs,
physical wire retirement, CS ownership, supported DMA mappings and the same
stop/drain sequence before OTA Flash operations requiring bus silence.
