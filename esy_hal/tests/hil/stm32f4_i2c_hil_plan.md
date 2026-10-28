<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# STM32F4 I2C Transaction HIL Plan

## Purpose

Verify supported blocking/transfer operations and rejection of unsupported DMA.
DMA entry points reject before native submission, without borrowing a physical
stream or retaining callbacks. SDK compile/link checks do not establish bus behavior.

## Integration

Enable I2C and its required dependencies through the consumer feature import.
Supply the board controller, clock, SDA/SCL routing and pull-ups. Use a known
7-bit slave, one serialized task or baremetal owner, a zero-initialized handle
and a logic analyzer attached to SDA/SCL.

The master capability mask contains `ESY_I2C_CAP_MASTER_TRANSFER` and omits
`ESY_I2C_CAP_DMA_TRANSFER`. Consumers query capabilities and choose supported
blocking/transfer operations explicitly. DMA submissions do not provide an automatic blocking fallback.
Repeated-START is unsupported: a combined transfer requiring
it returns NOT_SUPPORTED before any write, read or START. Slave mode also
returns NOT_SUPPORTED before board pin setup, controller claiming or IRQ/native
driver setup; this port supplies no complete slave event/address/data dispatcher.

## Procedure

1. Build and link a real STM32F4 HAL consumer with both
   `esy_hal_i2c_write_dma()` and `esy_hal_i2c_read_dma()` retained. Verify the
   initialized master's capability mask and successful blocking write/read.
   Capture address, exact data, ACKs and final STOP.
2. Submit well-formed TX-only/RX-only DMA descriptors with sentinel buffers,
   completion/error counters and short/long lengths. Require
   `ESY_ERR_NOT_SUPPORTED`, unchanged descriptors/buffers, no callback and no
   SDA/SCL edges, including after subsequent event/error/DMA IRQs.
3. Prepare a generic DMA request on a route configurable for I2C, without
   starting it. Reject I2C DMA submissions and verify that the generic request's
   physical lease, registers, callback identity and payload remain unchanged.
   Start/finish or abort that request using its original owner. Repeat with an
   active independent peripheral DMA request on the same physical stream.
4. Repeat rejection at least 100 times, alternating TX/RX. Run a supported
   blocking/transfer transaction after each rejection. Check that no recovery,
   DMA stream reset or peripheral reinitialization becomes necessary.
5. Repeat with a registered device graph and a suspended controller. The
   temporary usage reference must be released after rejection. Verify runtime
   suspend, unregister and deinit/reinit afterward; no callback or DMA lease may
   remain attached. Board metadata queries and graph resume may precede the
   unsupported backend result, but no native DMA or bus transaction may start.
6. Reject NULL handle/configuration/memory, zero length and lengths above
   `UINT16_MAX` as invalid arguments. Slave-role DMA calls must also reject.
   Attempt slave initialization with valid callbacks/address and confirm
   NOT_SUPPORTED, an unchanged zero handle, no board configure/reset call and
   no enabled EV/ER IRQ. A subsequent master initialization must work.
7. Attempt another master handle on the same physical controller,
   including while its graph owner is runtime-suspended. Require
   `ESY_ERR_BUSY` before board pin changes or native reinitialization. Resume
   the original handle and verify communication/callbacks remain valid. Full
   successful deinit must release the reservation and permit the second handle.
   A slave request remains NOT_SUPPORTED and must not change the live master.
8. Inject `HAL_I2C_Init` and `HAL_I2C_DeInit` failures independently. Failed
   cleanup must retain context, native handle and controller ownership without
   resetting pins; remove the fault and require successful deinit before reuse.
   If initialization fails and cleanup succeeds, retry must work. Native
   suspend cleanup failure must return `ESY_ERR_STATE` through the PM adapter,
   faulting the graph while retaining ownership for system recovery. Verify an
   active unrelated DMA stream on that I2C route never makes idle I2C busy.
9. Send a combined nonempty TX/RX request with repeated_start=true. Require
   NOT_SUPPORTED with no SDA/SCL edges, unchanged buffers and no native transmit
   or receive call. Repeat against a peer whose write command has visible side
   effects and verify none occur. A supported request with repeated_start=false
   must still execute the write/STOP/read sequence exactly once afterward.
10. Clock-stretch each phase of a supported write/STOP/read request. Specify a
    positive `timeout_ms`, observe the remaining timeout passed to the native
    receive, and measure one total budget. Require TIMEOUT on expiry and no
    retained caller storage or graph reference. Repeat TX-only/RX-only, HAL tick
    wraparound and zero selecting the finite 1000 ms default. Keep HAL ticks
    running; record tick resolution and bounded SDK retirement latency. A bus
    already busy at entry returns BUSY before the native fixed bus-busy wait.
    Release external faults before requiring subsequent traffic to succeed.
11. In 7-bit mode reject 0x80, 0x100 and 0x3ff through blocking/combined calls
    without START or address truncation. Ten-bit initialization remains
    NOT_SUPPORTED. Reject timeout_ms above INT32_MAX before native submission.

## Acceptance Evidence and Limits

Record board/MCU, SDK/toolchain, feature inputs, bus speed, descriptor lengths,
return values, callback and usage counters, physical DMA ownership and analyzer
traces. Native DMA may be enabled only after shared stream claims, native IRQ
dispatch and retryable abort/terminal retirement are implemented and verified.

Record compile/link and host HAL checks separately from wire observations.
