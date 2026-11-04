<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors
SPDX-License-Identifier: Apache-2.0
-->

# Classic ESP32 Explicit Sleep

## Purpose

Provide explicitly requested Light-sleep and reset-class Deep-sleep on classic
ESP32 through the pinned ESP-IDF 5.3.4 provider. Native sleep owns its SDK clock,
timer and cross-core handshake. Application quiescence remains with one system
owner. This path does not implement automatic device-graph or FreeRTOS tickless
idle policy.

## Public Surface

- `esy_hal_power_sleep()` accepts `RETENTION` and a positive relative timer delay
  in microseconds, returning entry, elapsed-time and timer/GPIO wake evidence.
- `esy_hal_shutdown(STANDBY)` enters Deep-sleep; success never returns and wake
  starts a new boot.
- Existing `esy_hal_gpio_enable_wakeup()` bindings supply optional GPIO wake
  sources for explicit Light-sleep. Their handles and callbacks remain owned by
  the GPIO owner through `esy_hal_gpio_disable_wakeup()`.
  Pass `ESY_GPIO_IRQ_HIGH_LEVEL` or `ESY_GPIO_IRQ_LOW_LEVEL`: ESP32 rejects edge
  requests without acquiring a binding. A level already asserted at sleep entry
  may prevent sleep or wake immediately; no new edge is required.

Generic atomic power capabilities remain `ACTIVE` only. The hardware alarm
interface used by the generic power manager returns `NOT_SUPPORTED`, as does
the existing ESP-IDF SMP tickless registration. Explicit sleep on Pico/STM32F4
returns `NOT_SUPPORTED` and preserves their existing atomic power interfaces.

## Integration

Enable `ENABLE_ESY_SYSTEM` and `ENABLE_ESY_LOW_POWER` in the consumer's selected
feature file and link `esy_hal`. The ESP32 backend needs the native
`esp_hw_support`, `esp_timer` and `esp_pm` components, with no Esy OS/middleware
link. The startup owner must install immutable execution checks through
`esy_hal_system_set_context_callbacks()`; `esy_os_init()` installs the standard
OS adapter binding. A native-SDK-only consumer supplies equivalent checks.
Without a binding, explicit sleep returns `ESY_ERR_STATE`. GPIO wake additionally
requires `ENABLE_ESY_GPIO` and successfully enabled HAL GPIO wake bindings.
`CONFIG_PM_ENABLE` may be enabled or disabled; if enabled, native automatic
Light-sleep must remain disabled. Do not install the generic tickless adapter
for this explicit path.

The following callable helper uses only the public HAL API. Its caller must
already hold the system quiescence barrier described below. No caller-owned
handle, workspace or provider resource needs initialization for this operation.

```c
#include "esy_hal/esy_hal_low_power.h"

esy_err_t sleep_quiescent_system(uint32_t duration_us,
                                 esy_power_sleep_result_t *out_result) {
  const esy_power_sleep_config_t request = {
      .state = ESY_POWER_STATE_RETENTION,
      .duration_us = duration_us,
  };
  return esy_hal_power_sleep(&request, out_result);
}
```

Check both the returned status and `entered` before recovery: a cleanup failure
can follow successful hardware entry. Reacquire application resources only
after the operation and any required cleanup retry have completed.

## Contracts

Call only from an ordinary schedulable task with local interrupts enabled. The
native OS boundary rejects ISR, masked-interrupt and suspended-scheduler calls.
The SDK can wait for its own locks and send cross-core IPC; do not invoke sleep
from an atomic power callback or park the other core in a non-interruptible
loop. The caller must serialize native sleep/PM reconfiguration for the full
operation and exclusively own the native timer-wake configuration. Only this
operation may own timer wake during Light-sleep; configure no other native wake
sources besides the borrowed HAL GPIO bindings. No previous native timer-wake
configuration is adopted or restored.

