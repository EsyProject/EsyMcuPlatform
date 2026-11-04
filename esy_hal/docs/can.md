<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# CAN

## Purpose

The CAN HAL provides caller-driven frame transmission, reception, acceptance
filters, error diagnostics and optional explicit bus-off recovery. Providers
use STM32F4 bxCAN registers or classic ESP32 ESP-IDF TWAI for Classical CAN. The portable
frame and capability types also describe CAN FD; no built-in provider currently
implements FD or bit-rate switching.

The HAL creates no task or application interrupt callback. STM32F4 has no DMA
or OS dependency and works from an existing task or bare-metal owner loop.
ESP32 uses its native FreeRTOS-based driver with an SDK IRQ and RX queue.
Product scheduling,
transceiver control, OTA coordination and protocol policy remain consumer-owned.

## Public Surface

Include `esy_hal/esy_hal_can.h` and link `esy_hal`. Shared value types live in
`esy_common/defs/peripheral/esy_can_defs.h`.

| API | Behavior |
| --- | --- |
| `esy_hal_can_default_cfg`, `init`, `deinit` | Configure and own a stable caller handle; initialize into stopped state. |
| `start`, `stop` | Enter bus operation, or abort pending transmission and quiesce it. |
| `send` | Copy at most one frame to a free hardware mailbox. |
| `receive` | Copy and release at most one received frame. |
| `set_filters` | Replace all acceptance rules while stopped. |
| `get_status` | Sample error state, counters, overrun and pending frame counts. |
| `recover` | Request manual bus-off recovery; completion is observed through status. |
| `get_caps` | Query implemented modes, filters, recovery and FD/BRS capabilities. |

Frame lengths are byte counts. Classical CAN accepts 0..8 bytes; the FD value
type accepts 0..8, 12, 16, 20, 24, 32, 48 or 64. FD forbids RTR; BRS and ESI
require FD, and transmit ESI must be false. Unsupported FD requests fail before
frame submission. All identifiers and masks are unshifted 11-bit or 29-bit
values; vendor register types do not appear in public headers.

## Integration

Set the feature in the consumer's `esy_feature_import.cmake`:

```cmake
set(ENABLE_ESY_CAN ON)
```

The providers support STM32F405/407/415/417 device selections, such as
`ESY_TARGET_DEVICE=STM32F407ZG`, and classic `ESY_TARGET_DEVICE=ESP32`.
Other STM32F4 families and Pico fail configuration explicitly.
Disabling the feature omits both CAN wrapper and backend sources. Neither
provider requires `ENABLE_ESY_SYSTEM` or `ENABLE_ESY_DMA`. STM32F4 requires no OS
target or vendor `HAL_CAN_MODULE_ENABLED`/CAN driver source. ESP32 requires
the native `idf::driver` provider. Its SDK driver uses the native kernel internally;
the backend has no dependency on the Esy OS wrapper. The pinned
provider is ESP-IDF v5.3.4; its TWAI API belongs to the `driver` component.

For STM32F4, before initialization the consumer prepares the board's CAN pin routing,
peripheral input clock and external transceiver. Retain these resources until
deinitialization succeeds. The driver does not infer pins or control transceiver
enable/standby. The backend enables the CAN peripheral gates, preserving their
previous enable bits; successful deinit restores those bits. APB1 clock-rate
configuration remains consumer-owned. Supply timing for that actual input clock:

```text
bitrate = input_clock_hz / (prescaler * (1 + segment1 + segment2))
```

For bxCAN, `prescaler` is 1..1024, `segment1` is 1..16, `segment2` is 1..8 and
`sjw` is 1..4, with `sjw <= segment2`. The default timing is an initializer,
not a promised bitrate for the board.

This setup accepts standard data frames with identifier `0x123`. The caller
owns handle lifetime and supplies board-derived timing:

