<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# ESP32 PWM Shared Timer HIL Plan

## Purpose

Verify that LEDC timer ownership follows the live PWM channels, conflicting
frequencies leave peers unchanged, and failed setup or cleanup remains
recoverable. Native SDK compilation establishes API and linkage compatibility;
it does not establish waveform timing or successful hardware cleanup.

## Integration and Ownership

- Use the ESP32 port with PWM enabled and the native LEDC/GPIO providers. The
  current implementation uses 13-bit low-speed LEDC timers. Channels 0/1 share
  timer 0; 2/3, 4/5 and 6/7 share the remaining timers.
- The board supplies distinct legal output pins for the tested channels. Use
  one owner to serialize every PWM call, including calls on different handles.
  Do not reconfigure these LEDC channels or timers through another SDK user.
- Initialize handles with `{0}` and use the public HAL lifecycle. Failed
  cleanup retains the handle and board pin claim; retry deinit before reuse.
- Record the board, pins, firmware revision, SDK/toolchain version, requested
  frequency, measured frequency/duty and native error results. Use a logic
  analyzer or oscilloscope on both channels. Divider quantization means the
  measured frequency need not equal the exact requested integer.

## Native Consumer Build

Compile and link public calls for two handles on one timer, same/conflicting
frequency init, repeated init, set-wrap, enable, deinit and reinit at a different
frequency. Include the native `ledc_timer_config` deconfigure field and pause
API in the real SDK build. A dormant compile/link probe must not be reported as
executed HIL or as verification of the return values below.

## Hardware Procedure

1. Initialize channel 0 at 1 kHz and 25% duty. Initialization must leave its
   output disabled. Enable it and record the steady waveform. Repeated init on
   the same live handle must return STATE without changing the output.
2. Initialize channel 1 at 2 kHz. Expect BUSY before native timer or channel
   configuration. Channel 0 must retain its frequency, duty and output state.
   Retry the same zero handle at 1 kHz; initialization must now succeed, and
   explicitly enabling it must produce the same frequency with its own duty.
3. With both handles alive, set either handle to its existing 1 kHz request;
   expect OK without reprogramming/resetting the timer. Request 2 kHz through
   either handle; expect BUSY and unchanged waveforms on both pins. Repeat with
   the peer disabled: ownership, rather than output-enable state, still vetoes
   a conflicting timer change.
4. Deinitialize channel 1 while channel 0 is enabled. Channel 0 must continue
   without a timer reset, pause or duty change. Channel 0 is now the sole owner;
   changing it to 2 kHz must succeed. A new channel 1 request at the obsolete
   1 kHz must fail BUSY; a matching 2 kHz request must succeed.
5. Deinitialize both handles. Verify that final cleanup pauses and deconfigures
   the timer only after the final channel stops. Initialize channel 0 at 3 kHz
   and then enable it; verify that the new waveform uses the new request rather
   than the original cached frequency. Repeat release/reinit in the reverse
   channel order and repeat on a second timer pair.
6. Reject zero frequency, duty above 100%, an input-only/invalid GPIO and an
   invalid physical index before acquiring a timer or programming a channel.
   Follow each failure with a valid init on the same handle. Request a
   frequency unachievable at 13-bit resolution; a native timer error must leave
   the channel unowned and permit a later legal request. An existing peer must
   remain unaffected by every rejected request.
7. As sole owner, request an unachievable frequency through set-wrap. Expect an
   error, the original waveform and original accepted frequency. Joining the
   timer at the original request must still succeed; joining it at the failed
   request must fail BUSY. This checks that the accepted frequency cache changes
   only after successful native reconfiguration.
8. With a debugger, force `ledc_channel_config` to fail at entry before it
   programs a channel. The supported SDK's own error paths also occur before
   channel programming. Verify that rollback releases the acquired timer
   without calling channel-stop for a channel that was never configured. The
   peer case must drop only the failed claimant and preserve the live output.
9. Inject persistent errors at native stop, final timer-pause and final
   timer-deconfigure entry, one stage at a time. Keep the error active across
   the HAL's automatic init-cleanup attempts when testing partial init. Expect
   a cleanup error, preserved handle/pin/timer ownership and rejection of a
   new init at either the same or a conflicting frequency while final timer
   cleanup remains pending. Remove the fault and retry deinit;
   cleanup must complete once, with no owner-count underflow or stale claim.
   A subsequent init at a different frequency must succeed. Stopping a channel
   may already have silenced that channel when a later cleanup stage fails;
   failure does not promise rollback of completed hardware cleanup.
10. Request runtime polarity control and expect NOT_SUPPORTED. Verify that it
    does not change the configured idle level or the level used by a later
    disable/enable sequence. No native timer peer may be affected.
11. After init, call set-duty while disabled and require STATE before any
    native duty/update call. Verify the pin remains at the configured idle
    level. Enable at the original duty, disable, attempt a different nonzero
    duty, then disable again. Both disabled duty calls must preserve the
    original accepted duty and produce no pulses; enabling again must restore
    the original waveform. Repeat with both start-low polarities and with a
    peer channel enabled on the same timer.

## Evidence and Limits

Call the public wrap-handler API with a non-NULL callback on an initialized
channel. Require NOT_SUPPORTED, no callback or native ISR allocation,
unchanged zero optional capability mask, and unchanged
peer output/timer settings. NULL unregister must succeed without changing the
waveforms. If a native registration is retained by a target fault-injection
fixture, fail its release: NULL unregister/deinit must retain the resource for
retry and report failure until release succeeds. LEDC fade completion is not a
period event and must not be substituted for one.

Keep waveform captures and native-call traces for both channels, especially
conflict rejection, peer deinit and the final-owner transition. Error injection
must return before the selected SDK operation mutates hardware; it must not
simulate a documented pre-mutation failure after actually performing the call.
DMA, wrap IRQ, complementary output and low-power support are outside this plan.
