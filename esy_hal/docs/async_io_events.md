<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Native DMA and Scheduled I/O

## Purpose

Separate interrupt-driven hardware DMA from queued I/O execution. Native DMA
completes automatically; applications do not call a HAL process/poll function.
OS workers and task-context callbacks belong to the optional I/O scheduler.

## Public Surface

Pico/STM32F4 expose supported native DMA entry points and bounded ISR callbacks.
Pico UART TX/RX DMA completes from DMA controller interrupts. TX completion
certifies source-buffer retirement, independently of UART FIFO/shift-register
drain; the busy query covers physical transmission. STM32 UART TX additionally
waits for its UART TC interrupt. XIO TX also certifies source retirement.
UART TX peripheral interrupts and DMA controller completion interrupts are
distinct capabilities; absence of UART TC does not disable TX DMA.

ESP32 public DMA operations return `ESY_ERR_NOT_SUPPORTED` and capability masks
omit DMA bits. Its synchronous SPI implementation may still use SDK-internal
DMA; that acceleration does not expose a caller-driven asynchronous descriptor.
ESP32 UART stream callbacks are unsupported. GPIO/timer/other interrupt APIs
retain their own contracts.

## Integration

Enable the peripheral and link `esy_hal`; generic DMA additionally requires
`ENABLE_ESY_DMA`. The caller provides stable handles, buffers and callback
arguments. A finite accepted transfer delivers its terminal callback from an
IRQ after safe retirement. Half/circular callbacks retain transfer ownership.

```c
/* dma is initialized and request contains a valid route and ISR-safe callbacks. */
esy_err_t result = esy_hal_dma_start_single(dma, request);
if (result != ESY_OK) {
    esy_err_t cleanup = esy_hal_dma_abort(
        dma, request->inner.dreq, request->inner.channel);
    return cleanup == ESY_OK ? result : cleanup;
}
/* No process/poll call: hardware IRQs deliver completion/error. */
```

Use the optional SPI OS scheduler or UART TX OS adapter when the submitting task
needs queued execution and a task-context result callback. Caller-driven scheduler
cores remain available for an existing execution owner. Schedulers call public
HAL only; they never access backend/board/SDK APIs.

## Contracts

DMA callbacks must be bounded, nonblocking and ISR-safe. They may publish results
or use compatible ISR notification APIs, but must not reenter driver lifecycle,
start/abort or configuration calls. Backend terminal handling retires native
callback/storage access before reporting reusable buffers. Duplex SPI combines
both directions and its physical tail before releasing CS. UART TX DMA success certifies at least source retirement; use the physical busy
query before stopping clocks or assuming the final bit has left the wire.

Hardware that fails a bounded IRQ-side stop retains its claim, buffers and
callback context for explicit task-side abort/recovery. Such a fault does not
produce an early terminal success. Failed cancellation retains resources;
successful cancellation suppresses pending terminal callbacks.

The HAL/backend has no platform OS dependency, task or worker. Native ESP-IDF
components retain their own SDK kernel dependencies. Scheduling resources are
explicitly assembled in middleware via public OS wrappers.

## Resource Model

Descriptors are copied, buffers and callback arguments borrowed through completion
return or successful cancellation. On Pico, ISR retirement releases the logical
DMA claim while caching the SDK channel reservation; task release/deinit flushes
it and subsequent task-side prepare may reuse it. Keep lifecycle resources alive
until cleanup succeeds. A configured generic controller deinit can return BUSY
while its physical routes cannot be released.

Scheduler workers, stacks, queues and request storage are caller-owned and passed
through aggregated static resource configurations. Queued synchronous fallback
is asynchronous to the submitter but occupies its worker during the HAL call.
UART fallback has a finite per-request millisecond budget. SPI synchronous SDK
calls retain their existing provider blocking contract; queued stop/cancel cannot
interrupt a call already executing. No hard shutdown latency is implied.

## Verification

HAL/fake/scheduler tests cover automatic IRQ completion, callback context,
exactly-once finite delivery, half/circular progress, explicit abort and failed
cleanup retention. Consumer builds verify public API, native providers and the
absence of HAL/backend OS linkage.

## Known Limits

Supported DMA directions follow capability masks. ESP32 public DMA is unavailable;
use supported synchronous HAL operations or scheduled I/O. Pico and STM32F4 retain
native UART TX DMA; do not infer DMA capability from a UART TC interrupt. OS callbacks run only when an OS adapter is explicitly assembled.
Caller-driven middleware processing schedules queued work, not hardware DMA
retirement. Old HAL `*_process` hooks and call sites must be removed and all
objects rebuilt after the public layout change.
