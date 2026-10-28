<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# ESP32 Timer Callback Lifecycle HIL Plan

## Purpose

Verify timer slot reuse from callbacks and native cleanup retention against the
real ESP-IDF timer provider. A consumer build proves SDK compatibility and
linkage; it does not establish delivery ordering or execute this plan.

## Integration and Ownership

- Enable the consumer's timer feature and initialize the HAL timer once.
- Run lifecycle calls and SDK timer callbacks on the documented owner core.
  Serialize management from application tasks with SDK timer callback dispatch.
  Do not manage these timers from an
  unrelated ISR or deinitialize the module from a callback.
- Keep callback work bounded. Record IDs, counters, callback argument identity
  and timestamps without blocking the timer task. Cancellation does not stop a
  callback already executing; preserve that callback's arguments until return.
- ESP32 reserves eight logical slots. An inactive slot with failed native
  cleanup remains reserved, suppresses user delivery and reports active until
  a later cancel/deinit retires the native handle.

Record ESP-IDF version, chip/board, core affinity, task priorities, timer
intervals, callback execution time, raw traces and native resource counts.

## Target Build Checks

Compile/link with the real ESP-IDF `esp_timer` component. Retain HAL init/start,
cancel/is_active/deinit calls and callback code that cancels its current ID and
starts a one-shot replacement. Build the backend with warnings as
errors. No backend host unit test substitutes for this target build or HIL.

## Hardware Procedure

1. Start one periodic timer A. In A's first callback, cancel A and start a
   one-shot B with a distinct callback argument. Arrange all lower-numbered
   slots to be occupied if necessary so B reuses A's backend slot. B must fire
   exactly once after its requested delay; A must not fire again. B's callback
   argument must match B rather than A.
2. Repeat with one-shot A. Its callback starts another one-shot without an
   explicit cancel. The completed A must release its native handle before user
   code runs, allowing a replacement even when all eight slots had been used.
3. Repeat periodic cancel/replacement with both one-shot and periodic B. Include
   a callback that starts B then cancels another timer. No returning dispatcher
   may stop, clear or delete a replacement timer.
4. Run several thousand one-shot callback replacements. At quiescence, verify
   there are no active logical timers, next-wake reports no timer, and the SDK
   timer allocation count returns to its baseline after deferred SDK deletes.
   Fill all eight slots; the next start must return ESY_ERR_NO_MEM without
   disturbing any existing timer.
5. Use a target-side debugger/fault hook to make native stop fail once. Cancel
   A, then verify user delivery stays suppressed, is_active(A) remains true and
   its slot cannot be reused. Restore stop, cancel again, and verify retirement
   and later slot reuse. Also inject native delete failure after successful
   stop; retry must accept the already-disarmed provider state and delete once.
6. Inject delete failure while a one-shot is being retired before user delivery.
   Its callback must run once, its native handle must remain reserved, and a
   replacement must use another available slot. A later cancel/deinit must
   recover the retained slot without delivering the completed callback again.
7. Inject native start failure followed by delete failure. Start must return an
   error, retain the native handle and suppress user delivery. Deinit must retry
   that cleanup. If cleanup still fails, init must return ESY_ERR_BUSY rather
   than overwrite the retained handle. Restore the provider and repeat deinit,
   init, start and cancel successfully.
8. Deinitialize with multiple active timers, then repeat deinit and reinitialize.
   No callback may access a released user argument. An injected deinit cleanup
   failure must retain native ownership while suppressing all user callbacks;
   repeated deinit must retire it after the provider is restored.

## Pass Criteria and Limits

The replacement callback executes exactly as configured, including when its
slot is reused from an older callback. Native stop/delete failures preserve the
handle and capacity claim until a successful retry; no error path leaks a live
native timer by clearing its only handle. ESP-IDF may defer native deletion to
its timer task, so measure resource counts after that task has drained deletes.

Callback jitter bounds and operation across OTA/Flash blackouts are outside
this plan.
