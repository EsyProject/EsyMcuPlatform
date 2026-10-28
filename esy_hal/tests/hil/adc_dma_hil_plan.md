<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# STM32F4 ADC DMA HIL Plan

## Purpose

Verify that ADC conversion, the generic DMA stream owner, callback retirement
and optional device graph leases agree on real hardware. HAL host tests exercise
the public facade with fakes; target compilation and linking do not establish
analog accuracy, stream routing, physical IRQ delivery or error retirement.

## Integration and Equipment

- Enable ADC, DMA, system and their required HAL features through the consumer's
  feature import. Use the official ADC, ADC extension and DMA providers in the
  selected STM32F4 HAL SDK. Link the final firmware, not only object files.
- Select an available native ADC index (1, 2 or 3), valid external channels and
  one documented ADC/DMA request route. The board supplies stream/request,
  channel, actual stream IRQ and IRQ priority in `esy_board_adc_get_dma_info()`.
  Reserve that stream for this ADC; do not use generic DMA prepare/start or
  another peripheral on it while the ADC owns it.
- The ADC MSP hook enables ADC clocks and configures analog GPIO. It must not
  allocate or link another DMA handle: the backend links the native ADC to the
  generic DMA owner's already prepared handle. Board setup owns the pin setup
  for every scan channel, including channels beyond the handle's default channel.
- Link the optional `esy_interrupts` provider for the strong `ADC_IRQHandler`
  and DMA stream handlers. With custom vectors, forward the shared ADC vector
  once to `esy_hb_adc_handle_irq()` and each DMA vector once to
  `esy_hb_dma_handle_irq(dreq, channel)`, without a duplicate handler/provider. The ADC and
  DMA IRQs use the configured stream priority; concurrently active ADCs must
  request the same priority because they share the ADC IRQ and callback state.
- Use a stable, known analog source or signal generator, a voltage reference
  and a logic analyzer for callback/trigger timing. Keep the clock tree fixed.
  Reserve any external trigger timer separately; do not share TIM2/TIM5 with
  the logical timer backend, which owns their whole peripherals.
- Start with zero-initialized handles and a two-byte-aligned destination in
  DMA-accessible SRAM; STM32F4 CCM is unsuitable. Surround buffers with canaries.
  Direct backend callers also require `{0}` before first init; after failed init
  retain that handle and complete deinit before retrying or releasing storage.
  Use one task/startup owner serializing lifecycle across all ADC handles and
  PM transitions; callbacks only record counters/results
  or notify it. They must not abort, reconfigure, deinit or restart ADC work.

Record board/MCU, firmware revision, SDK/toolchain, RCC/ADC clock, request route,
ADC and DMA IRQ priorities, channel/sample timing, source voltage/accuracy, raw
buffer data, callback order and graph usage counts. The backend selects a 12-bit
right-aligned ADC and PCLK2/4; verify this is within the device's ADC clock limit.

## Target Build Checks

Compile ADC and generic DMA against real SDK headers with warnings as errors.
Check at least an ADC1-only device and a multi-ADC device when claiming both.
The final ELF must retain the strong ADC/DMA vectors, native ADC DMA start,
backend ADC callbacks and generic DMA IRQ dispatch. Confirm no second native
DMA handle or competing ADC callback implementation is supplied by the board.

## Hardware Procedure

1. Sample the known voltage with the blocking API, then finite DMA buffers of
   one, two and many samples. Supply byte lengths of 2, 4 and the full buffer
   size. Confirm exact sample count, canaries, one terminal completion, idle
   state before the terminal callback and no later callback or buffer writes.
2. Request a two-channel and a longer scan with distinct source voltages. Verify
   repeated interleaved channel order for complete sequences and one finite
   terminal callback. Reject zero/odd lengths, incomplete scan groups, unaligned
   destinations, more than 16 channels and more than 65,535 samples without
   starting DMA or changing the active owner. Verify the caller's descriptor and
   channel list can be reused immediately after submission.
3. Verify software-triggered finite buffers terminate at the requested length.
   Configure a supported external trigger and prove one conversion/sequence per
   trigger with no unsolicited conversion between edges. Stop the trigger to
   demonstrate the application's timeout followed by successful abort. Check
   unsupported trigger pairs fail without changing the previous policy.
4. Run continuous DMA for many wraps, with and without a half callback. Confirm
   alternating half/full notifications as configured, repeated full-buffer
   callbacks, busy state throughout, stable canaries and no terminal lease
   release on a circular completion. The continuous entry selects circular mode
   even if the submitted descriptor's circular flag is false; finite entries
   reject a circular descriptor. Do not read a half while DMA is updating it.
5. Abort finite work before completion and circular work at both half/full
   boundaries. Confirm no ordinary completion from cancellation, no buffer access
   or callback after successful abort, idle state, and successful next transfer.
   Repeat abort on an idle/zero handle. Repeated init and a second handle for the
   same physical ADC must fail without disturbing the live owner.
   Count board configure/reset calls while the first handle samples a known
   signal: a second zero handle on that controller must return `BUSY` before
   either board hook, leave the first handle sampling and retain no second
   context. Repeat while first-owner cleanup is failing; after cleanup succeeds,
   initialize the contender and then exercise successful deinit/reinit.
6. Exercise a busy DMA stream and invalid stream/channel/IRQ metadata. Confirm
   rejection without stealing the other transfer. Start two distinct ADCs where
   available; equal IRQ priorities work, while a different shared ADC priority is
   rejected without disrupting the first. Seed stale stream flags/pending IRQs
   before prepare and confirm they cannot deliver a callback to the new request.
7. Inject native ADC init/channel/start failure and DMA prepare/start failure
   using a target debugger or a dedicated target fault-injection image. Confirm
   HAL init retires partial setup before returning an ordinary failure, or
   preserves the handle when cleanup fails. Direct backend init may retain its
   partial context on failure: preserve it and complete backend deinit before
   retrying. Failed DMA submission/cleanup retains buffer and callback arguments
   while busy; retry task-side abort before releasing them.
   Verify successful deinit resets pins only after hardware retirement; failed
   deinit keeps the live handle for retry rather than allowing a second owner.
8. Inject DMA transfer, direct-mode and FIFO errors and ADC overrun. Include a
   pending completion together with an error and verify one error notification,
   no false success, no duplicate callback and no residual buffer writes after
   retirement. Exercise a stream whose EN bit cannot retire immediately: the ISR
   must return without polling, keep busy/storage/lease, and defer the error
   callback until a successful task-side abort. Use an application deadline to
   start this recovery; record the deferred callback's task context.
9. With a device adapter, inspect graph usage during finite completion, multiple
   circular cycles, error, failed start/abort and successful abort. There must be
   exactly one retained reference while hardware owns DMA and zero after terminal
   retirement. Runtime suspend must reject busy DMA. Suspend/resume when idle and
   verify a previously selected external trigger is restored. Inject partial
   hardware deinit failure during suspend; verify `ESY_ERR_STATE` and a faulted
   graph rather than reporting a usable active device. For normal retirement,
   stop producers and finish/abort DMA, deinitialize the registry, unregister
   the typed adapter, then deinitialize/reinitialize the ADC handle. A faulted
   graph follows the documented system recovery contract instead.

## Pass Criteria and Limits

Every transfer has one native DMA handle and one start. Finite transfers retire
before success; circular notifications retain ownership; terminal error or
successful abort retires callbacks and buffer access before storage reuse.
Failure retains enough state to retry cleanup. ISR work never waits for stream
shutdown, and task retirement remains bounded by the SDK's abort behavior.

Analog values meet the device/reference/error budget recorded for the board.
