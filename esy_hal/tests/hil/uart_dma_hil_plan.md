<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# UART DMA Ownership and Cancellation HIL Plan

## Purpose

Verify buffer retirement, UART DMA request gating and cancellation independently
of optional device power-management registration. SDK compilation verifies APIs;
it does not establish hardware timing, interrupt races or successful retirement.

## Integration and Ownership

Enable UART and provide board TX/RX routes and interrupt priorities. Peripheral
DMA does not require the optional generic DMA facade; enable that facade only
when the test also exercises its independent controller API.
Use one serialized owner task or baremetal loop. Initialize each handle with
`{0}`. Call `esy_hal_uart_poll()` on each owner turn while work is pending.
All built-in finite transfer notifications require this poll except ESP32 TX,
which may complete inline. A poll visits at most both directions, but SDK
retirement and user callback execution contribute to
its duration. Choose the polling period from the consumer latency requirement.
Callbacks are bounded notifications and never reenter UART APIs. Keep buffers,
handles and callback arguments alive through terminal callback return or
successful abort. Failed retirement retains that ownership for a later retry.

STM32 supports finite ordinary TX/RX DMA only; circular submissions reject with
`NOT_SUPPORTED` before starting. Continuous RX uses the dedicated RX stream API.
Pico/STM32/XIO finite DMA notifications and ESP32 RX run in the owner poll;
ESP32 TX may complete inline. ESP32 finite RX consumes at most 1024 bytes and
emits at most one terminal callback per poll. No platform task or event queue
advances RX. Poll regularly or apply sender backpressure before the SDK RX ring
fills. Serialize cancellation with the owner; callback reentry is forbidden.
Cancellation retires source/buffer access, but does not promise that bytes already in a UART
TX FIFO or shift register disappear from the wire.

Record firmware, chip, board routes, toolchain/SDK, baud rate, buffer lengths,
IRQ/core ownership, polling period and callback context with each result.

## Procedure

1. On Pico/STM32, send a finite buffer and receive a finite buffer over loopback with
   independent TX/RX routes. Verify exact bytes and exactly one completion per
   direction. On STM32 inspect CR3: successful RX submission enables DMAR, TX enables
   DMAT, and terminal retirement disables only the completed direction. Stop
   polling after IRQ publication: no terminal callback or lease release occurs
   until polling resumes. TX success occurs after the final stop bit.
2. Start and abort TX before TC, then send again on the same handle. Repeat for
   an incomplete RX request, then receive again. Aborts must retire native DMA,
   request bits and pending callbacks. No canceled terminal callback may occur.
   After successful abort overwrite old storage with a sentinel and verify that
   further wire traffic cannot modify it. Repeat at least 100 times.
3. Repeat steps 1 and 2 with an ordinary unregistered handle and with a registered
   device graph. Both must cancel/restart. Registered usage remains held through
   callback return and until successful abort, preventing runtime suspend.
4. On STM32 submit circular TX and RX. Each rejects `NOT_SUPPORTED`, starts no
   DMA stream and leaves no DMAT/DMAR request or graph lease. Subsequent finite
   transfers must work. Verify dedicated continuous RX still starts/stops.
5. Inject native abort failure after submission and after error IRQ publication.
   Poll/abort reports the failure, is_busy remains true, and deinit/re-init must
   not clear the retained handle. Restore the provider and retry: ownership
   releases once, and a new transfer works. ISR paths must not wait for abort.
6. Race owner abort with completion/error delivery, including a Pico peer-core
   DMA IRQ. Count user notifications and graph usage. There must be no duplicate
   release, negative usage, callback after successful abort, or use of old
   callback arguments. Calls made from a callback must return BUSY or otherwise
   remain prohibited; they must never enter a native wait for the current IRQ.
7. On ESP32 start incomplete RX, cancel it before new data arrives, then start a
   replacement request. Bytes retained in the SDK RX ring may serve that new
   request but must never access canceled storage. Pause owner polling after
   submission: no RX callback may
   occur and the buffer remains borrowed. Resume polling and verify the bounded
   RX budget, exactly one terminal callback and no callback reentry. Check that
   no platform task/event queue is created, and retry cancellation only from
   the serialized owner outside a callback.
8. Exercise RX stream/ordinary RX exclusion and simultaneous independent TX/RX.
   A rejected submission must not cancel another direction, reconfigure its
   route, clear its request bit or release its graph usage.
   With registered and unregistered handles, start a stream and require blocking
   RX and ordinary RX DMA to return BUSY without consuming any FIFO/DR byte;
   TX must still work. Fail stream stop (and separately fail start with retained
   storage), verify the same exclusion remains, then retry stop successfully and
   verify blocking RX works again. Repeat through the direct Pico/STM32 backend.
9. On STM32F4, loop back 7E/7O and 8N/8E/8O at both supported stop-bit settings.
   For 8-bit payloads include 0x80 and 0xff; verify parity and the payload width
   on the wire after init, baud-rate change and suspend/resume. Use one-byte
   blocking TX/RX buffers surrounded by canaries. Reject 9N, 9E/9O and 7N before
   board pin configuration, native init or buffer access. The public byte count
   must never become a native halfword count.
