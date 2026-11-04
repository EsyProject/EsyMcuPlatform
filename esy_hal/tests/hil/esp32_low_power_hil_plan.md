<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors
SPDX-License-Identifier: Apache-2.0
-->

# Classic ESP32 Explicit Sleep HIL Plan

## Purpose

Qualify the task-context Light-sleep and reset-class Deep-sleep paths against
ESP-IDF 5.3.4 on the actual classic ESP32 board. Native compile/link evidence
does not prove sleep current, wake races, clock recovery or system quiescence.

## Setup

Record board/revision, SDK commit, toolchain, firmware revision, selected feature
file, SDK PM/timer/watchdog/Flash configuration, CPU/core placement, GPIO mapping
and slow-clock source. Use a current probe, independent reference clock and
logic analyzer. Mark call entry and return on a safe output; emit logs only
outside native sleep. Use default Flash retention for initial qualification.

One system task exclusively owns native sleep/wakeup setup. Disable native
automatic Light-sleep. Stop network borrowers and Wi-Fi/Bluetooth, all active
I/O/DMA, native periodic timers and Flash/OTA. Obtain bounded acknowledgments
from every application task on both cores; leave the native peer interruptible.
Stop the RTC-watchdog monitor/owner before disabling its hardware watchdog.
Suspend or remove application task/OS timer waits whose wall-time behavior must
not be compressed. Never mask IRQs or suspend the scheduler around the API.

## Light-sleep Procedure

1. With no GPIO wake binding, request 1 ms, 10 ms, 100 ms and 1 s. Confirm
   `RETENTION`, `entered=true`, `timer_wakeup=true`, plausible `elapsed_us`,
   restored output/UART behavior and a measured current reduction. Exercise a
   near-minimum duration: `BUSY` is allowed for native rejection; it must leave
   no timer wake configuration or repeated sleep behind.
2. Bind a HAL GPIO wake source on its supported owner core. Confirm its normal
   callback/argument lifetime remains intact. Apply wake before native entry
   and at random points during sleep. Permit native rejection of an already
   active source; otherwise require `gpio_wakeup=true`, bounded wake latency and
   no missed wake. Release the input before another call to avoid level-triggered
   wake loops. Disable the binding successfully before reclaiming its storage.
3. Repeat timer/GPIO calls with both cores enabled, varying the sleeping task's
   core. Confirm the SDK handshake does not deadlock and the peer resumes with
   its callbacks and output state valid. Exercise ordinary local interrupt
   masking and a locally suspended scheduler separately: each call must return
   `STATE` before changing native wake configuration. ISR invocation also rejects.
4. Run 100,000 randomized short sleeps and external wake offsets. Compare
   monotonic ESP Timer progress and returned elapsed values with the reference
   clock. Check no regression around the low 32-bit microsecond rollover and
   no reactivation after the completed request's timer period. A later explicit
   call must create a fresh one-shot request.
5. Keep a native/OS timer diagnostic instrumented during one controlled trial.
   Confirm manual sleep advances ESP Timer but supplies no promise of RTOS tick
   compensation, and overdue native callbacks follow SDK behavior. Restore the
   required production quiescence and verify application deadlines are restarted
   from monotonic time, rather than assuming scheduler wall time was settled.
6. Enable native automatic Light-sleep: explicit sleep and shutdown must return
   `STATE` without disturbing its configuration. Disable it through its owner.
   Enable the RTC watchdog: explicit operations must return `BUSY` without
   feeding, disabling or changing that watchdog. Confirm native TWDT/IWDT and
   system-monitor behavior under the actual sleep duration/configuration.
7. Where a native fault mechanism is available, fail entry and timer cleanup.
   Entry failure must retire this call's timer while preserving GPIO bindings.
   Cleanup failure must retain ownership; the next valid call must only retire
   it and report `entered=false`. Confirm shutdown rejects pending cleanup.
   Record unavailable injection mechanisms instead of treating them as passed.

## Deep-sleep Procedure

1. Quiesce the system again. With no compatible native deep wake configured,
   invoke `esy_hal_shutdown(STANDBY)`, confirm it never returns and measure deep
   current. Apply external reset and verify normal boot and fresh handle/deadline
   initialization. No old Light-sleep timer may unexpectedly wake the chip.
2. If the consumer separately owns native RTC timer/RTC GPIO wake configuration,
   repeat with that explicitly documented configuration. Confirm shutdown
   preserves it and the wake reset cause is observed during the new boot.
   Ordinary digital HAL GPIO Light-sleep bindings must not be presented as deep
   wake support. Do not reuse pre-reset ordinary RAM handles or monotonic epochs.

## Integration Acceptance

For each task, record its sleep/OTA policy and bounded stop acknowledgment,
including maintenance tasks, ISR/DMA sources, outputs, queues and watchdog.
Attempt sleep while OTA/Flash is active through the product owner interface:
its quiescence barrier must defer or reject the request before HAL entry. The
HAL does not infer activity from native PM locks. Cancel, failure and wake must
restore resources in their owners' order, and permit a clean Wi-Fi reconnect.
Record configuration-based non-applicability when OTA, radio or DMA is absent.

Atomic power capabilities must stay `ACTIVE` only; generic wakeup-alarm init and
ESP-IDF SMP tickless registration must continue to reject with `NOT_SUPPORTED`.
Do not report unified automatic tickless support from successful manual sleep.
