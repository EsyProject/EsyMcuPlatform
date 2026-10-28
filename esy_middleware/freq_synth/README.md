<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Frequency Synthesizer

## Purpose

`esy_freq_synth` supplies a reusable, allocation-free RF synthesizer facade with
ADF4351, ADF4372, LMX2594 and LMX2592 backends. Hardware access goes through public HAL
SPI/GPIO APIs. The consumer owns feature selection, hardware resources, task
serialization, RF startup policy and system recovery.

## Public Surface

All public headers are in `include/esy_middleware/freq_synth/`:

- `esy_freq_synth.h`: caller-owned handle, backend operations, tuning, output
  enable, power and diagnostics.
- `esy_freq_synth_device.h`: optional caller-owned device-graph binding.
- `esy_adf4351.h` and `esy_adf4372.h`: configuration, defaults, init and deinit;
  ADF4372 additionally provides calibration capture and fast tuning.
- `esy_adf4351_device.h` and `esy_adf4372_device.h`: typed graph registration.
- `esy_lmx2594.h` and `esy_lmx2592.h`: independent chip configurations,
  init/deinit, register readback and lock checks. Both TI backends use synchronous caller-driven calls without an OS dependency or a
  device-graph/PM adapter.

`set_frequency_hz` is the core attachment capability. An attached handle may be
faulted; attachment alone does not imply that operations are available. Missing
optional operations return `ESY_ERR_NOT_SUPPORTED`.

The handle borrows a `const esy_freq_synth_vtable_t *ops` and a backend context.
Every concrete backend shares one static const operations table. The table and
context stay valid and immutable until matching backend deinit succeeds; live
replacement is forbidden. Custom providers supply a complete const table with
`set_frequency_hz` and any optional output/power/info hooks, using NULL for an
unsupported operation. They initialize a `{0}` handle, set `ctx` and then `ops`,
and detach only after their own resources have been safely retired. Typed chip
APIs recognize their concrete table identity, so copied or substituted tables
are custom providers even if they contain the same function pointers.

Frontend device/fault state is private and cannot be read or changed through
public fields. `frontend` is pointer-aligned opaque lifecycle storage; `backend`
has a 160-byte default and maximum natural alignment for drivers. Applications
that need frequency sequences own their steps and timing.

## Integration

Select `ENABLE_ESY_FREQ_SYNTH` and the needed `ENABLE_ESY_ADF4351`,
`ENABLE_ESY_ADF4372`, `ENABLE_ESY_LMX2594` or `ENABLE_ESY_LMX2592` gates in
the consumer's `esy_feature_import.cmake`. Enable the required HAL GPIO,
SPI and system-delay primitives. A disabled module creates no target; each
backend source is compiled only when selected. The facade links only common
types and the device graph used by its optional reference guard. With every
chip gate OFF and GPIO OFF, it compiles and runs without a HAL target or HAL
header paths. Selected chip drivers require `ENABLE_ESY_SPI`, `ENABLE_ESY_GPIO`,
`ENABLE_ESY_SYSTEM` and the public HAL target. Link `esy_freq_synth` for tuning:

```cmake
target_link_libraries(your_target PRIVATE esy_freq_synth)
```

GPIO PM and graph registration are a separate `esy_freq_synth_device` target,
assembled when `ENABLE_ESY_GPIO` is ON. Link it only when using generic or typed
`*_register_device()` and `esy_freq_synth_unregister_device()` APIs:

```cmake
target_link_libraries(your_device_owner PRIVATE esy_freq_synth_device)
```

The device target publicly links the facade and privately links HAL. Its
ADF4351/ADF4372 adapter sources follow their chip gates. The middleware aggregate
includes this target when it is available. Core-only consumers can link the
facade and selected chips without GPIO PM or graph-registration symbols. Chip
configuration headers expose public SPI handles, so enabled chip drivers
propagate the HAL header dependency; the generic facade does not. Private
compiler options remain private. ADF4351 requires initialized SPI and LE resources;
ADF4372 requires initialized SPI and active-low CS. Both accept optional CE.
LMX2594 and LMX2592 require initialized mode-0, eight-bit, MSB-first, manual-CS SPI,
active-low CSB and a MUXout GPIO input also wired to SPI MISO. SDI is a separate
MOSI output; CE is held high by the board. SPI speed is limited to 5 MHz by the
backend contract. A nonzero speed request must succeed; zero retains an already
configured and independently verified safe speed.

### Usage Example

Pass a zero-initialized, owner-held handle and a valid ADF4351 configuration
prepared with `esy_adf4351_default_config()` and the actual reference clock,
SPI and LE resources. This synchronous example tunes and queries the programmed
frequency with RF output disabled, then performs the matching hardware cleanup:

```c
#include "esy_middleware/freq_synth/esy_adf4351.h"

esy_err_t tune_and_read(esy_freq_synth_t *synth,
                        const esy_adf4351_config_t *config,
                        uint64_t requested_hz, uint64_t *actual_hz) {
    if (synth == NULL || config == NULL || actual_hz == NULL ||
        config->initial_output_enabled) {
        return ESY_ERR_INVALID_ARG;
    }
    esy_freq_synth_info_value_t info;
    esy_err_t result = esy_adf4351_init(synth, config);
    if (result == ESY_OK) {
        result = esy_freq_synth_set_frequency_hz(synth, requested_hz);
    }
    if (result == ESY_OK) {
        result = esy_freq_synth_get_info(
            synth, ESY_FREQ_SYNTH_INFO_ACTUAL_HZ, &info);
    }
    esy_err_t cleanup = esy_adf4351_deinit(synth);
    if (cleanup != ESY_OK) {
        return cleanup;
    }
    if (result == ESY_OK) {
        *actual_hz = info.u64;
    }
    return result;
}
```

Call from the serialized non-ISR SPI owner and keep output storage disjoint
from the handle/configuration. SPI/GPIO remain borrowed through successful
deinit; cleanup failure leaves the owner-held handle attached for another
`esy_adf4351_deinit()` attempt. Do not reset or reuse it. The frequency query
reports the programmed value, not a measured PLL lock. RF enabling is a separate
`esy_freq_synth_set_output_enable()` operation after product startup admission.

For PM, initialize the hardware and synthesizer, register the SPI parent and
then the matching typed synthesizer binding with that parent in its dependency
array. Initialize the binding with `{0}`. Start the registry after assembly.
The SPI backend must supply bounded blocking writes. PM writes use the public
`esy_hal_spi_write_blocking_for_pm()` entry with the transitioning child as
requester; it verifies the active SPI parent and direct graph dependency without
acquiring another reference during transition. The ADF core retains only the
private requester marker and this write selection to reuse the same register
sequence. GPIO profiles, graph registration and PM dispatch remain in the
optional device target; no callback framework or second register algorithm is
introduced.

Profiles describe auxiliary GPIO idle/sleep overrides and remain immutable
while claimed. Use output states that keep CS/LE inactive and preserve the
chip's register contents. Resume restores the configured pin modes and then
explicitly restores inactive CS/LE and active CE. SPI controller pads remain
owned by the HAL adapter. An enabled RF output prevents runtime idle
suspension; explicit system suspend saves and restores its enable state.

## Contracts

### Lifecycle and errors

The first initializer call requires a zeroed handle. Keep its address stable;
do not copy, move or clear a live handle or binding. Reinitialization returns
`ESY_ERR_STATE` without changing the live handle. Invalid configuration leaves
the handle unchanged. An I/O failure during init attempts cleanup. If cleanup
also fails, the handle stays attached and faulted; preserve all resources and
retry the matching backend deinit.

All backend deinitializers return `esy_err_t`. They attempt output disable,
CE disable when present, and inactive CS/LE; they detach only on success.
Failure retains the context and blocks normal operations. Successful deinit
leaves an empty handle and may be repeated. Generic `detach` performs no
hardware cleanup and must not replace a concrete backend deinit.
Both LMX backends additionally power down the chip. Failed tuning or output
programming faults an LMX handle because a partial register sequence cannot be
trusted; only deinit may then be retried. Diagnostic readback has a separate
restoration contract described below.

Unregister before backend deinit. Before graph start, unregister removes the
graph node directly. To retire a started graph, quiesce synthesizer outputs and
producers, deinitialize the registry, then unregister each typed binding and
deinitialize its concrete backend. Registry teardown leaves the typed owner
link and GPIO claims attached until unregister succeeds.
Failed GPIO release after graph removal preserves the binding's owner link;
restore the GPIO default PM state and retry unregister. Failed registration
also retains that link if its GPIO cleanup fails. `deinit` reports
`ESY_ERR_BUSY` while the binding still owns the handle.

Invalid parameters return `ESY_ERR_INVALID_ARG`, unavailable/faulted handles
return `ESY_ERR_STATE`, unsupported operations return `ESY_ERR_NOT_SUPPORTED`,
and incomplete SPI transfers return `ESY_ERR_IO`. GPIO errors propagate;
initial GPIO setup failure is reported as `ESY_ERR_FAIL`. ADF4351 returns
`ESY_ERR_FAIL` when no valid PLL plan can be found. ADF4372 rejects invalid
revision readback with `ESY_ERR_IO` (supported revisions: 0x09 and 0x0A).