10. For STM32 continuous RX, configure different UART IDLE and DMA HT/TC IRQ
    priorities in both orders. Arrange a pending peer IRQ at each DMA-position
    query/cursor-publication boundary. A physical span must be committed once,
    never twice or lost through a stale cursor. Verify bounded PRIMASK exclusion
    and that the stream callback runs after restoring the entry mask. Repeat
    overflow/error and stop/restart while the two sources are pending.

11. Repeat terminal/error/abort races on Pico XIO UART with TX-only, RX-only and
   both directions assembled. IRQs only publish events; user callbacks run in
   the owner poll after native callback pointers and pending IRQs retire.
   Inject abort failure: the source/destination, handle and graph lease remain
   borrowed, and retry retires once. Circular ordinary submissions reject before
   native DMA starts; the built-in Pico DMA backend provides no half callback.
   XIO TX completion certifies DMA source retirement, not PIO wire drain: queued
   bytes may still transmit. At a deliberately slow baud, complete a one-byte
   and FIFO-sized TX and poll the DMA source callback immediately. While the
   final byte/stop bit is still on the logic analyzer, require TX busy, runtime
   PM to leave the device active, system prepare to return BUSY, and deinit to
   retain the program/SM and handle. Continue owner busy checks until idle; only
   then may PM/deinit stop the SM. Require the entire final stop bit on the trace.
   The first FIFO-empty check clears old TXSTALL; a later fresh stall at blocking
   PULL proves drain. First starve the FIFO between earlier bytes, then append a
   last byte and repeat to ensure an old sticky stall cannot announce idle.
   Repeat after successful DMA abort with already queued bytes, and while an
   unrelated DMA IRQ fires. Delay the first DMA poll/busy check until the SM is
   already stalled at idle; clearing its sticky flag must allow a fresh TXSTALL
   observation and eventual idle, without requiring another transmitted byte.
   Blocking TX must return only after the final stop
   bit; repeat TX-only and both-direction builds on RP2040 and RP2350. Keep the
   PIO clock/SM enabled and serialize FDEBUG/busy/lifecycle on its task/core owner.

12. Initialize an idle native or XIO UART whose board DMA route overlaps a
    separately owned generic/peripheral transfer. While that foreign DMA is
    active, require UART TX/RX busy to remain false, and idle native baud-rate
    change/suspend or registered runtime PM to succeed without touching the
    foreign DMA registers, counts or callback. Repeat after the UART's own DMA
    source retirement. Its own active TX/RX/stream and undrained physical TX must
    still report busy and block PM, even when the raw DMA BUSY bit is already
    clear. Retry new UART DMA while the foreign owner holds the route: submission
    must return BUSY through the resource arbiter without canceling either owner.

## Acceptance Evidence

Keep logic-analyzer traces, request-bit and DMA state snapshots, injected failure
results, callback counts and usage counts alongside native compile/link and
HAL facade test results.

## Physical Controller Ownership

For each built-in hardware UART port (Pico, STM32F4 and ESP32), keep one owner
running blocking and asynchronous I/O. A second handle targeting the same
physical controller must receive BUSY before board pins or native configuration
change. Repeat during idle runtime/system suspend: ownership remains reserved
until the first handle completes final deinit. Separate controller indices must
remain independently usable.

Inject a native init failure after ownership is acquired and a cleanup failure
where the SDK can report one. HAL deinit returns an error and retains its handle,
board pins and claim until cleanup succeeds. Verify no asynchronous callback or
buffer access survives ESY_OK. On STM32F4, exercise failed HAL_UART_DeInit and
successful retry. In addition, fail HAL_UART_DeInit during runtime/system
suspend: ESY_ERR_STATE must fault the registered graph, reject later graph I/O,
and retain the controller claim and native cleanup state until system
recovery/reset. A faulted graph cannot be deinitialized or unbound through the
ordinary lifecycle APIs; final deinit retry applies to unregistered handles.
It must never advertise unchanged ACTIVE state after that uncertain transition.
On ESP32, exercise failed driver deletion after finite RX and callback
retirement in the owner poll. Deinit of active DMA, a continuous RX stream or
undrained physical TX returns BUSY; explicitly stop/poll/abort the owner before retrying.

On Pico, contend independent owner tasks on both cores while stream IRQs are
pending; the guarded dispatcher must never reach a released handle. On STM32F4,
verify pending UART/DMA interrupts do not reach an old binding after stream stop
and successful native cleanup. A competing failed init must never reset the
successful owner's pins or detach its dispatcher.

## Pico RX IRQ Priority Acceptance

In a fresh boot, start UART0 RX streaming on one core at a valid board priority
such as 0x80. Start UART1 there at 0x40 and 0xc0: each must return BUSY before
borrowing its storage/callback or changing IRQ/FIFO state. The first stream must
continue receiving exact bytes. Retry UART1 at 0x80 and exercise simultaneous
pending RX with a bounded callback trace barrier; the peer UART IRQ must wait
until the guard owner returns, then service its FIFO without an interrupt storm
or lost foreground progress. Stop/deinit both and verify another priority on
that core still returns BUSY, because its static dispatchers remain installed.
Restart at 0x80 and prove storage/callback reuse. In a separate boot, start the
two streams on different cores with different priorities and verify delivery,
bounded peer-core exclusion and successful owner-controlled stop. Native users
must not independently change the owned IRQ priorities. Record priorities,
pending status, byte/callback counts and core heartbeats with the target traces.
