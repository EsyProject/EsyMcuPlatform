<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# SPI Buffer and Full-Duplex HIL Plan

## Purpose

Verify ESP32 synchronous transfer bounds and public DMA rejection, Pico 16-bit
blocking full-duplex framing, and automatic IRQ completion on supported native
SPI and XIO DMA providers. Record buffer canaries, transferred data, callback
ownership and the corresponding CS/SCLK edges.

## Integration and Equipment

- Use one serialized owner, an SPI loopback fixture or a known slave, and a
  logic analyzer on CS, SCLK, MOSI and MISO. Supply a real board route and its
  DMA metadata hook for supported public DMA paths. ESP32's synchronous native
  driver owns its internal SDK DMA resources.
- Enable SPI through the consumer feature import and retain the public blocking,
  write/read DMA and full-duplex DMA calls in the final firmware link. Use real
  Pico SDK 2.1.1, ESP-IDF 5.3.4 or the selected STM32F4 providers.
- Allocate naturally aligned buffers with a canary on each side. For ESP32
  synchronous RX, lengths above four bytes must be multiples of four; short RX
  uses the native transaction's inline word and an exact-length copy. SDK
  copying/allocation for other buffer locations follows the native provider.
  Record board,
  firmware, SDK/toolchain, mode, frame width, requested speed and pin routing.
  Capture native error codes, callback order, canaries and wire traces.
- Keep supported asynchronous buffers, handles and callback storage alive
  through terminal callback return or successful task-context abort. Native
  Pico/STM32F4 and Pico XIO DMA complete automatically from IRQ context. Keep
  terminal callbacks bounded and nonblocking; they must not reenter lifecycle
  or submit new HAL I/O. No HAL processing or polling call retires a transfer.

## ESP32 Procedure

1. At 8-bit width, run blocking full-duplex transfers of 1, 2, 3, 4, 64 and
   4092 bytes. Require matching loopback contents, unchanged canaries, exactly
   the requested wire length and a returned byte count equal to the request.
   The synchronous call returns after native completion and CS release; no
   asynchronous callback or result-retirement step is involved.
2. Exercise blocking write-only and read-only APIs separately. Verify that the
   SDK's absent direction transfers no caller memory and that canaries remain
   intact. The full-duplex blocking API still requires both TX and RX buffers;
   a NULL endpoint is invalid. Transmit-only lengths 1, 2, 3 and 4094 bytes
   remain valid, without an incrementing dummy buffer outside caller storage.
3. Configure 16-bit frames and repeat blocking operations with aligned 2-, 4-,
   8-, 64- and 4092-byte buffers. Supply halfword counts to the uint16_t APIs
   and require halfword return counts. Interpret data according to the selected
   SDK bit order; the public uint16_t API does not promise wire byte order.
4. Reject zero-length or NULL-buffer blocking calls, byte requests above 4094
   and halfword requests above 2047 with INVALID_ARG. For read/full-duplex,
   reject RX lengths 5, 6 and 4094 bytes with NOT_SUPPORTED before a native
   transaction or CS assertion. RX lengths 1..4 use the inline word and copy
   exactly the requested bytes; longer RX accepts only multiples of four up to
   4092. Repeat at halfword width, including accepted counts 1, 2 and 4 and a
   rejected count of 3. Preserve both canaries for every accepted/rejected case.
5. Repeat valid synchronous TX from flash or non-DMA-capable memory and valid
   RX into writable memory with natural element alignment but no four-byte DMA
   alignment. Record native return values and SDK heap/copy behavior. The SDK
   may use temporary DMA-capable buffers; these calls carry no zero-allocation
   promise. Short RX still copies only its declared byte range, and long RX
   still requires a byte length divisible by four.
6. Submit otherwise valid public write-DMA, read-DMA and full-duplex DMA
   descriptors, including single-direction requests, with a callback and
   canary-protected endpoints. Require NOT_SUPPORTED, no bus/CS activity, no
   borrowed buffer/callback after return, idle state and no later callback.
   Repeat registered and unregistered. A graph resume may precede a backend
   rejection, but the temporary usage reference must be released. Confirm
   DMA_TRANSFER is absent from capabilities and a following blocking transfer
   and teardown succeed. SDK DMA used inside blocking calls does not imply
   support for these public asynchronous entries.
7. Force a synchronous native transmit failure and require a negative result
   with no deferred callback/result descriptor. Observe CS and canaries, then
   verify the next valid transaction and deinit follow the native error outcome.
   For OS-backed queued application work, repeat through the SPI scheduler and
   distinguish its owner-context result callback from a public HAL DMA callback.

## Shared DMA Descriptor and Completion Procedure

1. On supported Pico/STM32F4/XIO paths, reject unequal TX/RX byte lengths or
   widths, zero length, a supplied NULL memory endpoint, both directions absent,
   byte counts above SIZE_MAX/8, circular mode, 32-bit elements, incomplete
   halfwords and misaligned halfword endpoints. Common descriptor errors reject
   before board/backend activity or callback retention. Single-direction helpers
   use fixed dummy/discard words for the absent direction; inspect both native
   transfer counts and ensure dummy storage never increments beyond that word.
