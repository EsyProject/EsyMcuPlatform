<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# CAN Hardware Verification

## Purpose

Verify native bxCAN or classic ESP32 TWAI frame transfer, acceptance filters,
diagnostics, ownership and the provider's control/cleanup behavior. Host facade tests and
source checks do not establish electrical behavior or hardware completion.

## STM32F4 Integration and Equipment

- Use an STM32F405/407/415/417 board with CAN1 or CAN2 routed through a compatible
  external transceiver and a correctly terminated bus.
- Connect an independent CAN peer/analyzer capable of standard, extended and
  RTR traffic, acknowledgement control, error injection and counter capture.
- Record device/package, board and transceiver revision, SDK/CMSIS version,
  input clock, nominal timing, bus bitrate, firmware revision and `poll_limit`.
- Configure pin routing, peripheral input clock and transceiver before init.
  Use one serialized non-ISR owner for every CAN operation. Avoid raw native
  ownership of either CAN1 or CAN2 while the HAL owns their shared domain.
  The backend owns CAN gate enablement during its lifetime; APB1 clock-rate
  setup remains consumer-owned.

## STM32F4 Target Build Checks

1. Configure the real consumer with explicit `ESY_TARGET_DEVICE`,
   `ENABLE_ESY_CAN=ON` and the STM32F4 provider. Build and link its firmware with
   the embedded toolchain; inspect that both CAN HAL and backend sources are
   included. Repeat with the feature off and confirm those sources are absent.
2. Build the enabled module with `ESY_RTOS=None` and no OS/DMA/system feature
   dependencies. Repeat in the intended RTOS firmware with its actual owner.
3. Confirm unsupported target selection fails configuration clearly. Confirm
   enabling FD returns `NOT_SUPPORTED` before any controller change.

## STM32F4 Hardware Procedure

1. Initialize a `{0}` handle. Verify stopped state, no bus transmission, zero
   accepted frames and unchanged unrelated peripheral configuration. Start with
   no filters and verify that peer traffic is rejected.
2. While stopped, install an exact standard-ID data-only rule. Send matching and
   nonmatching standard IDs, extended IDs with the same low bits and RTR frames.
   Only matching standard data frames may arrive. Repeat with extended masks,
   RTR-only rules and ignored RTR. Check a partial identifier mask. Replace with
   zero rules and verify rejection; install both zero-mask formats and verify
   acceptance of standard and extended traffic.
3. Transfer lengths 0..8 with distinctive byte patterns, maximum IDs and RTR
   requests. Verify received IDs, flags, length, payload and zeroed unused bytes.
   Compare analyzer capture with transmitted values. Exercise loopback and
   listen-only mode; listen-only must not transmit or acknowledge.
   Inject Classical wire DLC values 9..15 for data and RTR frames; each must
   report length eight, with eight data bytes or an empty RTR data array.
4. Suppress peer acknowledgement and fill all three TX mailboxes. A fourth
   submission returns `BUSY` promptly. Verify status reports pending requests.
   Restore acknowledgement and confirm eventual drain. Repeat with automatic
   retransmission disabled; record native completion/error observations.
5. Fill receive FIFO0, continue traffic without owner reads and verify sticky
   overrun. Drain one frame per call, confirm empty `BUSY`, then observe that
   status reads do not clear the overrun indication. Deinit/init clears it.
6. Generate warning and error-passive conditions and compare status with native
   ESR error counters and LEC. Verify a status snapshot does not falsely promise
   atomicity across rapidly changing native counters.
7. Induce bus-off. Observe `BUS_OFF`, no accepted TX submissions and no automatic
   return to bus operation. Request recovery once; repeated requests while the
   bus idle sequence remains incomplete must return `BUSY` without restarting.
   Keep polling state while withholding, then permitting, the required recessive
   sequence. Confirm `RECOVERING` changes to active/passive only after hardware
   clears bus-off. Verify new TX works and an already completed request never
   restarts without a new bus-off and explicit request.
8. Stop with all TX mailboxes pending. Confirm abort retirement and stopped
   state before success, no later transmitted frame, and explicit draining of
   any pre-stop receive frames. Restart and confirm a fresh transmission works.
9. Claim the other physical controller with a second handle while the first is
   stopped, running, faulted or recovering. Every attempt returns `BUSY` without
   modifying clocks, filters or the live owner. After successful deinit, repeat
   the complete smoke sequence using the other available controller.
10. Use a deliberately insufficient observation budget or controlled clock fault
    to exercise a transition timeout. Confirm bounded return, `FAULT`, retained
    storage/domain ownership and rejection of a competing handle. Restore the
    clock, retry stop/deinit, then initialize a new handle successfully. Repeated
    init must not change a live owner; repeated zero-handle deinit succeeds.
    When init itself times out, only deinit is legal: status/stop return `STATE`
    until a subsequent initialization succeeds. Test stop/status `FAULT` behavior
    using a transition failure after successful init.
11. Record CAN clock-gate bits before init, verify required gates remain enabled
    throughout ownership, and verify successful deinit restores the original
    bits. Confirm complete-domain reset discards receive entries on deinit and
    leaves unrelated peripheral gates unchanged.

## STM32F4 Pass Criteria and Limits

Record API results, analyzer traces and counter observations for each step.
Stop/deinit must establish quiescence before board resources are released.
Measure control-call elapsed time under representative preemption and record it
separately from register observation counts; the API does not promise a
millisecond deadline. Validate receive scheduling against actual bus load and
the three-frame FIFO capacity.

