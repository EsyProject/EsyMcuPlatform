<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# DMA and SPI HIL Plan

## Purpose

Verify that public byte lengths, logical DMA routes, physical streams and SPI
terminal ownership describe the same real transfer. Compile/link checks and HAL
facade fakes do not establish physical DMA bounds, stream requests or wire timing.

## Integration and Equipment

- Enable the required DMA, SPI and system features in the consumer's feature
  import. Supply real STM32F4 DMA/SPI SDK providers and the final IRQ dispatchers.
  Do not override the backend's native DMA handles or completion callbacks.
- The board supplies valid DMA controller/stream encodings, request channels,
  actual stream IRQs and priorities. Reserve these streams for one owner. The
  template's unconfigured SPI route must be completed by the hardware consumer;
  successful compilation does not imply a board route is present.
- Use DMA-accessible, naturally aligned SRAM buffers with canaries. Do not use
  CCM. Use a loopback fixture or known SPI slave and a logic analyzer on SCLK,
  CS and data. Start with software-controlled CS inactive.
- Serialize prepare/start/abort and peripheral lifecycle on one task/startup
  owner. Generic public DMA callbacks are delivered by esy_hal_dma_poll();
  backend/peripheral IRQ callbacks only notify their owner. Drive generic DMA
  poll or SPI poll until the corresponding request retires.
  Keep clocks fixed and keep buffers, handles and callback arguments alive while
  busy, including after a submission or cleanup failure that still reports busy.

Record MCU/board, firmware revision, SDK/toolchain, clock tree, SPI mode/frame
width/speed, route table and IRQ priorities. Capture native DMA CR/NDTR/address
registers, canaries, wire traces, callback order and optional graph usage counts.

## Target Build Checks

Compile DMA, SPI and ADC with the real ARM toolchain and SDK headers, with
warnings as errors. Link a consumer that retains the transfer, abort and poll
APIs and the real DMA vectors. Check ADC separately because it uses the same
generic prepare but passes a sample count to its native ADC start operation.
Use at least one ADC1-only and one multi-ADC device when claiming both.

## Pico Reload and Live Counter Regression

Run these checks on RP2040 and RP2350 with the real SDK DMA backend. Use one
serialized task/startup owner, a reserved idle channel and aligned SRAM buffers
with canaries. Native IRQ callbacks only record completion/error; keep the
owner token, descriptor, buffers and callback storage alive until abort succeeds.
Do not use a backend host fake to establish these register semantics.

1. Prepare a finite memory copy without triggering it. Record `CHx_DBG_TCR`,
   `TRANS_COUNT`, both addresses and `BUSY`: the reload value must match the
   requested beat count even if the live counter is zero. Start through
   `esy_hb_dma_start()` and require success, exact copied contents, untouched
   canaries and one completion. Repeat through a real SPI consumer and verify
   data/SCLK activity and successful owner poll retirement; a prepared channel
   with zero live count must not produce a rejected-start error.
2. Retain the prepared backend claim after completion. Confirm live count is
   zero and the reload value is unchanged. Restore both endpoints without a
   trigger, then start the same channel again. Require another complete bounded
   transfer and one additional completion; repeat with a mask containing two
   channels. Do not bypass the public generic HAL's owner-poll retirement flow
   to retain a claim: use an explicitly owned native backend fixture here.
3. Restore both endpoints and repeat using a triggering read-address update,
   then a triggering write-address update. Both must accept a valid reload with
   zero live count and transfer exactly the recorded beat count. Restore the
   other endpoint before each trigger so completed increments cannot escape the
   buffer. Change the count without triggering and repeat, proving the next
   trigger uses the new reload value and remaining-count queries report the
   live counter rather than the reload value.
4. While a paced transfer is active, require `BUSY` from another start or address
   control and preserve its registers. Also check unprepared channels, foreign
   owners, invalid masks/counts and misaligned addresses: no rejected operation
   may trigger any selected channel or mutate another request. Abort, confirm
   callback/buffer retirement, then prepare and start a new transfer successfully.

## Hardware Procedure

