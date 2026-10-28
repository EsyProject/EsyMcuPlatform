<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# STM32F4 CAN Hardware Verification

## Purpose

Verify native bxCAN frame transfer, acceptance filters, diagnostics, ownership,
bounded control transitions and manual bus-off recovery. Host facade tests and
source checks do not establish electrical behavior or hardware completion.

## Integration and Equipment

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

## Target Build Checks

1. Configure the real consumer with explicit `ESY_TARGET_DEVICE`,
   `ENABLE_ESY_CAN=ON` and the STM32F4 provider. Build and link its firmware with
   the embedded toolchain; inspect that both CAN HAL and backend sources are
   included. Repeat with the feature off and confirm those sources are absent.
2. Build the enabled module with `ESY_RTOS=None` and no OS/DMA/system feature
   dependencies. Repeat in the intended RTOS firmware with its actual owner.
3. Confirm unsupported target selection fails configuration clearly. Confirm
   enabling FD returns `NOT_SUPPORTED` before any controller change.

## Hardware Procedure

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

## Pass Criteria and Limits

Record API results, analyzer traces and counter observations for each step.
Stop/deinit must establish quiescence before board resources are released.
Measure control-call elapsed time under representative preemption and record it
separately from register observation counts; the API does not promise a
millisecond deadline. Validate receive scheduling against actual bus load and
the three-frame FIFO capacity.

No software timer or timestamp comparison drives CAN recovery, so tick rollover
tests do not apply to this backend. The procedures above cover hardware
completion, fault cleanup and explicit recovery state. CAN FD, ESP32 TWAI,
CAN3, simultaneous CAN1/CAN2, PM registration and ISO-TP/CANopen are unsupported
and are not acceptance claims for this implementation.