Cached frequency, power and enable state represent the last completed write
sequence. A failed bus or GPIO operation can partially change hardware; cached
state is not proof of the current RF output. Retry a complete tuning operation
or disable/deinitialize before resuming product operation. Failed writes
invalidate the applicable register shadow. Suspend callbacks restore their
entry state on recoverable failure. Failed rollback returns `ESY_ERR_STATE`
and faults both handle and registry; the product must enter system recovery.

### Frequency and calibration

ADF4351 accepts 35 MHz to 4.4 GHz and rejects out-of-range requests. Its solver
uses the exact reference/R ratio, writes registers in R5-to-R0 order, and
commits double-buffered R1/R2 changes with a final R0 write even if R0 is
unchanged. Band-select clock mode is selected to satisfy the configured divider
and PFD limits; unsupported combinations are rejected. `band_sel_div` must be
1..255. Slow mode requires PFD/divider at most 125 kHz; fast mode permits at most
500 kHz and restricts the divider to 1..254. Zero does not select an automatic
divider.

ADF4372 supports MAIN RF8x from 62.5 MHz to 8 GHz or MAIN RF16x from 8 GHz to
16 GHz. AUX controls RFAUX8x enable only. Reference input must be 10–500 MHz
single-ended or 10–600 MHz differential. R is 1–32 and PFD must not exceed
125 MHz. Higher-PFD operation needs a separate half-PFD calibration and lock
sequence and is deliberately rejected. Planning uses the exact reference/R
ratio, validates INT before narrowing, encodes R=32 as zero, keeps MOD2 at
least two, and enables variable modulus when FRAC2 is nonzero. Calibration
ALC timeout is at least two and is calculated from the actual reference ratio.

Four-wire mode keeps MUXOUT disabled so the shared pin remains available for
SDO readback; enabling MUXOUT in four-wire mode is an invalid configuration.
Set `vco_supply` to the physical supply for revision-specific ALC thresholds.

Calibration capture requires the caller to establish lock. It restores the
FSM selector on all recoverable exits and leaves its output object unchanged
on failure. Failed FSM restoration faults the handle. Fast tuning requires
one-hot VCO core values (1, 2, 4 or 8), a valid bias and calibration for the
same channel and nearby frequency in the same VCO region. The caller must
establish validity across reference, supply and temperature changes. Fast
updates write changed fields and always trigger with INT_L last. Both direct
ADF4372 APIs hold a logical-device reference throughout their hardware access.

