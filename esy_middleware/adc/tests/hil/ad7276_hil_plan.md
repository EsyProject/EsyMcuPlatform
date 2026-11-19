<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# AD7276 acquisition and decoder integration

Status: hardware execution and waveform qualification have not been run. The
AD7276 core decodes completed words; it does not acquire hardware.

## Prerequisites

- A real consumer owns acquisition through public HAL resources, including
  buffers, per-conversion CS framing and completion/abort semantics.
- Record the board, ADC variant, supply/reference, serial timing, controller
  input pipeline settings, loads and measured pin delays.
- Capture CS, SCLK and data at the device pins and controller pads. Use one
  control owner for shared wires and retain raw data with buffer canaries.

## Acceptance

1. Capture known analog inputs using electrically valid 14-clock and 16-clock
   profiles, where supported by the selected transport. After DMA retirement,
   deinterleave any parallel streams into the documented per-device words.
2. Compare conversion codes, frame order and sample counts with measured serial
   bits. Check zero, near-full-scale and changing nonrepeating inputs. Nonzero
   ignored or floating bits must not invalidate otherwise valid samples.
3. Measure per-conversion CS framing, acquisition/quiet intervals and sampling
   edges independently of decoding. Check the device's power-up requirements.
   A 14-clock decoder does not establish 14-bit DMA transport support.
4. Decode in place only after the capture engine stops accessing the buffer.
   Verify completion and abort fully retire native transfers before buffer reuse.
5. Test one device, then any consumer-selected shared control group. Verify
   simultaneous sampling, frame pairing and failure handling in that consumer;
   the device core does not impose a channel count or scheduling policy.
6. During owner suspension, reset, flash maintenance and any applicable OTA,
   quiesce acquisition before releasing resources. Reestablish supply and serial
   timing before accepting codes after restart.

Record measured results and unsupported transport configurations. This plan
does not certify a sampling rate or downstream measurement accuracy without
those results.
