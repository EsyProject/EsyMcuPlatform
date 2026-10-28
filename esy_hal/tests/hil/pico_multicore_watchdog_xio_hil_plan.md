<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Pico Multicore, Watchdog and XIO Contract HIL Plan

## Purpose and Setup

Verify caller-owned queue bounds, watchdog lifecycle, DMA IRQ affinity and PIO
placement/encoding on real RP2040 and RP2350 consumers. Enable the corresponding
features through the consumer feature import. Use real Pico SDK providers, a
logic analyzer and buffers surrounded by canaries. Record the MCU, SDK/compiler,
feature set, clock tree, core assignments, IRQ priorities and firmware revision.
The backend has no host unit suite; source/consumer checks and these hardware
observations provide different evidence and must be reported separately.

## Multicore Startup, Queue and Build

1. Build a baremetal consumer with only the common, multicore HAL/backend and
   board targets. It must compile and link without independently adding
   `pico_multicore`: the enabled backend supplies that provider dependency.
   A missing provider must fail configuration with the feature-specific error.
2. Pass the actual backing capacity as `buf_size` to the queue-init API. Allocate `item_size * (depth + 1)` bytes: `depth` is usable capacity
   and the extra slot belongs to the native full/empty distinction. For item
   sizes 1, 4 and 17 and depths 1, 2 and 7, fill exactly `depth` items, verify
   the next try-send fails, drain in order and verify the next try-receive fails.
   Repeat fill/drain and alternating enqueue/dequeue for many wrap cycles. Check
   both canaries after every cycle, especially depth-one enqueue/dequeue/enqueue.
3. Supply one byte less than the required capacity and an undersized
   `item_size * depth` buffer. Require `NO_MEM` before changing the queue or
   buffer. Zero dimensions and dimensions above `UINT16_MAX` are invalid.
   Initialize with uninitialized queue-object storage only on its first use;
   stop every endpoint before reinit/deinit. Backing bytes need not be cleared.
4. Run multiple producer/consumer task/core endpoints with item sequence IDs.
   Verify no loss/duplication beyond deliberate nonblocking full rejection.
   Exercise blocking operations from task context only and stop both endpoints
   before releasing caller-owned storage.
5. In a fresh-boot startup fixture, have one serialized core-0 owner launch a
   nonreturning core-1 entry through the public API. Record its heartbeat and
   require OK once. Repeat start while that entry runs: require BUSY promptly,
   with no FIFO boot handshake, core reset, changed entry or heartbeat gap.
   From core 1 and a core-0 ISR, require STATE without changing the live core.
   NULL entry and an invalid core ID return INVALID_ARG. Do not launch/reset
   core 1 through raw SDK or another provider; ownership lasts until reset.

## Watchdog Configuration and Reset Acknowledgement

1. Initialize with 250 ms and pause-on-debug enabled. Call enable repeatedly;
   measure that the configured timeout/debug behavior survives. Deinit, enable
   again and verify Pico retains that last successful configuration. With no
   prior successful init, enable uses 1000 ms and continues while debugging.
2. Cause a watchdog reset. On the next boot, require `was_reset()` before init
   to report true. Calling enable must still start the watchdog. After feeding
   it, call clear-reset and require `was_reset()` to report false for this boot.
   Verify that acknowledgement neither enables a stopped watchdog nor feeds,
   shortens, extends or changes the debug behavior of a running watchdog.
   In particular, no 1 ms timeout may be programmed by acknowledgement.
3. Repeat acknowledgement before init and after deinit. Pico acknowledges the
   public status in software because the native reset-reason register is
   read-only. It does not erase the hardware diagnostic register. A new actual
   watchdog reset starts a new boot epoch and must be visible again.
4. Reject zero and greater-than-8000 ms initialization without changing the live
   configuration. The timeout lower-bound query returns STATE while stopped
   and NOT_SUPPORTED while running until a certified clock bound is provided;
   failed queries preserve the output.

## DMA IRQ Core Ownership

1. In separate fresh boots, select core 0 and core 1 for each DMA IRQ before
   its first prepare. Without explicit selection the owner is core 0. Attempt
   first prepare from the other core: require STATE, unchanged native IRQ and
   channel registers, no callback, and no retained new channel claim. Retry on
   the selected core and require success.
2. After first assembly, submit independent channels from either core and prove
   callbacks run only on the selected IRQ core. Rebinding an assembled IRQ must
   return BUSY. Do not independently enable that IRQ on the other core: the SDK
   NVIC APIs operate on the executing core and no remote NVIC access is used.
3. Race two first prepares from different cores with independent channels. A
   wrong-core caller returns STATE; same-owner-core contention may return BUSY
   during installation. Retry and prove exactly one shared handler is installed
   and both accepted transfers finish. Cross-core channel ownership, callback
   retirement and buffers still follow the ordinary DMA contract.