LMX2594 follows [TI SNAS696C](https://www.ti.com/lit/ds/symlink/lmx2594.pdf),
Tables 2, 5, 8 and 23, and Sections 7.5 and 7.6. The bypassed reference path
accepts 5..200 MHz OSCin with PLL_R=1..255, requires an integral-Hz PFD of
5..100 MHz and uses no reference doubler or multiplier. A fixed configured
output divider selects VCO directly or a documented channel-divider value.
VCO must be 7.5..15 GHz; divisors of eight or greater additionally require
VCO <=11.5 GHz. With divider four, 2.0..3.7 GHz corresponds to 8.0..14.8 GHz.
The solver reduces the exact fractional ratio, uses integer mode or third-order
MASH, and selects PFD_DLY_SEL from TI's minimum-N table. `MOD`, `FRAC` and `INT`
diagnostics use `u32`; frequency/PFD diagnostics use `u64`. Channel spacing is
the integer-Hz portion of PFD/DEN/output-divider.

Initialization resets the chip, writes every required register R78 down to R0,
waits 10 ms and triggers FCAL. Automatic no-assist calibration uses amplitude
start 300, capcode start 183 and VCO7, except 11.9..12.1 GHz uses VCO4 as TI
requires. Every tune programs N, NUM/DEN, MASH/PFD fields and triggers FCAL.
RFoutA is MAIN; RFoutB is AUX and starts powered down, routed to VCO to avoid
unnecessary divider power. Enabling AUX routes it to the same fixed frequency
as MAIN. Both power codes are 0..63 and must be characterized with the actual
output pullups.
`mute_till_lock` programs TI's OUT_MUTE and complementary OUT_FORCE fields for
calibration muting. It does not establish the analog output's settling time or
provide an RF safety interlock.

MUXout normally indicates combined calibration and indirect-Vtune lock detect
(LD_TYPE=1, LD_DLY=1000). Readback temporarily selects its serial-data function
and restores lock detect with FCAL cleared, including after transfer failures.
Every R0 write still triggers amplitude calibration, so diagnostic readback
restarts the 50 us software lock guard and requires settling again. Failed
restoration faults the handle; register output objects remain unchanged on
errors. Ordinary lock checks never write registers: after the guard they read
only MUXout. Once a lock check observes the guard complete, time-counter wrap
does not restart it; a new tune or diagnostic readback rearms it.
The guard rejects immediate stale status; it is not evidence of
analog settling. The explicit lock waiter polls at 5 us with a caller-supplied
timeout; it uses the HAL time counter and can be extended by scheduling and
blocking transfers. Initialization and ordinary tuning do not wait for lock.
Consumers must characterize RF and analog response before choosing measurement
delays.

LMX2592 follows [TI SNAS646G](https://www.ti.com/lit/ds/symlink/lmx2592.pdf),
Tables 7-2 and 7-4, Figure 7-5 and Sections 7.3 through 7.6. It is a distinct
backend: LMX2594's register values, 7.5..15 GHz VCO and charge-pump coding are
not compatible. The supported reference path accepts 5..200 MHz OSCin,
PLL_R=1..255 and an integral-Hz PFD of 5..100 MHz; OSCin doubler/multiplier and
RF VCO doubler are disabled. PLL_N_PRE is two, so planning uses
`FVCO = FPFD * 2 * (N + NUM/DEN)`. The exact fraction is reduced. Integer mode
requires N>=9 and PFD_DLY=1. Fractional planning selects third-order MASH for
N>=18, second-order for N>=16, or first-order for N>=11, with PFD_DLY=2, 2 or 1.
This allows the 3.55..3.6 GHz direct-output interval at a 100 MHz PFD.

`output_divider=0` automatically selects the lowest legal channel divisor, or
direct VCO output at 3.55 GHz and above. Fixed divisors are 1 (VCO), 2, 3, 4,
6, 8, 12, 16, 24, 32, 36, 48, 64, 96, 128 and 192. Supported outputs are
20 MHz..7.1 GHz, subject to Table 7-4's per-divider output and VCO intervals.
With the automatic setting, 2.0..3.55 GHz uses divide-by-two and higher
frequencies use the direct VCO. Route changes disable both output buffers
before reprogramming their distribution path, then restore the requested
output enables. This brief muting does not continue throughout calibration or
settling. RFoutA is MAIN, RFoutB is AUX; both follow the same selected frequency.
B initially remains disabled, and unused distribution paths are powered down.
Unlike LMX2594, this backend has no `mute_till_lock` field or hardware OUT_MUTE
assumption. The consumer must manage any calibration-period RF restrictions.

The independent LMX2592 initialization resets the chip and writes only the
43 documented R/W addresses, in descending order from R64 to R0. It preserves
all unnamed bits, enables reference and charge pump, enables amplitude and
automatic VCO calibration, sets LD_TYPE=1, and leaves calibration overrides and
fast calibration disabled. The 10 ms delay before final FCAL is an intentional
power-on margin, rather than a data-sheet-mandated delay. Every tune writes
R38's N, R40/R41's denominator, R44/R45's numerator, legal MASH/PFD settings,
and VTUNE_ADJ according to the 6.5 GHz VCO threshold, then triggers FCAL. R42/43
are MASH seed and are not mistaken for the numerator. R68..R70 are read-only
status registers, and no unlisted addresses are programmed.

Charge-pump UP and DN use the same configurable five-bit current mask;
CP_ICOARSE accepts only 0 (x1) or 2 (x1.5), as TI recommends for lock-detect
accuracy over temperature. The default mask 12 and multiplier 0 select nominal
3.125 mA. Product loop-filter stability, settling and noise depend on the selected
current. Output power accepts codes 0..31 or 48..63, the ranges defined by TI;
measure the resulting power with the actual pullups.

LMX2592 lock queries only read MUXout with LD_EN=1 and LD_TYPE=1, after a
software guard. The guard scales 800 us at a 100 MHz calibration clock by the
actual OSCin calibration-clock ratio. CAL_CLK_DIV keeps that clock below
200 MHz. The data sheet's 800 us value is specified at its test conditions;
the scaled guard is a conservative implementation choice, not a lock-time
or settling guarantee across all hardware conditions. Diagnostic readback
switches MUXout through R0 with FCAL clear, restores it on all recoverable
exits, and conservatively restarts the guard. Failed restoration faults the
handle. Frequency and PFD diagnostics use `u64`, N/NUM/DEN use `u32`, and
`RF_DIV_SEL.u8` reports the actual divisor (one means direct VCO). Channel
spacing is the integer-Hz portion of `2 * PFD / DEN / divider`.

### Concurrency

All synthesizer APIs, graph lifecycle and PM callbacks are task-context only.
One owner or an external serializer must cover the handle, binding, PM
transitions and each complete CS/LE-framed transaction on a shared SPI bus.
A per-transfer SPI lock alone does not protect a multi-transfer register read.
Do not call these APIs from ISR or concurrently with graph transitions. PM
callbacks may run with the scheduler suspended and must not use RTOS waits.

Zero-initialize handles/bindings and check cleanup results before reclaiming
resources. Unsupported PFD/MUXOUT combinations return errors.

## Resource Model

No backend or facade allocates memory. Each handle contains aligned opaque
storage checked at compilation; its backend borrows initialized SPI/GPIO
handles through successful deinit. Device registration borrows the binding,
registry, name, dependency array and GPIO profile through successful unregister.
There are no module-created tasks, callbacks, DMA buffers or owned RTOS objects.
Product policy determines RF behavior during OTA/flash operations and must
serialize or stop RF activity before operations that require it to be quiet.

## Verification

Host suites in `tests/` use shared fakes from `esy_test/fake/hal`. Run:

```sh
cd esy_middleware/freq_synth/tests
ceedling test:all
ceedling gcov:all
python3 -B test_cmake_consumer.py
```

From the platform root, run the sanitizer configuration with isolated artifacts:

```sh
ruby tools/quality/run_module_tests.rb esy_middleware/freq_synth/tests /tmp/esy-freq-synth-sanitizer test:all --sanitizer
```

Tests cover shared const operations, foreign-table identity rejection, public
error semantics, first/repeated initialization, cleanup
retry, binding ownership, malformed dependencies and wrong backend identity;
started-graph teardown, typed unbind, GPIO cleanup retry and backend reuse;
short SPI writes and GPIO edge faults; register commit order and exact
reference-ratio vectors; zero and slow/fast upper band-divider limits; secondary
modulus, R32 and ALC limits; FSM restoration;
logical reference ownership; PM rollback/fault propagation and active CE
restoration. LMX2594 and LMX2592 tests decode transmitted integer/fractional frequencies,
check descending initialization and delayed FCAL, exercise 101-point stepping,
readback restoration, lock timeouts, rejected configurations and cleanup retry.
LMX2594 lock-guard regressions cover a complete microsecond-counter cycle,
continued MUXout sampling and rearming after tuning or diagnostic readback.
LMX2592 additionally checks sparse initialization, the independent prescaler and
fraction fields, all fixed divider segments, MASH minimum-N changes, the
3.55 GHz route boundary and the 6.5 GHz VTUNE_ADJ threshold.
CMake tests compile, link and execute C11/C++17 consumers for all sixteen chip
combinations, generic facade use without HAL, adapter availability and missing
hardware diagnostics. Core archives are checked for absence of GPIO PM and
graph-registration references. Each ADF device-target consumer runs actual
registered graph suspend/resume, restores enabled output and releases its device
reference. Unit tests additionally preserve wrong-backend rejection even when an
a foreign immutable table copies every callback, competing GPIO claims, registration-error
cleanup and retry. The repository gate includes these consumer tests
and the module's source-contract checks. Coverage thresholds reject missing or
empty reports instead of accepting a partial module set.

## Known Limits

- Hardware lock, output frequency/power, spectral purity, SPI timing and
  retention depend on the chip and board; host fakes do not establish these
  properties.
- Only MAIN is supported by ADF4351.
  ADF4351/ADF4372 do not poll lock; their PFD diagnostics are integer-Hz
  approximations even though planning preserves the reference ratio.
- Both LMX backends support explicit lock polling but have no calibration
  cache/full assist, reference multiplication or PM adapter. LMX2594 does not
  implement hardware ramp, SYNC or SysRef. LMX2592 does not enable fast
  calibration, VCO doubling or phase adjustment.
  No scan-duration, noise or lock-time requirement is established by host tests.
- ADF4372 PFD above 125 MHz and calibration-region discovery are unsupported.
  Fast-lock validity is a caller responsibility, not inferred from frequency
  proximity alone.
- GPIO profiles must retain chip register contents. Power removal/reset needs
  a full reinitialization policy and is not modeled as ordinary PM retention.

The Analog Devices backends follow the
[ADF4351 data sheet](https://www.analog.com/media/en/technical-documentation/data-sheets/ADF4351.pdf)
and [ADF4372 data sheet](https://www.analog.com/media/en/technical-documentation/data-sheets/adf4372.pdf).
