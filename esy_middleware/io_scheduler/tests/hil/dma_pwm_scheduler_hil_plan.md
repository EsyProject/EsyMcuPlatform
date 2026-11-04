<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# DMA and PWM scheduler hardware verification

## Scope and setup

Exercise the caller-driven [scheduler cores](../../README.md) through public HAL
on a supported Pico or STM32F4 DMA/PWM mapping. Select the corresponding
scheduler and HAL features explicitly. Generic ESP32 DMA and PWM DMA are not
supported by the current backend.

Use persistent aligned request slots and DMA-accessible buffers, initialized
quiescent HAL handles and one serialized execution owner driving active processing.
Capture the board/device, SDK/toolchain revision, features, IRQ priorities,
DMA route/width, clocks, buffer placement and logic-analyzer traces. Instrument
owner steps and terminal/progress callbacks without logging in IRQ.

## Scenarios

1. Queue distinct finite transfers up to FIFO capacity. Verify FIFO order,
   full-queue rejection, exact data/sample order and exactly one terminal result
   per accepted request. Rejected requests must not receive a callback.
2. Verify back-to-back, delayed and coalesced IRQ events. Terminal and progress
   callbacks execute only during owner processing. Active transfers request a
   finite processing interval even before a result becomes available. Processing
   retries preserve native/output ownership until retirement without a separate
   DMA wake callback; an idle core requests an indefinite wait.
3. Hold a prepared DMA route by injecting start failure. A nonbusy native channel
   must not release the request before successful abort. Inject abort failure,
   then recovery; verify retained buffers/context and one final error callback.
4. For PWM, observe the output after finite source DMA completion. Before the
   core publishes a terminal result or starts its next waveform, output must be
   disabled. Record final-sample truncation/period behavior; a full final PWM
   period is not a core guarantee. Inject disable failure and verify retry
   retains all active storage without duplicate submission or completion.
5. Exercise half/circular progress across multiple cycles. It must retain sample
   ownership. Request stop and confirm admission closes while active circular
   work remains owned. Request abort and confirm physical DMA/output retirement,
   queued cancellation and exactly one terminal result for every accepted item.
6. Request stop while finite work is active, with and without queued cancellation.
   Check deinit returns BUSY until owner processing completes retirement and
   callbacks. Fail cleanup repeatedly; no timeout or clock wrap may revive an
   already completed/cancelled request or implicitly release live buffers.
7. After successful core deinit, reuse the borrowed HAL directly and recreate a
   core on the same zeroed storage. Core deinit must not deinitialize hardware.
   Attempt core init against active DMA/output and verify BUSY without disruption.
8. Before a Flash/OTA operation requiring silence, stop producers and obtain
   bounded drain/abort confirmation. Test timeout policy before Flash starts,
   including retained callbacks/IRQ resources. Resume only under the application's
   documented rollback/recovery or restart policy.

## Evidence

Record observed outcomes per scenario, including error injection, callback
context, ownership duration, owner processing count when relevant and
physical output traces. Leave unavailable scenarios explicitly unexecuted.
