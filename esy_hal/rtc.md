<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Calendar RTC

## Purpose

The RTC facade provides UTC Gregorian calendar reads/writes, one absolute
one-shot alarm and optional signed frequency calibration through an explicit
provider. It supports native or external RTC providers without exposing hardware
SDK types. The supplied software provider makes every operation usable from an
existing monotonic microsecond clock on any platform, without an OS dependency.

Calendar RTC is separate from scheduling delays and physical wakeup alarms.
Use monotonic time for elapsed-time budgets and the existing wakeup timer HAL
for supported low-power wakeup transactions.

## Public Surface

Include `esy_hal/esy_hal_rtc.h` and link `esy_hal`:

- `esy_hal_rtc_init/deinit`: attach/detach an already initialized provider.
- `esy_hal_rtc_get_caps`: query `ESY_RTC_CAP_ALARM` and
  `ESY_RTC_CAP_CALIBRATION`; unsupported optional operations return
  `ESY_ERR_NOT_SUPPORTED`.
- `esy_hal_rtc_get_datetime/set_datetime`: whole-second UTC calendar operations.
- `esy_hal_rtc_alarm_set/cancel/get_pending`: one absolute, latched alarm.
- `esy_hal_rtc_set_calibration/get_calibration`: signed correction in parts per
  billion; a positive correction makes the RTC advance faster.
- `esy_hal_rtc_software_init/deinit/get_ops`: supplied software provider.

Shared calendar and timestamp types are defined in `esy_common/defs`.
`esy_common/utils/time/esy_datetime.h` validates calendars and converts them to/from
Unix timestamps. The range is 1970-01-01 through 9999-12-31 UTC, without leap
seconds, local timezone or daylight-saving conversion.

## Integration

Select `ENABLE_ESY_RTC` through the consuming project's feature import. It
selects the HAL source and common datetime utilities, without enabling an OS,
network provider, task or hardware peripheral.

For example, an existing owner can supply an always-on microsecond clock:

```c
#include "esy_hal/esy_hal_rtc.h"
#include "esy_hal/esy_hal_system.h"

static esy_err_t read_monotonic_us(void *ctx, uint64_t *out_time_us) {
  (void)ctx;
  return esy_hal_system_always_on_time_us(out_time_us);
}

typedef struct {
  esy_hal_rtc_software_t software;
  esy_hal_rtc_t rtc;
} rtc_example_ctx_t;
static rtc_example_ctx_t s_ctx;

static esy_err_t start_rtc(const esy_datetime_t *initial_utc) {
  if (initial_utc == NULL) return ESY_ERR_INVALID_ARG;
  const esy_hal_rtc_software_config_t software_config = {
      .monotonic_us = read_monotonic_us,
      .initial_datetime = *initial_utc,
  };
  const esy_hal_rtc_config_t config = {
      .ops = esy_hal_rtc_software_get_ops(),
      .provider_ctx = &s_ctx.software,
  };
  esy_err_t status = esy_hal_rtc_software_init(&s_ctx.software, &software_config);
  if (status != ESY_OK) return status;
  status = esy_hal_rtc_init(&s_ctx.rtc, &config);
  if (status != ESY_OK) (void)esy_hal_rtc_software_deinit(&s_ctx.software);
  return status;
}
```

The example requires an enabled system provider supporting
`esy_hal_system_always_on_time_us`; another clock provider can be supplied
without the system feature. Initial UTC must come from an explicit consumer
source, such as retained storage, operator input or a validated time sync.

For a hardware RTC, initialize its provider under its resource owner, then pass
its immutable `esy_hal_rtc_ops_t` and borrowed state to the same facade.
Provider code performing hardware access belongs in HAL/backend boundaries,
without middleware dependencies on SDK or backend headers. An unset retained
RTC may attach and return `ESY_ERR_STATE` until its first successful calendar set.

## Contracts

Every operation runs synchronously on one externally serialized non-ISR owner.
The facade does no waiting or allocation itself; provider latency, including
clock sampling or external-RTC bus I/O, belongs to the caller's budget. Provider
ops and state remain valid until successful facade detach. The facade validates
calendar inputs and outputs, returning `ESY_ERR_IO` for malformed successful
provider reads. Output parameters remain unchanged on errors.

