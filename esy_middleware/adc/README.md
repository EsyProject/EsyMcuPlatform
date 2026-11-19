<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# External ADC middleware

## Purpose

Provide device-specific support for external ADCs. The current AD7276 core
decodes completed serial acquisitions into unsigned 12-bit codes, following
[Analog Devices AD7276/AD7277/AD7278 Rev. E, Figures 31 and 32](https://www.analog.com/media/en/technical-documentation/data-sheets/AD7276_7277_7278.pdf).
It does not impose a channel count, measurement type or acquisition schedule.

## Public Surface

[`esy_adc_ad7276.h`](include/esy_middleware/adc/esy_adc_ad7276.h) defines the
14-clock and 16-clock frame layouts and `esy_adc_ad7276_decode()`. Detailed
buffer, error and concurrency contracts are documented in that header.

## Integration

Enable the device in the consumer's selected feature entry:

```cmake
set(ENABLE_ESY_ADC_AD7276 ON)
```

Link `esy_adc_ad7276` or the middleware aggregate. `ENABLE_ESY_ADC` selects
the MCU HAL ADC and is unrelated to this external-device core. Neither the
sensor facade nor HAL or OS targets are required.

The acquisition owner supplies completed, right-aligned MSB-first words and
caller-owned output storage. After DMA has retired and the owner has exclusive
buffer access, decode in its main loop or task:

```c
#include "esy_middleware/adc/esy_adc_ad7276.h"

esy_err_t decode_completed_ad7276_block(const uint16_t *wire_words, size_t count,
                                      uint16_t *samples, size_t capacity) {
    return esy_adc_ad7276_decode(wire_words, count,
        ESY_ADC_AD7276_FRAME_16_CLOCKS, samples, capacity);
}
```

This example propagates errors to the capture owner. The selected layout must
match the transport: conversion bits occupy 13:2 in a 16-clock word and 11:0 in
a 14-clock word. Parallel captures require deinterleaving into per-device words
before decoding. No initialization or cleanup is needed for this pure function.

## Contracts

One aligned `uint16_t` input element represents one complete serial frame with
the last captured bit at bit zero. Decoding preserves order and ignores every
bit outside the conversion code, including floating leading/trailing captures.
It cannot validate electrical quality or timing.

Validation completes before output writes. Exact in-place decoding is supported;
partial overlap is rejected. Zero count permits null buffers. The public header
defines the complete argument and range checks. Calls retain no storage and
perform bounded O(count) work; concurrent calls require nonconflicting buffer
access. ISR use requires a bounded block size that fits the caller's ISR budget.

## Resource Model

The core has no handle, mutable global state, heap, callback, task or timer.
The caller owns buffers, CS/SCLK, capture start/stop, DMA retirement and buffer
handoff. No timestamp or timeout is compared. Output is an ADC code; calibration,
physical-unit conversion and any OTA suspension belong to the consumer.

## Verification

From `tests/`, run `ceedling test:all` and `python3 -B test_cmake_consumer.py`.
Tests exhaust all 4096 codes in both
layouts and check ignored bits, ordering, in-place decoding, overlap, zero count,
invalid inputs and size/address overflow. The local CMake consumer tests
also cover feature isolation, C/C++ headers and linkage without HAL or OS.
Hardware acceptance is described in the
[AD7276 HIL plan](tests/hil/ad7276_hil_plan.md).

## Known Limits

The current core decodes captured words; it does not start conversions, own a
SPI handle, generate clocks, acquire multiple inputs or manage ADC power state.
A 14-clock decoder does not add 14-bit DMA support to a transport restricted to
8/16-bit elements. Sampling timing, power-up behavior, rates and signal integrity
require separate hardware qualification.
