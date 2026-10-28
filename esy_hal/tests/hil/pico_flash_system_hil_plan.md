<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Pico Flash Source and System Error HIL Plan

## Purpose and Setup

Verify that Pico Flash programming never consumes an XIP source while XIP is
disabled, and that an unattainable system-clock request returns an error rather
than entering the SDK panic path. Run separate startup fixtures on RP2040 and
RP2350 with real SDK providers. Record the MCU, SDK/compiler, feature inputs,
firmware revision, Flash geometry, source addresses, clock requests and results.

Enable Flash and system through the consumer feature import. Reserve a complete
test sector outside executable images, metadata and live storage through the
consumer's layout. The consumer's Flash owner must quiesce every XIP user,
including peer cores and IRQs, using its normal flash-safe execution policy.
Use caller-owned source storage in the main on-chip SRAM mapping; aliases and
external memory are unsupported. No platform staging buffer is allocated.

## Flash Source Procedure

1. Install the real flash-safe hook with an operation-entry counter, and prepare
   a page-aligned test destination according to its reported erase geometry.
   Program one page from a main-SRAM buffer, read it back, and compare every byte
   and source canary. Require one hook/operation execution and synchronous OK.
2. For a legal target and page length, submit a `static const` page whose address
   is confirmed to lie in XIP. Require INVALID_ARG before the hook counter
   changes. Repeat with the supported XIP alias addresses, ROM, external-memory
   addresses and SRAM aliases. None may dereference the source, enter the Flash
   operation, alter the target or hang. Do not erase the target between these
   rejected write attempts; compare its existing bytes after each rejection.
3. Submit a source just below main SRAM, at its end, and a range beginning near
   the end that crosses the SRAM boundary. Also submit an overflowing pointer
   range, NULL and zero length. Require INVALID_ARG before hook dispatch, with
   unchanged target, source canaries and controller usability.
4. Copy a Flash-resident source into the caller's SRAM page buffer, erase only
   the reserved sector, then program and verify normally. Repeat a multi-page
   write with its entire source in SRAM. A rejected source must not prevent a
   later valid write, deinit or reinit.

## System Clock Procedure

1. In a dedicated startup image, request a nonzero frequency that the selected
   SDK/device PLL cannot realize. Require NOT_SUPPORTED from backend system
   init, no SDK panic, and a responsive debugger/owner after return. Do this
   before starting clock-dependent drivers or tasks; do not infer rollback of
   every earlier voltage or board initialization step from the error status.
2. Follow the rejected request with a supported board-approved frequency.
   Require OK and verify the measured system clock and stdio startup. Repeat
   across a fresh boot and verify the same errors propagate through the public
   HAL when the board fixture supplies the unattainable configuration.

## Evidence and Limits

Preserve source/target address snapshots, return codes, hook counters, canaries,
readback bytes and debugger traces. SDK compilation checks symbols and types;
host HAL fakes do not establish actual Flash safety or clock error behavior.
Power-loss recovery, voltage limits and concurrent OTA policy belong to the
consumer integration; record their measured behavior in that integration.
