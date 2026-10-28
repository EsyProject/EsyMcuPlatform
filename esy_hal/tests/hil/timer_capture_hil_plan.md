<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Timer Capture Ownership and Sampling HIL Plan

## Purpose

Verify microsecond capture, exclusive ownership, coherent ISR publication, and
retryable cleanup on real Pico, ESP32, and STM32F4 hardware. STM32F4 also needs
APB timer clock selection and hardware-counter rollover checks.
Target compilation and host HAL fakes do not establish physical timing accuracy.

## Integration and Equipment

- Build the consumer with `ENABLE_ESY_TIMER_CAPTURE` and its required HAL
  features enabled in the consumer's `esy_feature_import.cmake`.
- Supply the GPIO and any timer/channel identities in `config.inner` before
  initialization. The board configure hook must validate its physical mapping
  without replacing those identities; failure must leave hardware unchanged.
- On Pico and ESP32, run capture lifecycle on the GPIO interrupt dispatch core;
  use the same core that installs the shared GPIO IRQ service. Lifecycle calls
  from ISR callbacks are forbidden. Other direct users of the same GPIO or timer
  must remain stopped while a capture handle owns it.
- For STM32F4, reserve one whole supported timer, its input channel, and its IRQ lifecycle.
  Do not share it with PWM, logical timers, or another capture handle. A shared
  vector must not have another independently managed owner.
- Supply clock and GPIO setup/cleanup through the board's HAL MSP hooks, and
  route the vector to `esy_hb_timer_capture_handle_irq()` with the selected
  physical capture index.
- Use an accurate external pulse generator and a logic analyzer. Keep the clock
  tree fixed while a capture handle is initialized.
- Initialize the public handle with `{0}`; callbacks must be bounded ISR code
  that only records measurements or posts a permitted notification.

Record the board and MCU, firmware revision, toolchain, HAL version, timer and
channel, RCC/APB/TIMPRE settings, reference-clock accuracy, and raw captures.

## Target Build Checks

Compile Pico and ESP32 capture with their native SDK headers and the verified
32-bit C11 atomic provider. Confirm the final firmware links any required SDK
atomic primitives; public-header or host fake builds alone are insufficient.

Compile the real STM32F4 capture backend with the consumer's ARM toolchain and vendor
headers. Exercise a device without TIMPRE, such as STM32F407, and a device with
TIMPRE, such as STM32F429, when claiming support for both clock configurations.
Use warnings as errors to catch declaration/definition mismatches. Include
the final firmware link and actual vector routing in board acceptance evidence.

## Common Ownership and Publication Procedure

1. With no signal, query value and interval: both return zero with `ESY_OK`.
   After one edge the interval remains zero. After two edges it equals their
   unsigned microsecond difference, including an edge whose timestamp is zero.
2. Feed a fixed 100-microsecond period and continuously query while the ISR
   captures. After the second edge every interval must be 100 microseconds
   within the measured ISR timestamp jitter on Pico/ESP32. There must be no
   artificial zero or doubled interval from mixing capture generations.
   Separate timestamp and interval calls are independent snapshots; a pair
   spanning an interrupt may correspond to different edges.
3. Repeat near the 32-bit microsecond wrap on Pico/ESP32, or with a provider-
   supported timebase fixture. Consecutive intervals must remain correct when
   the gap is less than one wrap. No 64-bit task read should be required.
4. Keep handle A active. Try handle B on the same physical slot, then a distinct
   slot with the same GPIO. On STM32F4 also try another slot/channel on A's
   timer. Each returns `ESY_ERR_BUSY` before its board configure/reset hook;
   A's signal, callbacks, routing, and measurements must continue unchanged.
5. Suspend A through its graph owner. Repeat the competing initialization: it
   still returns `ESY_ERR_BUSY`. Resume A and verify capture in its new domain.
   Only successful final deinit permits another handle to claim its identity.
6. Reject a board mapping with no hardware changes. Inject native attach/init
   failure, and verify only successfully configured resources are reset. Where
   the provider can fail cleanup, inject stop/deinit failure and check the
   returned status, retained handle/claim, and a later successful deinit retry.
   Storage and callback context stay alive through every unsuccessful cleanup.
   During graph suspend, a teardown error returns `ESY_ERR_STATE` and faults the
   graph, since capture or IRQ state may already have changed. Follow system
   recovery/reset; a faulted registry does not permit local unregister/deinit.
   Do not clear it to bypass ownership or resume ordinary I/O.
7. Deinitialize under an input pulse train. After success, verify no callback
   or borrowed IRQ argument is used. Reuse the old storage and reinitialize
   on the same input. On Pico/ESP32, cross-core suspend/deinit attempts must
   return `ESY_ERR_STATE` without releasing ownership; retry on the owner core.

## STM32F4 Clock and Hardware Capture Procedure

1. Select a valid channel on TIM2 or TIM5. Verify a 1 MHz counter, an upcounter,
   and a full 32-bit auto-reload range. Apply edges 1000 microseconds apart and
   confirm the reported interval against the external reference.
2. Repeat on a supported 16-bit timer, such as TIM3, with a full 16-bit range.
   The timestamp wraps every 65,536 microseconds. Place consecutive captures
   across rollover with a known interval below that limit and verify the same
   interval in microseconds. A gap of one full wrap or more is outside contract.
3. Exercise APB prescalers of 1 and greater than 1 on APB1 and APB2 capture
   timers. Where supported, repeat with TIMPRE cleared and set. Verify the
   timer kernel frequency and the physical interval for each clock tree.
4. Capture an edge at counter value zero, then the next edge. Verify that zero
   is a valid timestamp and that the second interval is calculated normally.
   Before the second capture the STM32F4 interval query returns zero.
5. Initialize an already initialized handle, then a second handle using either
   the same physical slot or the same timer. The first returns `ESY_ERR_STATE`;
   the latter attempts return `ESY_ERR_BUSY` without disturbing the live capture
   or invoking board configure/reset hooks for the rejected owner.
6. Supply an unsupported channel or a clock rate that cannot divide exactly
   to 1 MHz. Verify rejection without allocating a slot or enabling capture.
   A subsequent valid initialization must succeed.
7. Inject HAL initialization, channel configuration, and start failures. Verify
   disabled capture interrupts, released MSP resources, no callback after
   failure, and successful retry. Then deinitialize a live handle, verify no
   later callback or pending IRQ, and initialize it again.
8. If a device graph adapter is used, suspend and resume through its owner.
   Verify the timebase is restored and treat the resumed timestamps as a new
   domain. Drain all callers and unregister before final deinitialization.

## Pass Criteria and Limits

- Intervals match the pulse generator within its accuracy and one microsecond
  of counter quantization, plus any documented board input timing uncertainty.
- Callback timestamps and queried timestamps use the same microsecond counter
  domain. Separate value and interval queries are independent snapshots.
- Rollover is correct for the actual 16-bit or 32-bit counter width; multiple
  wraps are not inferred or reported as an absolute elapsed duration.
- Rejected competing initialization leaves the existing owner unchanged. Failed
  initialization releases its own resources when cleanup succeeds; otherwise it
  retains the claim and reports the cleanup error until an explicit retry succeeds.
- Successful deinitialization retires IRQ and callback access before storage is
  reused. Suspend retires native access while retaining physical ownership.

The consumer selects board resources, a generator and a flashing/debug probe.