```c
#include "esy_hal/esy_hal_can.h"

esy_err_t configure_can(esy_can_t *can, const esy_can_timing_t *timing) {
  esy_can_config_t config;
  esy_can_filter_t filter = {
      .id = 0x123u,
      .mask = ESY_CAN_STD_ID_MAX,
      .match_rtr = true,
      .rtr = false,
  };
  esy_err_t result;

  if (can == NULL || timing == NULL) return ESY_ERR_INVALID_ARG;
  esy_hal_can_default_cfg(&config);
  config.phys_index = 1u;
  config.nominal = *timing;
  result = esy_hal_can_init(can, &config);
  if (result == ESY_OK)
    result = esy_hal_can_set_filters(can, &filter, 1u);
  if (result == ESY_OK) result = esy_hal_can_start(can);
  return result;
}
```

Initialize the caller's handle with `{0}` before this function. On any setup
failure, call `esy_hal_can_deinit()` and retain the handle and board resources
until cleanup succeeds. The function intentionally returns the original setup
error so that the caller can apply its own retry/recovery policy.

### Classic ESP32 Setup

Physical index 1 selects the only native TWAI controller (native index 0).
Supply distinct `config.port.tx_pin` and `rx_pin` from the board resource map;
TX must be output-capable and RX must be a valid GPIO. The SDK configures their
GPIO-matrix routing during init. The consumer owns pin exclusivity and external
transceiver power/standby until deinit succeeds; the backend does not infer a
board hook or release/reset the pins on its behalf.

`config.port.rx_queue_capacity` selects 1..255 native frame slots. The default
initializer uses `ESY_CAN_DEFAULT_RX_QUEUE_CAPACITY` (16), unselected pins and
physical index zero. STM32F4 ignores the appended `port` fields. This extends
the public configuration layout: rebuild consumers; existing STM32F4 field
initializers remain valid.

TWAI timing uses the APB input clock and the same bitrate equation. The
prescaler must be even in 2..128; builds selecting minimum silicon revision 2
or later additionally permit multiples of four in 132..256. Segments and SJW
have the same 1..16, 1..8 and 1..4 limits, with SJW no greater than segment2.
The generic default prescaler of one is deliberately rejected. For an 80 MHz
APB clock, `{10, 13, 2, 1}` selects 500 kbit/s. Native PM locks protect the
provider's clock according to its configured SDK power-management contract.

This setup borrows board-selected pins and an already assembled SDK/FreeRTOS
provider. Call it from the serialized non-ISR owner with a stable `{0}` handle;
schedule receive/status in that same owner and deinitialize before releasing
pins or the transceiver:

```c
esy_err_t configure_esp32_can(esy_can_t *can, uint32_t tx_pin, uint32_t rx_pin) {
  esy_can_config_t config;
  const esy_can_filter_t filter = {.id = 0x123u, .mask = ESY_CAN_STD_ID_MAX};
  if (can == NULL) return ESY_ERR_INVALID_ARG;
  esy_hal_can_default_cfg(&config);
  config.phys_index = 1u;
  config.nominal = (esy_can_timing_t){10u, 13u, 2u, 1u};
  config.port.tx_pin = tx_pin;
  config.port.rx_pin = rx_pin;
  esy_err_t result = esy_hal_can_init(can, &config);
  if (result == ESY_OK) result = esy_hal_can_set_filters(can, &filter, 1u);
  if (result == ESY_OK) result = esy_hal_can_start(can);
  return result;
}
```

The example assumes an 80 MHz APB clock. On any setup failure call deinit and
retry cleanup until it succeeds; retain the handle and board resources meanwhile.
Normal operation copies frames through send/receive; no frame pointer remains
borrowed after either returns.

## Contracts

All calls share one externally serialized non-ISR owner, including lifecycle
operations on different handles. Neither public APIs nor status calls provide
task synchronization. Do not mix HAL ownership with direct SDK/register access
to the same CAN domain.

Initialization starts stopped and rejects every frame until filters are
installed. STM32F4 routes up to 28 identifier/mask rules to receive FIFO0. Rules always
match standard versus extended format, and optionally match RTR. Zero rules
reject all frames; to accept every identifier of both formats, install separate
standard and extended rules with zero identifier masks. Filter replacement is
legal only while stopped, and invalid rules are rejected before mutation.

