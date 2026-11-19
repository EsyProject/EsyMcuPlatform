<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# TCD1254 core and executor integration

Status: hardware execution and waveform qualification have not been run. The
TCD1254 module compiles plans and qualifies owner-confirmed readouts; it does
not acquire hardware.

## Prerequisites

- A real consumer owns a phase-capable public HAL waveform executor and ADC
  acquisition resources, including buffers and completion/abort semantics.
- Record the board, sensor/ADC parts, supplies, executor tick source, actual
  master period, input/output pipeline settings, loads and measured pin delays.
- Capture control and data signals at the device pins and controller pads.
  Use one control owner for shared wires. Keep raw data and result canaries.

## TCD1254

1. Execute accepted plans at integer and quantized profile boundaries. Measure
   master frequency/duty, ICG-fall to SH-rise t2, every SH width t3, SH-fall to
   ICG-rise t1, master-high phase at ICG rise, purge spacing and integration gap.
   Verify every repeated frame preserves phase and no pulse overlaps a frame
   origin. Compare measured timing with both compiled ticks and device limits.
2. Establish the actual D0 sampling window, all 2547 output slots and the active
   range [32, 2532). Check raw curves under dark, illuminated and occluded
   conditions before extracting dimensions. This core does not choose ADC phase.
3. After stable power, acknowledge ten verified complete readouts as discarded;
   the next can be usable. Extra/short lines must invalidate qualification.
   Do not acknowledge a line solely because a software delay expired.
4. Cancel while readout is pending only after the hardware executor and DMA
   have stopped. Verify pending buffers remain borrowed until native retirement,
   completion cannot race buffer reuse, and stale software tokens are rejected
   after cancellation and deinit/reinit.
5. Test one independently controlled sensor, then a consumer-selected shared
   control group. The software cores must not impose a channel count. Verify
   paired timestamps/frames and all-or-error policy in that consumer separately.
6. During owner suspension, reset, flash maintenance and any applicable OTA,
   quiesce waveforms/ADC work before releasing resources. Reestablish supply,
   hardware phase and startup qualification explicitly before accepting images.

Record measured results and unsupported executor configurations. This plan does
not certify a specific pixel rate, frame rate, dual-channel schedule or diameter
accuracy without those results.
