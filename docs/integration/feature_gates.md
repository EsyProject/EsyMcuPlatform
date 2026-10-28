<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Feature Gates

EsyMcuPlatform consumes feature variables from the firmware application that
integrates it. The application owns product policy, board selection, startup
order, and the single source of truth for feature selection.

The consumer selects the device and SDK/toolchain providers and owns board
resources and feature policy. Optional [build integration helpers](build_integration.md)
provide conventional paths, build defaults and reusable SDK/provider/output steps.
A build override file is optional; supplied values remain authoritative.
Consumers use these helpers from their own CMake files or prepare the same
inputs independently with an existing build system.

The helpers also provide a default application-version header from the
consumer's `project(... VERSION ...)`; custom templates and disabling generation
are integration overrides. These version settings do not define feature values
or change the library's version.

After early import and the consumer's `project()`, adding the library performs
project setup and assembly together:

```cmake
add_subdirectory(external/EsyMcuPlatform)
```

An explicit `esy_project_import.cmake` include remains available for advanced or
existing builds. Without early import, direct library consumers continue to
prepare their feature variables, SDK/providers, board and optimization targets
explicitly.

The consumer selects `ESY_TARGET_DEVICE`, for example
`-DESY_TARGET_DEVICE=PICO2_W` or `-DESY_TARGET_DEVICE=STM32F407ZG`.
The shared resolver normalizes case and derives the internal `ESY_TARGET_PLATFORM`
values `PICO`, `PICO_W`, `PICO2`, `PICO2_W`, `STM32F4` or `ESP32`.
Do not configure the derived platform separately.
Device values are validated. Build overrides cannot
supply or replace selection. Consumers prepare SDK/toolchain inputs before early
import; the helpers do not discover private environment files. `.project.conf` is not a
library input and is never read. `ESY_BOARD_PATH` points to a CMake
directory that creates the consumer's `esy_board` target.

The optional early helper selects an existing consumer
`esy_feature_import.cmake` by convention unless `ESY_FEATURE_IMPORT_FILE` is already
defined. An explicit empty value disables import; an explicit path selects that
file. Integrated library assembly reads the selected file, preserves its feature
values and reuses existing provider/optimization targets. Resolved feature and
RTOS settings return to the caller for subsequent consumer modules. Missing
optimization targets use the configured option lists, with conventional defaults
supplied by the early helper. Shared `esy_project_options` settings propagate
through these targets. SDK/provider prerequisites still follow the platform
capability matrix and module contracts. Merely setting a variable does not supply
missing SDK sources, board wiring or module support.

## Ownership

Feature variables must be defined by the consuming project, normally in
`esy_feature_import.cmake`.

EsyMcuPlatform must only consume these variables. It must not create a second
feature source with `option()`, cache defaults, application-specific CMake
files, or source-level fallback macros.

Declare feature values with ordinary `set()` assignments in the selected feature
file. These assignments determine the effective value even when a command-line
argument, preset or existing cache contains another value. For example:

```cmake
set(ENABLE_ESY_LOGGER ON)
set(ESY_RTOS FreeRTOS)
```

External device, build-type, toolchain, slot and integration-path inputs retain
their separate contracts.

When a feature is disabled, source files that depend on it must not be added to
the corresponding target. Do not keep disabled code in the build and hide it
with ad-hoc preprocessor branches in `.c` files.

## Variable Classes

`ENABLE_ESY_*` variables enable capabilities at every level: reusable modules,
module families, HAL wrappers, software adapters, protocol channels and optional
helpers. Module and child names express their relationship, for example
`ENABLE_ESY_LOGGER` and `ENABLE_ESY_LOGGER_BACKEND_UART`. A gate describes a
capability, rather than a particular source file or directory layout.

Module gates retain whole-module selection. Child gates select capabilities
within an enabled module; they do not implicitly enable its parent.

`ESY_TARGET_DEVICE`, `ESY_RTOS`, `ESY_BOARD_PATH`, and third-party paths such as `LVGL_PATH`
are integration inputs selected by the consuming project or its adopted
build conventions. They are not feature gates by themselves, but feature-gated
modules may require them.

`ESY_RTOS` selects `FreeRTOS` or `None`. It remains a platform integration input
in the consuming feature file, separate from application-specific `APP_*`
choices. Each enabled module still has to support the selected execution model.

## Current Top-Level Gates

