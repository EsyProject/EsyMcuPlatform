<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# STM32F4 and ESP32 Watchdog Lifecycle HIL Plan

## Purpose and Setup

Verify default activation and boot-local reset acknowledgement using native
SDKs and actual reset hardware. Use one serialized owner for init, feed,
enable and reset-status calls. Record MCU, clock sources, SDK/toolchain,
debugger state and firmware revision. These are hardware procedures, not host
backend unit tests.

## STM32F4 Default Activation

1. In a fresh boot, call feed before any watchdog init. It must not access a
   NULL native Instance or fault. Then call enable without init and feed for
   several seconds: execution must continue, with IWDG configured for the
   public default 1000 ms and debug pause disabled.
2. Stop feeding and measure reset time against the configured policy and LSI
   tolerance. On reboot confirm the watchdog reset flag, clear it, and confirm
   a second status query is false without changing watchdog operation.
3. Initialize at a distinct legal timeout/debug policy and call enable again.
   It must preserve that policy. IWDG cannot stop once started: deinit remains
   NOT_SUPPORTED and must not claim that hardware protection stopped.
4. Inject a native initialization failure and verify no NULL Instance feed.
   Since native IWDG setup can already start hardware before failure, retain
   the valid native handle and inspect actual protection/reset behavior.
5. Initialize with pause_on_debug=true and inspect DBGMCU APB1 IWDG_STOP set.
   Halt longer than the configured timeout, resume and feed; no watchdog reset
   may occur while halted. Explicitly initialize again with false, inspect that
   the bit is cleared, then halt without feeding and require a watchdog reset.
   Repeat false-to-true and true-to-false policies on each supported device.
   Record debugger options that independently modify freeze registers; disable
   those overrides so they cannot conceal incorrect firmware configuration.

## ESP32 Reset Acknowledgement

1. Cause an RTC watchdog reset. On the next boot, require was-reset true before
   init; clear-reset then require was-reset false on repeated queries.
2. Repeat acknowledgement with the watchdog stopped, running and after deinit.
   Inspect native reset reason and watchdog stage configuration: software
   acknowledgement must not alter diagnostics, enable/disable, feed, or change
   the timeout/debug policy. Confirm init and deinit do not forget an
   acknowledgement within the same boot.
3. Cause another watchdog reset and verify the new boot reports true again.
   A non-watchdog reset must report false regardless of acknowledgement.

## Evidence and Limits

Keep reset-cause reports, native register/SDK traces, measured reset timing and
failure-injection observations. QEMU no-op behavior, native compilation and HAL
forwarding tests do not establish watchdog hardware behavior or timing bounds.
