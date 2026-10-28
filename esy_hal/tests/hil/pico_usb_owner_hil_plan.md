<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Pico USB Callback Ownership HIL Plan

## Purpose and Setup

Verify exclusive callback ownership on the Pico USB controller with the actual
TinyUSB provider and a host cable. Enable USB through the consumer feature
import. One startup owner initializes the board/TinyUSB stack once, owns its
class drivers and dispatch, and performs final controller shutdown separately.
Initialize two independent HAL handles with `{0}`. Run their lifecycle
and TinyUSB callback dispatch on one serialized owner context; stop callback
dispatch before releasing handle or callback-argument storage.

## Procedure

1. Initialize handle A at physical controller zero with callback A, then
   initialize B on the same controller with callback B. Require BUSY from B,
   an unchanged live A, and no retained B context. Host connect/disconnect and
   suspend/resume events must reach A only.
2. Deinitialize rejected B and confirm that A still receives host events.
   Repeated init of live A returns STATE without replacing callback A.
3. Quiesce TinyUSB callback dispatch, deinitialize A, then initialize B.
   Resume dispatch and require events to reach B only, with no access to A's
   released callback storage. The controller must remain enumerated and any
   independently owned TinyUSB class traffic must continue: handle deinit only
   detaches the event binding. Repeat A/B handover at least 100 times, including
   direct backend init/deinit with a zero-initialized handle.
4. Request a nonzero physical index and require INVALID_ARG before replacing
   any controller context or callback owner. Follow rejection with a legal
   index-zero init on the same zero handle.
5. With no HAL event binding, exercise host traffic and the shared stack owner's
   class APIs; no retired HAL callback may run. Only after every class user is
   stopped may the unique stack owner perform its own disconnect/controller
   shutdown. Measure that separate operation when hardware quiet is required;
   do not use handle deinit as a shutdown or low-power acknowledgement.

## Evidence and Limits

Record MCU, SDK/TinyUSB/compiler versions, feature inputs, return values,
callback counts/arguments and host event traces. Generic endpoint support is
outside this plan.