4. On one core, assemble DMA IRQ0 at a valid priority such as 0x80, then prepare
   another channel on IRQ1 at 0x40 and at 0xc0 in separate attempts. Both must
   return BUSY, preserve IRQ0 and its pending transfer, and retain no new channel
   claim. Retry IRQ1 at 0x80 and require success. A later request on either
   installed IRQ with a different priority must still return BUSY after all
   transfers retire: dispatcher priority remains reserved until reset.
5. Pend both same-core DMA IRQs while the first callback holds a bounded target
   trace barrier. The peer IRQ must stay pending until that callback and its
   dispatcher return, then deliver exactly once; foreground must continue. In
   another boot assign the IRQs to different cores with different priorities:
   both may initialize and complete, including peer-core guard contention.
   Record native priorities, pending flags, callback order and the core heartbeat.

## Native SPI and XIO

1. Request LSB-first on the native Pico SPI controller. Require NOT_SUPPORTED
   before board pin setup, native initialization, ownership changes or wire
   activity. The native controller supports MSB-first only; select XIO explicitly
   if its separate bit-order contract meets the consumer's requirement.
   For both native MASTER and MASTER_NO_CS, change the idle enabled controller
   between two supported speeds. Require OK and measure the new SCK after each
   call. In MASTER_NO_CS keep CS owned by the fixture and verify the backend
   never toggles it. Disabled and active-DMA handles must still reject changes.
2. Initialize generic XIO program and SM objects with `{0}` before first use.
   Repeat program init and SM claim while live: require STATE without replacing
   the owner. Exercise invalid descriptor and instruction-memory exhaustion,
   retry after releasing resources, then release every SM before program deinit.
   Successful release/deinit returns each object to `{0}` for reuse. Do not test
   arbitrary poisoned handle flags as a supported first-use input.
3. Reserve another PIO program/SM so SPI loads at a nonzero instruction offset.
   Record the actual entry offset. Run master and slave transfers, abort finite
   DMA at multiple points, then restart. Verify restart jumps to that SPI entry,
   preserves the unrelated program and canaries, and retires buffers/callbacks.
4. Repeat nonzero placement with externally triggered master SPI, CPHA 0 and 1,
   CPOL 0 and 1, and multiple supported frame widths. Inspect native SM config:
   one physical side-set pin plus optional enable requires a total count of two
   and the optional bit set; ordinary master mode has count one and no optional
   bit. Confirm adjacent GPIOs are untouched, each trigger produces exactly the
   configured SCK edges, and SCK returns to its configured idle level while
   waiting for the next trigger. Completion polling must recognize the relocated
   trigger wait entry and release the DMA request exactly once.
   Capture non-stalled intra-frame SCK periods at several requested rates:
   ordinary master loops use four PIO cycles per bit and triggered loops use
   five. Both must produce the requested SCK within divider quantization, rather
   than a fixed 20% triggered-mode frequency error. Record high and low periods
   separately, as the five-cycle loop is not a 50% duty cycle. Trigger wait and
   frame-boundary latency are separate from intra-frame frequency.
5. Hold unrelated GPIO output latches at both high and low levels and keep a
   peer SM driving an independent pin. With a stopped target SM at a nonzero
   TX pin, call the pin-mask helper for that one pin with values zero and one.
   Every unrelated latch and peer output must stay unchanged. Repeat by
   initializing XIO UART TX, which uses this helper to establish its idle level.
6. Reject helper counts 0, 33 and 255, bases ESY_PIN_UNUSED/32/255, and a range
   beginning at the final valid GPIO with count two. Reject generic SM OUT, IN,
   SET, side-set and JMP mappings outside GPIO0..31 or the selected chip's bank.
   Require INVALID_ARG before changing registers, GPIO routing or SM config.
   Test the upper valid pin with count one, ignored high mask bits, and count
   32/base zero only on a chip exposing all 32 GPIOs. Inspect compiler/target
   behavior for shift bounds; do not substitute a host backend unit suite.
7. Initialize direct backend XIO SPI and UART TX/RX with complete `{0}` handles.
   Repeat each init while idle and during finite DMA: require STATE with the
   same program offset, SM/native claims, buffers and callbacks, and no duplicate
   IRQ registration. Finish owner poll/abort, deinit, then reuse the same slots.
   UART direction deinit must preserve its peer. Fail first init through a busy
   native SM and exhausted instruction memory; temporary claims must retire and
   a later legal init must work. Keep an active-DMA handle alive when deinit
   retains it; never force-clear storage to bypass retirement.