No software timer or timestamp comparison drives CAN recovery, so tick rollover
tests do not apply to this backend. The procedures above cover hardware
completion, fault cleanup and explicit recovery state. CAN FD,
CAN3, simultaneous CAN1/CAN2, PM registration and ISO-TP/CANopen are unsupported
and are not acceptance claims for this implementation.

## ESP32 Integration and Equipment

- Use classic ESP32 with ESP-IDF v5.3.4, an external 3.3 V-compatible CAN
  transceiver, a correctly terminated bus and an independent ACK-capable peer.
  Record silicon revision, SDK commit, SDK errata configuration, board pins,
  actual APB frequency, timing, bitrate and native RX queue capacity.
- Enable CAN with native `idf::driver`; confirm the HAL/backend target does not link
  Esy OS/middleware targets even though TWAI uses the SDK kernel internally.
  Set physical index 1, distinct valid `port.tx_pin`/`rx_pin`, nonzero
  `port.rx_queue_capacity`, and supported bit timing. The generic default
  prescaler one is invalid on TWAI. At 80 MHz, `{10,13,2,1}` is 500 kbit/s.
- One non-ISR owner drives every API. It owns both pins and the transceiver;
  no independent native TWAI/GPIO owner may reconfigure them until successful
  deinit. Keep native silicon-errata workarounds enabled.
- Compile/link the real SDK provider and exercise enabled/disabled feature
  assembly. A dormant native probe establishes compatibility only; it is not
  hardware or runtime evidence.

## ESP32 Hardware Procedure

1. Initialize a zero handle and confirm stopped state, no traffic and capability
   mask `LISTEN_ONLY | FILTER`. Zero rules must deliver no frame. Reject FD,
   internal loopback, invalid pins, equal pins, zero queue capacity, invalid
   segments/SJW and unsupported prescalers before native installation. Retry a
   valid configuration after each rejection. Check the revision-dependent
   prescaler extension where applicable.
2. Install standard/extended masks and data-only/RTR-only rules while stopped.
   Include same low ID bits in both formats and opposite RTR values. Confirm
   exact matches only, zeroed unused data bytes, lengths 0..8, maximum IDs and
   Classical wire DLC 9..15 normalized to eight. Replace all 28 rules and reject
   a 29th without changing the accepted set. Overwrite the caller filter array
   after success and confirm the backend retained its copy.
3. Queue an unmatched frame followed by a matching frame. The first receive
   returns `BUSY` without changing output; the next returns the matching value.
   Verify one raw entry and at most 28 rule checks per call. `rx_pending` counts
   raw queued frames, including rejected ones. Saturate RX with both matching
   and unrelated traffic and verify sticky overrun across status reads.
4. Suppress ACK and occupy the single native TX buffer. The next send must
   return `BUSY`, with no software TX backlog. Restore ACK and confirm drain.
   Repeat single-shot transmission with `auto_retransmit=false`. No submitted
   frame buffer remains borrowed after send returns.
5. Select listen-only, then transmit from an independent node. Reception must
   work but this controller must neither transmit nor acknowledge. Native
   listen-only error counters are unavailable and read as zero. Normal-mode
   warning/passive status must follow native TEC/REC; `last_error` stays zero.
6. Stop while traffic is pending. Confirm electrical quiescence and no later
   TX. Drain required queued RX while stopped. Start resets any unread native
   RX queue; check that this is not mistaken for frame-preserving restart.
7. Force bus-off and hold the bus dominant. Verify `BUS_OFF`, rejected send,
   `recover=NOT_SUPPORTED`, and no automatic revival. Stop must succeed while
   remaining electrically silent; start then returns `STATE`. Deinit must
   succeed directly from BUS_OFF, without awaiting recessive bits. Reinitialize
   after fixing the bus and confirm normal communication. Repeat direct deinit
   from BUS_OFF without a preceding stop. Native bus-off arriving between stop's
   status snapshot and stop call must still permit quiet cleanup.
8. Compete with a second facade handle and a separately installed native driver.
   Require `BUSY` without adopting/reconfiguring the existing owner. Repeated
   init returns `STATE`; successful deinit zeroes the handle, zero deinit is
   idempotent, and a later independent owner can initialize.
9. Inject native entry failures before mutation at install/start/stop/uninstall.
   Install failure must release partial SDK resources. Later control/cleanup
   failures retain handle, native owner and pins; retry stop/deinit after removing
   the fault. Failed deinit must reject further data/start rather than clearing
   live resources. Measure native heap/queue/IRQ usage across repeated lifecycle
   cycles and verify that data/filter/status operations create no SDK resources.
10. For OTA, the application owner must stop CAN and acknowledge quiescence before
    any Flash step that depends on silence. Confirm TX, SDK RX IRQ, transceiver
    outputs and watchdog policy are handled by that owner. Resume/reinitialize
    only under its explicit post-OTA policy. If a product instead requires CAN
    during cache-off Flash activity, separately qualify native IRAM configuration,
    queue capacity, sustained bus load and SDK errata behavior; this plan does
    not claim that parallel mode is safe.

## ESP32 Pass Criteria and Limits

Record analyzer traces, API results, SDK status, memory/IRQ measurements and
owner scheduling under real bus load. Zero native waits and fixed frame/filter
budgets do not establish a wall-clock deadline. Native queues and errata may
lose frames; all traffic must enter the receive scheduling budget before
software filtering. No software timer or timestamp drives this provider.
FD/BRS, internal loopback, explicit recovery, bare-metal use, PM registration
and per-frame TX-completion callbacks remain unsupported.
