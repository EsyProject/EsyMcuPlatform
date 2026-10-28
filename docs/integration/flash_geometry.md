<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Flash geometry and image layout

The Flash backend owns hardware geometry. A board's boot layout owns the image
and metadata regions selected by the product. These are separate inputs: a
layout must fit the hardware rather than redefine its capacity or erase units.

## Public queries

Include `esy_hal/esy_hal_flash.h` and link the `esy_hal` target with
`ENABLE_ESY_FLASH` enabled:

```c
esy_flash_geometry_t geometry;
esy_err_t status = esy_hal_flash_get_geometry(&geometry);
if (status != ESY_OK) {
  return status;
}

esy_flash_sector_t sector;
status = esy_hal_flash_get_sector(geometry.base_address, &sector);
if (status != ESY_OK) {
  return status;
}
```

`esy_flash_geometry_t` contains `base_address`, `size_bytes` and `program_size`.
Its address range is half-open: `[base_address, base_address + size_bytes)`.
`esy_flash_sector_t` contains the `start_address` and `size_bytes` of the erase
sector containing the requested address. This supports variable-size sectors
without exposing a vendor sector number or requiring a single global erase size.

Both queries return `esy_err_t`, reject invalid pointers and preserve the output
on failure. They do not initialize, unlock, erase or program Flash and may be
called before `esy_hal_flash_init()`. Serialize them with backend lifecycle and
Flash access as required by the selected port. STM32's size register is available
without peripheral initialization; ESP32 still requires the SDK's default Flash
chip to be ready.

Writes require address and length alignment to `program_size`. Erases require
both endpoints to lie on actual sector boundaries and must cover whole sectors.
Range and boundary checks finish before the backend starts modifying Flash.
Neither operation rounds a caller's range outward. Callers that pad a final
program unit must ensure the padded range remains inside their assigned region.
Flash operations are synchronous and must be serialized by their owner. A
geometry query does not prove that a range is safe to erase while executing from
Flash or while another core, IRQ, DMA engine or watchdog is active.

## Platform sources

| Port | Address convention | Capacity source | Programming and erase units |
|---|---|---|---|
| Pico | Absolute XIP addresses | Selected SDK board's `PICO_FLASH_SIZE_BYTES` | SDK `FLASH_PAGE_SIZE` and `FLASH_SECTOR_SIZE` |
| STM32F4 | Absolute internal-Flash addresses | Factory `FLASHSIZE_BASE` register | Word programming and private single-bank sector map |
| ESP32 | Physical Flash offsets, starting at zero | `esp_flash_get_size()` for the SDK's default chip | Ordinary unencrypted `esp_flash_write()` supports byte granularity; erase size comes from the default chip driver |

