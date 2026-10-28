<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# SPI Buffer and Full-Duplex HIL Plan

## Purpose

Verify ESP32 DMA-named transfer bounds and missing-direction handling, and Pico
16-bit blocking full-duplex framing. Host HAL tests establish public descriptor
validation and dispatch; SDK builds establish native API compatibility. Neither
establishes buffer bounds or the bytes and clock edges observed on the wire.

## Integration and Equipment

- Use one serialized owner, an SPI loopback fixture or a known slave, and a
  logic analyzer on CS, SCLK, MOSI and MISO. Supply a real board route and its
  DMA metadata hook; ESP32's native driver owns its DMA allocation internally.
- Enable SPI through the consumer feature import and retain the public blocking,
  write/read DMA and full-duplex DMA calls in the final firmware link. Use real
  Pico SDK 2.1.1 or ESP-IDF 5.3.4 providers for the corresponding build evidence.
- Allocate naturally aligned buffers with a canary on each side. Record board,
  firmware, SDK/toolchain, mode, frame width, requested speed and pin routing.
  Capture native error codes, callback order, canaries and wire traces.
- Keep all buffer and callback storage alive through terminal callback return.
  ESP32 completes accepted transfers synchronously in the owner caller. This
  plan does not change Pico/STM32 DMA retirement; use the
  [DMA/SPI plan](dma_spi_hil_plan.md) for that lifecycle.

## ESP32 Procedure

1. At 8-bit width, submit full-duplex buffers of 1, 2, 3, 4, 64 and 4092 bytes. Verify
   loopback contents, unchanged canaries, exactly the requested wire length and
   one inline successful terminal callback with matching TX/RX byte counts.
2. Submit transmit-only and receive-only requests through both helper APIs and
   full-duplex API calls with one configuration omitted. Verify the absent
   endpoint uses the native SDK's missing-buffer mode, without a NULL-pointer
   rejection or an incrementing dummy buffer. Confirm the requested wire length
   and untouched canaries. With a terminal callback, an absent memory direction
   reports zero transferred bytes; the present direction reports its byte count.
3. Configure 16-bit frames and repeat with aligned 2-, 8-, 64- and 4092-byte
   buffers and 16-bit DMA elements. Interpret data according to the selected
   SDK bit order; the public uint16_t API does not guarantee wire byte order.
4. Reject TX=64/RX=1 and TX=1/RX=64, zero length, an explicitly supplied NULL
   memory endpoint, both configurations missing, and a byte count exceeding
   SIZE_MAX/8. Reject incompatible element widths, 32-bit elements, circular
   mode, an odd byte count at 16-bit width and a misaligned halfword endpoint.
   Also reject ESP32 requests above 4094 bytes, RX lengths 5, 6 and 4094, and a
   width that differs from the initialized frame format. RX up to four bytes
   uses the SDK's inline word with an exact-length copy; larger RX must contain
   whole four-byte DMA words. Repeat these RX bounds through blocking read and
   transfer APIs at byte and halfword widths. Transmit-only permits 4094 bytes.
   No rejected request may assert CS, clock data,
   touch either buffer, retain a callback or invoke one later.
5. Force a native transaction failure on the real target. Expect a negative
   result, idle state and no success callback. Preserve canaries and confirm the
   next valid transaction succeeds. Repeat accepted and rejected calls while
   registered with the device graph. Common public descriptor rejections must
   not acquire a usage reference or resume a suspended device. Backend-specific
   size/frame restrictions may be checked after resume; they must release the
   acquired reference without a transfer or callback.

## Pico Procedure

1. Initialize 16-bit SPI and run blocking full-duplex loopback with one word,
   then a multiword nonrepeating pattern. Include values differing from 0xffff.
   Require RX[i] to match the response to TX[i], a return count in halfwords,
   one uninterrupted CS assertion and exactly 16 clocks per halfword. There
   must be no extra dummy frame between transmitted words.
2. Repeat at slow and fast supported speeds and with a known SPI slave. Confirm
   the single-word transaction does not discard its immediate response or
   instead report the response to an extra 0xffff word. Inspect canaries.
