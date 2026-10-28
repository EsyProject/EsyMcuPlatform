<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Hardware Layers

## Repository scope

EsyMcuPlatform supplies common types and errors, public hardware APIs, private
hardware backends, reusable middleware, and cross-project test support. The
consumer owns application startup, feature selection, task/service/model code,
product resource policy, the board port and final firmware linking. The library
does not require a particular application directory layout.

| Directory | Responsibility |
| --- | --- |
| `esy_common/` | Hardware-independent types, errors, utilities, data structures and wire-format codecs. |
| `esy_hal/` | Public hardware abstraction and private assembly of board/backend resources. |
| `esy_hal_backend/` | Hardware SDK adaptation for each supported platform. |
| `esy_middleware/` | Reusable capabilities such as OS wrappers, logging, protocols, boot/OTA, USB and GUI integration. |
| `cmake/` | Optional SDK/provider and firmware build helpers. |
| `esy_test/` | Reusable host fakes, fixtures and test helpers. Module-specific tests stay in their module's `tests/` directory. |
| `tools/` | Quality and maintenance scripts for this repository. |

## Interfaces and dependencies

Consumers check out the library at `external/EsyMcuPlatform`. Middleware and
application code use public `esy_hal_*` APIs. HAL assembles board resources and
hardware backends privately; its public headers expose neither layer.
`esy_hal_backend` implements `esy_hb_*` operations using the hardware SDK.
`esy_board` owns physical resources and board initialization.

HAL's handle-oriented APIs assemble board resources, backend operations, DMA
metadata and operation tables. Middleware reaches hardware only through public
HAL APIs; it does not include or link board/backend implementations directly.
`esy_common` remains independent of HAL, middleware, RTOS and board code.

Boot image validation, metadata, slot selection, trial transitions and
confirmation belong to `esy_middleware/bootloader`, with public
`esy_bootloader_*` APIs and the `esy_bootloader` target. HAL supplies Flash,
CPU operations and boot-layout queries through `esy_hal_boot_get_layout()`.
The neutral boot-layout type is declared by
`esy_common/defs/image/esy_boot_defs.h`.

Image handoff is exposed by `esy_hal_boot_jump_to_app()` and selected by
`ENABLE_ESY_BOOTLOADER`; it is separate from system time, IRQ and reset APIs.
Pico and STM32F4 use direct image handoff. ESP32 keeps the ESP-IDF bootloader and
`otadata` authoritative for image selection, trial state and rollback; activation
selects a partition for the next reset. See
[ESP32 managed boot and OTA](flash_geometry.md#esp32-managed-boot-and-ota) for
partition, confirmation and reset requirements.

## Communication and GUI adapters

Pure codecs remain in `esy_common/protocol_codec`. The Modbus manager owns response
matching, timeout and retry; its UART channel adds RTU frame detection and uses
the protocol-independent UART TX scheduler for physical transmission. All
protocol-specific channels belong to `protocol`, with flat public headers and
family-owned implementation/test directories. Channels are caller-driven;
explicit workers own tasks and drive the same channel APIs. Transport schedulers
do not select or depend on a protocol. The ProtoBin TCP channel owns framing and
session state over an exclusive borrowed `esy_network_connection_t`. MQTT borrows its
byte-stream contract. Network endpoints own connection resources; the consumer
owns connection policy, provider execution and ordered shutdown.

LCD and touch facades keep active-I/O device references. Optional
`esy_lcd_device` and `esy_touch_device` targets add registration and auxiliary
GPIO power integration. Complete LCD/touch handles, private-storage capacities
and driver callbacks belong to their respective facade headers; common headers
retain shared value types such as orientation, pixel format and samples.
LVGL publicly depends on the LCD facade used by its configuration.
LVGL owns one GUI task and accepts an optional generic
pointer-input callback. `esy_lvgl_touch` adapts the touch facade without adding
a task. A custom display without input has no touch or GPIO dependency.

## Consumer assembly

Include narrow public headers and link the targets actually used:

```c
#include "esy_hal/esy_hal_system.h"
#include "esy_middleware/bootloader/esy_bootloader.h"
```

```cmake
target_link_libraries(firmware PRIVATE esy_hal esy_bootloader)
```

The consumer's `esy_feature_import.cmake` supplies feature selection.
`ENABLE_ESY_BOOTLOADER` assembles the bootloader and HAL layout adapter and
requires Flash and system support. The bootloader supports `ESY_RTOS=None`; OTA
requires the bootloader feature.

Select hardware explicitly with `ESY_TARGET_DEVICE` before early import.
The resolver derives `ESY_TARGET_PLATFORM`; it is not a separate user input.
`ESY_PLATFORM_*` compile-time selectors and integration inputs such as
`ESY_PLATFORM_COMPILE_OPTIONS`, `ESY_PLATFORM_LINK_OPTIONS` and
`ESY_PLATFORM_SDK_ROOT` select hardware and build settings.

## STM32F4 interrupt ownership

Link `esy_interrupts` (alias `Esy::interrupts`) to the final executable to use
the platform's vector entries. The interface adds compiled objects directly so
startup weak aliases cannot prevent their inclusion from a static archive.

Existing peripheral features select GPIO, DMA, USART1/2/3, ADC and logical timer
TIM2/TIM5 vectors, which dispatch to the backend's `esy_hb_*` operations. Disabled
features add no corresponding vector source. `ENABLE_ESY_SYSTEM` selects the
baremetal SysTick implementation unless `ESY_RTOS=FreeRTOS`; the OS port then owns
the single SysTick entry, advances the HAL timebase and forwards ticks only
after the scheduler has started.

Consumers with custom vector ownership can omit this target and supply their
own entries. Consumers adopting it must remove overlapping application or board
IRQ sources. Board ports retain pin mapping, interrupt priorities and DMA
resource bindings; they do not duplicate the shared vector entries.

## Verification and ownership

Run module tests, public-header checks and an embedded consumer build after
changing these boundaries. Target boot/OTA tests establish real Flash, reset
and jump behavior. `esy_bootloader_mark_success()` confirms a trial image.
Consumers own startup order, board policy and the final firmware link.

For a new device or hardware family, follow the [porting guide](porting.md).
Device maps, direct SDK/toolchain and image entries, and namespaced build hooks
live in `cmake/ports/<port>/`. Hardware implementations stay in the selected
backend and native RTOS bridges in the selected OS port. The build-hook file
only declares functions/macros; shared entry points control when they execute.
Backend and OS owners register vector OBJECT targets for shared aggregation.
