<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# RTC consumer and provider HIL plan

The supplied RTC provider is software and requires no new peripheral. Run these
checks against each consumer clock used for it. The hardware-provider checks
below cover native/external provider integration separately from the software
provider.

## Software provider clock integration

1. Enable RTC and the chosen clock provider. Initialize caller-owned storage with
   `{0}`, select an explicit UTC epoch and attach one facade under its owner.
2. Compare calendar progression over an external reference interval, including
   the intended maximum idle/Flash-operation interval. Record clock resolution,
   observed drift, maximum read latency and owner polling latency.
3. Exercise each supported low-power state. If time accounting must include
   sleep, demonstrate the supplied counter continues and calendar progression
   matches elapsed reference time. Otherwise record that software UTC freezes.
4. Arm an alarm across a minute/day boundary. Poll before, at and after due time;
   consume pending twice, then move UTC backward and forward. Confirm the alarm
   reports once, cancel clears pending and only explicit rearm creates new work.
5. Apply positive and negative supported ppb corrections for an interval long
   enough to resolve the expected difference. Confirm correction is arithmetic
   only and does not change unrelated hardware timers.
6. Detach/deinit/reinitialize; verify no outstanding callback/IRQ/native resource
   exists. Reset/power-cycle and confirm UTC is explicitly re-established.
7. Trigger clock-provider failures or reset where injection is available. Confirm
   errors propagate and explicit calendar set safely rebases the new epoch.

## Hardware-provider integration checks

- Record native device/SDK, supported calendar range, clock source/accuracy,
  backup supply and exactly which resource owner initializes backup/RTC domains.
- Demonstrate an unset retained calendar returns STATE until first valid set,
  invalid dates never reach the device and calendar snapshots are coherent
  across rollover. Validate leap days and provider-specific year limits.
- For an advertised alarm, demonstrate one-shot latch/cancel/consume behavior,
  calendar-step behavior and no later reactivation. If IRQs are used privately,
  prove bounded handler work, ownership and safe teardown; waking from sleep
  needs separate explicit provider support and power integration evidence.
- For advertised calibration, measure signed correction, representable range,
  quantization and effective readback. Unsupported operations return
  NOT_SUPPORTED and are absent from the capability mask.
- Exercise init failure, cleanup retry, repeated lifecycle, shared-resource
  conflicts, independent consumer stop/restart and power-loss retention.
- Record build/smoke results for the actual target. A software-provider pass
  does not establish native RTC hardware support.