8. On a claimed generic SM, configure divider ratios `mul/div` of 1/1,
   1/65536, 0/0, UINT32_MAX/UINT32_MAX and 65536/UINT32_MAX. Verify exact
   16:8 register encoding: 65536 uses integer/fraction zero, and the final
   ratio truncates to integer 65535, fraction 255. Reject 2/1, 1/65537,
   2/131073 and UINT32_MAX/(UINT32_MAX-1) with INVALID_ARG before changing
   any SM register, GPIO routing, pin direction or output latch. The existing
   claim/program offset must remain valid for legal reconfiguration or release.
   A ratio field of zero independently defaults to one. Repeat with native
   parameter assertions enabled and disabled; neither may panic or mask the
   error through float rounding.
9. Test direct and public UART TX/RX rates zero, UINT32_MAX/8+1,
   UINT32_MAX and rates placing clk_sys/(8*baud) below one or above 65536.
   Use separate TX-only and RX-only handles. Require INVALID_ARG and no retained
   program/SM claim. Direct backend rejection must not change pin routing;
   public HAL attempts may already have invoked board-owned pin configuration,
   which must be recorded separately from backend SM/pin writes.
   Immediately retry a representable rate on the same native SM and handle.
   Repeat master SPI with ordinary and triggered programs at representable
   divider endpoints and outside them; failed generic setup must release all
   temporary program/SM resources. Measure accepted UART/SPI rates against the
   documented 1/256 divider truncation, including a ratio near an endpoint.
10. Compile all backend/HAL and consumer translation units with default XIO
    private storage sizes, then with both globally overridden to larger values.
    Rebuild all objects, including each UART direction and SPI, and enlarge
    their enclosing UART/SPI private storage consistently when compile-time
    capacity assertions require it. Compare C/C++ object layout and embedded
    byte-array capacities; no macro redefinition or silent return to 64 bytes
    is allowed. Repeat create/use/release/reinit with canaries around handles.

## Native SDK Resource Coexistence

1. Serialize external raw SDK allocation with HAL DMA/XIO claims through the
   consumer's assembly owner. The built-in Wi-Fi provider shares a private
   nonblocking allocation reservation with HAL, including lazy station
   enable/join and retry paths. Race these built-in paths from two cores:
   contention must return BUSY for retry without native panic, resource leaks
   or waiting. Do not race an external raw fixed-ID SDK claim against a HAL
   claim: native fixed-ID APIs panic and are not try-claim operations. Release
   must retire hardware/IRQ access and make native unclaim its last mutation
   of that resource.
2. Claim a DMA channel through the SDK and keep it physically idle. HAL prepare
   for that channel must return BUSY without changing addresses/count/IRQ
   enables or callback data. Prepare a different free channel, then complete
   and abort it; the native claim must remain intact. After native unclaim,
   HAL may acquire the former channel. A HAL-acquired channel must report
   claimed through the SDK and must be excluded by its unused allocator until
   successful HAL retirement releases it. Repeat both initialization orders.
   Direct HAL-backend abort/release attempts with a foreign owner token must
   also return BUSY while the idle native SDK claim remains held.
3. Claim a PIO SM through the SDK and leave it disabled. XIO manual claim of
   that SM must return BUSY before loading its program or changing SM state.
   The same request may succeed after native unclaim; XIO release must clear
   exactly its SDK claim. Reverse the order and verify native unused allocation
   skips an XIO-owned SM. Exhaust instruction memory and prove failed XIO
   program allocation releases the newly claimed SM without releasing peers.
4. On Pico W and Pico 2 W, initialize Wi-Fi before HAL DMA/XIO and repeat in
   reverse order, with explicit nonconflicting consumer resource selections.
   Run sustained Wi-Fi traffic while HAL DMA and XIO transfer. Exercise a
   shared DMA IRQ with native-provider completion pending alongside a HAL
   completion; both owners must receive exactly their own terminal events.
   HAL IRQ handling must leave native-provider pending flags for its handler.
   Stop one provider and reuse only its released resources while the other
   continues. Record SDK claim bits, IRQ flags, transfer canaries and traffic.
5. During immediate HAL-to-native channel handover, trigger an unrelated HAL
   channel IRQ on the peer core. The dispatcher must not carry a stale retired
   channel mask across SDK unclaim and consume the new native owner's event.
   Repeat handover under IRQ stress and prove both completion counters remain
   exact. Pico backend release from ISR must return NOT_SUPPORTED without
   metadata, SDK-claim or callback changes; retire it later from its task owner.

## Evidence and Limits

Capture canary values, queue item IDs, timeout measurements, reset reports, IRQ
core IDs, PIO entry/config registers, waveforms and callback counts. Record the
MCU and SDK for each result; do not infer another MCU/SDK's behavior from one
target.