3. Reject the public DMA descriptor errors from ESP32 step 4 that are common to
   all HAL backends. A rejection must occur before a DMA route is prepared or
   CS changes. Continue accepted DMA retirement checks in the DMA/SPI plan.
4. Build with `PARAM_ASSERTIONS_ENABLED_HARDWARE_SPI=1`. Initialize slave mode
   with speed_hz zero and nonzero in separate lifecycle cycles. Require success
   without an SDK assertion, a valid native CPSR divider, and the slave-enable
   bit set. Clock traffic from an external master and verify data/frame width.
   Repeat deinit/reinit and graph suspend/resume on RP2040 and RP2350. The
   external master controls SCK; speed_hz must not cause a zero SDK baudrate.

## Pico XIO SPI Procedure

1. Build and final-link a consumer with XIO SPI enabled, native SPI disabled and
   generic DMA disabled. Retain calls to generic SPI blocking, transfer DMA,
   poll, abort and unregister APIs. No native SPI provider may be required.
2. With a zero handle, valid SCK/MOSI/MISO and a selected SM, request slave mode
   with pin_cs=ESY_PIN_UNUSED in all four modes. Require NOT_SUPPORTED before
   board pin configuration, GPIO/PIO changes or program/SM claims. Hold MOSI high
   and verify no transaction is accepted. Retry with a physical CS input and
   require successful transfers without a leaked resource from rejection.
3. For both bit orders, loop back nonrepeating 8-bit frames with 8-bit DMA
   elements, then 16-bit frames with 16-bit DMA elements. Each memory element
   must produce exactly its numeric frame value; verify canaries and byte counts.
4. Reject 2-, 4-, 7-, 9-, 12-, 15- and 32-bit frame DMA, mismatched 8/16-bit
   frame/element widths, and every 32-bit DMA descriptor through both generic and
   XIO convenience entries. Require NOT_SUPPORTED before DMA claims, FIFO drain
   or CS changes. Follow immediately with an accepted format on the same handle.
   Blocking 2..16-bit framing remains governed by its separate packing contract.
5. For slave MISO, explicitly select a distinct `slave_cs_sm_index` in the same
   instance. Missing (0xff), out-of-range and conflicting selections must return
   INVALID_ARG before board setup or native claims. Reserve the monitor SM with
   an independent native owner: init returns BUSY and releases its temporary
   data SM/program without changing that owner. Release it and retry. Exhaust
   instruction memory and verify NO_MEM cleanup permits later init. Master and
   MISO-less slave configurations require no monitor SM. Repeat successful
   deinit/reinit and graph suspend/resume and verify both SM claims/programs
   disappear while suspended and after final teardown.
6. Connect another slave to shared MISO with a weak bias and inspect its voltage
   using a scope. With both CS polarities, MISO must become high impedance after
   this slave's CS deasserts, including when its data SM waits for SCK, an empty
   TX FIFO or a full RX FIFO. Hold the CPU in an interrupt-disabled region or
   preempt the owner to confirm no software handler is required. Record the
   observed CS-to-enable/release delay at the configured `clk_sys`, including pad
   propagation. The design budget is six system-clock cycles plus pad delay;
   allow that margin before the first clock and before another slave drives.
   Verify the peer's transfers without bus contention or corrupted bits. Repeat
   with slow data settings; the monitor must retain divider one. Initialization,
   failed init and final deinit must leave MISO high impedance while unselected.
7. In every SPI mode and both bit orders, run blocking TX-only transfers with
   lengths 1, 2 and 32 at 8 and 16 bits, plus supported narrow frame widths.
   Preempt the owner after its final TX FIFO write and before its final RX FIFO
   read; let the external master clock every requested frame and deassert CS,
   then resume the owner without another CS assertion or clock. Require return
   count `len`, correct transmitted pattern and no indefinite wait. Repeat
   full-duplex and RX-only with lengths beyond the FIFO depth: preserve every
   received frame, canaries and exactly the requested clock count, with no
   extra dummy frame carried into the next transaction. The master must provide
   all requested clocks; this does not qualify truncated-frame recovery.
