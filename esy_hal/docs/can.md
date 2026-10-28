<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# CAN

## Purpose

The CAN HAL provides caller-driven frame transmission, reception, acceptance
filters, error diagnostics and explicit bus-off recovery. The initial hardware
provider uses STM32F4 bxCAN registers and supports Classical CAN. The portable
frame and capability types also describe CAN FD; no built-in provider currently
implements FD or bit-rate switching.

The HAL has no task, interrupt callback, DMA or OS dependency. A consumer can
drive it from an existing task or bare-metal owner loop. Product scheduling,
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

The current provider supports STM32F405/407/415/417 device selections, such as
`ESY_TARGET_DEVICE=STM32F407ZG`. Other STM32F4 families and enabling CAN on Pico
or ESP32 fail configuration explicitly.
Disabling the feature omits both CAN wrapper and backend sources. CAN does not
require `ENABLE_ESY_SYSTEM`, `ENABLE_ESY_DMA`, any OS target, or the vendor
`HAL_CAN_MODULE_ENABLED` configuration and CAN driver source.

Before initialization the consumer prepares the board's CAN pin routing,
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

## Contracts

All calls share one externally serialized non-ISR owner, including lifecycle
operations on different handles. Neither public APIs nor status calls provide
task synchronization. Do not mix HAL ownership with direct SDK/register access
to the same CAN domain.

Initialization starts stopped and rejects every frame until filters are
installed. Up to 28 32-bit identifier/mask rules feed receive FIFO0. Rules always
match standard versus extended format, and optionally match RTR. Zero rules
reject all frames; to accept every identifier of both formats, install separate
standard and extended rules with zero identifier masks. Filter replacement is
legal only while stopped, and invalid rules are rejected before mutation.

`send()` success reports mailbox acceptance. It does not report acknowledgement
or delivery. Three hardware mailboxes can remain occupied when automatic
retransmission is enabled; the consumer must decide when to stop or abandon
traffic. Full TX and empty RX return `ESY_ERR_BUSY` without waiting. Receive
copies the complete value and clears unused payload bytes. Classical wire DLC
values 9..15 are normalized to eight payload/request bytes. The consumer's
receive frequency must account for the three-entry hardware receive FIFO and
actual bus traffic; there is no hidden software queue.

Status samples hardware counters and the last native error code without
clearing sticky diagnostics. Separate register samples are not a single atomic
bus snapshot. The bxCAN `last_error` field uses raw ESR LEC values 0..7. Receive
overrun remains observable until deinit/init.

Automatic bus-off management is disabled. After observing `BUS_OFF`, call
`recover()` once and continue status polling. A successful request enters
`RECOVERING`; it does not promise an operational bus. bxCAN must observe its
required 128 occurrences of 11 recessive bits before recovery completes.
Repeated requests during recovery return `BUSY` without restarting it. Submit
new frames only after status reports `ERROR_ACTIVE` or `ERROR_PASSIVE`. The
consumer owns any recovery deadline, retry limit and decision to restart the
application; the driver has no delayed retry that can reactivate later.

Each control handshake has at most `poll_limit` volatile register observations,
from 1 through `ESY_CAN_MAX_POLL_LIMIT`; the default is
`ESY_CAN_DEFAULT_POLL_LIMIT`. This is a CPU-work budget, not milliseconds or a
hard wall-clock deadline. CPU frequency, bus access and preemption affect elapsed
time. No tick counter or rollover arithmetic is used. A failed transition
returns `TIMEOUT`, faults the handle and retains ownership for stop/deinit retry.
If init itself failed, only deinit is legal until cleanup succeeds; status and
stop require a previously successful init.

Stop aborts outstanding transmission and confirms quiescence before success.
Already queued receive frames remain available for explicit draining while
stopped. A successful deinit clears the handle and permits reinitialization;
repeated deinit of a zero handle succeeds. Repeated init of a claimed handle
returns `STATE`. Never clear a live or faulted handle to bypass failed cleanup.

## Resource Model

The consumer owns a stable, zero-initialized `esy_can_t` and all board routing,
clock-input and transceiver resources. Frames, configurations and filter arrays
are borrowed only during their calls. The backend holds fixed static state and
does not allocate memory or retain payload pointers.

STM32F4 CAN1 and CAN2 share filter and clock resources. The initial provider
reserves that complete domain for one handle, selecting physical controller 1
or 2. A competing handle returns `BUSY`, including when it selects the other
controller. This intentionally excludes simultaneous CAN1/CAN2 use and native
third-party ownership of either controller. Initialization and successful deinit
reset that complete hardware domain, discarding pending receive entries on
deinit. No interrupt handler or DMA channel is installed. CAN3 and automatic
power-management registration are unsupported.

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
toolchain, then execute the [CAN hardware plan](../tests/hil/can_hil_plan.md).
Record the actual board, input clock, timing, transceiver,
bus load and observed recovery behavior.

## Known Limits

- Only STM32F405/407/415/417 bxCAN devices are supported. Pico has no built-in CAN controller;
  an external-controller or PIO implementation is outside this module.
- ESP32 TWAI and native CAN FD providers are not implemented. Capability bits
  describe implementation support, not merely silicon features.
- CAN FD, BRS, simultaneous CAN1/CAN2 use, CAN3, interrupt callbacks, receive
  timestamps and per-frame transmit-completion reporting are unavailable.
- ISO-TP and CANopen are not included. Add protocol middleware through the public
  HAL when required by a product.
- Application-level delivery, response time, low-power, OTA quiescence and
  recovery policies belong to the consumer. Board procedures exercise these
  policies with the selected transceiver, timing and bus load.