1. Prepare memory-to-memory copies on a controller/stream supporting that mode
   with 8-, 16- and 32-bit widths. Set byte lengths, then inspect NDTR before
   start: it must be byte length divided by width. Verify incrementing source
   and destination, exact copied contents and untouched canaries. In particular,
   a 128-byte halfword transfer must access 128 bytes and program NDTR to 64.
   Inspect PSIZE and MSIZE separately: both must encode the selected halfword
   or word width in their own register fields, without overlapping encodings.
   STM32F4 requires DMA2 for these accepted copies. On every available DMA1
   stream, submit an otherwise valid memory-to-memory descriptor and require
   NOT_SUPPORTED before any clock, register, IRQ, callback or ownership change.
   Repeat while that stream belongs to another prepared/active request and
   confirm its addresses, NDTR, callbacks and traffic remain intact. Rejected
   DMA1 requests must not prevent a later valid peripheral-direction transfer.
2. Exercise zero length, invalid width, nonintegral beat lengths and unaligned
   endpoints. Exercise the 65,535-beat boundary at each width and one beat over.
   Reject invalid configuration before changing an existing stream. Non-NULL
   source/destination addresses must match the selected alignment; a missing
   peripheral address or either missing memory-copy endpoint is invalid.
3. Configure two logical routes using distinct streams but the same native
   request-channel number. Start by logical route-table index, independently and
   together; verify the intended physical streams only. A duplicate physical
   stream with a different request channel must be rejected. Inspect the backend
   resource query for valid routes and preservation of its output on invalid
   input. Supply an invalid or unprepared bit in a backend physical start mask
   and prove no other selected stream is enabled by that rejected batch.
4. For each direction, update an idle prepared read address and write address.
   Verify memory-to-peripheral uses M0AR as the source and PAR as the destination;
   peripheral-to-memory and memory-to-memory use PAR as source and M0AR as
   destination. The transfer-count control/query uses native beat units; only
   prepare's descriptor length is bytes. Confirm a subsequent start uses the
   changed endpoints and count without reading or writing outside their buffers.
5. Run SPI full-duplex loopback at 8-bit and 16-bit widths with equal byte lengths.
   Confirm requested data, NDTR and canaries. Delay delivery of either DMA IRQ:
   SPI poll must not release CS, report terminal success or release a graph lease
   until both terminal events and physical BSY clearance have been observed.
   Confirm one terminal result, exact byte counts and no late callback/write.
6. Run SPI transmit-only and receive-only buffers of at least 64 bytes at both
   frame widths. Inspect the absent direction: M0AR points at its one-word dummy,
   MINC is clear and CIRC is clear. Use a hardware data watchpoint or a target
   diagnostic to verify adjacent dummy/callback storage is untouched. Confirm
   both streams terminate, CS deasserts after the last wire bit, busy clears and
   the next transfer succeeds. Repeat with registered SPI and inspect its lease.
7. Reject SPI circular descriptors, unequal direction lengths, frame/transfer
   width mismatch and two directions mapped to one physical stream. Reject an
   already busy SPI handle. None of these preflight failures may enable streams
   or retain a new request. Keep a different stream busy, force the second SPI
   prepare to fail, and verify cleanup only retires the stream prepared by this
   request; the other owner continues undisturbed.
8. Inject first/second prepare and start failure on target. Rejection with busy
   false must retain no callbacks/buffers. If hardware cleanup fails, preserve the
   original request and graph lease, observe busy, and continue poll or retry
   abort. Poll must deliver one terminal error only after all owned streams and
   wire activity retire. No result may report success for an injected DMA error.
9. Abort during finite SPI DMA and during an error-retirement retry. Verify that
   new DMA requests stop, both owned stream callbacks retire, and successful
   cancellation produces no ordinary completion or later buffer access. If a
   slave/master still reports BSY, abort reports failure/busy and retains the
   request for retry; do not reuse storage or deinit until retirement succeeds.
   After successful abort, restart and verify normal completion.
10. Run finite and circular ADC DMA as described in the
    [ADC DMA plan](adc_dma_hil_plan.md). Confirm descriptor bytes are converted
    once: generic prepare uses beats for NDTR and native ADC start receives the
    matching sample count, including the maximum supported buffer length.

11. Run generic public DMA both registered with the device graph and unregistered.
    Mask task polling after physical completion: no user callback may execute,
    busy stays true, prepare/deinit/controls must reject reuse, and a registered
    route retains its usage reference. Poll from its serialized owner; verify
    native abort detaches pending IRQ/callback access before notification, exactly
    one terminal callback runs in that task, and the usage reference stays held
    inside the callback and drops after it returns. Repeat without user callbacks.
