<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# XIO Program Hardware Verification

Use a Pico-family target, a logic analyzer and caller-reserved PIO/SM/GPIO/DMA
resources. Enable only required platform features. Native build success is a
prerequisite and does not constitute execution or waveform acceptance.

1. Load three small programs at nonzero instruction offsets. Give one an entry
   outside its wrap range. Confirm relative JMP, entry and wrap behavior after
   init/reset and repeated deinit/reinit. Conflicting SM claims must return BUSY.
2. Preload FIFOs while disabled, then start equal-divider SMs as a group. Measure
   coincident first instructions and long-run phase. Include a fractional divider
   and bound short/long individual cycles in system clocks. Reject duplicate,
   mixed-instance and already-enabled groups without starting any new SM.
3. Exercise finite 32-bit TX and RX DMA with guard words, two adjacent input pins
   and shared pin patterns. Confirm exact bit ordering, independent DMA channels,
   byte counts and callback status. Configure input bypass through an earlier
   transport, then verify program init restores synchronized inputs.
4. Exhaust TX data deliberately and stall RX service. Confirm sticky per-SM
   TXSTALL/RXSTALL, selective clear and unaffected neighboring SM flags. A stalled
   user waveform must be treated according to its caller's invalid-run policy.
5. Observe TX completion before queued edges finish and RX completion before
   trailing instructions. Keep resources claimed through callback until abort;
   verify a second DMA start is BUSY. Inspect program PC for final wire completion.
6. Stop a program at each active pin phase, set explicit safe latches and abort
   DMA. After successful abort, poison former buffers/context and verify no later
   access or callback. Inject retirement contention, retain storage on BUSY and
   retry. Deinit must preserve live objects on cleanup failure.
7. Verify reset changes PC/FIFOs/registers while preserving output levels,
   including mandatory side-set clocks and neighboring SM-owned pins. Stop and
   deinit leave caller-defined idle levels intact. Repeat with both PIO instances.
8. Run an `OUT pins, 8` loop with right shift, autopull enabled and threshold 8.
   Follow each OUT with a NOP using a side-set strobe to identify completed
   output slots, including a zero byte.
   After fresh SM init, preload TX words `0x000000a5` and `0x0000003c`, then start.
   Observe bytes `0xa5`, `0x3c` with no leading zero byte. Stop, retire any DMA,
   reset and repeat with different words; old FIFO/OSR data must not precede them.
   Cover both a partially consumed OSR (using a separate `OUT pins, 1` loop)
   and an empty-FIFO autopull stall before reset. Also start with an empty FIFO:
   confirm TXSTALL and PC at the first OUT, then supply the first word and verify
   it is the first emitted byte. Repeat on both PIO instances and preserve
   neighboring and mandatory side-set pin levels throughout reset.

Record clock/divider, instruction words, wiring, analyzer sample rate, DMA routes,
measured bounds and failures. Device-specific timing and processing acceptance
belong to the consumer using this general executor.