`send()` success reports mailbox acceptance. It does not report acknowledgement
or delivery. STM32F4's three hardware mailboxes can remain occupied when automatic
retransmission is enabled; the consumer must decide when to stop or abandon
traffic. Full TX and empty RX return `ESY_ERR_BUSY` without waiting. Receive
copies the complete value and clears unused payload bytes. Classical wire DLC
values 9..15 are normalized to eight payload/request bytes. The consumer's
receive frequency must account for the three-entry hardware receive FIFO and
actual bus traffic; STM32F4 has no software queue.

ESP32 has one hardware TX buffer and disables the SDK TX queue. An occupied
buffer returns `BUSY`, and `auto_retransmit=false` maps to native single-shot
transmission. The SDK ISR copies received frames into its configured RX queue.
Native acceptance admits all frames; receive copies/releases at most one queue
entry and checks at most `ESY_CAN_ESP32_FILTER_CAPACITY` (28) copied rules.
Format, identifier mask and optional RTR matching remain exact. A rejected
entry returns `BUSY` without changing the caller output, even if a matching
frame remains later in the queue. Consequently `BUSY` is not an empty-queue
certificate. `rx_pending` describes raw queue occupancy, before software
filtering; unrelated traffic can fill that queue and must be included in the
owner's scheduling and capacity budget. No receive loop scans an unbounded
number of unmatched frames. Setting filters only changes fixed backend storage;
it creates no SDK object and retains no caller array.

Status samples hardware counters and the last native error code without
clearing sticky diagnostics. Separate register samples are not a single atomic
bus snapshot. The bxCAN `last_error` field uses raw ESR LEC values 0..7. Receive
overrun remains observable until deinit/init. ESP32 samples SDK TEC/REC, derives
warning/passive state, reports zero `last_error` because the SDK does not expose
it, and latches `rx_overrun` when SDK missed/overrun counters are nonzero. Missed
frames include RX queue exhaustion and SDK errata-workaround loss. Listen-only
error counters are unavailable and the SDK reports them as zero.

STM32F4 automatic bus-off management is disabled. After observing `BUS_OFF`, call
`recover()` once and continue status polling. A successful request enters
`RECOVERING`; it does not promise an operational bus. bxCAN must observe its
required 128 occurrences of 11 recessive bits before recovery completes.
Repeated requests during recovery return `BUSY` without restarting it. Submit
new frames only after status reports `ERROR_ACTIVE` or `ERROR_PASSIVE`. The
consumer owns any recovery deadline, retry limit and decision to restart the
application; the driver has no delayed retry that can reactivate later.

ESP32 capabilities are `LISTEN_ONLY | FILTER`; `RECOVERY` is absent and
`recover()` returns `NOT_SUPPORTED`. Bus-off remains electrically silent until
the owner chooses cleanup. Stop accepts that state without calling native
recovery and reports stopped; pending native TX cannot restart. Start then
returns `STATE` until successful deinit/init. SDK uninstall accepts BUS_OFF,
so cleanup/reinitialization never waits for a recessive recovery sequence.

Each STM32F4 control handshake has at most `poll_limit` volatile register observations,
from 1 through `ESY_CAN_MAX_POLL_LIMIT`; the default is
`ESY_CAN_DEFAULT_POLL_LIMIT`. This is a CPU-work budget, not milliseconds or a
hard wall-clock deadline. CPU frequency, bus access and preemption affect elapsed
time. No tick counter or rollover arithmetic is used. A failed transition
returns `TIMEOUT`, faults the handle and retains ownership for stop/deinit retry.
If init itself failed, only deinit is legal until cleanup succeeds; status and
stop require a previously successful init.

ESP32 uses synchronous SDK control operations with no software timeout/retry
loop. `poll_limit` remains range-checked for configuration consistency but is
not interpreted as an SDK time deadline. Zero-wait send/receive process one
native frame; SDK locking/ISR activity still contributes latency. No timestamp
or software timer controls this backend, so tick-wrap tests do not apply.

Stop aborts outstanding transmission and confirms quiescence before success.
Already queued receive frames remain available for explicit draining while
stopped. A successful deinit clears the handle and permits reinitialization;
repeated deinit of a zero handle succeeds. Repeated init of a claimed handle
returns `STATE`. Never clear a live or faulted handle to bypass failed cleanup.
ESP32 start resets its native RX queue, so drain required pre-stop frames before
restart. Any control/cleanup failure retains the native driver and exclusive
claim; retry stop/deinit as applicable. Failed deinit blocks data/start until
cleanup succeeds. SDK installation failures release their partial resources
and leave a zero handle reusable.