Handles start at `{0}`. Repeated init returns `ESY_ERR_STATE` and preserves the
original binding/provider. Failed software init borrows no state and permits
retry. Facade detach restores `{0}` and permits reattachment, but does not cancel
an alarm, stop a clock or release native resources. Cancel an unwanted alarm,
detach the facade, then deinitialize the software/native provider. Keep live
storage at a stable address and never copy or clear it to bypass cleanup.

An alarm deadline is whole-second UTC and strictly later than the current RTC.
A successful arm replaces the previous alarm and clears its pending flag.
Expiration sets a latched pending flag and permanently retires that alarm.
`alarm_get_pending(..., true, ...)` consumes the flag; `false` preserves it.
Cancel is idempotent and clears armed/pending state. A later calendar step or
counter wrap cannot reactivate a retired alarm; only a new successful arm does.
An armed alarm follows calendar changes: moving UTC past it expires it, while
moving UTC backward postpones it. A failed arm preserves the previous alarm.
The next successful time sample still observes its elapsed deadline.

The software clock source is unsigned 64-bit microseconds, modulo 2^64
(approximately 584,542 years). It must advance monotonically modulo that width;
less than one full cycle may pass between samples. The accepted elapsed interval
also cannot exceed the complete supported UTC span, and corrected UTC must stay
in range. One wrap is supported by unsigned subtraction. Sleep freezes RTC
progress if the supplied source stops; choose an always-on source when sleep
must count. No tighter polling interval is needed for time accounting, although
alarm observation latency equals owner access latency.

Reset/backward jumps generally produce an out-of-range interval and
`ESY_ERR_STATE`. Explicit `set_datetime` samples the new source epoch and
recovers from source reset or calendar range exhaustion. A full unseen counter
period cannot be inferred from two samples and is outside this contract.
Software calibration accepts -1,000,000 through +1,000,000 ppb inclusive,
retains sub-nanosecond fractional progress across samples, and advances to the
change instant with the old rate before applying the new one. Software
calibration changes arithmetic rate; it does not trim a hardware oscillator.

## Resource Model

The facade borrows only an immutable operation table and provider context.
Software state occupies caller-owned, naturally aligned opaque storage. It
borrows one clock callback/context, creates no timer, task, IRQ or callback, and
uses no heap or global mutable state. Each owner may create independent software
providers. A provider must have at most one attached facade; this serialization
and detachment ordering are caller preconditions.

Native/external provider resources, clock domains, backup supply, bus ownership
and calibration granularity must be documented by that provider. Hardware alarm
or wake support must not be inferred from the software alarm capability.

## Verification

`esy_hal/tests/test_esy_hal_rtc.c` exercises the public facade and software
provider with reusable RTC/clock fakes under `esy_test/fake/hal`:

```sh
cd esy_hal/tests
ceedling test:esy_hal_rtc
ceedling gcov:esy_hal_rtc
```

Coverage includes lifecycle failures/retry, malformed provider data, optional
capabilities, leap days, counter wrap, exact alarm boundaries, completed/canceled
alarm retirement and explicit rearming, clock steps, positive/negative
calibration, frequent fractional samples, full UTC range and recovery from
clock/range failures. Complete unseen 64-bit periods are outside the supported
time-accounting range; tests verify explicit rebase keeps retired alarms inert.
The [RTC HIL plan](tests/hil/rtc_hil_plan.md) defines consumer clock and
hardware-provider integration checks; host tests do not prove oscillator
accuracy or low-power retention.

## Known Limits

This release supplies the portable facade and software provider. It does not
supply native RP2040/STM32/ESP32 or external I2C RTC adapters, backup-domain
configuration, battery retention, physical RTC alarm IRQs or calibration-register
programming. Such providers may implement the public table with their actual
capabilities and resource contract.

Software RTC state is lost on reset or power loss. Its alarm is evaluated only
when the owner reads time/pending state, arms another alarm or changes
calibration/calendar; it cannot wake the CPU. Accuracy and sleep continuity are
limited by the supplied monotonic source. There is one alarm per provider, no
periodic/wildcard matching and no asynchronous callback. Time sync policy and
SNTP belong to the separate time middleware.