8. Repeat delayed owner polling for finite slave DMA at 8-bit/8-bit and
   16-bit/16-bit widths, with both bit orders and all modes, through TX-only,
   RX-only and full-duplex APIs. Withhold polling until TX and RX DMA remaining
   counts are zero and the master has deasserted CS. One owner poll must retire
   the completed transaction and deliver exactly one success callback without
   requiring another CS assertion, SCK edge or TXSTALL. Absent memory directions
   still require the matching dummy DMA endpoint to count every wire frame.
   Verify callback byte counts, released buffers/device usage, and immediate
   successful deinit or graph suspend after retirement. Retain master-side
   physical wire-drain checks from the DMA/SPI plan.

## Controller Ownership and Lifecycle Procedure

Run these checks on Pico, STM32F4 and ESP32 with the real board hooks and native
providers. Serialize operations on each handle; use separate task/core endpoints
only when racing distinct handles. ESP32 native master/slave users must be absent
for the same host throughout this HAL lifecycle, including suspend. An installed
native master driver context can be detected before board work; there is no
complete public SDK query for a bus without master devices or a slave driver. SPI1 remains reserved for flash on ESP32.

1. Initialize A and repeatedly transfer a known pattern. Attempt B on A's physical
   controller, both normally and in a two-core claim race where applicable. Exactly
   one claim succeeds; the loser returns BUSY before board/native changes. Observe
   pin mux, clock/mode, CS and A's wire traffic. B's rejected init/deinit must not
   reset A's pads or alter its register configuration. Copy A's handle into other
   storage and reject I/O, deinit and reservation release through that copy.
2. On STM32F4, request frame widths 1, 7, 9 and 15: require NOT_SUPPORTED before
   the board configure hook or native HAL init. Widths 0 and 17 are INVALID_ARG.
   Confirm exactly 8 or 16 clocks for accepted 8-/16-bit frames; no successful
   width conversion is allowed. Reject invalid mode, role, index and zero master
   speed before changing pads on all ports.
3. Inject init failure before and after native resources are acquired. On ESP32,
   separately fail bus initialize, add-device and CS configuration. Inject failures
   in remove-device and bus-free; after each failure keep the caller handle and
   claim, retry deinit, and verify only still-owned SDK resources are retired. A
   new owner must remain BUSY until native retirement and the final board reset
   finish. Successful deinit zeroes the handle and permits B and later A to init.
4. Suspend A through runtime PM and system PM. B remains BUSY throughout suspend,
   resume and any failed resume. Verify native DMA/controller retirement happens
   before board idle/sleep selection and resume reconfigures only A. Fail the pad
   selection: either A resumes active configuration with the original error, or
   STATE faults the graph when rollback fails. Inject partial native cleanup and
   failed resume: the registry must fault and retain the handle, binding and
   controller reservation for system recovery/reset. Ordinary registry teardown
   and unregister remain unavailable in the faulted state.
5. Keep finite DMA active and then physically complete it while withholding owner
   polling. Deinit remains BUSY in both states, preserving caller buffers, callback
   arguments and pending terminal notification. Poll or successfully abort before
   retrying deinit. Observe no late callback, DMA access or board reset after a
   successful teardown; follow the DMA/SPI plan for stream retirement details.
6. On ESP32, install an independent native master bus and add a native device
   before HAL init. HAL must
   return BUSY before its board configure hook and leave that native bus intact.
   After its native owner removes it, HAL init succeeds. Inspect SDK allocations
   after repeated successful init/deinit and partial-init cleanup retries.
7. On Pico and STM32F4, initialize idle enabled native MASTER and MASTER_NO_CS
   handles in turn. Change between two supported SCK rates, require OK, and
   measure the new rate on the next blocking transaction. With MASTER_NO_CS,
   keep fixture-owned CS asserted only around that transaction; the backend
   must not drive CS during rate changes or I/O. Verify disabled and busy DMA
   still reject changes, then retire work and change speed successfully again.

## Pass Criteria and Limits

Accepted transfers use only the provided memory ranges and deliver their
declared framing and terminal byte counts. Rejected descriptors perform no
board/backend dispatch through the public HAL. ESP32 backend-specific rejects
perform no native transaction or CS change. Pico blocking 16-bit full duplex
produces one frame per requested halfword and preserves the corresponding RX.

Record target canary checks, callback observations and wire traces separately
from compile/link results.
