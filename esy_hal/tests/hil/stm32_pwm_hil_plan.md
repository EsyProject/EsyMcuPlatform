<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# STM32F4 PWM Clock, Period and DMA HIL Plan

## Purpose

Verify physical PWM frequency and duty under the selected STM32F4 RCC clock
tree. SDK compilation checks register/provider compatibility; it does not
establish a measured output frequency or correct board pin configuration.

## Integration and Ownership

- Enable PWM and its HAL dependencies. Use one owner for timer configuration,
  channel operations and RCC transitions. Do not share the selected timer with
  logical timers or timer capture; PSC/ARR are shared by all of its PWM channels.
- The board supplies the selected timer/channel, output pin mux, clock enable
  and hardware initialization. Exercise one APB1 timer and one APB2 timer where
  available, respecting the device's clock and pin limits.
- Keep the clock tree fixed while output is active. Stop output before an RCC
  change, publish the new clock configuration, reconfigure PWM frequency and
  restart. Clock changes and other channel owners must not race configuration.
- Use an external frequency counter or logic analyzer. Record MCU/board,
  firmware revision, SDK/toolchain, HCLK, PCLK1/PCLK2, APB dividers, TIMPRE,
  timer/channel, PSC/ARR/CCR and measurement uncertainty.

## Target Build Checks

Compile against real STM32 headers with warnings as errors. Include a device
without TIMPRE, such as STM32F407, and one with TIMPRE, such as STM32F429/F446,
when claiming both clock paths. Include F401/F411 to verify that unavailable
TIM8 resources are not referenced. Link the actual board's timer and TIMEx
providers. HAL wrapper forwarding tests do not replace these target checks.

## Hardware Procedure

1. Select legal HCLK/APB settings for divider 1, 2, 4 and 8 on each tested bus.
   On TIMPRE-capable parts repeat with TIMPRE clear and set. With TIMPRE clear,
   the timer input is PCLK at divider 1 and 2*PCLK otherwise. With TIMPRE set it
   is HCLK at dividers 1/2/4 and 4*PCLK otherwise. Read the selected reference
   manual and verify the device's RCC configuration against these cases.
2. Initialize a 1 kHz, 50% PWM and measure frequency/duty. Compute the expected
   frequency from the recorded timer input divided by `(PSC+1)*(ARR+1)` using
   wide arithmetic. Verify the divider-1 case and TIMPRE cases use their
   selected timer-clock multiplier rather than an unconditional 2*PCLK.
3. Repeat with low and high frequencies on a 16-bit timer and TIM2/TIM5 when
   available. Include a request requiring PSC greater than zero, a request
   whose period lands near the 16-bit ARR limit, and a frequency that does not
   divide the input clock exactly. Check integer quantization against the actual
   PSC/ARR rather than requiring every requested frequency to be exact.
4. Change frequency while enabled and while disabled. Preserve the duty ratio
   within compare-tick quantization; disabled output must remain disabled.
   Reject zero and above-input-clock requests before changing live registers,
   output state or the other active channels' settings.
5. Exercise 0%, 25%, 50%, near-100% and 100% duty. For a maximum ARR, an exact
   100% request requires a compare value outside the timer's register width and
   must return ESY_ERR_NOT_SUPPORTED without wrapping CCR to zero. Other
   representable duties must succeed. A fractional compare is truncated only
   after wide period scaling, and its integer part must fit the register.
6. For the otherwise unreachable 32-bit arithmetic boundary, stop output and
   use a debugger to install ARR=UINT32_MAX before the duty/frequency calls.
   Verify a 50% compare becomes 2^31 and a 100% request is rejected. Reconfigure
   to an ordinary frequency and verify the preserved duty and absence of a
   division by zero. Treat this as boundary injection, not a normal generated
   clock configuration; never alter registers concurrently with active calls.
7. Stop output, change the clock legally, reconfigure and restart. Verify the
   new frequency uses the new APB/TIMPRE state. Confirm initialization rejects
   zero/unrepresentable clock inputs. Follow the DMA checks below separately;
   clock accuracy does not establish DMA routing or callback retirement.

## DMA Request and Retirement Procedure

1. Give the PWM owner exclusive use of one timer and one board-routed DMA
   stream/request pair. Install the real DMA IRQ vectors and record timer,
   channel, DMA stream/channel, IRQ priority and provider revision. Keep that
   stream reserved until `esy_hal_pwm_abort_dma` succeeds or a terminal callback
   delivered by `esy_hal_pwm_poll` returns. Do not reconfigure it through the
   generic DMA API or another peripheral while PWM owns it.
2. Start from disabled output. Submit an aligned buffer with byte length and
   16-bit elements, then repeat with 32-bit elements on TIM2/TIM5. Observe DMA
   NDTR equals byte length divided by element size. Reject partial elements,
   misalignment, zero length, too many beats and 32-bit elements on a 16-bit
   timer without enabling CCxDE or losing the original buffer ownership.
