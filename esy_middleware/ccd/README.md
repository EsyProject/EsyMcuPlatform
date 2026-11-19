<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# CCD middleware

## Purpose

Provide device-specific CCD timing and readout support. The current TCD1254GFG
core compiles control timing and qualifies completed readouts after startup.
One instance represents one shared timing domain; it contains no
ADC selection, channel count, axis names or diameter policy. This core follows
Toshiba's [TCD1254GFG Rev. 1.3, pages 5 through
8](https://www.eureca.de/files/pdf/optoelectronics/toshiba/TCD1254GFG_Web_Datasheet_en_20190124.pdf),
a preliminary document used as an explicit design baseline.

## Public Surface

[`esy_ccd_tcd1254.h`](include/esy_middleware/ccd/esy_ccd_tcd1254.h)
provides geometry constants, timing configuration and compiled plans, plus a
caller-owned readout lifecycle. `plan_compile()` is independent of that lifecycle.
`init()`, `begin()`, `complete()`, `cancel()` and `deinit()` maintain software
qualification; none executes or stops hardware. The header is the authoritative
reference for parameter ranges, errors, ownership and lifecycle rules.

## Integration

Enable this input in the consumer's selected feature entry and link
`esy_ccd_tcd1254`, or the middleware aggregate:

```cmake
set(ENABLE_ESY_CCD_TCD1254 ON)
```

The target depends only on common definitions, with no sensor facade, HAL or OS
dependency.
For example, compile a profile for an executor that really provides a 100 MHz
tick clock and a 50-tick master period:

```c
#include "esy_middleware/ccd/esy_ccd_tcd1254.h"

esy_err_t prepare_tcd1254_profile(esy_ccd_tcd1254_plan_t *plan) {
    const esy_ccd_tcd1254_config_t config = {
        .tick_hz = 100000000u,
        .master_period_ticks = 50u,
        .icg_delay_ns = 5000u,
        .icg_to_sh_ns = 500u,
        .sh_high_ns = 1000u,
        .sh_period_ns = 10000u,
        .integration_ns = 100000u,
    };
    return esy_ccd_tcd1254_plan_compile(&config, plan);
}
```

Check the result before passing the value plan to the hardware execution owner.
This example is a timing calculation, not hardware initialization or a fixed
clock requirement. Supply the actual executor clock and supported timing in a
real consumer; do not assume the processor clock equals this clock.

For readout qualification, initialize caller-owned handle storage with `{0}` and
call `init()` after the sensor supply is stable. Each `begin()` returns a copied
plan, an operation token and a discard flag. The hardware owner executes that
plan, captures a complete line with its chosen ADC, and retires every associated
I/O access before calling `complete()` with the same token and actual sample
count. Only `usable=true` permits publishing the active pixels. The first ten
successful complete lines are discarded; the eleventh can be published.

If hardware execution fails, stop all waveform/ADC work before `cancel()`.
On shutdown, quiesce the hardware and resolve the pending token before `deinit()`.
Shared control signals have one executor owner even when they drive several
physical CCDs. The consumer performs channel pairing separately.

## Contracts

The compiler checks device limits and quantized timing, preserving the master
phase across repeated frames. Failed compilation leaves the output unchanged.
A complete line contains 2547 slots: 32 leading, 2500 active and 15 trailing.
Output samples run at half the master frequency. See the public configuration
and plan types for exact timing ranges, rounding and waveform semantics.

This is a target waveform description, not a register program. The executor
must keep the master clock continuous, realize the specified relative phases,
and select the correct D0/ADC sampling window through public HAL. Pad delays,
input/output pipelines, jitter and quantization in that executor need separate
verification. Software delays between GPIO writes do not implement the plan.

One main-loop/task owner serializes lifecycle calls; none is ISR-safe. Short or
extra readouts and cancellation reset startup qualification. Tokens reject stale
completion, remain reserved across deinit/reinit and never wrap. Preserve the
same handle object; never copy or clear it to bypass its lifetime contract.

## Resource Model

The caller owns the handle, returned value plans and all hardware resources.
The core owns no buffers, heap, global state, callbacks, tasks, timers or IRQs.
Initialization/deinitialization changes software state only; it cannot establish
electrical idle or certify hardware quiescence. The owner must retain external
resources until their own completion/abort contracts permit release.

Durations use an explicit constant tick rate and checked arithmetic, producing
uint32 tick counts, not wrapping timestamps. Only explicit hardware completion
acknowledgement advances qualification. The acquisition owner determines
OTA/flash quiescence and restart behavior.

## Verification

From `tests/`, run `ceedling test:all` and `ceedling gcov:all`. Tests cover
geometry, min/max timing, quantization, repeated-frame phase, overflow rejection,
unchanged failure outputs, ten startup readouts, malformed lengths,
cancellation, stale tokens, reinitialization and exhaustion. The
[CMake consumer checks](tests/test_cmake_consumer.py) verify independent
feature selection, C/C++ layout, source closure and absent HAL/OS dependencies.
The [TCD1254 HIL plan](tests/hil/tcd1254_hil_plan.md) covers later executor
integration; passing core tests does not qualify electrical behavior.

## Known Limits

No phase-capable hardware executor or Pico PIO program is included in this core.
The current public HAL does not provide a ready-made multi-signal TCD1254
executor. Adding one remains a separate HAL/backend integration task; this core
alone cannot produce CCD clocks, initiate ADC DMA or deliver a physical image.
It does not choose an ADC sampling phase, power sequencing, voltage levels,
illumination, channel count, calibration or measurement cadence. The conservative
frame plan is not a certification of maximum sensor throughput.
