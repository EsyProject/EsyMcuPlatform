<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Pico I2C Transaction HIL Plan

## Purpose

Verify blocking/combined transactions and rejection of unsupported ordinary DMA
on RP2040 and RP2350. Raw FIFO DMA does not provide the command servicing, final
STOP, error handling and callback retirement required by an I2C transaction.
SDK compile/link checks do not establish wire behavior.

## Integration and Ownership

Enable I2C in the consumer feature file. The generic DMA feature may remain OFF.
Supply the board controller, SDA/SCL mapping and pull-ups. Use a known 7-bit
slave and one serialized task or baremetal owner. Initialize the HAL handle with
`{0}`. Keep a logic analyzer attached to SDA/SCL and retain caller buffers and
callback arguments until each synchronous call returns.

Pico does not advertise `ESY_I2C_CAP_DMA_TRANSFER`. Well-formed DMA submissions
return `ESY_ERR_NOT_SUPPORTED`; they do not start a bus transaction, borrow a
buffer, register a callback or reserve a native DMA channel. Callers select
blocking or combined transfers explicitly. This port does not perform an
automatic DMA fallback.

## Procedure

1. Build and link the actual SDK consumer with both `esy_hal_i2c_write_dma()` and
   `esy_hal_i2c_read_dma()` retained in the firmware. Query capabilities after
   master initialization: master transfer/repeated START are present, DMA is not.
2. Capture a successful blocking write and read. Verify address, exact data,
   ACKs and final STOP, with SDA/SCL idle afterward.
3. Capture a combined write/read with repeated START. Verify a single command
   sequence with repeated START between phases and STOP after the final byte.
4. Submit well-formed TX-only and RX-only DMA descriptors with sentinel buffers,
   completion/error counters and both short and greater-than-FIFO lengths.
   Each call must return `ESY_ERR_NOT_SUPPORTED`. Verify no SDA/SCL edges,
   callback, buffer change or native DMA allocation, including after later IRQs.
   The descriptors must remain unchanged.
5. Repeat rejection at least 100 times, alternating TX/RX. A blocking or combined
   transaction after every rejection must still work. Rejection must not leave
   SCL low, change the target address, or require bus recovery.
6. Repeat with a registered device graph, including a suspended handle. A failed
   DMA submission must release its temporary usage reference. Check runtime
   suspend and unregister after rejection; neither may retain a DMA lease.
7. Deinitialize, reinitialize and repeat. With a missing/NACKing slave, record
   blocking/combined provider error behavior separately from DMA rejection.
8. Initialize a second master and slave handle on the same controller. Require
   `ESY_ERR_BUSY` before any board pin changes or native reinitialization. Verify
   traffic and callbacks on the original owner remain intact. Repeat while its
   graph device is suspended: its controller reservation must remain exclusive,
   and the original owner must resume. After successful full deinit, the second
   handle may initialize. Repeat this cycle on each available controller.
9. While native transfer or slave callback activity is present, require deinit
   to return `ESY_ERR_BUSY` without resetting pins or releasing ownership.
   Callback code only publishes notifications; lifecycle actions run on the
   serialized task owner. After retirement, require `ESY_OK` from deinit and
   verify the handle has returned to its zero state.
10. In separate boots, initialize directly as a 7-bit slave, then run
    master-init, successful master traffic, master-deinit, slave-init on the
    same controller. Inspect reset status: slave init must release the reset
    left by master deinit before installing/enabling the slave IRQ. Drive
    writes and reads from an external master and require receive/request/finish
    callbacks and correct data in both sequences. Repeat the transition at
    least 100 times on each controller.
11. Register a slave with the device graph, suspend and resume it, then repeat
    external-master traffic. Callbacks and controller ownership must survive
    the reconstruction. Inspect that no stale master configuration or asserted
    reset prevents reception after resume.
12. Use a peer that clock-stretches the write and read phases. Request a small
    positive `timeout_ms` and measure the entire combined call: both phases
    share one absolute deadline, rather than receiving a fresh budget each.
    Repeat with and without repeated START, TX-only and RX-only. On expiry,
    require `ESY_ERR_TIMEOUT`, no retained buffer/callback or usage reference,
    and inspect STOP/abort retirement. Release the external fault before testing
    subsequent traffic. Zero selects a finite 1000 ms default, also for the
    blocking helpers; test a stuck peer against that default. Keep the native
    timebase running and record timer resolution and SDK cleanup latency.
13. In 7-bit mode pass addresses 0x80, 0x100 and 0x3ff through blocking and
    combined APIs. Require INVALID_ARG before any bus activity, never traffic
    to a truncated address. Ten-bit initialization remains NOT_SUPPORTED.

## Acceptance Evidence

Record board/chip, SDK/toolchain, feature inputs, bus speed, slave, descriptor
lengths, return values, callback/usage counters and analyzer traces. Successful
compile/link and host HAL tests are separate evidence from these hardware checks.
