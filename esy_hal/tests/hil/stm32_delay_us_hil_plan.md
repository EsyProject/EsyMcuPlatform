<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# STM32F4 Microsecond Delay HIL Plan

## Purpose

Verify DWT-based synchronous delays using an external timing instrument. An ARM
build confirms CMSIS/provider compatibility; it does not measure actual timing.
The [Arm Cortex-M4 reference manual](https://documentation-service.arm.com/static/5fce431be167456a35b36ade)
describes the DWT cycle counter and its trace configuration.

## Integration and Contracts

Enable system support and complete successful HAL system initialization before
calling the delay. Startup owns enabling CYCCNT, leaves its current value intact,
and keeps it enabled. Profilers may read it but must not reset or disable it.
Record MCU, SDK/toolchain versions, firmware revision, oscillator tolerance,
HCLK/SYSCLK and AHB divider, debugger state and measurement resolution.

The clock owner maintains SystemCoreClock and serializes clock/sleep transitions
with delays. The delay reads HCLK each call; it does not configure product clocks.
It neither consumes a TIM peripheral nor changes the interrupt mask. Interrupts
and scheduling may extend the wait, so this is for short synchronous hardware
settling, not precise pulse generation. Do not use it for low-power timekeeping.
The STM32F4 system timestamp remains millisecond-resolution.

## Target Build Checks

Compile the changed source with warnings as errors using actual Cortex-M4 CMSIS
headers. Include STM32F401, F407 and F429/F446 groups when claiming those variants.
Link a real consumer with its startup, clock provider and system support. Verify
the delay symbol is retained and its disassembly reads CYCCNT rather than using
HAL_GetTick, HAL_Delay, an empty loop calibrated by instruction count, or a TIM.

## Hardware Procedure

1. Boot without a debugger. Check that successful init enables TRCENA/CYCCNTENA
   and CYCCNT advances. Repeat with DWT already enabled and a nonzero CYCCNT;
   init must not zero it or modify unrelated trace-control bits. On an injected
   missing/nonadvancing counter, init must return ESY_ERR_NOT_SUPPORTED and
   restore only the enable bits it acquired. Do not proceed with delays on failure.
2. Toggle a measurement GPIO around delay_us calls for 0, 1, 2, 5, 10, 100 and
   1000 microseconds. Measure and record GPIO/call overhead separately. Nonzero
   waits must last at least the request within oscillator/instrument tolerance;
   overhead is expected, especially for 1 microsecond requests. Zero has no wait.
3. Repeat with different supported HCLK values, including an AHB divider greater
   than one and an HCLK not divisible by 1 MHz if the board supports it. Update
   SystemCoreClock through the clock owner before each run. This distinguishes
   processor-clock conversion from an incorrect SYSCLK or truncated-MHz scale.
4. Call short delays from a supported ISR and with PRIMASK already set. Verify
   completion without SysTick progress and verify the entry mask is preserved.
   Record observations in fixed storage; do not log or block on a driver while
   masked. Restore the mask before checking or printing results.
5. Inject a bounded interrupt/task preemption during a delay. The call may return
   late and must not shorten the requested wait. Repeat with a nested short ISR
   delay; both readers must work without resetting the shared counter.
6. With no delay active, use the debugger to place CYCCNT near UINT32_MAX, then
   run across wrap and check the measured wait. Never reset CYCCNT concurrently
   with a delay. At high HCLK, select a request longer than
   floor((UINT32_MAX / 2) * 1000000 / HCLK) microseconds to exercise chunking.
   Review the 64-bit conversion for UINT32_MAX microseconds; a full-duration
   maximum request is not required for routine hardware acceptance.
7. With logical timers enabled, verify their periods/counters are unchanged by
   short delays. Verify STOP clock restoration leaves the system-owned DWT
   enable bits intact and a subsequent delay still works after the restored
   clock is published. Do not enter sleep while a delay is in progress.
8. Repeat with enable_us_tick false. Delays remain available after successful
   init; that setting does not upgrade STM32F4 time_32_us timestamp resolution.

## Evidence and Limits

Preserve timing traces, init-failure observations, wrap/chunk results and IRQ
mask readings with the consumer's target acceptance evidence. CYCCNT halts with
the processor
or debug clock and is not a sleep-surviving wall-clock source. Timing accuracy
depends on the board clock and interrupt latency.