## Resource Model

The consumer owns a stable, zero-initialized `esy_can_t` and all board routing,
clock-input and transceiver resources. Frames, configurations and filter arrays
are borrowed only during their calls. The backend holds fixed static state and
retains no payload pointers. STM32F4 performs no allocation. ESP32's native
provider allocates its driver, RX queue, semaphore, IRQ and optional PM resources
during init and releases them during deinit; those calls may allocate/block
inside the SDK. They are not ISR or hard-time operations. No provider resource
is created or destroyed by start, stop, filter, data or status calls. Fixed
backend context owns the copied filters. No platform task or application
callback is created; the native ISR accesses only SDK-owned storage.

STM32F4 CAN1 and CAN2 share filter and clock resources. The initial provider
reserves that complete domain for one handle, selecting physical controller 1
or 2. A competing handle returns `BUSY`, including when it selects the other
controller. This intentionally excludes simultaneous CAN1/CAN2 use and native
third-party ownership of either controller. Initialization and successful deinit
reset that complete hardware domain, discarding pending receive entries on
deinit. No interrupt handler or DMA channel is installed. CAN3 and automatic
power-management registration are unsupported.

ESP32 owns one TWAI controller and both selected pins exclusively. A competing
facade/native driver is rejected without adoption. The SDK owns IRQ placement
and native synchronization; all HAL calls remain externally serialized in
non-ISR context. SDK initialization/teardown does not transfer transceiver or
board pin ownership. Automatic HAL power-management registration is unsupported.

## Verification

Run the focused HAL facade tests and feature-assembly regressions:

```sh
cd esy_hal/tests
ceedling test:esy_hal_can
cd ../..
python3 esy_hal/tests/test_cmake_consumer.py
tools/checks/check_public_header_compatibility.sh esy_common/defs esy_hal esy_hal_backend
tools/checks/check_esy_hal_backend_contracts.sh
tools/checks/check_no_dynamic_alloc.sh
```

Facade tests use a deterministic reusable provider fake for validation,
dispatch, outputs, errors and retained cleanup. Control-timeout regressions check
FAULT reporting, retained ownership, rejected restart and successful stop/deinit
cleanup before reuse. They do not simulate CAN
arbitration, register transitions, electrical behavior or bus-off recovery.
Compile the real backend with the consumer's STM32F4 CMSIS provider and Arm
toolchain, or build the ESP32 provider against the pinned real SDK. Then execute
the applicable [CAN hardware plan](../tests/hil/can_hil_plan.md).
Record the actual board, input clock, timing, transceiver,
bus load and observed recovery behavior.

## Known Limits

- STM32F405/407/415/417 bxCAN and classic ESP32 TWAI are supported. Pico has no built-in CAN controller;
  an external-controller or PIO implementation is outside this module.
- ESP32 internal loopback and explicit bus-off recovery are unsupported. Its
  native driver/IRQ depends on FreeRTOS; it does not provide a bare-metal port.
- ESP32 traffic may be lost through RX queue saturation or native silicon-errata
  workarounds. Keep SDK workarounds enabled and validate actual bus loading.
  No lossless, hard-real-time or OTA-parallel operation is promised.
- CAN FD, BRS, simultaneous CAN1/CAN2 use, CAN3, interrupt callbacks, receive
  timestamps and per-frame transmit-completion reporting are unavailable.
- ISO-TP and CANopen are not included. Add protocol middleware through the public
  HAL when required by a product.
- Application-level delivery, response time, low-power, OTA quiescence and
  recovery policies belong to the consumer. Board procedures exercise these
  policies with the selected transceiver, timing and bus load.

The SDK behavior is documented in the
[ESP-IDF v5.3.4 TWAI guide](https://docs.espressif.com/projects/esp-idf/en/v5.3.4/esp32/api-reference/peripherals/twai.html)
and its [public driver API](https://github.com/espressif/esp-idf/blob/v5.3.4/components/driver/twai/include/driver/twai.h).