2. Complete both DMA directions while the application owner task is preempted.
   Require one terminal callback from IRQ context without a HAL progress call.
   Successful master completion follows the final wire bit and CS release;
   slave completion follows the final RX frame without a future CS/SCK edge.
   Buffers and any device usage reference remain borrowed through callback
   return. Record callback byte counts and verify immediate subsequent owner
   teardown or PM suspend after successful retirement.
3. Delay one direction's native IRQ and verify the other cannot prematurely
   notify, release CS or return buffers. On IRQ delivery, native DMA retirement
   and the bounded wire-tail check complete automatically. The tail budget uses
   the programmed divider: one frame on Pico SPI, two on STM32F4, eight PIO
   cycles on XIO, plus register synchronization, capped at 8192 inspections.
   These are register/CPU iteration budgets, not OS timeouts or elapsed-time
   guarantees. A master configuration exceeding the cap rejects DMA with
   NOT_SUPPORTED before DMA claims or CS activity; blocking I/O remains valid.
   XIO CS hold/inactive timing must also fit its 8192-core-cycle budget.
4. Inject native DMA stop/retirement failure. Require busy state, preserved
   endpoints/callback arguments and graph usage, with no early terminal callback.
   Retry explicit task-context abort; only successful retirement permits reuse,
   and successful cancellation suppresses the callback. No owner polling loop
   or future IRQ is a prerequisite for this cleanup attempt.
5. Exhaust the master wire-tail budget after DMA memory access has retired.
   Require a terminal DMA_ERROR instead of success. Native Pico/STM32F4 disable
   the SPI handle and require deinit/init before another operation. XIO follows
   its SM reset/retirement path. Preserve canaries and ensure no later DMA access
   or duplicate callback follows successful terminal cleanup.

## Pico Procedure

1. Initialize 16-bit SPI and run blocking full-duplex loopback with one word,
   then a multiword nonrepeating pattern. Include values differing from 0xffff.
   Require RX[i] to match the response to TX[i], a return count in halfwords,
   one uninterrupted CS assertion and exactly 16 clocks per halfword. There
   must be no extra dummy frame between transmitted words.
2. Repeat at slow and fast supported speeds and with a known SPI slave. Confirm
   the single-word transaction does not discard its immediate response or
   instead report the response to an extra 0xffff word. Inspect canaries.
3. Run the shared DMA descriptor/completion procedure above. Rejections precede
   route preparation and CS changes. Include full-duplex, TX-only and RX-only
   accepted transfers and automatic IRQ terminal delivery.
4. Build with `PARAM_ASSERTIONS_ENABLED_HARDWARE_SPI=1`. Initialize slave mode
   with speed_hz zero and nonzero in separate lifecycle cycles. Require success
   without an SDK assertion, a valid native CPSR divider, and the slave-enable
   bit set. Clock traffic from an external master and verify data/frame width.
   Repeat deinit/reinit and graph suspend/resume on RP2040 and RP2350. The
   external master controls SCK; speed_hz must not cause a zero SDK baudrate.

## Pico XIO SPI Procedure

1. Build and final-link a consumer with XIO SPI enabled, native SPI disabled and
   generic DMA disabled. Retain generic SPI blocking, transfer DMA, abort and
   unregister APIs and their IRQ callback closure. No native SPI provider may
   be required.
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
8. Preempt the application owner during finite slave DMA at 8-bit/8-bit and
   16-bit/16-bit widths, with both bit orders and all modes, through TX-only,
   RX-only and full-duplex APIs. When both DMA directions complete, IRQ retirement
   must deliver exactly one success callback without another CS assertion, SCK
   edge, TXSTALL or owner task execution. Absent memory directions
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
5. On supported Pico/STM32F4/XIO DMA, keep a transfer active and require deinit
   to return BUSY while preserving caller storage and callbacks. Preempt the
   owner task and let normal IRQ delivery finish it: terminal retirement must
   occur automatically. Separately delay a native terminal IRQ or fail native
   stop; retained work remains BUSY until IRQ retirement or successful explicit
   task abort. Observe no late callback, DMA access or board reset after successful
   teardown. On ESP32, repeat lifecycle checks after synchronous returns and
   rejected public DMA calls; no asynchronous request remains to retire.
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
declared framing and byte counts. Common invalid descriptors rejected by HAL
preflight perform no board/backend dispatch. ESP32 synchronous bounds checks
and unsupported public DMA perform no native transaction or transfer CS change;
registered-device resume may precede a backend check. Supported DMA paths deliver
one IRQ terminal result after safe retirement, retaining storage after native
stop failure until successful explicit abort. Pico blocking 16-bit full duplex
produces one frame per requested halfword and preserves the corresponding RX.

Record target canary checks, callback observations and wire traces separately
from compile/link results.
