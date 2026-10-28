<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# ESP32 I2C Master HIL Plan

## Purpose

Verify board-completed SDA/SCL routing, master addressing, repeated START,
synchronous completion and recovery through the native ESP-IDF master driver.
SDK compilation establishes interface closure; the procedures below cover
physical pin routing, bus timing and acknowledgements.

## Setup and Ownership

- Use the classic ESP32 target and native `esp_driver_i2c`, `esp_driver_gpio`
  and `esp_timer`
  components. The verified SDK provider is ESP-IDF 5.3.4, including addressless
  master devices and `i2c_master_execute_defined_operations`.
- One owner serializes the handle, controller and board mapping. Do not share
  the controller with another native driver or independently owned handle.
  Initialize handle storage to `{0}` and have the board hook fill distinct
  output-capable `inner.sda_pin` and `inner.scl_pin`. HAL initializes these
  fields to `ESY_PIN_UNUSED` before invoking the hook.
- Use board-approved pins, suitable pull-ups, a logic analyzer and known 7-bit
  and 10-bit peers as applicable. The configured speed must be nonzero and at
  most 1 MHz, within the tested controller, peer and board's actual limits.
- Record firmware revision, board/MCU, SDK/toolchain, controller, physical pins,
  pull-ups, measured speed, SDK configurations and native return codes. Record
  SDK heap usage after initialization separately from transfer-path heap use.

## Procedure

1. Initialize through `esy_hal_i2c_init`. Break at `i2c_new_master_bus` and
   `i2c_master_bus_add_device`; verify board-selected pins and configured speed.
   Exactly one bus and one addressless device must be created. The async queue
   depth remains zero and no native completion callback is installed.
2. Exchange bytes using blocking read/write and combined transfer APIs. Exercise
   at least two different target addresses and verify address changes create no
   new native devices. Measure SDK heap use over repeated traffic and verify no
   per-transfer command-link or device allocation. Transfers retain neither
   caller buffers nor callback contexts after returning.
3. Capture combined write/read with `repeated_start=true`: verify an address
   write phase followed by repeated START and address read phase, without an
   intervening STOP. Repeat with `repeated_start=false`: verify two complete
   transactions with STOP then START. Both native transactions share one
   `timeout_ms` budget; the second receives only the remaining time. Use a
   clock-stretching peer to consume part of the write budget and inspect the
   reduced receive timeout. Expiry returns TIMEOUT and zero selects a finite
   1000 ms default. Record native millisecond/tick resolution and cleanup latency.
   Exercise single-byte and multi-byte reads, with ACK on intermediate bytes
   and NACK on the final byte. For 10-bit peers, verify both address bytes and
   the required repeated START before the read header, including read-only
   transfers and combined transfers with separate write/read transactions.
   Explicitly exercise raw addresses 0x100, 0x355 and 0x3ff through blocking and
   DMA-named helpers as well as combined transfers. Verify the full 10-bit
   address on the wire, without an 8-bit truncation. In 7-bit mode, the same
   addresses must reject before bus activity; reject 0x400 in 10-bit mode.
4. Query capabilities and require MASTER_TRANSFER, REPEATED_START,
   DMA_TRANSFER and RECOVERY. When board DMA metadata allows the DMA-named API,
   verify it executes synchronously, invokes completion exactly once before
   returning success, and retains no buffers afterward. Inject a NACK/timeout;
   verify failure returns without invoking the success callback.
5. While idle, call `esy_hal_i2c_recover_bus`. Break at
   `i2c_master_bus_reset`; verify the existing bus/device are retained and
   traffic resumes on the same pins at the original speed. Capture any recovery
   clocks supplied by the native SDK, without assuming every external stuck
   bus can be recovered.
6. Use a separate zero-initialized handle to verify slave initialization returns
   `ESY_ERR_NOT_SUPPORTED` without reserving a controller or configuring a slave.
   Runtime `set_speed` on the original master also returns
   `ESY_ERR_NOT_SUPPORTED` without replacing the native device. Existing master
   traffic must retain its original frequency after a rejected speed change.
   Deinitialize, initialize at another supported speed, and verify the board
   hook runs again and the measured frequency changes.
7. Exercise invalid pins (unused, equal, input-only and out-of-range), zero or
   above-limit frequency, invalid addressing mode, out-of-range target address,
   NULL buffers with nonzero lengths and oversized byte counts/timeouts.
   Initialization rejects invalid input before creating a native bus; transfer
   validation rejects before bus activity. Use GPIO choices valid for the
   tested device and avoid reserved or live shared outputs.
8. Inject SDK bus/device creation failure where supported. If cleanup succeeds,
   verify the handle is reusable and the controller reservation is released.
   Also fail bus deletion after device creation has failed: initialization must
   return a cleanup error while retaining its context and reservation. Another
   handle must return `ESY_ERR_BUSY` before changing board pins. Remove the
   cleanup fault, require `ESY_OK` from deinit, then retry initialization.
9. Separately fail `i2c_master_bus_rm_device` and `i2c_del_master_bus` during
   deinit. Require an error, retained handle/reservation and no board pin reset.
   After a successful device removal followed by failed bus deletion, retries
   must delete only the still-owned bus. Require successful deinit before handle
   reuse; repeated successful deinit is harmless. Record SDK heap counts through
   repeated failure/retry cycles and require no leaked native resources.
10. Attempt a second master handle on the same controller while the first is
    active and runtime-suspended. Require `ESY_ERR_BUSY` before any board
    pin/configuration or SDK call. A separate zero-initialized slave handle must
    instead return `ESY_ERR_NOT_SUPPORTED`, both while the master owns the
    controller and after its deinit; unsupported slave mode is rejected before
    controller reservation. Neither rejection may change the original owner.
    Resume the original master and confirm communication still works. After
    full successful deinit, initialize the second master handle. During native
    suspend cleanup failure, require the device graph to fault and retain the
    original reservation for system recovery.

## Evidence and Limits

Preserve configuration/return snapshots, raw bus captures, heap measurements and
callback counts. This plan does not qualify concurrent I/O, native-driver sharing,
slave mode, native asynchronous DMA, electrical low-power behavior or all
external bus recovery cases.
