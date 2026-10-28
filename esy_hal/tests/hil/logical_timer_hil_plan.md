<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# STM32F4 Logical Timer HIL Plan

## Purpose

Verify independent logical deadlines, cancellation, callback reuse, counter wrap
and initialization cleanup on real STM32F4 hardware. Target compilation and HAL
wrapper tests do not establish compare-interrupt behavior or physical accuracy.

## Integration and Resource Ownership

- Enable the consumer's timer feature and initialize the HAL timer once from its
  startup/task owner. Serialize task calls on the same core as the timer IRQs.
- Reserve complete TIM2 and TIM5 peripherals, their CC1 channels, clocks and IRQ
  lifecycles. TIM2 serves millisecond timers; TIM5 serves microsecond timers.
  Both count at 1 MHz with a full 32-bit range. Neither may be shared with timer
  capture, PWM, another driver, or an independently managed IRQ owner.
- Link the platform's optional `esy_interrupts` target for its TIM2/TIM5 vectors,
  or route custom TIM2 IRQ to `esy_hal_timer_handle_ms_irq()` and TIM5 IRQ to
  `esy_hal_timer_handle_us_irq()`. Use exactly one vector owner. These entries
  inspect and retire CC1 directly;
  do not additionally dispatch the same interrupt through `HAL_TIM_IRQHandler()`.
  No global vendor period-elapsed callback is installed by this backend.
- Keep APB timer clocks fixed and running while initialized. CPU interrupt
  service must occur within 2^31 microseconds, the signed deadline comparison
  horizon. STOP or clock changes require owner-controlled timer retirement and
  reinitialization; this backend does not provide a STOP wake source.
- Use bounded ISR callbacks that record timestamps or toggle measurement GPIOs.
  Arguments remain valid until delivery completes or cancellation has retired
  the timer. Cancellation cannot terminate a callback already executing.
- Both modes must use the same board-provided IRQ priority while any slot or
  dispatcher is active. A conflicting priority is rejected before hardware or
  slot changes. This prevents the timer callbacks from preempting each other's
  HAL state publication. A callback that cancels all timers still retains its
  priority until its dispatcher returns. Only task/startup owners and these
  timer callbacks may manage timer lifecycles; unrelated ISRs must not do so.

Record MCU and board, revision, toolchain/HAL versions, RCC/APB/TIMPRE settings,
IRQ routing and priorities, callback duration, and raw logic-analyzer traces.

## Target Build Checks