3. Record CR2.CCDS before start. Verify it selects update events while streaming,
   and only the selected CCxDE bit is enabled. Test initial CCR=0 and CCR=ARR+1
   where representable: update requests must continue regardless of compare
   matches. Capture decreasing NDTR and changing CCR; a successful call with no
   requests is a failure. Record any preload delay between CCR writes and pins.
4. For finite DMA, inspect IRQ entry/exit and confirm CCxDE stops on the
   terminal event while PWM output continues to latch and hold the final duty. The IRQ must not wait in generic DMA abort or call the user.
   Before owner polling, mutation/unbind remains busy and the registered device
   retains its usage reference even when hardware NDTR/BUSY is already zero.
   Poll on the owner, verify generic callback storage is retired first, then one
   completion callback occurs and the usage reference drops after it returns.
   With a debugger, inject TC between the poll's initial event load and its
   notification exchange. A finite TC must remain pending for terminal
   retirement on the next poll, never take the circular notification path.
   PWM output remains enabled and vetoes runtime idle until explicitly disabled.
   Finite completion reports buffer consumption, not N complete output periods:
   the last compare becomes effective at its next preload update and then holds.
   Disable output explicitly only after the application is ready to stop it.
5. Run circular DMA for multiple cycles. Both `loop=true` and descriptor
   `circular=true` must select native circular mode. Poll regularly and observe
   repeated half/full callbacks on the owner without releasing the DMA borrow
   or PM reference. Slow polling may coalesce repeated notifications; it must
   not alter native waveform repetition. The application remains responsible
   for DMA-safe buffer updates; coalesced callbacks are not a per-cycle counter.
6. Abort finite and circular runs at several positions. Verify the request is
   disabled before DMA retirement, output is disabled, prior CCDS is restored,
   no pending callback is delivered after success, and restart uses the new
   buffer. Idle abort is idempotent and leaves ordinary non-DMA output alone.
7. Inject DMA transfer/FIFO errors, including a simultaneous completion flag.
   Error wins, CCxDE/output stop in IRQ, and the owner poll retires DMA before
   delivering the error callback. No success callback may follow that terminal
   error. Test partial setup/start failure and abort failure: do not clear the
   handle, reset board pins, drop its device reference or reuse buffer/route
   until a retry successfully retires the driver.
8. Invoke controls/abort/poll/deinit from a DMA user callback and verify reentry
   is rejected. Keep the handle and callback argument alive until callback
   return. Repeat normal completion, error and abort followed by graph deinit,
   typed unbind, hardware deinit and reinit; confirm there is no stale callback.
9. Use a second PWM handle mapped to the same physical DMA resource with a
   different request selector. Submission must reject that alias while the
   first still owns it. Separately verify that an unsupported timer (for
   example TIM9 without CC DMA) returns NOT_SUPPORTED rather than starting a
   stream that can never receive timer requests.

## Wrap IRQ Lifecycle Procedure

Reserve the complete selected timer and its update vector; route that vector
once to `esy_hal_pwm_handle_irq` for one initialized logical index on that timer.
The backend notifies all registered PWM channels on the same timer. Do not
dispatch the same UPDATE through a second native handler. Use one task owner on the IRQ core;
callbacks only record a counter or post a notification and never reenter PWM.

1. Register a wrap callback, enable output and observe one notification per
   delivered UPDATE. Verify UIF is cleared before the callback and the foreground
   continues to run. No native flag acknowledgement is required from the caller.
2. Invoke the dispatch entry with no UIF, with UPDATE disabled, and with an
   unrelated timer status flag. None may notify or clear an unrelated flag.
3. Pass NULL to unregister while a wrap is pending. Verify UIE is disabled, the
   owned pending UPDATE is cleared and no later callback uses the old argument.
   Register another callback/argument and confirm no stale event is replayed.
4. Deinitialize while output and wrap delivery are active, then reclaim the
   callback argument. Verify no UPDATE IRQ storm, no late access, correct NVIC
   retirement and successful reinitialization. Inject native PWM cleanup failure:
   the handle remains retained, but the unregistered callback stays retired.
5. Configure two logical indices on different channels of the same TIM under
   the same owner and IRQ priority, each with a distinct callback counter. A
   single UPDATE dispatched through either index must acknowledge UIF once and
   increment both counters once. Immediately forward the other index without a
   new UPDATE; neither counter may increment. Unregister one callback and verify
   the peer continues to receive UPDATE notifications. Unregister the final
   callback and verify UIE/NVIC retirement. Reject conflicting priorities before
   altering either callback. Keep output stopped during timer-wide configuration
   and deinitialization; channel configuration arbitration is still the owner's
   responsibility.

## Evidence and Limits

Preserve register snapshots and raw timing captures for each clock case.
Oscillator tolerance, integer PSC/ARR and compare quantization bound accuracy;
runtime clock changes require explicit owner coordination. Target SDK
compilation and HAL fake tests cover API compatibility and facade behavior;
register snapshots and captures cover physical DMA routing and waveform timing.
Finite DMA retirement leaves ordinary PWM running at its final compare; it
marks buffer consumption and does not promise a waveform period has ended. Complementary outputs, independent shared-channel arbitration and
low-power transitions remain outside this plan.