| Gate | Adds target or subtree | Notes |
| --- | --- | --- |
| Always on | `esy_common/defs`, `esy_common/utils`, `esy_common/device` | Core types, utilities, and the explicit device graph are always assembled. |
| `ENABLE_ESY_COMMON_PROTOCOL` | `esy_common/protocol_codec` | Adds `esy_common_protocol_codec` when the module is present. |
| `ENABLE_ESY_COMMON_PROTOCOL_JSON` | `esy_common/protocol_codec` | Requires `ENABLE_ESY_COMMON_PROTOCOL`; adds the independent `esy_common_protocol_json` / `Esy::protocol_json` target with a private coreJSON v3.3.1 `esy_json_parser_provider`. Optional project integration creates the provider from explicit `ESY_COREJSON_PATH`. Disabling JSON leaves the existing ProtoBin/TLV/Modbus codec target independent of the provider. No HAL, OS, transport or management schema is selected. |
| `ENABLE_ESY_COMMON_DS` | `esy_common/ds` | Adds deterministic data structure targets when the module is present. |
| `ESY_RTOS STREQUAL "FreeRTOS"` | `esy_middleware/os` | Adds `esy_os` wrappers for FreeRTOS. |
| `ENABLE_ESY_EVENT_BUS` | `esy_middleware/event_bus` | Adds OS-specific `esy_event_bus` for local publish/subscribe delivery through independent static subscriber queues. Requires `ESY_RTOS=FreeRTOS` and `ENABLE_ESY_OS_QUEUE`. Publishers perform nonblocking task/ISR fanout; no task, hardware IRQ or application topics are created automatically. See the [event bus contract](../../esy_middleware/event_bus/README.md). |
| `ENABLE_ESY_BOOTLOADER` | `esy_middleware/bootloader` | Adds OS-independent `esy_bootloader` and HAL boot layout/runtime/handoff support; requires `ENABLE_ESY_SYSTEM` and `ENABLE_ESY_FLASH`. ESP32 managed boot owns the native `app_update` dependency; system-only assembly does not require it. Uses the existing gate without a second module switch. |
| `ENABLE_ESY_DUAL_FLASH_STORE` | `esy_middleware/dual_flash_store` | Adds OS-independent `esy_dual_flash_store`; requires `ENABLE_ESY_FLASH`. The consumer supplies two exclusive whole-sector regions, a caller-owned workspace and serialization with all other Flash users. No partition, Flash lifecycle or safe-execution policy is selected automatically. |
| `ENABLE_ESY_FILE` | `esy_middleware/file` | Adds `esy_file` and its caller-owned RAM provider; common types only. One handle binds one file without paths, directories, automatic partitions or OS resources. |
| `ENABLE_ESY_FILE_FLASH` | File provider | Requires `ENABLE_ESY_FILE`, `ENABLE_ESY_FLASH` and public HAL; adds `esy_file_flash` for create/read/remove in one caller-owned whole-sector region. It does not advertise atomic replacement. |
| `ENABLE_ESY_FILE_DUAL_FLASH_STORE` | File provider | Requires `ENABLE_ESY_FILE`, `ENABLE_ESY_DUAL_FLASH_STORE` and `esy_dual_flash_store`; adds `esy_file_dual_flash_store` with atomic replacement backed by the existing two-region transaction. |
| `ENABLE_ESY_FS` | `esy_middleware/fs` | Adds synchronous caller-owned `esy_fs` volume, file and directory interfaces without OS, HAL or third-party dependencies. No formatting or task is automatic. |
| `ENABLE_ESY_FS_LITTLEFS` | Filesystem provider | Requires `ENABLE_ESY_FS` and explicit `ESY_LITTLEFS_PATH`; adds `esy_fs_littlefs` and its static-buffer native provider. Includes whole-file atomic replacement through an exclusively reserved staging path. |
| `ENABLE_ESY_FS_FATFS` | Filesystem provider | Requires `ENABLE_ESY_FS` and explicit official R0.16 `ESY_FATFS_PATH`; adds `esy_fs_fatfs` and its configured native provider. The consumer supplies synchronous sector operations; no SD hardware driver or atomic replacement is implied. |
| `ENABLE_ESY_FS_FLASH` | Filesystem media adapter | Requires `ENABLE_ESY_FS`, `ENABLE_ESY_FS_LITTLEFS`, `ENABLE_ESY_FLASH` and public HAL; adds `esy_fs_flash` with whole-sector validation and borrowed controller lifetime. |
| `ENABLE_ESY_FILE_FS` | File provider | Requires `ENABLE_ESY_FILE` and `ENABLE_ESY_FS`; adds `esy_file_fs` binding a mounted volume and path. CREATE/ATOMIC_REPLACE require a capable backend and reserved staging path; otherwise READ/REMOVE remain available. |
| `ENABLE_ESY_SETTINGS` | `esy_middleware/settings` | Adds OS-independent `esy_settings`; requires `ENABLE_ESY_FILE`. String keys and binary values live in caller-owned RAM; explicit commit replaces one complete snapshot through a borrowed file supporting atomic replacement. Flash and dual Flash store selection belong to the chosen backend. Defaults, schema, application and backend arbitration remain consumer-owned. |
| `ENABLE_ESY_DIAGNOSTICS` | `esy_middleware/diagnostics` | Adds `esy_diagnostics` for metadata-only enumeration, current health queries and optional explicit synchronous self-tests. Borrows caller-owned immutable registrations and contexts; common types only, without a task, timebase, storage or automatic hardware tests. |
| `ENABLE_ESY_REMOTE_MANAGER` | `esy_middleware/remote_manager` | Adds caller-driven `esy_remote_manager` for authorized discovery, configuration, complete typed image upload/control, slot enumeration/confirmation/rollback, read-only statistics, structured diagnostics and Coredump collection through normalized owner callbacks. Includes a caller-owned live RAM statistics table without automatic metrics, persistence or a collection task. Depends only on common types; no OS, Flash, Settings, transport, bootloader, diagnostics engine or OTA engine is selected. See the [remote manager contract](../../esy_middleware/remote_manager/README.md). |
| `ENABLE_ESY_FAULT` | HAL/backend fatal capture | Installs a boot-lifetime capture binding on RP2040, secure-only Arm RP2350, verified STM32F405/407/415/417, or classic ESP32/ESP-IDF 5.3.4. The consumer initializes it before Cortex peers/scheduler start, or during ESP32 native app startup before application-owned tasks; callback/state remain in RAM. ESP32 uses the version-pinned native panic adapter and requires IRAM panic handling. No capture policy or retained partition is allocated automatically. |
| `ENABLE_ESY_COREDUMP` | `esy_middleware/coredump` | Adds OS-independent `esy_coredump_core` for retained-RAM capture and record validation. The consumer reserves retained RAM consistently across bootloader and application images and supplies the faulting image identity. |
| `ENABLE_ESY_CORE_LOCKOUT` | `esy_middleware/core_lockout` | Adds `esy_core_lockout_core`; requires system support, independent of Flash. FreeRTOS additionally assembles the `esy_core_lockout` worker adapter; `ESY_RTOS=None` uses the core directly. Core init requires one active owner core and every peer inactive until reset; check initialization status before execution; Flash hook binding belongs to the consumer. |
| `ENABLE_ESY_LOGGER` | `esy_middleware/logger` | Adds the `esy_logger` interface aggregate and frontend-only `esy_logger_core`. Each enabled backend has its own core target and, with FreeRTOS, a separate OS adapter target. |
| `ENABLE_ESY_NUMERICS` | `esy_middleware/numerics` | Adds the `esy_numerics` target for small dense linear algebra, regression and polynomial fitting without an inference-library dependency. |
| `ENABLE_ESY_PROTO_BIN` | `esy_middleware/protocol` | Adds `esy_proto_bin_session` and header-only `esy_proto_bin_contract`; requires common codecs/data structures, HAL and system tick. Channels remain separately selected. |
| `ENABLE_ESY_PROTO_MODBUS` | `esy_middleware/protocol` | Adds `esy_proto_modbus_manager`, independently of ProtoBin; channels/workers remain separately selected; requires common codecs/data structures, HAL and system tick. |
| `ENABLE_ESY_NETWORK` | `esy_middleware/network` | Adds the OS-independent `esy_network` basic stream and connection-extension contracts. MQTT borrows the basic stream; ProtoBin requires the whole-write/session/peer-ACK extension. Neither owns endpoint lifecycle. |
| `ENABLE_ESY_BLE` | `esy_middleware/ble` | Adds `esy_ble` / `Esy::ble`, the portable Bluetooth LE GAP, GATT server and security facade for advertising, scanning, connections, service registration, notifications and pairing/bond management. The core has no HAL or OS target dependency. The consumer supplies the software-stack provider, execution owner and Bluetooth policy. |
| `ENABLE_ESY_MQTT` | `esy_middleware/mqtt` | Adds caller-driven `esy_mqtt` with no HAL or OS dependency. Requires a coreMQTT v2.3.1 `esy_mqtt_provider` target; optional project integration creates it from explicit `ESY_COREMQTT_PATH`. Requires `esy_network` for its shared stream contract. Connection ownership and scheduling remain with the consumer. |
| `ENABLE_ESY_HTTP_CLIENT` | `esy_middleware/http` | Adds caller-driven `esy_http_client` / `Esy::http_client` for HTTP/1.1 requests and streaming responses. Requires `ENABLE_ESY_NETWORK` and a private llhttp v9.4.3 `esy_http_parser_provider` target; optional project integration assembles pre-generated sources from explicit `ESY_LLHTTP_PATH`. No OS, TCP endpoint, TLS provider, OTA writer or application policy is selected. |
| `ENABLE_ESY_INPUT` | `esy_middleware/input` | Adds OS-independent, caller-driven `esy_input` for debounced buttons, click/long-press gestures, quadrature encoders and unified input events. With `ENABLE_ESY_GPIO`, also assembles `esy_input_gpio`, which borrows initialized public HAL GPIO handles. Consumers own sampling, time, event delivery and GPIO lifecycle; no task, IRQ, LVGL provider or board resource is selected automatically. |
| `ENABLE_ESY_SENSOR` | `esy_middleware/sensor` | Adds the OS-independent `esy_sensor` channel, unit, sample and event facade. With `ENABLE_ESY_I2C`, also adds `esy_sensor_i2c` for SHT3x, MPU6050 and INA219; this target requires `ENABLE_ESY_SYSTEM` and a consumer-provided `esy_hal` target. Drivers borrow initialized public HAL bus handles. The caller owns sensor instances, sampling and event delivery; see the [sensor integration contract](../../esy_middleware/sensor/README.md#integration). No worker, IRQ, queue or board resource is selected automatically. |
| `ENABLE_ESY_TOUCH` | `esy_middleware/touch` | Adds `esy_touch`. Controller gates choose sources; GPIO support additionally assembles `esy_touch_device` for explicit device/PM binding. |
| `ENABLE_ESY_LCD` | `esy_middleware/lcd` | Adds `esy_lcd`. Panel gates choose sources; GPIO support additionally assembles `esy_lcd_device` for explicit device/PM binding. |
| `ENABLE_ESY_LVGL` | `esy_lvgl` runtime | Requires a consumer-provided LVGL v9.6.0 `lvgl` target, LCD facade, FreeRTOS OS queue and system support. Pointer input is optional; `ENABLE_ESY_TOUCH` assembles the independent `esy_lvgl_touch` adapter. Base runtime does not require touch or GPIO. Native ports require always-lock-free 32-bit atomics; RP2040 uses the SDK IRQ-spinlock backend. The consumer imports the provider; see the [runtime contract](../../esy_middleware/lvgl/README.md). |
| `ENABLE_ESY_OTA` | `esy_middleware/ota` | Adds encoding-independent `esy_ota_core` for caller-owned BEGIN/WRITE/FINISH/STATUS/ABORT/ACTIVATE transactions. Depends only on common types, with no HAL, Flash, bootloader, protocol or OS target; optional storage providers remain consumer-owned. |
| `ENABLE_ESY_WATCHDOG_MONITOR` | `esy_middleware/watchdog_monitor` | Adds `esy_watchdog_monitor_core`; requires `ENABLE_ESY_WATCHDOG` and `ENABLE_ESY_SYSTEM`. FreeRTOS additionally assembles the `esy_watchdog_monitor` worker adapter; `ESY_RTOS=None` uses the core directly for caller-driven supervision. |
| `ENABLE_ESY_USB` | `esy_middleware/usb` | Adds `esy_usb` only when CDC and its backend are enabled; both child gates OFF assemble no USB target or protocol USB channel. |
| `ENABLE_ESY_IO_SCHEDULER` | `esy_middleware/io_scheduler` | Adds the selected protocol-independent SPI and UART TX schedulers; an empty selection is valid. Consumers link the concrete targets they use. |
| `ENABLE_ESY_FREQ_SYNTH` | `esy_middleware/freq_synth` | Adds `esy_freq_synth`. Synthesizer gates choose sources; GPIO support additionally assembles `esy_freq_synth_device` for explicit device registration and GPIO PM integration. |
| `ENABLE_ESY_WIFI_MANAGER` | `esy_middleware/wifi` | Adds `esy_wifi_manager`; requires `ENABLE_ESY_WIFI`. Pico W-class Wi-Fi requires `esy_wifi_provider`, supplied by integrated assembly or explicitly by direct consumers. |
| `ENABLE_ESY_POWER_MANAGER` | `esy_middleware/power_manager` | Adds the OS-independent manager core and `esy_power_hal_backend`; with FreeRTOS also adds the separate tickless adapter target. Requires `ENABLE_ESY_LOW_POWER`. |

`esy_hal_backend` and `esy_hal` are always added after the board subtree, but
their source files are selected by `ESY_TARGET_PLATFORM` and `ENABLE_ESY_*` gates.
The device graph core and optional `device` fields do not enter a low-power
state by themselves. A consumer that leaves `ENABLE_ESY_POWER_MANAGER`,
`ENABLE_ESY_LOW_POWER`, and RTOS tickless idle disabled retains ordinary active
mode behavior.

Calendar and time support uses independent gates:

| Gate | Target effect | Dependency contract |
| --- | --- | --- |
| `ENABLE_ESY_RTC` | Adds the RTC facade and software provider to `esy_hal`. | Requires `esy_common_utils` for Gregorian calendar conversion; no OS or native RTC SDK dependency. |
| `ENABLE_ESY_TIME` | Adds the caller-driven `esy_time` UTC clock. | Requires common types; the consumer supplies a monotonic 64-bit microsecond clock. No HAL or OS target is required by the core. |
| `ENABLE_ESY_TIME_SNTP` | Adds independent `esy_sntp`. | Requires `ENABLE_ESY_TIME`; the consumer owns a bounded UDP transport pinned to its chosen server. No TCP/lwIP or OS provider is added implicitly. |
| `ENABLE_ESY_TIME_RTC` | Adds independent `esy_time_rtc`. | Requires `ENABLE_ESY_TIME`, `ENABLE_ESY_RTC`, `esy_hal` and `esy_common_utils`; loads UTC from an RTC or stores synchronized UTC in it. |

Define these gates in the consumer's `esy_feature_import.cmake`; they remain
disabled when undefined. RTC provider initialization, UTC validity, server
selection, retry periods and scheduling are explicit consumer responsibilities.
Software RTC alarms are polled calendar deadlines and do not wake a sleeping
CPU or retain UTC after reset/power loss. Native hardware RTC adapters are not
included; the provider contract permits them without exposing SDK types. See
the [RTC contract](../../esy_hal/rtc.md) and
[UTC/SNTP integration](../../esy_middleware/time/README.md).

## Common Implementation Gates

The following gate families are consumed by current CMake assembly:

| Gate family | Area | Purpose |
| --- | --- | --- |
| `ENABLE_ESY_SYSTEM`, `ENABLE_ESY_GPIO`, `ENABLE_ESY_DMA`, `ENABLE_ESY_UART`, `ENABLE_ESY_TIMER`, `ENABLE_ESY_I2C`, `ENABLE_ESY_PWM`, `ENABLE_ESY_MULTICORE`, `ENABLE_ESY_WATCHDOG`, `ENABLE_ESY_ADC`, `ENABLE_ESY_SPI`, `ENABLE_ESY_TIMER_CAPTURE`, `ENABLE_ESY_FLASH`, `ENABLE_ESY_BOOTLOADER`, `ENABLE_ESY_LOW_POWER`, `ENABLE_ESY_USB_DEV` | `esy_hal_backend`, `esy_hal` | Select hardware sources; bootloader additionally assembles its middleware target through the same gate. |
| `ENABLE_ESY_WIFI` | Wi-Fi HAL/backend | Requires a Pico W-class platform and `esy_wifi_provider` with polling CYW43/lwIP and consumer network configuration requirements. The public HAL supplies borrowed network execution callbacks for owner injection; no separate network hardware adapter gate is needed. No native RTOS path is assumed. |
| `ENABLE_ESY_PROTO_BIN_UART_CHANNEL` | ProtoBin UART channel | Requires `ENABLE_ESY_PROTO_BIN`, UART/DMA, `ENABLE_ESY_IO_SCHEDULER` and `ENABLE_ESY_UART_TX_SCHEDULER`; reuses the owner-driven TX scheduler without an OS dependency. Hardware support alone never enables the channel. |
| `ENABLE_ESY_PROTO_BIN_USB_CHANNEL` | ProtoBin USB channel | Requires `ENABLE_ESY_PROTO_BIN` and an assembled USB CDC provider target. USB availability alone never enables the channel. |
| `ENABLE_ESY_PROTO_BIN_TCP_CHANNEL` | ProtoBin TCP channel | Requires `ENABLE_ESY_PROTO_BIN` and `ENABLE_ESY_NETWORK`. The channel borrows a connection extension; its consumer selects and owns the endpoint/provider. |
| `ENABLE_ESY_PROTO_JSON_TCP_CHANNEL` | JSON TCP channel | Requires `ENABLE_ESY_COMMON_PROTOCOL`, `ENABLE_ESY_COMMON_PROTOCOL_JSON` and `ENABLE_ESY_NETWORK`; adds independent `esy_proto_json_tcp_channel` / `Esy::proto_json_tcp_channel`. Uses a four-byte big-endian length followed by one complete JSON document over an exclusively borrowed connected peer. No ProtoBin, Modbus, DS, HAL/system, OS, native network provider or management schema is selected. |
| `ENABLE_ESY_PROTO_JSON_USB_CHANNEL` | JSON USB channel | Requires `ENABLE_ESY_COMMON_PROTOCOL`, `ENABLE_ESY_COMMON_PROTOCOL_JSON`, `ENABLE_ESY_USB`, `ENABLE_ESY_USB_CDC` and an assembled `esy_usb`; adds independent `esy_proto_json_usb_channel` / `Esy::proto_json_usb_channel`. Borrows one initialized CDC interface with disconnect-session isolation. The channel selects no TCP/network, ProtoBin, Modbus, DS, HAL or OS support; the consumer-selected USB provider retains its own hardware/OS requirements and stack owner. |
| `ENABLE_ESY_NETWORK_LWIP` | Shared lwIP endpoints | With `ENABLE_ESY_NETWORK`, adds `esy_network_lwip`; requires common DS and `esy_network_lwip_provider`. Supports client/server TCP and DNS, with caller-owned RX storage and explicit progress/owner hooks. |
| `ENABLE_ESY_CREDENTIALS` | Credential bundles | Adds `esy_credentials` / `Esy::credentials`; borrowed DER/PEM CA and optional client identity, with leases preventing replacement while in use. No HAL, OS, storage or native crypto dependency. |
| `ENABLE_ESY_CREDENTIALS_STORE` | Optional credential persistence | Requires `ENABLE_ESY_CREDENTIALS` and `ENABLE_ESY_FILE`; adds `esy_credentials_store` / `Esy::credentials_store` inside the Credentials module. Borrows a READ/ATOMIC_REPLACE file for complete confirmed/candidate snapshots; selects no Flash layout, network owner or native crypto provider. |
| `ENABLE_ESY_CRYPTO` | Cryptographic RNG | Adds `esy_crypto` / `Esy::crypto`; requires an explicit `esy_mbedtls_provider` for Mbed TLS 3.6.x. The caller supplies qualified entropy; no HAL random source is selected implicitly. |
| `ENABLE_ESY_TLS` | TLS 1.2 client | Adds `esy_tls` / `Esy::tls`; requires `ENABLE_ESY_NETWORK`, `ENABLE_ESY_CREDENTIALS`, `ENABLE_ESY_CRYPTO` and the compatible software provider. No network endpoint, OS task, product trust root or UTC source is selected automatically. |
| `ENABLE_ESY_BLE_BTSTACK` | Bluetooth LE software adapter | Requires `ENABLE_ESY_BLE` and consumer target `esy_ble_btstack_provider`; adds `esy_ble_btstack` / `Esy::ble_btstack`. The adapter borrows an initialized BTstack runtime and keeps native software types private. No HCI transport, hardware target, scheduler or persistent storage is selected automatically. |
| `ENABLE_ESY_XIO`, `ENABLE_ESY_XIO_SPI`, `ENABLE_ESY_XIO_UART_RX`, `ENABLE_ESY_XIO_UART_TX` | XIO HAL/backend wrappers | Select XIO support and transport-specific files. XIO SPI assembles generic SPI I/O independently of native `ENABLE_ESY_SPI`. |
| `ENABLE_ESY_OS_QUEUE`, `ENABLE_ESY_OS_MUTEX`, `ENABLE_ESY_OS_SEMAPHORE`, `ENABLE_ESY_OS_TIMER`, `ENABLE_ESY_OS_WORK`, `ENABLE_ESY_OS_STREAM_BUFFER`, `ENABLE_ESY_OS_MSG_BUFFER` | OS middleware | Select FreeRTOS wrapper source files. OS requires `ENABLE_ESY_SYSTEM`; work requires queue and timer. See [OS integration and native configuration](../../esy_middleware/os/README.md#integration). |
| `ENABLE_ESY_LOGGER_BACKEND_UART`, `ENABLE_ESY_LOGGER_BACKEND_PRINTF`, `ENABLE_ESY_LOGGER_BACKEND_RTT` | Logger middleware | Select the respective `esy_logger_backend_<name>_core` engine; each requires `ENABLE_ESY_SYSTEM`. FreeRTOS additionally assembles `esy_logger_backend_<name>` with OS requirements. UART/printf require `ENABLE_ESY_COMMON_DS`; UART also requires `ENABLE_ESY_UART` and `ENABLE_ESY_DMA`. Only the RTT engine consumes the SEGGER RTT provider. The frontend has none of these backend dependencies. |
| `ENABLE_ESY_COREDUMP_LOGGER` | Coredump middleware | Adds `esy_coredump_logger` when `ENABLE_ESY_COREDUMP` is enabled; requires `ENABLE_ESY_LOGGER`. Normal-context, best-effort CD1 chunk export retains the record for repeated collection. |
| `ENABLE_ESY_COREDUMP_FILE` | Coredump middleware | Requires `ENABLE_ESY_COREDUMP` and `ENABLE_ESY_FILE`; adds `esy_coredump_file` for normal-context first-record persistence/restore and ID-checked clear. The bound file owns storage and must support reads and creation. Capture itself remains HAL/OS/file independent. |
| `ENABLE_ESY_OTA_FLASH` | OTA storage provider | Requires `ENABLE_ESY_OTA`, system, Flash and bootloader dependencies; adds `esy_ota_flash` for checked image writing/verification and separate activation. Core OTA remains storage-independent. |
| `ENABLE_ESY_REMOTE_MANAGER_OTA` | Remote manager provider adapter | Requires `ENABLE_ESY_REMOTE_MANAGER`, `ENABLE_ESY_OTA` and `esy_ota_core`; adds `esy_remote_manager_ota`. Borrows an initialized typed core and owner INFO callback, and copies optional slot/confirm/rollback callbacks with their independent context. No Flash/HAL/OS/bootloader/protocol dependency; Flash installation separately selects `ENABLE_ESY_OTA_FLASH`. |
| `ENABLE_ESY_REMOTE_MANAGER_PROTO_BIN` | Remote manager middleware | Requires `ENABLE_ESY_REMOTE_MANAGER`; adds `esy_remote_manager_proto_bin` for request/response payload conversion. Does not require the ProtoBin session, common wire codec, HAL or OS; the consumer separately selects and owns any framing, channel and delivery resources. |
| `ENABLE_ESY_REMOTE_MANAGER_SETTINGS` | Remote manager provider adapter | Requires `ENABLE_ESY_REMOTE_MANAGER`, `ENABLE_ESY_SETTINGS` and target `esy_settings`; adds `esy_remote_manager_settings`. Borrows initialized Settings plus caller key scratch. SAVE uses the selected file backend through Settings; configuration schema, admission and runtime apply remain with the owner. |
| `ENABLE_ESY_REMOTE_MANAGER_COREDUMP` | Remote manager provider adapter | Requires `ENABLE_ESY_REMOTE_MANAGER`, `ENABLE_ESY_COREDUMP` and target `esy_coredump_core`; adds `esy_remote_manager_coredump_store` for INFO/range READ of an initialized RAM store. Flash/Logger backends remain separate. CLEAR is available only through an explicit owner callback that discards all authoritative sources. |
| `ENABLE_ESY_REMOTE_MANAGER_DIAGNOSTICS` | Remote manager provider adapter | Requires `ENABLE_ESY_REMOTE_MANAGER`, `ENABLE_ESY_DIAGNOSTICS` and target `esy_diagnostics`; adds `esy_remote_manager_diagnostics`. Borrows the shared diagnostics instance; independent ENUMERATE/READ/RUN capabilities and authorization remain explicit. Does not select a wire codec. |
| `ENABLE_ESY_USB_CDC`, `ENABLE_ESY_USB_BACKEND_TINYUSB` | USB middleware | Select CDC support and TinyUSB backend; enable both or neither. When enabled, a consumer-provided `tinyusb_device` target is required. |
| `ENABLE_ESY_COMMON_UTILS_CRC32_TABLE` | Common utils | Enables the table-based CRC32 implementation compile definition for that target. |
| `ENABLE_ESY_NUMERICS_QUANTIZE` | Numerics middleware | Selects optional normalization and signed 16-bit quantization source code. |
| `ENABLE_ESY_TOUCH_XPT2046` | Touch middleware | Selects the XPT2046 touch controller implementation. |
| `ENABLE_ESY_LCD_ILI9488` | LCD middleware | Selects the ILI9488 LCD implementation. |
| `ENABLE_ESY_SPI_SCHEDULER` | I/O scheduler middleware | Explicit ON selects `esy_spi_scheduler_core`; FreeRTOS additionally assembles the `esy_spi_scheduler` worker adapter. This gate is independent of HAL `ENABLE_ESY_SPI`. |
| `ENABLE_ESY_UART_TX_SCHEDULER` | I/O scheduler middleware | Selects owner-driven UART TX scheduling without an OS task; UART RX remains HAL-owned. Protocol consumers select this dependency explicitly. |
| `ENABLE_ESY_PROTO_MODBUS_UART_CHANNEL` | Protocol middleware | Selects caller-driven `esy_proto_modbus_uart_channel`; requires the Modbus family, UART/DMA/system and an explicitly enabled UART TX scheduler; no OS dependency. |
| `ENABLE_ESY_PROTO_MODBUS_UART_WORKER` | Protocol middleware | Selects `esy_proto_modbus_uart_worker`; requires the UART channel, FreeRTOS and OS task support. Only explicit worker creation starts a task. |
| `ENABLE_ESY_PROTO_MODBUS_TCP_CHANNEL` | Protocol middleware | Selects caller-driven `esy_proto_modbus_tcp_channel`; requires the Modbus family and `esy_network`. Master/slave instances borrow established peer sessions; no UART, OS worker or native stack is selected. |
| `ENABLE_ESY_ADF4351`, `ENABLE_ESY_ADF4372`, `ENABLE_ESY_LMX2594`, `ENABLE_ESY_LMX2592` | Frequency synthesis middleware | Select concrete synthesizer implementations. LMX2594 and LMX2592 use independent register maps through synchronous public HAL calls, without an OS dependency or PM adapter. |

## Dependency Rules

`ENABLE_ESY_MULTICORE`, `ENABLE_ESY_USB_DEV` and `ENABLE_ESY_XIO` require `PICO`,
`PICO_W`, `PICO2` or `PICO2_W`. Enabling any of these gates for another platform
fails during CMake configuration with the gate and selected platform in the
diagnostic. Leaving a gate OFF or undefined adds none of its implementation
sources. This restriction does not enable prerequisites or supply SDK providers.
`ENABLE_ESY_ADC` similarly requires `STM32F4`; the backend assembly rejects any
other platform in direct backend consumers as well as full library consumers.

`ENABLE_ESY_DMA` selects the generic `esy_hal_dma_*` controller facade. UART,
SPI, I2C, PWM, ADC and XIO do not require this gate. Their native DMA paths,
physical-resource arbiter and IRQ vectors are assembled when the selected
peripheral implementation needs them. With the generic gate OFF, `esy_hal_dma.c`
is absent and applications must not call that API; peripheral DMA APIs remain
available according to each port's capability mask. Middleware that calls generic
DMA helpers retains its explicit `ENABLE_ESY_DMA` dependency.

Enable the module gate and the required child gates together. For example,
`ENABLE_ESY_LOGGER_BACKEND_UART` requires `ENABLE_ESY_LOGGER`; the child does not
add the module directory on its own. `ENABLE_ESY_BOOTLOADER` directly assembles
the independent bootloader middleware and HAL layout adapter.

Enable HAL or OS primitives required by a middleware module before linking
application code against that middleware. For example, an application using
Management OTA over UART enables `ENABLE_ESY_OTA`,
`ENABLE_ESY_REMOTE_MANAGER`, `ENABLE_ESY_REMOTE_MANAGER_OTA`,
`ENABLE_ESY_REMOTE_MANAGER_PROTO_BIN`, `ENABLE_ESY_PROTO_BIN`,
`ENABLE_ESY_PROTO_BIN_UART_CHANNEL`, `ENABLE_ESY_IO_SCHEDULER`,
`ENABLE_ESY_UART_TX_SCHEDULER`, and the UART/DMA support required by its board
port. ProtoBin and Modbus are independent families; no protocol aggregate feature
or target is provided. Each selected family validates its timed dependencies,
while codec-only use remains independent of HAL and OS. Invalid channel selections
fail configuration rather than silently enabling a family or provider.

Optional module headers and APIs are only valid when the corresponding module is
enabled and linked. Application code must not include or call a disabled module
and rely on C preprocessor guards to make the build pass.

## Adding a New Gate

When adding a feature gate:

1. Define the variable in the consuming application's feature import file.
2. Consume it in EsyMcuPlatform CMake only where source assembly or target linkage
   needs to change.
3. Keep public headers independent from disabled implementation details.
4. Add explicit dependency checks when one gate requires another target or
   third-party path.
5. Update this document with the new variable, target effect, and dependency
   contract.

Do not add `option()` or cache defaults in EsyMcuPlatform modules for application
features. That would create a second source of truth and make submodule
integration order-dependent.

### Watchdog lifecycle and low-power integration

Watchdog init starts hardware on all ports. HAL/backend deinit returns a
status; irreversible STM32F4 IWDG returns NOT_SUPPORTED and remains owned/running.
Monitor stop/deinit are pollable and may return BUSY until its task is parked.
Only successful cleanup releases caller-owned storage. Callback replacement also
returns BUSY while an invocation retains the old context. Handle cleanup errors
before reclaiming resources.

Watchdog deadline constraints require a certified timeout lower bound in the
supplied clock domain. Current hardware ports fail closed with NOT_SUPPORTED
because nominal oscillator/programming values do not certify that bound. Power
management must reject/defer sleep on this error; supervision remains operational.

### Bluetooth LE provider integration

Define `ENABLE_ESY_BLE` in the consumer's `esy_feature_import.cmake` to assemble
the generic `esy_ble` target. The consumer injects its selected software-stack
provider through the public contract. The core does not import a protocol stack,
select a controller/HCI transport, configure a native kernel or reserve bond
storage. Disabled-module assembly adds no BLE target or provider dependency.
See the [module contract](../../esy_middleware/ble/README.md).

Optional `ENABLE_ESY_BLE_BTSTACK` assembles the BTstack software adapter. Define
`esy_ble_btstack_provider` before platform assembly with the selected stack's
public headers, `btstack_config.h` and final software-stack link requirements.
The adapter's public header exposes no native BTstack types; the provider is a
private dependency. Missing parent/provider dependencies fail configuration.
With this child gate disabled, no adapter source or provider dependency is added,
even if the consumer has independently defined a provider target.

The native provider and adapter must share `btstack_config.h` with an explicit
`NVN_NUM_GATT_SERVER_CCC=0`; apply it to every native source, including
`att_server.c`, rather than only the adapter target. This keeps CCC subscriptions
connection-local and disables native CCC TLV writes/restoration. Missing/nonzero
settings fail adapter compilation. `ENABLE_GATT_OVER_EATT` is rejected because
the adapter supports only the fixed ATT bearer.

Supply the required caller-owned `read_buffer` and `read_buffer_size` in
`1..UINT16_MAX` with the adapter's other static resources. Native length probes
stage complete value slices before copying so read errors become proper ATT
error responses. Callbacks must be nondestructive and consistent with their
length queries; undersized staging storage returns an ATT resource error.
Notifications above the fixed ATT MTU minus three fail with
`ESY_ERR_INVALID_ARG`. The module contract defines the exact staging budget.

Compatibility is checked against BTstack 1.6.2 at commit
`501e6d2b86e6c92bfb9c390bcf55709938e25ac1`, as pinned by Pico SDK 2.1.1.
The consumer initializes the HCI transport, run loop, L2CAP, security manager and
durable TLV/bond database through their proper integration layers. Adapter control
calls and native callbacks must share one serialized, non-ISR BTstack context.
The adapter borrows this runtime; final runtime teardown remains consumer-owned.
There is no default global BTstack provider import or source download. Upstream
BTstack and selected SDK/provider licenses remain separate from this library's
Apache-2.0 license.

Core dependency independence does not establish that a selected software stack
supports baremetal execution or a given device. Pairing policy, bond storage and
access protection, native callback dispatch and final stack/controller teardown
belong to the consumer and provider integration.

One non-ISR owner serializes core control and lifecycle calls. Provider callbacks
must hand events to that owner without reentering core control/lifecycle APIs.
Keep the provider, callback and borrowed user context alive until deinit succeeds
and the provider's callback-drain prerequisites are satisfied. Provider init
failure still requires deinit; a cleanup error retains ownership for retry.

Registered service definitions, characteristic arrays and callback contexts stay
at stable addresses until successful unregister or deinit quiesces their
callbacks. The provider translates the portable schema, enforces requested
read/write/notification security and manages notification subscriptions. It owns
key storage, restores bonds before initialization succeeds and reports storage
errors instead of claiming a durable bond. The facade contains no radio backend
or key-storage implementation.

### Wi-Fi providers and lifecycle

Wi-Fi uses `esy_wifi_provider`. On Pico W/Pico 2 W, early-helper opt-in enables
library assembly to create a missing provider from the SDK's polling CYW43/lwIP
target and `esy_project_options`, including the selected native target for FreeRTOS builds.
The helper supplies `esy_network_lwip_provider` only when `ENABLE_ESY_NETWORK`
and `ENABLE_ESY_NETWORK_LWIP` are enabled. Existing providers remain
authoritative when defined before library assembly, including after an explicit
project import. Default targets are created at the library assembly boundary.
Consumers still supply lwipopts and any required native RTOS configuration
through configured include paths.

Direct consumers without the optional integration helpers define their providers
before adding the library, linking the polling SDK target and a
network-configuration target. No consumer target name or native port path is fixed
by the library. Disabled Wi-Fi
has no dependency on its provider, and a disabled network module does not
assemble a TCP provider through the helper.

The manager owns copies of credentials, permits one initialized instance,
and exposes stop/deinit. First-use binding storage requires `{0}`. See the
[Wi-Fi manager contract](../../esy_middleware/wifi/README.md) for task-safe
requests/queries, PM integration, errors and teardown order.

### I/O scheduler targets

`ENABLE_ESY_IO_SCHEDULER` groups protocol-independent SPI and UART TX schedulers in
`esy_middleware/io_scheduler`.
SPI requires its transport, DMA, system and common data structures. Its optional
worker additionally requires FreeRTOS. UART TX requires UART/DMA/common data
structures without an OS or system dependency. Neither scheduler selects or
links a protocol. HAL support alone selects no scheduler.

`ENABLE_ESY_SPI_SCHEDULER` selects `esy_spi_scheduler_core`; FreeRTOS also builds
`esy_spi_scheduler`. `ENABLE_ESY_UART_TX_SCHEDULER` selects
`esy_uart_tx_scheduler`. Consumers explicitly link the target for their execution
model. Direct Pico/STM32 DMA users must call `esy_hal_spi_poll()` from their
serialized owner; the SPI scheduler does so internally. Retain storage until
quiesce/drain and destruction have completed. See the
[I/O contracts](../../esy_middleware/io_scheduler/README.md).

This module only schedules peripheral I/O; network connections, Wi-Fi station
management and Bluetooth LE services remain separate modules.

### Protocol channels and workers

Protocol families and their channel implementations live under
`esy_middleware/protocol`, with flat public headers and family-owned sources
and tests. Concrete targets and API names use `esy_proto_<family>_*`.
`ENABLE_ESY_PROTO_BIN_UART_CHANNEL`, `ENABLE_ESY_PROTO_BIN_USB_CHANNEL` and
`ENABLE_ESY_PROTO_BIN_TCP_CHANNEL` select caller-driven ProtoBin channels.

`ENABLE_ESY_PROTO_MODBUS_UART_CHANNEL` selects a caller-driven Modbus RTU UART
channel. Its consumer also enables `ENABLE_ESY_PROTO_MODBUS`, UART/DMA/system,
`ENABLE_ESY_IO_SCHEDULER` and `ENABLE_ESY_UART_TX_SCHEDULER`. There is no automatic
scheduler selection. Modbus manager-only consumers acquire no UART or OS target.

`ENABLE_ESY_PROTO_MODBUS_UART_WORKER` independently adds a FreeRTOS worker that
owns one channel and the command bridge. Worker and channel handles, storage and
callbacks are distinct. A channel creates no task; worker create is explicit.
Worker shutdown confirms channel cleanup and cooperative task parking before
resource release. See the [protocol contracts](../../esy_middleware/protocol/README.md).

`ENABLE_ESY_PROTO_MODBUS_TCP_CHANNEL` separately selects master/slave TCP channels
with `ENABLE_ESY_PROTO_MODBUS` and `ENABLE_ESY_NETWORK`. Connections are initialized by
their endpoint owner and borrowed into a caller-driven channel. This selection
adds no UART scheduler, OS worker or native networking provider. Channel/session
failure is terminal; the owner retires that peer before recreating on a fresh
session. MBAP input and local TX backpressure use fixed, caller-owned storage.

`ENABLE_ESY_PROTO_JSON_TCP_CHANNEL` independently selects a caller-driven JSON
channel with `ENABLE_ESY_COMMON_PROTOCOL`, `ENABLE_ESY_COMMON_PROTOCOL_JSON` and
`ENABLE_ESY_NETWORK`. It requires neither the ProtoBin/Modbus families nor HAL,
system time, common DS or OS support. Its owner supplies a connected peer with
prefix send/receive and connection status callbacks, plus bounded RX/TX buffers.
The channel frames each document with an unsigned four-byte big-endian byte
length, excluding the prefix and any C terminator. It validates complete JSON
before admission or delivery. There is no implicit network provider, connection
lifecycle, timeout, retry or request/response schema. The endpoint owner stops
the channel before retiring or replacing the peer. See the
[protocol contracts](../../esy_middleware/protocol/README.md).

`ENABLE_ESY_PROTO_JSON_USB_CHANNEL` separately selects a caller-driven JSON
channel with the common protocol/JSON and USB/CDC gates. The consumer selects
the CDC backend and supplies `esy_usb`, then lends one initialized CDC instance
with `discard_on_disconnect = true` and a nonzero session generation. The same
four-byte big-endian length convention frames complete JSON documents.
The channel does not call `esy_usb_cdc_task()`: the existing USB stack owner
pumps once and then polls its channel instances under the backend's threading
contract. This avoids duplicate global pumping when several CDC interfaces are
used. The channel may remain initialized while disconnected; observed session
generation changes discard local partial RX/staged TX and prior errors before
admitting the new peer. No message is queued while closed. Native USB provider
hardware, OS dependencies, initialization and stack timing remain consumer-owned.

## Caller-driven middleware

Logger buffered backends, SPI schedulers and watchdog monitoring share
their engines with optional OS adapters. Their `_core.h` headers and concrete
core targets have no OS resource/header/target requirement. Protocol channels
are caller-driven under their plain channel names; explicitly selected protocol
workers add OS execution. Consumer selection does not initialize modules or
create workers.
Use explicit core initialization and call each module's documented processing
step from its owner loop. Budgets limit bytes/blocks/requests/slots, not provider
or callback elapsed time. See the [Logger](../../esy_middleware/logger/README.md),
[I/O schedulers](../../esy_middleware/io_scheduler/README.md),
[protocol](../../esy_middleware/protocol/README.md) and
[watchdog](../../esy_middleware/watchdog_monitor/README.md) ownership and stop
contracts.
Core coordination is synchronous: `esy_core_lockout_core_init()` initializes
local-IRQ protection without workers or a polling step. It requires every peer
core to remain inactive until reset; it does not provide baremetal multicore
lockout. The FreeRTOS adapter retains permanent peer workers. Mode cannot switch
after selection, including partially failed worker initialization. Consumers call
`esy_core_lockout_execute()` explicitly; Flash users bind it with
`esy_hal_flash_set_safe_execute()` before mutations. Neither init installs a
Flash hook. Manual archive consumers must add the core library. See the
[Core lockout contracts](../../esy_middleware/core_lockout/README.md).

Other modules retain their existing runtime requirements; no empty OS backend
is supplied to make an OS-only module appear compatible.

`esy_lvgl` is intentionally OS-only and exempt from the caller-driven
core/OS-adapter split. It retains its FreeRTOS GUI task, request queue and
notification-based waits for asynchronous display completion. No baremetal GUI
runtime or separate core target is provided; this boundary is intentional rather
than an incomplete middleware split. The upstream LVGL provider remains configured
with `LV_USE_OS=LV_OS_NONE`; scheduling belongs to `esy_lvgl`. See the
[LVGL runtime contract](../../esy_middleware/lvgl/README.md).
