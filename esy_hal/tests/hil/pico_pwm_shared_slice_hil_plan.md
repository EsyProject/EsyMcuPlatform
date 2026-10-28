<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Pico PWM Shared Slice HIL Plan

## Purpose

Verify physical channel ownership and independent A/B output lifecycle on one
PWM slice. Real SDK compilation establishes API compatibility; the hardware
procedures below cover waveforms, IRQ retirement and DMA quiescence.

## Integration and Ownership

- Use RP2040 and RP2350 Arm consumers with PWM enabled. Supply two distinct
  legal board GPIOs mapping to A/B of one slice, and an additional distinct GPIO
  aliasing the first channel where the board exposes one. Logical indices must
  never map to the same GPIO; all pin configuration/reset belongs to that index.
- One task on one core serializes all public PWM calls across every handle.
  No other driver may configure the slices, output GPIO overrides or that core's
  PWM wrap IRQ. Keep clk_sys unchanged while any slice owner remains alive.
- Start handles with `{0}`. Callbacks must not call PWM APIs. Retain DMA samples,
  callback arguments and handles until a terminal callback returns or successful
  abort/deinit certifies retirement. An IRQ or an idle DMA engine is insufficient.
- Record board, GPIO-to-slice/channel mapping, firmware revision, SDK/toolchain,
  clk_sys, requested/measured frequency and duty, polarity and return status.
  Capture both outputs with a logic analyzer; divider quantization is expected.

## Native Consumer Build

Compile the Pico backend and link public two-handle lifecycle, same/conflicting
frequency, duty, polarity, enable, suspend/resume, wrap handler and DMA calls
with the real SDK. Verify that the PWM gate supplies hardware PWM, GPIO, clocks,
IRQ and synchronization targets without requiring the generic GPIO feature.
Dormant compile/link probes are not hardware execution or return-value evidence.

## Hardware Procedure

1. Initialize A at 1 kHz, 25% duty, normal polarity. Its pin must remain at the
   inactive low level until enabled. Enable it and capture the waveform. Init
   another zero handle at the same logical index must return BUSY before any
   board configure/reset call; the first waveform and handle remain unchanged.
2. Initialize B at 2 kHz. Expect BUSY with A unchanged. Retry B at 1 kHz and
   75% duty; it must initialize without resetting A's counter or compare value.
   B stays inactive until enabled, then both pins run at the common frequency.
   Request the current frequency through either handle and expect an idempotent
   OK. Changing frequency while both handles exist returns BUSY, even if one
   channel is disabled or suspended.
3. Configure a distinct GPIO aliasing A's actual slice/channel under another
   logical index. Init must return BUSY, release only the rejected GPIO, and
   leave both existing pins and slice registers unchanged. This does not test
   duplicate board GPIO assignments, which violate the board ownership contract.
4. Change A's polarity and duty repeatedly while B runs. B's polarity, compare
   setting, counter and frequency must remain unchanged. Repeat in reverse.
   Disable A at different counter phases: A must immediately use its logical
   inactive level while B continues. Re-enable A and verify retained duty and
   polarity. Repeat with inverted polarity, whose inactive level is high.
   Transition edges and preload updates need not be glitch-free.
5. Suspend A while B is running. A must become inactive and receive no wrap
   callbacks; B continues unchanged. Alter B's duty/polarity, then resume A.
   Resume must restore only A's output without restoring stale B compare values
   or resetting the shared counter. Suspend both and verify the counter stops;
   resume in both orders and verify each handle's own enabled/disabled state.
6. Register wrap callbacks for A and B at the same board IRQ priority. Each
   enabled channel receives one notification for each delivered shared wrap IRQ;
   hardware wraps may coalesce while interrupts are masked. Register a callback
   on another slice with a conflicting priority and expect BUSY without changing
   prior handlers. NULL unregisters one channel only. After unregister returns,
   reclaim that callback context and verify no later access. Deinit one channel
   while the peer runs and verify its callback/output continue. Final handler
   removal must disable/release the backend's wrap IRQ on the owner core.
7. Deinit B while A runs. A's waveform must continue. A can now change frequency;
   rejoining B at the old frequency returns BUSY, while a matching request
   succeeds. Deinit both in both orders and reinit at another frequency. Final
   cleanup stops the slice and releases both logical and physical claims.
8. With both A/B handles initialized, DMA output on either must return BUSY even
   when both pins are disabled. Neither waveform, compare nor peer callback may
   change. Remove B, start finite aligned 16-bit samples on disabled A, and verify
   a new B init returns BUSY through DMA completion IRQ and until owner poll
   retires the dispatcher and invokes the terminal callback.
9. After finite completion and poll, verify A remains enabled at its final sample.
   Disable it, join B at the same frequency, and verify ordinary shared operation.
   Repeat a sole-owner DMA transfer with explicit abort and DMA bus-error fault
   injection: output must become inactive; no peer can be admitted before safe
   retirement. Failed abort retains ownership and storage for retry, suppressing
   buffer reuse. Successful abort suppresses pending user DMA notifications.
10. Reject zero frequency, frequency above clk_sys, invalid GPIO/index, duty
    above 100%, and a divider below the minimum representable frequency. At a
    full 65536-tick period, reject an unrepresentable 100% compare before changing
    live timing. Each rejection preserves existing owners and permits a valid
    retry on the zero handle. Invalid logical indices and generic zero-frequency
    or out-of-range-duty arguments must fail before any board configure call.

## Evidence and Limits

Keep dual-pin captures and register/IRQ traces for peer initialization, polarity,
disable, suspend, deinit, alias rejection and DMA retirement. Exercise both
channel orders and at least two slices on RP2040 and RP2350. Error injection must
respect the native operation's pre/post-mutation behavior. This plan does not
qualify glitch-free output, cross-core control, direct SDK sharing, circular DMA,
complementary output, dead time or clock changes while a slice is owned.
