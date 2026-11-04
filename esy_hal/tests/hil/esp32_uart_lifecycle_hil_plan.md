<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# ESP32 UART Synchronous Lifecycle Scenarios

1. Initialize a zero handle, validate exclusive native ownership and repeat
   init/deinit with injected driver cleanup failures. Failure retains its owner.
2. Public TX/RX DMA and RX stream start return NOT_SUPPORTED without invoking
   callbacks or retaining caller storage. Capability discovery advertises no DMA
   or stream bits. Exercise both a board with no DMA route and one that supplies
   a route to the unsupported native DMA calls. After each rejection, assert
   both UART busy queries are false, public abort returns STATE (no tracked
   request), bounded synchronous TX and ordinary RX remain usable, and final
   deinit succeeds. Repeat with a registered device: its usage count returns to
   zero, both DMA leases are clear, and unregister/deinit succeed. The backend's
   empty abort succeeds without flushing synchronous TX already in the FIFO.
   The native peripheral probe retains executable assertions for the idle,
   bounded-TX and cleanup sequence; its default compile/link entry does not run
   those assertions on hardware. No HAL process function, task or event queue exists.
3. Exercise synchronous TX/RX and finite send_timeout. Reject invalid budgets;
   timeout never leaves a driver borrow of caller data. Queued FIFO bytes may
   continue transmitting; busy/suspend/deinit retain their physical-drain checks.
4. Use the UART TX scheduler OS adapter for queued task-context results. Cover
   full admission, bounded fallback, stop/drain, queued cancellation, task parking
   and cleanup retry. Callbacks occur once and no business process call is needed.
5. Verify backend source/target dependencies include no OS abstraction or direct
   RTOS headers. ESP-IDF retains its own native kernel implementation.
