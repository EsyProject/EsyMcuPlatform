<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# ESP32 interrupt-mask HIL plan

## Purpose

Verify that public HAL interrupt save/restore preserves the SDK's local interrupt
level through nested task and ISR calls. Native SDK compilation checks the port
API boundary; only target execution establishes interrupt-mask behavior.

## Integration

Build the ESP32 consumer with `ENABLE_ESY_SYSTEM` and the native ESP-IDF provider.
Use one owner core for the test. Record the SDK version, processor, core count,
interrupt priorities, toolchain and firmware revision. Shared objects accessed
from another core need their own synchronization; the IRQ token is not a lock.

Keep masked regions short. Record observations into fixed storage and check or
print them only after restoring the entry mask. Never log, block, allocate, or
perform Flash operations while testing with interrupts masked.

## Procedure

1. Enter from an ordinary task with interrupts enabled. Record `PS.INTLEVEL`
   using the debugger, call `esy_hal_system_irq_save()`, and verify that the SDK
   mask level is active. Restore its token and verify the original level.
2. Save an outer token, then an inner token. Restore the inner token and verify
   that the outer region remains masked. Restore the outer token and verify the
   original entry level. Repeat with the outer region already masked before the
   first tested save; its restore must not enable interrupts.
3. With an outer mask held, call a public HAL timer query that internally saves
   and restores interrupts. It must leave the outer level intact. Repeat through
   a permitted Logger path without output or blocking inside the masked region.
4. Pend an interrupt at a priority masked by the SDK level. It must remain pending
   after the inner restore and execute after the outer restore. Higher-priority
   exceptions outside the SDK mask are outside this exclusion guarantee.
5. Repeat nesting in a supported interrupt context, restoring tokens on the same
   core in reverse order. Verify callback nesting and scheduler operation after
   restoring the original mask. Do not exercise unsupported high-priority ISR
   APIs or migrate a saved token to another core.

Steps 2 through 4 verify that nested restore preserves the outer mask. Preserve
the raw observations with the consumer's target acceptance evidence; a successful
build or a HAL forwarding fake does not count as a passing HIL run.

## GPIO Wake Ownership Procedure

Also enable GPIO and reserve a valid wake-capable pin connected to a controlled
external signal. Keep GPIO lifecycle calls on the ISR service's owner core and
serialize them. The callback only records or posts a bounded notification; it
does not call lifecycle APIs. Record native return values, pin wake/IRQ state,
callback counts and handle state in fixed storage. Use debugger fault injection
or a consumer-local native-call interceptor for the failure cases below; do not
modify vendor sources.

1. Attach an ordinary GPIO IRQ, then attempt wake enable on the occupied pin.
   Expect `ESY_ERR_BUSY` without replacing the callback or changing wake state.
   Repeat rejected requests from the other core, with an invalid pin and with
   both-edge mode; the existing registration and wake state must remain intact.
2. Inject failure of `esp_sleep_enable_gpio_wakeup()`. Separately fail each
   rollback operation: `gpio_wakeup_disable()`, `gpio_intr_disable()` and
   `gpio_isr_handler_remove()`. Wake enable must report the error and preserve
   ownership whenever rollback fails. Keep the handle and callback argument
   alive. Repeated wake enable must return `ESY_ERR_STATE`; failed disable/deinit
   must preserve the handle and must not reset the pin.
3. Restore the failing native operation and retry disable or deinit. Only an
   `ESY_OK` result permits releasing the callback argument. Confirm the handler
   is detached, no later signal invokes the callback, and the pin no longer
   wakes the device. After successful deinit, initialize and enable again.
4. Repeat steps 2 and 3 with failure of per-pin `gpio_wakeup_enable()`, and with
   cleanup failure after an otherwise successful enable. A request rejected
   before borrowing resources must still permit successful ordinary deinit.
5. Exercise successful rising/high-level and falling/low-level wake cycles.
   Enable a second wake pin, detach the first, and verify the second remains a
   wake source. The shared GPIO wake class belongs to system sleep policy; pin
   cleanup must not disable that class for its peers.
6. Attach an ordinary edge IRQ and count external edges. Apply another input
   pull state and restore the configured state through the public GPIO API.
   Both calls must return `BUSY` before `gpio_config`; confirm the native trigger,
   pull, callback binding and edge delivery remain unchanged. Repeat with an
   active wake source and inspect its level trigger/wake state. Deinitialize the
   ordinary IRQ handle, or disable the wake source, then reconfigure successfully.
7. Claim a GPIO PM profile with a peer pin preceding the IRQ/wake pin. Request
   sleep so the peer changes first. Require `BUSY`, the peer restored to default,
   the IRQ/wake pin unchanged and continued event delivery. Release the profile,
   detach the IRQ/wake source and repeat a successful profile transition. Do not
   remove the ISR service or migrate the lifecycle owner during these checks.

Save the injected-failure trace and physical wake observations with the target
acceptance evidence. The observations cover native cleanup and physical wake
behavior separately from HAL forwarding tests.