The system owner must obtain bounded stop acknowledgments for every active
application task and asynchronous provider before entry. This includes Flash
writes/erases and OTA, DMA/IRQ work, I/O, network borrowers, Wi-Fi/Bluetooth and
their callbacks. Hardware outputs must be safe. Native PM locks restrict the
automatic PM algorithm, not manual `esp_light_sleep_start()`: holding a native
driver lock cannot replace this system barrier. The API checks and rejects
native automatic Light-sleep but does not inspect all driver activity. A native
RTC watchdog must be stopped by its already-quiescent owner; an enabled RTC
watchdog causes `BUSY` rather than an implicit feed or disable.

Timer durations cover `1..UINT32_MAX` microseconds. The native RTC slow clock
determines wake resolution; entry/exit overhead and scheduling can extend the
call duration. The SDK can reject an insufficient duration or an already
asserted wake condition with `BUSY`. Each call is a new one-shot request: the
timer is disabled before return, including rejected entry. Elapsed time comes
from the signed 64-bit monotonic ESP Timer across the native call. Negative or
regressing observations return an error. No software wrapping deadline or
periodic retry is kept after completion; only an explicit call can rearm.

ESP Timer is advanced after Light-sleep. This manual path does not execute the
native automatic-sleep `vTaskStepTick()` settlement. All task and OS timer waits
must therefore be quiescent; use the returned monotonic duration or current
monotonic time to restart application deadlines. Cancel native timer work that
must not accumulate overdue callbacks. The provider preserves its native clock
restoration and timer behavior; no hard latency guarantee is made.

Returning paths disable only this operation's timer source. Existing GPIO wake
bindings are unchanged. A timer cleanup failure retains singleton ownership;
the next valid call performs cleanup only, returns `entered=false`, and cannot
reenter sleep. Preserve the same system owner and quiescence until cleanup
succeeds. Shutdown rejects a pending timer cleanup. Neither success nor failure
permits clearing live GPIO handles or callback storage.

Deep-sleep preserves the exclusive system owner's preconfigured native wake
sources. The shutdown API supplies no deep timer or RTC GPIO configuration,
and ordinary Light-sleep GPIO bindings are not deep wake sources. Without a
compatible configured source the chip needs external reset. Deep wake invalidates
ordinary handles and deadlines: reinitialize through the normal boot path.
RTC watchdog and automatic-Light-sleep rejection also apply to shutdown.

## Resource Model

The backend has one task-owned flag retaining an outstanding native timer
cleanup. It creates no module heap object, task, queue or timer callback. The
SDK owns its native sleep locks, RTC alarm programming, clock restoration and
cross-core control. Native provider configuration may impose additional resource
and callback costs; qualify those with the actual consumer. GPIO bindings and
their callback lifetimes stay with their existing owners.

## Verification

HAL unit tests cover invalid-request isolation, the full duration field, provider
error propagation and preservation of successful-entry evidence after failure.
The native consumer verifies compilation/linking of the real SDK call closure.
Run the [ESP32 low-power HIL plan](../tests/hil/esp32_low_power_hil_plan.md)
for actual timing, wake races, watchdog behavior, quiescence and current draw.

Provider references are the pinned [sleep API](https://docs.espressif.com/projects/esp-idf/en/v5.3.4/esp32/api-reference/system/sleep_modes.html),
[PM API](https://docs.espressif.com/projects/esp-idf/en/v5.3.4/esp32/api-reference/system/power_management.html),
[ESP Timer](https://docs.espressif.com/projects/esp-idf/en/v5.3.4/esp32/api-reference/system/esp_timer.html)
and [native PM implementation](https://github.com/espressif/esp-idf/blob/v5.3.4/components/esp_pm/pm_impl.c).

## Known Limits

- No automatic power-manager or unified tickless integration on ESP-IDF SMP.
- No preservation of a live Wi-Fi/Bluetooth connection across explicit sleep.
- The wrapper does not take over watchdog owners or settle task deadlines or
  quiesce devices. ESP-IDF may temporarily suspend and restore its native TWDT
  inside the SDK sleep sequence; qualify that behavior with the selected SDK.
- No native wake-source snapshot/restore API; exclusive ownership is required.
- No deep timer, RTC GPIO, touch, ULP, UART wake or RTC-memory retention interface.
- Initial qualification retains Flash power through Light-sleep. Consumer
  power-domain overrides require separate board-specific qualification.
