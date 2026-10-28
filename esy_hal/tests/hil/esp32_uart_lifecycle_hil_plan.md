<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# ESP32 UART Lifecycle and Owner-Poll HIL Plan

## Purpose

Verify exclusive controller ownership, bounded finite RX progress, failed
cleanup retention and repeated suspend/resume with the native ESP-IDF driver.

## Integration and Ownership

Enable UART and discover `idf::esp_driver_uart`. Peripheral UART DMA-style APIs
do not require the optional generic DMA facade. ESP32 system integration also
requires the native `esy_os_context` target and `ESY_RTOS=FreeRTOS`; the platform
root assembles this OS-port target without a dependency on the complete HAL.

Initialize the SDK first and give each handle one serialized owner. No platform
UART task or event queue is created. Finite RX advances only through owner
`esy_hal_uart_poll()`, with at most 1024 bytes and one terminal callback per
call. The owner must poll often enough for the line rate and SDK ring capacity,
and arrange sender backpressure where needed. Callbacks run inside poll, must
be bounded, and must not reenter UART I/O or lifecycle. Blocking UART operations
busy-poll hardware and provide no scheduler-yield guarantee.

Suspend requires retired RX/callbacks and physically idle TX. It gates RX data
interrupts while retaining the installed driver and its ring buffers. It does
not power down the UART clock or implement system sleep. Quiesce the sender
before suspension. Only successful deinit permits releasing handle storage;
native deletion failure retains the driver, board pins and controller claim.

Record SDK/toolchain versions, board/pins, baud rate, core configuration,
owner polling interval, callback duration and firmware revision.

## Procedure

1. Record native task/queue counts before and after init. The facade must create
   no platform event task or event queue. Repeat init/deinit and confirm native
   driver resources return to baseline without heap drift.
2. Submit finite RX into fixed canary-protected storage. Without owner polling,
   no completion or caller-buffer write may occur. Poll and verify exact bytes,
   at most one terminal callback, and unchanged canaries.
3. Fill the SDK RX ring before submitting RX, then stop the sender. Poll must
   consume the preexisting bytes without a new event. Repeat with more than
   1024 bytes pending; each call respects the byte budget, and later polls finish.
4. Leave RX incomplete. Suspend and final deinit must return BUSY. Abort from
   the owner, verify no terminal callback or later caller-buffer access, then
   suspend/deinit successfully. Attempt callback reentry at a bounded barrier;
   live work must remain borrowed until callback return.
5. Exercise hardware TX through the last stop bit. Suspend/deinit must remain
   BUSY while the FIFO/shift register is active, even after submission returns.
6. Repeat at least 100 idle suspend/resume cycles. Confirm the same driver and
   rings remain installed, RX IRQs are gated throughout suspend, and finite RX
   resumes without creating tasks or event queues. Suspended data/baud changes
   reject with STATE without touching the request buffers.
7. Inject RX interrupt disable/enable failures. Disable failure leaves active
   operation intact; enable failure retains the suspended state and driver for
   retry. Poll while suspended must not re-enable reception.
8. Inject driver-install and parameter-configuration failures separately. Only
   resources created by this handle may be cleaned up. Inject driver deletion
   failure: retain ctx, pins and claim; retry before allowing another owner.
9. While handle A is active or suspended, initialize B on the same controller
   using different pins/baud. Reject before any pin/native mutation. Repeat
   against an externally installed SDK console driver, whose lifecycle must be
   excluded from facade ownership by the consumer.
10. Through device registration, prove active RX/TX leases block runtime suspend.
    Retire all I/O, unregister, deinit, then reinitialize the same storage. Race
    independent controller claims across cores; only one owner may succeed.
11. Test supported framing and injected native read errors. Bytes lost by wire
    errors or SDK buffering cannot be recovered by owner polling; record sender
    flow control, overrun detection and recovery policy at the integration layer.

## Acceptance Evidence

Keep byte traces, logic-analyzer timing, polling budgets, canaries, native
resource counts and failed/retried transitions. Generic ESP32 DMA and continuous
RX streaming remain unsupported capabilities. Host facade tests and compilation
do not establish physical line timing or native driver retirement.