12. Inject FE, DME and TE separately, then each error with TC and HT pending in
    the same stream IRQ snapshot. The STM32F4 single-buffer dispatcher must report
    error only, mask stream interrupts, clear pending flags and request EN clear.
    Measure IRQ duration: it must never wait for EN to clear. Stall or fail the
    task-side abort and verify poll returns failure without notifying/releasing
    storage or a graph lease; retry poll or abort to retire safely. A later IRQ
    must not enter the vendor TE busy-wait or produce duplicate error/success.
13. Repeat finite and circular native ADC completion/half/error through the
    shared DMA owner: HAL_ADC_Start_DMA replaces native DMA callback pointers.
    Verify native ADC Parent/State/Lock and error codes still transition correctly,
    adc poll/abort can stop conversions and retire the same DMA handle, and no
    ADC callback runs after successful task abort. The backend configures only
    single-buffer DMA; do not install native double-buffer or M1 callbacks.
14. For generic circular DMA, delay owner polling across multiple HT/TC events.
    Expect at most one half and one cycle notification per poll, rather than an
    exact event count. The request/lease stays live until error retirement or
    successful abort; clients needing lossless sample accounting need their own
    buffer/protocol arrangement. A terminal error suppresses co-pending success.
15. For every address/count control, test a valid prepared idle resource and
    invalid route, missing prepare, active/pending request, misalignment and native
    counter overflow. Failure must preserve registers and public prepared/started
    state. STM32 count accepts 1..65535 beats; Pico must not place count bits into
    RP2350 TRANS_COUNT.MODE. Public generic chain returns NOT_SUPPORTED without
    arming either route on all ports. Test the Pico backend-only primitive only
    under a separately owned native chain lifecycle; STM32/ESP reject it.
16. Initialize two generic controller handles with overlapping physical routes.
    Prepare A, then prepare B before A starts: B returns BUSY and preserves A's
    addresses, count and callbacks. Start A and prove only A's buffer and callback
    are used. B's backend start/abort/address/count operations with its own token
    must return BUSY without modifying A, including while A is physically idle
    but awaits owner polling. After A retires, prepare/start B successfully.
    Repeat with different STM32 request selectors aliasing the same stream.
17. Repeat the conflict with generic DMA versus enabled SPI, UART TX/RX/stream,
    PWM and ADC providers, and between two native providers. Attempt the conflict
    while the first owner is prepared, active, terminal-but-unpolled and retrying
    failed cleanup. No second provider may replace callbacks, clear pending IRQs,
    stop another owner's transfer or release its claim. After successful abort,
    every provider must permit a new owner. STM32 I2C DMA is unsupported and must
    reject before acquiring a stream or asserting a bus transaction.
18. On Pico multicore, race distinct owner tokens for one physical channel from
    otherwise serialized task endpoints. Exactly one prepare succeeds; losing
    callers leave the winner's descriptor and callback untouched. At retirement,
    stress immediate re-claim on the peer core: old metadata cleanup must finish
    before the new owner can configure hardware. A delayed/failed second SPI
    direction cleanup must not query, abort or include counts from a direction
    that has already retired and been acquired by the peer.
    Also prepare two distinct channels sharing an initially uninstalled DMA IRQ
    from separate cores after selecting its owner core (default core 0). First
    assembly must execute on the selected core: the other core receives STATE
    without native IRQ/channel changes or a retained new claim. Owner-core
    installation contention may return BUSY; retry there, then verify either
    core can submit through the installed handler. Exactly one handler is
    installed and callbacks execute on the selected core. Race core binding
    against that installation: binding must complete before reservation or return
    BUSY, with no torn core/registration state or SDK/metadata lock deadlock.
19. Force STM32 UART RX stream preparation success followed by rejected start.
    Successful cleanup releases the stream and permits another owner; failed
    cleanup retains active state and callback/storage ownership until stream stop
    succeeds. For finite native ADC, try a conflicting prepare inside its client
    completion callback: the existing claim remains BUSY until callback return,
    then becomes available without any returning native dispatcher touching it.

## Pass Criteria and Limits

Every programmed memory access stays within the caller's byte range or the
fixed dummy word. Logical route masks address the intended exclusive streams.
Successful SPI completion follows both stream and wire retirement; successful
abort leaves no asynchronous access, and failed cleanup preserves ownership.
Generic DMA notification follows hardware/callback retirement and registered
leases outlive the user terminal callback. Poll visits a bounded route count,
but provider abort, peer-core callback exclusion and user callback execution
contribute to its duration; it has no promised wall-clock bound.

Measure real IRQ delivery, canaries and CS/SCLK timing on the selected board.
Slave wire idle can depend on the external master; a failed abort does not
permit storage reuse.