The STM32F4 Flash port currently supports STM32F405/407/415/417 with 512 KiB or
1 MiB of internal Flash. Their single-bank map has four 16 KiB sectors, one
64 KiB sector and subsequent 128 KiB sectors up to the detected capacity.
`ENABLE_ESY_FLASH` rejects other STM32F4 device families during CMake
configuration. The source also rejects unverified CMSIS device definitions.
An unexpected factory capacity returns `ESY_ERR_NOT_SUPPORTED`; it never falls
back to a guessed 1 MiB range. Supporting another family requires verifying its
geometry and programming/erase behavior before extending the accepted set.
The current STM32 port uses word programming and voltage range 3, requiring a
2.7–3.6 V supply during Flash operations. Lower-voltage programming is not part
of this port's contract.
The factory-size register, sector map and programming parallelism follow
[ST RM0090](https://www.st.com/resource/en/reference_manual/dm00031020-stm32f405-and-stm32f407-advanced-armbased-32bit-microcontrollers-stmicroelectronics.pdf).

Pico's selected SDK board supplies external Flash capacity; identifying only the
RP2040/RP2350 chip does not determine the fitted memory. For ESP32, accessible
capacity follows the SDK's configured default chip, which may be smaller than
the physical device's raw capacity. The port uses offset addresses consistently;
memory-mapped cache addresses are not a second accepted Flash address space.
ESP32's sector metadata is read through the private SDK
`spi_flash_chip_driver.h` definition. This coupling stays in the backend and
must be checked by consumer compilation when changing ESP-IDF versions.
The configured-capacity and ordinary byte-write semantics follow the
[ESP-IDF 5.3 Flash API](https://docs.espressif.com/projects/esp-idf/en/v5.3/esp32/api-reference/peripherals/spi_flash/index.html).

## Boot and OTA consumers

`esy_bootloader_layout_t` contains only slot/metadata locations and
capacities. `meta_size[slot]` is the independently erasable capacity of the
metadata region at `meta_addr[slot]`, not the serialized metadata record length.
The entries may differ on nonuniform Flash geometry; initialize every entry,
even when both capacities are equal. This replaces the former scalar
`meta_size` and changes the public struct's source and binary layout. Rebuild
all providers and consumers together; do not mix objects built against the old
and new headers. The persistent metadata format and record length are unchanged.
Board ports must not supply duplicate `flash_write_size` or
`flash_erase_size` fields. Bootloader and OTA obtain geometry from public HAL and
validate their assigned regions against the actual address range and sector map.
Metadata erase must remain inside its assigned region. OTA erases each actual
sector only after confirming that it belongs entirely to the target image region.
Image-programming failures stop the writer before metadata publication. A later
metadata-programming or readback error does not prove that the persistent record
is unchanged; bootloader image/hash/signature validation remains authoritative.

The board still decides where images and metadata live; these queries do not
allocate partitions, choose an OTA slot or reserve a bootloader area. In
particular, placing a metadata record inside a large STM32 sector does not make
that record independently erasable. Allocate a complete sector to metadata when
the update flow erases it separately.

## ESP32 managed boot and OTA

ESP32 uses the ESP-IDF bootloader rather than direct vector-table handoff. The
boot runtime query reports managed mode and the physical offset of the running
app partition, obtained from `esp_ota_get_running_partition()`. Its result is
independent of a partition selected for the next reset. The CPU program counter
remains an execution address; do not compare it with physical slot offsets.

ESP-IDF `otadata` is the authority for native image selection, first-boot trial
state and rollback. Esy metadata retains the image size, version, hash, signature
and confirmation record. It does not replace `otadata`. Managed bootloader
initialization validates the running image instead of selecting another image
from Esy `TRY_BOOT` records. In particular, a record left behind after native
rollback must not cause the failed image to be retried automatically.

### Consumer configuration

Use an ESP-IDF partition table with the following consumer-owned resources:

| Resource | Required relationship to the board boot layout |
| --- | --- |
| Two native OTA app partitions, `ota_0` and `ota_1` | Each slot address and capacity exactly match its corresponding native app partition. Both app partitions have the same capacity. |
| Native OTA data partition, `data, ota` | Writable, unencrypted and at least two erase units; reserved exclusively for ESP-IDF boot records, not Esy metadata. |
| Dedicated `data` partitions with application-defined subtypes `0x40` through `0x7F` | Each Esy metadata region fits a writable, unencrypted custom data partition and owns complete erase sectors. |

Keep image and metadata regions disjoint, and keep the board layout consistent
with the actual partition table. These resources are configured by the consumer;
EsyMcuPlatform does not generate a product partition table. Provision the initial
Esy image metadata when installing the initial image; a native partition table
does not create that record. The running image must be in `ota_0` or `ota_1` on
the default Flash chip; running factory/test app partitions are outside this
managed boot contract. A freshly installed OTA image may have no native state
record yet; it is treated as an initial image rather than a pending trial.

Enable `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` in the consumer's SDK configuration.
Managed boot/OTA requires native rollback support and rejects an unsupported
configuration. Native Secure Boot still requires a correctly signed ESP-IDF
application image; the Esy metadata signature does not replace that image
signature. Native image verification is performed before selecting a new boot
partition. Follow the consumer's SDK security provisioning and image-generation
process rather than changing native security state through this library.

Flash encryption is unsupported by this update path. The current Flash backend
uses raw `esp_flash_read()` and `esp_flash_write()`; these do not transparently
decrypt or encrypt partition contents. Managed boot runtime queries reject an
encrypted device with `ESY_ERR_NOT_SUPPORTED`, so an update cannot begin through
the middleware. Supporting encryption would require partition-aware read/write
and its programming-alignment contract, not only a different capacity value.
See the [ESP-IDF 5.3 Flash encryption contract](https://docs.espressif.com/projects/esp-idf/en/v5.3/esp32/security/flash-encryption.html#reading-and-writing-data-in-encrypted-flash).

### Update and confirmation order

1. Initialize the SDK, system and Flash, then query and validate the board layout
   and boot runtime. All boot/Flash operations share one serialized owner.
2. Confirm a newly running trial image only after the consumer's diagnostics
   succeed. While native state is `PENDING_VERIFY`, `update_allowed` is false
   and a new OTA writer must reject the operation before erasing its target.
   This preserves the previous image needed for rollback.
3. Write the inactive image and finish its integrity checks. OTA finish writes
   the Esy metadata, then calls `esy_bootloader_activate_image()` to select the
   completed native image. Activation failure is an OTA failure.
4. Reset through the consumer's normal shutdown/restart policy after successful
   activation. Activation itself does not reset. Do not select the same image
   again for an ordinary reset: `esp_ota_set_boot_partition()` creates a new
   native trial state when rollback is enabled.
5. On the new image's first boot, run diagnostics and call
   `esy_bootloader_mark_success()`. This writes
   the Esy confirmation record and then confirms the native running image.
   An already-confirmed Esy record still permits retrying native confirmation.
   Without successful native confirmation, an unexpected reset triggers the
   SDK's rollback behavior.

Confirmation fails if a different partition has already been selected for the
next reset. The SDK confirmation operation must not accidentally confirm that
new selection while the previous image is still executing. The partition
selection and trial-state behavior follow the
[ESP-IDF 5.3 OTA contract](https://docs.espressif.com/projects/esp-idf/en/v5.3/esp32/api-reference/system/ota.html)
and its [OTA implementation](https://github.com/espressif/esp-idf/blob/v5.3/components/app_update/esp_ota_ops.c).

### Persistent-state failure windows

Esy metadata and native `otadata` are separate persistent records, with no atomic
transaction spanning both. A reset after metadata publication but before native
activation leaves an image that is not yet selected. Managed startup does not
automatically activate that stale candidate. A reset after successful activation
enters the native trial/rollback flow even if the consumer has not yet observed
the completion result.

If Esy confirmation succeeds but native confirmation fails or power is lost
between them, the image remains subject to native rollback. Retry confirmation
while it is still running, or accept the SDK's rollback after reset. A reported
Flash or SDK error does not prove that a persistent write had no effect; callers
must not assume transactional rollback of metadata or boot selection.

Target recovery procedures cover activation and normal restart, a successful
confirmation, diagnostics failure, unexpected reset before confirmation,
confirmation retries, rejection of updates during a pending trial, and power
loss between both pairs of persistent writes. Record the selected image and
confirmation state after each reset or interrupted write.

## Verification

Use the status-returning geometry and sector queries to check Flash ranges.
Align writes explicitly; writes do not pad the final word and erase requests
do not round outward. ESP32 consumers pass physical offsets for every Flash
operation.

Verification combines HAL/bootloader/OTA contract tests, CMake acceptance and
rejection cases, source API closure, real SDK compilation and hardware checks.
The backend has no host unit-test project. A successful compile confirms SDK
compatibility and cannot establish register values or the safety of an erase.

On hardware, query geometry before and after initialization and verify identical
results. Check the first/last byte, the one-past-end address, a range overflowing
the address space and both sides of every sector transition. On both supported
STM32 densities, confirm that the upper boundary follows the factory register.
Use an explicitly reserved scratch region to verify that misaligned or
cross-region requests leave neighboring sectors untouched, and that aligned
erase/program/readback succeeds. Also verify protection/error reporting,
power-loss recovery, multicore/IRQ exclusion and watchdog timing for the actual
consumer. Repeat these checks when changing the device, Flash chip or SDK.