Compile and link against the actual STM32 SDK. Include a device without TIMPRE,
such as STM32F407, and a device with TIMPRE, such as STM32F429, when claiming both
clock configurations. A variant without 32-bit TIM2 and TIM5 is unsupported and
must fail configuration/compilation rather than substitute a 16-bit counter.
Use warnings as errors for the backend source. Independently exercise
HAL wrapper tests for missed periodic deliveries and wrapped wake deadlines.
For the compare path, verify the selected device reference manual's output
compare, `TIMx_SR.CC1IF` and `TIMx_CCER.CC1E` semantics: the match flag and interrupt
do not require driving an output pin. See ST's
[RM0090](https://www.st.com/resource/en/reference_manual/rm0090-stm32f407-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)
for the STM32F405/407/415/417/427/429 families.

## Hardware Procedure

1. Verify TIM2/TIM5 PSC produces exactly 1 MHz, ARR is `UINT32_MAX`, UIE is off,
   and no compare IRQ runs when no logical timer is active. A 1 microsecond
   periodic timer is a workload request, not the backend's idle tick rate.
2. Start MS periodic timers at 100 and 500 milliseconds. Measure both separately
   for at least ten seconds, then add a third period and repeat. Existing phases
   and rates must survive each new start. Repeat with independent US intervals.
3. Cancel one timer in each mode while others remain active. Only the selected
   callbacks stop; the counters keep running and other deadlines remain intact.
   Cancel an already inactive/invalid ID and verify no effect on active timers.
4. Fill all eight slots across both modes. A ninth start must return
   `ESY_ERR_NO_MEM` without changing any active period, counter, or compare
   schedule. Cancel one and verify a new timer can use the released capacity.
   Also reject zero/negative/out-of-range intervals, invalid source and IRQ
   priority, and a conflicting priority in either mode without affecting live
   slots. While an IRQ callback has canceled every slot, verify that starting
   either mode with another priority still returns `ESY_ERR_BUSY`. After its
   dispatcher returns and no timer is active, a new priority may be selected.
5. Run one-shot and periodic timers with equal and staggered deadlines. No
   callback may run before its deadline; each one-shot fires once. Invoke an IRQ
   entry with no enabled CC1 flag and verify no callback. Verify a stale pending
   compare after cancellation does not deliver the canceled timer.
6. In a one-shot callback start another timer, including when all slots were
   occupied before that one-shot completed. In a periodic callback cancel itself
   and start a replacement. Also cancel another due timer before its dispatch.
   The replacement must survive the old callback's return, and the canceled due
   timer must not be delivered. Exercise both domains and nested higher-priority
   unrelated interrupts while checking callback argument lifetimes.
7. Briefly delay IRQ service across several periodic deadlines, staying within
   the half-range horizon. One delivery per due slot is made in each IRQ; missed
   periods are coalesced and the next deadline preserves the original phase.
   A delayed ISR does not invoke a future timer merely because another is due.
   Measure IRQ duration: at most eight callbacks are dispatched per IRQ, plus
   bounded table scans and compare updates. Callback execution cost is included.
8. Use a halted debugger before starting timers to place the free-running CNT
   near `UINT32_MAX`; do not change CNT while active. Start deadlines crossing
   rollover and verify ordering, one-shot delivery, periodic phase and cancel.
   Include wrap during compare publication and a very short deadline that passes
   before CCR1 is armed; the software-generated compare must prevent a lost wake.
9. Repeat physical timing checks with APB1 prescalers 1, 2, 4 and 8. Where TIMPRE
   exists, test it cleared and set. Clock rates unable to divide exactly to
   1 MHz must fail before acquiring live timer resources.
10. Inject vendor base-init, channel-config and base-start failure for each
    domain. Verify both IRQs disabled, pending events cleared, clocks released,
    no later callback, and successful retry. Repeated init must fail without
    altering live timers. Deinitialize and repeat deinit, then initialize again.

## Public Callback-Start Interleaving on Pico

The shared HAL also permits a callback before start returns. On a Pico consumer,
use very short periodic intervals and bounded scheduler/IRQ timing injection to
exercise this valid schedule on the one owner core:

1. Let one-shot A fire after its start returned. Inside A, start periodic B.
   While A's native slot is still dispatching, B uses another native ID even
   though the HAL can reuse A's completed one-shot slot.
2. After A returns, start periodic C and cancel B. Start periodic D in B's freed
   HAL slot. Make D's first callback run before D's start returns; that callback
   cancels C using C's returned ID.
3. Require C canceled and D still active, with multiple subsequent D callbacks.
   The HAL next-wake query must describe D's period and must not retain C's old
   deadline. Cancel D and require the query to report no remaining timer.

Record the IDs, callback order and start-return markers into fixed storage.
Repeat until the intended interleaving is observed; a run without that schedule
does not cover this regression. Host HAL tests provide deterministic ID/schedule
injection, while this procedure qualifies the actual SDK/IRQ composition.

## Pass Criteria and Limits

Measured deadlines must match the selected clock within reference tolerance and
one microsecond counter quantization, plus measured interrupt latency. MS input
values are converted to microseconds; the maximum is 2147483 ms or
`INT32_MAX` us. More than half a 32-bit counter wrap without service is outside
contract. HAL's next-wake query uses the system clock, which currently has only
millisecond resolution on STM32F4; it is advisory for sub-millisecond deadlines
and is separate from the backend's 1 MHz compare clock.

Record measured interrupt latency and OTA/Flash interrupt blackout periods in
the consumer's service-time budget.
