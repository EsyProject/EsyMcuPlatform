<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Build Integration

## Purpose

EsyMcuPlatform supplies optional CMake helpers for platform selection, SDK import,
provider assembly and firmware output generation. Including these helpers adopts
conventional directory bindings and build defaults. A conventional project needs
no `cmake/esy_build_config.cmake`; an optional override file contains only settings
that differ from the defaults.

Consumers own device and feature choices, board implementations, linker
contents, SDK installations, application startup and product resource policy.
The helpers can be used directly from any consuming firmware project's CMake
files; the consumer supplies its feature file, board port and configuration.

## Public Surface

| Entry point | When to include it | Responsibility |
| --- | --- | --- |
| `cmake/esy_early_import.cmake` | Before the consumer's `project()` | Validate the explicit device, derive the platform, load optional build overrides, fill defaults and import the SDK/toolchain. Enable automatic project setup during library assembly. Script mode stops before SDK import. |
| `cmake/esy_project_import.cmake` | Normally automatic during library assembly; explicit after the consumer's `project()` for advanced setup | Read the consumer feature file, configure shared project options, generate the application version header and assemble selected SDK/software providers. |
| `cmake/esy_firmware.cmake` | After creating the firmware target and adding the library | Apply the selected port's linker/output steps to `ESY_FIRMWARE_TARGET`. |

The helpers are optional. Early import opts the consumer into automatic project
setup when adding the library. Without that opt-in, direct `add_subdirectory()`
retains the existing explicit SDK, feature, provider and optimization-target
requirements. Firmware output setup always remains an explicit consumer step.
Including only the project helper retains its explicit input contract; the early
helper establishes the conventions described here.

Platform-specific device rules, defaults, SDK/runtime providers and firmware
outputs live under `cmake/ports/<port>/` in exactly three files:
`device_map.cmake`, `pre_project.cmake` and `post_project.cmake`.
The descriptor contains pure device/CPU metadata. The early entry directly
includes the pre-project file for SDK/toolchain preparation; shared callers load
the post-project file and invoke its namespaced hooks at the required times.
Loading `post_project.cmake` only declares functions/macros; it performs no
setup. Its pure build-default hook can run before `project()` and in script
mode. The firmware helper calls `configure_firmware(target)` after the target
exists to apply linker and output settings. See
[adding device and platform support](porting.md) for descriptor contracts,
hook timing, backend/OS ownership and port verification.

## Integration

With conventional product files and installed SDK/toolchain locations, a
Pico 2 W executable assembly is:

```cmake
cmake_minimum_required(VERSION 3.20)
set(ESY_TARGET_DEVICE PICO2_W CACHE STRING "Build target")
set(ESY_TOOLCHAIN_ROOT "/path/to/arm-toolchain" CACHE PATH "GNU Arm toolchain root")
set(ESY_MCU_PLATFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/EsyMcuPlatform")
include("${ESY_MCU_PLATFORM_DIR}/cmake/esy_early_import.cmake")
project(firmware VERSION 0.1.0 LANGUAGES C CXX ASM)
add_subdirectory("${ESY_MCU_PLATFORM_DIR}")
set(ESY_FIRMWARE_TARGET firmware)
add_executable(${ESY_FIRMWARE_TARGET} main.c)
target_link_libraries(${ESY_FIRMWARE_TARGET} PRIVATE esy_opt_default esy_hal)
include("${ESY_MCU_PLATFORM_DIR}/cmake/esy_firmware.cmake")
```

`ESY_FIRMWARE_TARGET` names the executable target. Define it once and use its
value when creating and linking that target. Include the firmware helper after
the target exists.

Set `ESY_TARGET_DEVICE` before early import. The platform is derived internally. CMake
command-line values override the example's cache defaults, for example:

```sh
cmake -S . -B build -G Ninja -DESY_TARGET_DEVICE=PICO2_W
cmake --build build
```

The consumer supplies its feature file, board code and selected linker script,
plus providers required by enabled modules. The helper does not create these
product files. `.project.conf` is not a library input and is never discovered or
read. The library checkout and every configurable input path can be elsewhere
and can contain spaces. Use a fresh build directory when changing the device or
toolchain.

After early import and the consumer's `project()`, library assembly runs the
project helper before declaring EsyMcuPlatform's own project. Shared options,
feature loading, provider assembly and application-version generation therefore
retain the consumer's project identity and version. Use `ESY_PROJECT_ROOT` for
product-relative paths and `CMAKE_CURRENT_LIST_DIR` for feature-file-relative
paths: automatic setup evaluates the feature file from the library directory.

Resolved `ESY_*`, `APP_*`, `ENABLE_ESY_*` and `ALLOW_ESY_*` settings return to
the calling directory. So do `ESY_RTOS`, `FREE_RTOS_KERNEL_LIB`, `TINYUSB_OPT_OS`,
`LVGL_PATH`, `LV_BUILD_CONF_PATH` and `CMAKE_MODULE_PATH`; Pico SDK settings use
the SDK's own scope-promotion mechanism. These outputs remain available to
subsequent consumer modules. Other custom feature-file variables retain normal
CMake directory scope.
Pico's `PICO_SDK` marker and `CMAKE_EXECUTABLE_SUFFIX` also return to the caller.

An explicit project-helper include after the consumer's `project()` remains
supported when the root needs feature values or provider setup before adding the
library. This advanced path also supports custom builds without early import.
Completed setup is reused: keeping an existing explicit include, or including
the helper after automatic setup, does not repeat feature loading or provider
assembly.

### Selection and override order

`ESY_PROJECT_ROOT` defaults to `CMAKE_SOURCE_DIR`. Before importing the SDK, the
early entry performs these steps:

1. Normalize and validate the consumer's device selection and derive its platform.
2. Load an existing `cmake/esy_build_config.cmake` for consumer build overrides.
3. Fill unspecified common and port defaults, then directly include the selected
   port's `pre_project.cmake` before `project()`.

`pre_project.cmake` prepares platform integration before compiler detection: it
imports the selected SDK or selects a default CMake toolchain. STM32F4 uses that
same file as its default `CMAKE_TOOLCHAIN_FILE`, including compiler and
target-system configuration. CMake loads it again during `project()` and
compiler probes, so its toolchain work must remain safe to repeat in probe
projects. Shared CPU/ABI data stays in `device_map.cmake`. An explicitly supplied
`CMAKE_TOOLCHAIN_FILE` takes precedence and is not loaded by early import.

When migrating an existing STM32F4 build that cached the former
`cmake/ports/stm32f4/toolchain.cmake` path, configure a new build directory so
CMake selects `pre_project.cmake`. Consumer-owned toolchains are unaffected by
this internal path change.

Use `ESY_BUILD_CONFIG_FILE` to select a different build override file. An
explicitly empty value suppresses discovery of its conventional file. A missing
conventional override is optional; an explicitly selected, nonempty override
file must exist.

The consumer prepares SDK/toolchain inputs through CMake arguments, supported
environment variables or its own configuration files. The helpers do not
discover or load private environment files. A consumer may load its own SDK
settings before early import, for example:

```cmake
# Optional configuration owned and loaded by the consumer.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/sdk_paths.cmake" OPTIONAL)
include("${ESY_MCU_PLATFORM_DIR}/cmake/esy_early_import.cmake")
```

A build override can use normalized platform fragments and runs early enough to
set SDK/toolchain paths, build type or ESP-IDF component settings. For example:

```cmake
# Optional consumer cmake/esy_build_config.cmake.
set(ESY_SPECS_DIR "${ESY_PROJECT_ROOT}/config/${ESY_SPECS_PLATFORM_PATH}")
set(ESY_DEFAULT_COMPILE_OPTIONS -Os -ffunction-sections -fdata-sections)
set(ESY_SPEED_COMPILE_OPTIONS -O2 -ffunction-sections -fdata-sections)
```

Path, language and option defaults fill only undefined inputs, preserving explicit
normal/cache values and empty option lists. For example, `-DCMAKE_BUILD_TYPE=Release` preserves Release
and `-DESY_FEATURE_IMPORT_FILE=` disables feature-file discovery. User-authored
CMake files retain normal CMake `set()` semantics for other build settings.
Selection is fixed before the build override loads: provide `ESY_TARGET_DEVICE`
through command-line/cache values or explicit CMake
assignments before early import. The build override cannot select or replace
the target.
For example, an explicitly selected STM32F4 build uses:

```sh
cmake -S . -B build-stm32 -G Ninja -DESY_TARGET_DEVICE=STM32F407ZG
```

Device identifiers are case-insensitive and normalize to `PICO`, `PICO_W`,
`PICO2`, `PICO2_W`, `ESP32`, or a recognized concrete STM32F4 model such as
`STM32F407ZG`. These are supported build target IDs: Pico IDs identify board
profiles, whereas STM32 IDs identify parts. The consumer still supplies its
board implementation and product configuration.

`ESY_TARGET_DEVICE` is the only target-selection input. The shared
[target resolver](../../cmake/esy_target_device.cmake) derives
`ESY_TARGET_PLATFORM` and the build/board/specs paths on every configuration.
The derived platform is not cached. Do not set `ESY_TARGET_PLATFORM`.
Use a fresh build directory when changing architecture, SDK or toolchain.
Edits to `.project.conf` have no effect on a library build.

STM32 selection uses the explicit [device catalog](../../cmake/ports/stm32f4/device_map.cmake)
to map recognized package/density cores to CMSIS macros and matching startup
filenames. A core may include an ordering suffix, such as `STM32F401RET6`.
Bare family names, CMSIS macros used as model names, unknown families and
unrecognized package/density combinations fail before SDK import. The catalog
contains known combinations; it is not a complete ordering-code database.

| Model group | Exported CMSIS group |
| --- | --- |
| STM32F401 with B/C density | `STM32F401xC` |
| STM32F401 with D/E density | `STM32F401xE` |
| STM32F410 with T/C/R pin-count code | `STM32F410Tx`, `STM32F410Cx`, `STM32F410Rx` |
| STM32F411 with C/E density | `STM32F411xE` |
| STM32F412 with C/R/V/Z pin-count code | `STM32F412Cx`, `STM32F412Rx`, `STM32F412Vx`, `STM32F412Zx` |
| STM32F405/407/413/415/417/423/427/429/437/439/446/469/479 | Their explicitly listed `STM32Fxxx` + `xx` CMSIS groups |

For example, `STM32F401RE` selects `STM32F401xE` and
`startup_stm32f401xe.s`; `STM32F412RG` selects `STM32F412Rx` and
`startup_stm32f412rx.s`. The consumer board must accept the selected CMSIS macro
and supply the matching vendor header/startup source, clock configuration and
linker layout. A board that accepts only family-wide `...xx` groups must update
its validation before using the package/density-specific groups.

Device selection does not imply that every feature supports every family member.
When `ENABLE_ESY_FLASH` is enabled, the STM32F4 backend currently accepts only
STM32F405/407/415/417 single-bank layouts. Actual capacity comes from the chip's
factory Flash-size register rather than a family-wide constant. Other STM32F4
models require per-feature port and board verification with Flash disabled;
correct CMSIS/startup selection alone does not add peripheral or Flash support.
See [Flash geometry](flash_geometry.md) for the runtime geometry, address
conventions and board-layout boundary.

Selection exports `ESY_TARGET_PLATFORM`, `ESY_BUILD_PORT`,
`ESY_BOARD_PLATFORM_PATH` and `ESY_SPECS_PLATFORM_PATH`. Pico 2
variants use `pico2` for boards and `pico` for specs; original Pico variants use
`pico` for both. STM32F4 and ESP32 use their lowercase family names. STM32F4 also
exports `ESY_STM32F4_DEVICE_DEFINE` and `ESY_STM32F4_STARTUP_SOURCE`. In `cmake -P`
mode, selection, optional overrides and defaults are available without an SDK;
this mode does not establish a working compiler/toolchain.

Script-mode selection is independent of the consumer's root CMake file. For
example, `cmake -DESY_TARGET_DEVICE=PICO2_W -P /path/to/EsyMcuPlatform/cmake/esy_early_import.cmake`
normalizes Pico 2 W without importing an SDK. A consumer's dependency-discovery
script can include this entry with the same selection as its firmware build.
For a machine-readable mapping without project defaults or overrides, shell tools
can use the shared resolver directly:

```sh
cmake -DESY_TARGET_DEVICE=PICO2_W -DESY_TARGET_MAP_FILE=/tmp/esy-target.txt \
  -P /path/to/EsyMcuPlatform/cmake/esy_target_device.cmake
```

The output contains `KEY=VALUE` lines for the normalized device, derived platform,
build port, board/specs paths, and STM32 CMSIS/startup fields (empty on other
ports). This resolver never reads project metadata or imports an SDK. CMake
consumers can include the same file and call `esy_configure_target_device()`
before using the derived platform; library and backend entry points resolve it
as well.


### Conventional defaults

All paths below are ordinary CMake variables. The helper does not force them
into the cache or replace an explicit override.

| Input | Default |
| --- | --- |
| `CMAKE_BUILD_TYPE` | `Debug` when a single-configuration generator has no build type. Multi-configuration generators retain their own configuration selection. |
| Language settings | C11, C++17, required C standard, C extensions enabled, compilation database enabled. |
| `ESY_FEATURE_IMPORT_FILE` | `${ESY_PROJECT_ROOT}/esy_feature_import.cmake` if it exists. |
| `ESY_BOARD_PATH` | `${ESY_PROJECT_ROOT}/esy_board`. |
| `ESY_SPECS_DIR` | `${ESY_PROJECT_ROOT}/specs/${ESY_SPECS_PLATFORM_PATH}`. |
| `ESY_RTOS_CONFIG_DIR` | `${ESY_SPECS_DIR}/include/specs`, the exact native RTOS configuration-header directory. |
| `ESY_VERSION_TEMPLATE` | The library's `cmake/version.h.in`. |
| `GENERATED_INCLUDE_DIR` | `${CMAKE_BINARY_DIR}/generated`. |
| `FREERTOS_KERNEL_PATH` | Nonempty environment value, otherwise `${ESY_PROJECT_ROOT}/external/FreeRTOS-Kernel`. |
| `LVGL_PATH` | Nonempty environment value, otherwise `${ESY_PROJECT_ROOT}/external/lvgl`. |
| `LV_BUILD_CONF_PATH` | `${ESY_SPECS_DIR}/include/specs/lv_conf.h`. |
| `ESY_STM32_SUPPORT_ROOT` | `${ESY_BOARD_PATH}/stm32f4/support` on STM32F4. |
| `ESY_LINKER_SCRIPT_DEFAULT` | `${ESY_BOARD_PATH}/${ESY_BOARD_PLATFORM_PATH}/support/esy_memmap/default_memmap.ld`. |
| `ESY_LINKER_SCRIPT_A`, `ESY_LINKER_SCRIPT_B` | The same directory with `memmap_a.ld` and `memmap_b.ld`. |

An explicitly empty specs path also suppresses derived RTOS/LVGL config paths.
Provider paths are validated when their feature requires assembling the provider;
disabled capabilities do not require unused source trees.

`ESY_DEFAULT_COMPILE_OPTIONS` and `ESY_SPEED_COMPILE_OPTIONS` supply GNU/Clang
warning and configuration-specific optimization defaults. Debug uses `-Og` and
debugging information; Release uses `-O2` and `-O3`, respectively. RelWithDebInfo
uses `-Os` for both; MinSizeRel uses `-Os` and `-O3`, respectively. Release-family
profiles use function/data sections, and `ESY_DEFAULT_LINK_OPTIONS` enables
section garbage collection with supported GNU/Clang link drivers. GNU-specific
variable-tracking flags apply only to GNU compilers. Other compiler families
receive no GNU/Clang profile flags.

These are integration defaults, independent of feature policy. The helper does
not define `ENABLE_ESY_*`, select an OTA slot or set product IRQ
capacities.

### Application version header

With early import and library assembly, the library-owned `cmake/version.h.in`
generates `${GENERATED_INCLUDE_DIR}/version.h` without a consumer template. The
generated include directory propagates through `esy_project_options`. The
consumer's `project(... VERSION ...)` supplies the application version; the
library's own version does not replace it. The default header exposes:

- `APP_VERSION_MAJOR`, `APP_VERSION_MINOR` and `APP_VERSION_PATCH` as integer macros.
- `APP_VERSION` as `(major * 10000 + minor * 100 + patch)`.

Omitted version components become zero, so a project without `VERSION` produces
`0.0.0`, and `VERSION 2.3` produces `2.3.0`. A fourth CMake version component does
not enter these macros. Zero-padded components render as decimal integer macros,
so `VERSION 08.010.00` produces `8.10.0` without C octal interpretation. Header
generation leaves CMake's `PROJECT_VERSION` and its component variables unchanged,
including their original zero padding for custom templates.

An explicit nonempty `ESY_VERSION_TEMPLATE` selects a custom template. The helper
uses the library template when this input is undefined. Set `ESY_VERSION_TEMPLATE`
to an empty value to disable header generation entirely. A selected template must
exist and requires a nonempty `GENERATED_INCLUDE_DIR`.
If the build cache explicitly sets `ESY_VERSION_TEMPLATE`, remove that cache
entry to return to the library template.

Custom templates keep access to ordinary CMake inputs such as
`@PROJECT_VERSION@`. Consumers including only the project helper must supply
their template and generated directory explicitly. Direct library assembly
without either integration helper does not generate a version header.

### Project options and feature ownership

The consumer-owned feature file remains the only source of `ENABLE_ESY_*`
values. Default discovery selects that file when present; it does not invent
feature values. An explicit `ESY_FEATURE_IMPORT_FILE` may select any
other path, and an empty value disables import.
Automatic project setup reads this file when adding the library; resolved
feature values are then available to consumer subdirectories added afterward.

The consumer declares feature values through ordinary `set()` assignments in
the selected feature file. These assignments take precedence over conflicting
feature values from command lines, presets or an existing cache. The platform
only consumes the resulting values. Device, build type, toolchain, image slot
and integration-path settings follow their separate input contracts.

`esy_project_options` carries platform definitions and configured specs/generated
include paths. Link it to consumer targets that need those settings. The project
helper links it through `esy_opt_default` and `esy_opt_speed` for library targets.
Existing optimization targets retain their options; the helper appends shared
project options and the selected port's ABI compile/link requirements. Missing
optimization targets are created from the configured option lists.

`ESY_PROJECT_COMPILE_DEFINITIONS` adds consumer definitions to the shared
interface. Existing feature gates control QEMU/full-assert definitions.
`ESY_SPECS_DIR` and `ESY_RTOS_CONFIG_DIR` may be independently overridden: one is
a family root and the other is the exact native configuration-header directory.

### Effective feature artifact

Project import schedules `${CMAKE_BINARY_DIR}/esy_features.json` for CMake
generation. This artifact records the effective values
after feature-file evaluation and provider assembly; it is build evidence and
is not read back as configuration input. Direct library assembly without
project import does not produce it.

| Field | Meaning |
| --- | --- |
| `schema_version` | Integer `1`. |
| `target_device` | The normalized selected device. |
| `project_root` | The real source path from `CMAKE_SOURCE_DIR`, identifying the actual CMake project even when `ESY_PROJECT_ROOT` binds external product files. |
| `feature_import_file` | The real path of the imported feature file, or an empty string when no file was imported. |
| `features` | Defined `ENABLE_ESY_*` names with their effective values normalized to JSON booleans using CMake condition semantics. |

A declared normal value wins over an opposing cache entry in this snapshot.
Only defined feature names are included; a missing `ENABLE_ESY_LVGL` means the
GUI feature is not selected. Non-script early and project import invalidate
the previous snapshot before checking configuration inputs. Script-mode
dependency discovery does not create or invalidate firmware build artifacts.
Quality tooling must first require successful firmware configuration/build and
validate the artifact's identity before using it. File presence alone does not
establish success of the latest configuration command: CMake can write the
artifact and subsequently report a generation error. Callers must check the
configure/build command result before reading the snapshot.

### SDK and software providers

The selected port imports the SDK before compiler detection and initializes it
after `project()`. SDKs are supplied by the consumer or environment:

| Input | Owner and use |
| --- | --- |
| `PICO_SDK_PATH`, `PICO_TOOLCHAIN_PATH` | Installed Pico SDK and compiler-tool roots. |
| `ESY_TOOLCHAIN_ROOT` | GNU Arm toolchain search root for STM32F4; also supplies Pico's toolchain path. |
| `IDF_PATH` | Installed ESP-IDF build environment. |
| `ESY_PLATFORM_SDK_ROOT` | SDK-root alias for Pico SDK or ESP-IDF. |
| `ESY_SEGGER_RTT_ROOT` / `ESY_SEGGER_RTT_TARGET` | Consumer-selected RTT source tree or existing target. |
| `ESY_COREMQTT_PATH` / `ESY_COREMQTT_CONFIG_TARGET` | Explicit coreMQTT v2.3.1 source checkout and optional existing configuration target; used only when `ENABLE_ESY_MQTT` needs a provider. An existing `esy_mqtt_provider` takes precedence. |
| `ESY_COREJSON_PATH` | Explicit coreJSON v3.3.1 source checkout; used only when `ENABLE_ESY_COMMON_PROTOCOL_JSON` needs a provider. An existing `esy_json_parser_provider` takes precedence. |
| `ESY_LITTLEFS_PATH` | Existing official littlefs source directory, tested with v2.11.2; used only by `ENABLE_ESY_FS_LITTLEFS`. The adapter assembles its provider with static buffers and no dynamic allocation. |
| `ESY_FATFS_PATH` | Official FatFs R0.16 `source` directory; used only by `ENABLE_ESY_FS_FATFS`. The adapter validates the upstream source hashes and stages its explicit configuration without modifying the supplied checkout. |

Prepare SDK/toolchain paths before early import, through consumer configuration
or the build override. `ESY_TOOLCHAIN_ROOT` names the GNU Arm installation root;
executables may be directly under it or in `bin/`. The STM32F4 toolchain also
searches the supplied compiler's directory and normal CMake program paths,
including `PATH`. Pico uses SDK compiler discovery; its `PICO_TOOLCHAIN_PATH`
settings take precedence over `ESY_TOOLCHAIN_ROOT`. ESP32 uses the ESP-IDF
toolchain environment. Platform/device selection precedes the build override.

Existing provider targets remain consumer-owned. Enabled features determine
required providers; see the [feature guide](feature_gates.md) and module READMEs
for compatibility and runtime contracts. The helpers do not fetch source
checkouts or search developer home directories. SDKs retain their own auxiliary
tool discovery, version checks and source-build behavior. When the Pico port
initializes the SDK, it supplements that discovery as described below.

#### Pico host tools

The Pico port appends existing installer packages for `pioasm` and `picotool`
to the system package prefixes before its call to `pico_sdk_init()`. Candidate
directories are relative to the selected, canonical SDK root: the installer's
`tools/<sdk-version>/pioasm` and `picotool/<sdk-version>/picotool` siblings.
The version comes from SDK metadata, not the name of the SDK directory. Only
directories containing the respective CMake package configuration are added.
An ordinary SDK checkout without these packages retains SDK behavior.

Explicit package hints, environment inputs and existing CMake package search
prefixes retain their normal precedence. The port neither sets nor force-updates
`pioasm_DIR` or `picotool_DIR`; the SDK's own `find_package()` validates tool
versions and may cache its resolved paths. Clear or explicitly reselect those
standard package cache entries when switching installations. Existing tool
targets are preserved, and the port does not add a picotool candidate when
`PICOTOOL_FORCE_FETCH_FROM_GIT` requests a source build. SDK initialization
already performed by the caller remains caller-owned.

This provides the same discovery for IDE, command-line and separate OTA build
directories. Templates need not infer tool paths in editor settings or project
overrides. Using an installed tool avoids rebuilding it with the host compiler;
it does not establish source-build compatibility between an SDK and every host
compiler version.

The library's `./setup.sh` prepares host-test dependencies for contributors.
Library test tools discover those sources in a shared cache; firmware CMake
continues to use consumer-supplied targets and paths. Setup does not initialize
or replace a consuming project's LVGL or FreeRTOS submodules. See
[software provider setup](../../tools/README.md#software-provider-setup).

For Pico FreeRTOS integration, an unset or empty `TINYUSB_OPT_OS` receives
`OPT_OS_PICO`. Explicit values, including `0` (`OPT_OS_NONE`), are preserved; the
consumer validates the selected stack's concurrency contract. LVGL source imports
require CMake 3.21 or newer and default provider policies `CMP0077` and `CMP0126`
to `NEW`, preserving ordinary feature/configuration assignments across the
provider's older policy minimum without platform cache bridging. Pico
FreeRTOS source imports require `portable/ThirdParty/GCC/<port>` in the selected
kernel checkout. Pico RTT may reuse the SDK's bundled
SEGGER sources when no explicit source root or target is selected; it does not
enable another stdio owner.
The Logger RTT provider is imported only when both `ENABLE_ESY_LOGGER` and
`ENABLE_ESY_LOGGER_BACKEND_RTT` are enabled. Disabling Logger leaves a cached
RTT child selection inactive and does not require its source checkout. Existing
RTT providers used independently by the consumer or SDK remain consumer-owned.

The MQTT provider is imported only with `ENABLE_ESY_MQTT` and only when no
`esy_mqtt_provider` target exists. Set `ESY_COREMQTT_PATH` to a coreMQTT v2.3.1
checkout, or supply it through the same environment variable when the CMake
variable is unset. Explicit empty paths do not fall back to the environment.
The helper verifies the version and compiles only the three core protocol source
files; it does not download sources or create a network, TLS or OS provider.
Its default configuration uses `MQTT_DO_NOT_USE_CUSTOM_CONFIG`; an optional
existing `ESY_COREMQTT_CONFIG_TARGET` instead propagates the include paths and
definitions for consumer-owned `core_mqtt_config.h` consistently to the provider
and adapter. Provider configuration must meet the [MQTT contract](../../esy_middleware/mqtt/README.md).

Direct consumers may reuse the same explicit source assembly before adding the
library:

```cmake
set(ESY_COREMQTT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/external/coreMQTT")
include("${CMAKE_CURRENT_SOURCE_DIR}/external/EsyMcuPlatform/cmake/esy_coremqtt_provider.cmake")
esy_configure_coremqtt_provider()
```

Feature selection remains in the consumer's feature import file. With
`ENABLE_ESY_MQTT` disabled, normal library assembly adds neither `esy_mqtt` nor its
provider, and does not validate any coreMQTT path. Independently supplied
provider targets remain consumer-owned.

Bluetooth LE adds no provider automatically. Select `ENABLE_ESY_BLE` in the consumer
feature file and link `Esy::ble` for the provider-independent facade. The consumer
injects a software-stack provider through `esy_ble_provider_t`. The public core
target requires only common types and does not expose native stack, HAL or OS
types. Consumer-owned provider targets carry their own software-stack headers,
configuration and link requirements.

For the optional BTstack adapter, also select `ENABLE_ESY_BLE_BTSTACK` in the
consumer feature file and define `esy_ble_btstack_provider` before platform
assembly. This target supplies real BTstack public headers, the consumer's
`btstack_config.h` and software-stack link requirements. Link `Esy::ble_btstack`.
The adapter keeps native types out of its public header, so provider compile
requirements are private; static-library link requirements still reach the final
consumer. The adapter does not directly select a hardware transport target or
import a stack source checkout.

Compile all native BTstack provider sources, including `att_server.c`, and the
adapter with one compatible `btstack_config.h` that explicitly defines
`NVN_NUM_GATT_SERVER_CCC` as `0`. This disables automatic CCC TLV persistence and
restoration: subscriptions expire on disconnect, and subscription writes do not
perform native key-store I/O. Setting the macro only on the adapter target is
insufficient. Adapter compilation rejects missing/nonzero settings and
`ENABLE_GATT_OVER_EATT`; only the fixed ATT bearer is supported.

Adapter static resources additionally require an exclusive `read_buffer` and
`read_buffer_size` in `1..UINT16_MAX`. Dynamic reads snapshot
`min(total_length - offset, fixed_ATT_MTU - 1)` bytes during the native length
probe, reporting insufficient capacity or application errors before native
copying. Callbacks must return slices consistent with their length query and
have no destructive side effects; prefetched data may be discarded. Notification
payloads above the negotiated fixed ATT MTU minus three are rejected with
`ESY_ERR_INVALID_ARG`. See the module contract for capacity and callback details.

The checked compatibility baseline is BTstack 1.6.2, commit
`501e6d2b86e6c92bfb9c390bcf55709938e25ac1`, from Pico SDK 2.1.1. A consumer
supplies initialized HCI/run-loop/L2CAP/security-manager and durable TLV/bond
database resources through its proper integration layers. Core calls and native
callbacks execute in one serialized, non-ISR BTstack context. Runtime startup,
controller transport and final runtime shutdown remain consumer-owned; the
adapter attaches to and detaches from that borrowed runtime. Other stack
versions/configurations need their own compatibility validation.

To compile the adapter against an explicitly supplied real source checkout, run
`python3 -B esy_middleware/ble/tests/test_cmake_consumer.py --btstack-path /path/to/btstack`
from the repository root. `ESY_BTSTACK_PATH` is an alternative explicit test input.
Without it, core and target-contract checks run while real-header compilation is
reported skipped. The test fixture supplies a host configuration and validates
adapter source compilation; it does not prove native stack linkage or radio
operation. The module's separate runtime test uses real headers with local host
fakes and also links upstream `att_db.c` for real ATT request/error regressions;
it does not establish controller or durable persistence behavior.

Portable GAP, GATT server definitions and security events are translated by that
provider. It must enforce characteristic access/notification security, own
subscription state, quiesce registered callbacks before releasing their borrowed
definitions, and persist/restore bond keys with explicit storage error reporting.
No actual radio backend or persistent key storage is assembled by the core.

The library does not import a protocol-stack checkout, create a native scheduler,
configure a Bluetooth controller/HCI transport, or reserve bond storage. The
consumer supplies those integration decisions, compatible stack configuration
and runtime resources. See the [BLE module contract](../../esy_middleware/ble/README.md)
for ownership, native callback and persistence requirements. Core host checks
verify provider-independent behavior and target requirements. Native provider
builds check stack linkage; board procedures cover controller and radio behavior.

MQTT consumes the basic `esy_network_stream_t` contract: send, receive, explicit
progress and a borrowed context. ProtoBin consumes `esy_network_connection_t`,
which embeds that stream and adds whole-write and session/peer-ACK status using
its same context. Enable `ENABLE_ESY_NETWORK` for these header contracts; endpoint
connection-establishment states remain in the lwIP endpoint API. The optional `ENABLE_ESY_NETWORK_LWIP`
provider additionally requires common DS and an explicit
`esy_network_lwip_provider` with raw lwIP headers, sources and network
configuration. It owns client/server connections and caller-supplied RX storage.
Connection deadlines, reconnection and ordered cleanup belong to the consumer.
For a polling Wi-Fi backend, the application obtains an
`esy_wifi_network_runtime_t` through `esy_hal_wifi_get_network_runtime()` and
injects its progress/entry/exit callbacks into the existing lwIP configuration.
Use the runtime context for both network callback context fields. The descriptor
does not initialize Wi-Fi, create a task or select endpoint policy. Keep the
Wi-Fi backend alive until endpoint and borrower cleanup completes. No
Pico-specific network target or feature gate is required. See the
[network contract](../../esy_middleware/network/README.md).

On `PICO_W` and `PICO2_W`, integrated library assembly supplies default network
providers after SDK and RTOS setup. Missing targets are created before
HAL/backend and middleware assembly:

- `ENABLE_ESY_WIFI` adds `esy_wifi_provider`.
- `ENABLE_ESY_NETWORK` with `ENABLE_ESY_NETWORK_LWIP` adds
  `esy_network_lwip_provider`.

These interface targets link `pico_cyw43_arch_lwip_poll` and
`esy_project_options`; `ESY_RTOS=FreeRTOS` also links the selected
`FREE_RTOS_KERNEL_LIB`. Configured include paths therefore reach both the network
adapters and SDK sources propagated by the poll target. Supply `lwipopts.h`
through those paths, conventionally `${ESY_SPECS_DIR}/include/specs`; the consumer
still owns its contents, credentials and polling lifecycle. No target named
`specs` or fixed native RTOS port directory is required. A missing SDK poll target
fails configuration with the required provider and SDK target names.

Existing provider targets are preserved without adding default dependencies.
For custom integration, define them before adding the library. Existing explicit
project-helper setups may declare providers on either side of that include.
Project-only helper usage enables the same deferred provider assembly with
explicit configuration inputs. Direct consumers without integration helpers
continue to provide these targets
themselves. Defaults do not create Pico network providers on other platforms,
and disabled capabilities add none. `ENABLE_ESY_NETWORK_LWIP` alone does not
assemble a provider when `ENABLE_ESY_NETWORK` is disabled. Network provider
selection is independent of protocol assembly. These defaults retain the supported
polling stack contract; they do not enable threaded/background network stacks.

On ESP32 only, an existing `${ESY_PROJECT_ROOT}/app/esp32/CMakeLists.txt` enables
the conventional component binding: undefined `EXTRA_COMPONENT_DIRS` defaults to
that directory and undefined `COMPONENTS` defaults to `esp32`. Explicit values,
including empty ones, are preserved. Set custom component bindings in the build
override before SDK import. ESP-IDF retains its component and image lifecycle.

ESP32 managed boot/OTA also requires the native `app_update` and `esp_partition`
components under `ENABLE_ESY_BOOTLOADER`. System-only assembly does not require
`app_update`. Image handoff uses `esy_hal_boot_jump_to_app()`.
The managed-boot consumer partition table must
provide `ota_0`, `ota_1`, native
`otadata`, and separate writable, unencrypted `data` partitions with custom
subtypes `0x40` through `0x7F` for Esy metadata. Both app partitions must exactly
match the board's slot addresses and shared slot capacity; do not place Esy
metadata inside `otadata`. The running app must be in one of these OTA partitions.
Enable
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` in the SDK configuration. The current
raw Flash writer does not support Flash encryption, and managed update queries
reject encrypted devices. Other ESP32-series targets are not implied by the
`ESP32` build selection.

Activation verifies the native image and selects its partition without resetting;
the consumer owns shutdown and reset. On the next boot, diagnostics and explicit
confirmation complete the native trial. The SDK bootloader and `otadata` retain
boot-selection/rollback authority. See
[ESP32 managed boot and OTA](flash_geometry.md#esp32-managed-boot-and-ota)
for the complete setup, call order and power-loss windows. The build helpers do
not generate these partition or confirmation policies.

ESP32 I2C requires the native `esp_driver_i2c` and `esp_driver_gpio` components
and their `idf::` targets. Its verified provider is ESP-IDF 5.3.4 for the classic
ESP32, including `i2c_master_execute_defined_operations` and addressless master
devices. The consumer board's
`esy_board_i2c_configure_pins` hook must fill `inner.sda_pin` and `inner.scl_pin`
with distinct output-capable GPIO numbers for master initialization.
HAL passes a copied configuration, initializes those fields to `ESY_PIN_UNUSED`
before the hook, and forwards the completed metadata to the backend. Setting
pin fields in the application configuration does not override board routing.
Unfilled, equal or invalid pin assignments fail before SDK configuration; the
backend supplies neither disconnected placeholders nor product-specific defaults.
The native bus retains the accepted pins and speed for bus reset recovery.
Runtime speed changes and slave mode return `ESY_ERR_NOT_SUPPORTED`; changing
frequency requires deinitialization and a new master initialization. One SDK
bus and one addressless device are created at initialization; synchronous
transfers do not allocate per-address devices or command links. Other ports
may perform fixed routing in their board code. The consumer supplies its own
board hook; this repository does not generate a board pin map. Use the
[ESP32 I2C routing HIL plan](../../esy_hal/tests/hil/esp32_i2c_hil_plan.md) to verify
actual pins, transfers, recovery and SDK errors on hardware.

The STM32 toolchain supplies Cortex-M4 architecture and floating-point ABI flags
to compiler probes before any targets exist. Caller initial flags are retained;
targets receive the same ABI. `ESY_TEST_ARM_TOOLCHAIN_ROOT` enables the regression's
real GNU Arm atomic probe without an SDK or board.

### Firmware link and output steps

For Pico/STM32F4, `ESY_FIRMWARE_TARGET` names the executable receiving firmware
settings. Create that target before including the helper. Its name need not match
the project name. The conventional
linker filenames above are bindings to consumer files; they do not define a
partition geometry. Override any complete path as needed. `ESY_APP_SLOT=A` or `B`
selects its matching script; without a slot, the default script applies. Invalid
slots or missing selected scripts fail configuration.

Pico uses SDK linker/output functions; STM32F4 uses its selected linker script and
object-copy utility to generate HEX/BIN files. ESP32 accepts an existing IDF
component target and retains native linking/output handling. These helpers create
no application sources or bootloader slot policy.

`ESY_FIRMWARE_COMPILE_DEFINITIONS` applies privately to the selected target.
SDK-wide resource limits use the SDK's own defaults and configuration inputs.
For Pico, override `PICO_MAX_SHARED_IRQ_HANDLERS` only when actual simultaneous
registrations require more storage, using an SDK configuration header supplied
before SDK initialization, so all targets compiling SDK C and assembly sources
use the same value.

### Consumer setup

The root build needs early import, the consumer's `project()` and library
`add_subdirectory()`; an explicit project import is optional. The helpers supply
Debug/language defaults and ESP32 component bindings. Use
`cmake/esy_build_config.cmake` only for product-specific overrides.

Pass `ESY_TARGET_DEVICE` when configuring, including
dependency discovery with `cmake -P`. Build and discovery commands must receive
the same selection. Project metadata files, including `.project.conf`, do not select or
override a library build target.

For example, configure the Pico 2 W example:

```sh
cmake -S . -B build -DESY_TARGET_DEVICE=PICO2_W
```

`ESY_MCU_PLATFORM_DIR` names the library checkout.

Consumers that need private environment settings load their own files before
early import, or supply SDK/toolchain values through CMake arguments and supported
environment variables.

The library supplies the shared version template; select a custom template when
the product requires a different header contract.
The application's version stays in its `project(... VERSION ...)` declaration.

Include the firmware helper after creating the target named by
`ESY_FIRMWARE_TARGET`. The consumer owns its board, feature,
linker and application files. Direct `add_subdirectory()` consumers supply their
own SDK/toolchain, provider and output setup.

### Existing or custom build systems

An existing consumer may keep its own SDK, toolchain, provider and output setup
without using the optional helpers:

```cmake
add_subdirectory(external/EsyMcuPlatform)
# After creating the consuming executable:
target_link_libraries(firmware PRIVATE esy_hal)
```

Before adding the library, that consumer supplies:

- `ESY_TARGET_DEVICE`, a supported board profile or concrete STM32F4 part.
  The library derives `ESY_TARGET_PLATFORM`; it is not another input.
- `ESY_BOARD_PATH`, whose CMake directory creates `esy_board`.
- `esy_opt_default` and `esy_opt_speed`, which may be interface targets carrying
  the consumer's optimization settings. Keep ABI options consistent across
  application and library targets.
- The SDK/toolchain and its required targets: Pico SDK targets, the consumer's
  `STM32_Drivers` target for STM32F4, or the ESP-IDF component environment.
- Feature values from its `esy_feature_import.cmake`, `ESY_RTOS` when using OS
  adapters, configuration headers and providers required by enabled modules.

Without early-helper opt-in, library assembly does not run project setup or
firmware output helpers. The library root is an integration entry point rather
than a self-contained firmware project: configuring it directly requires the
same consumer inputs and targets.

### CMake targets

Link the targets actually used so their include paths and compile requirements
propagate to the consumer. Optional targets exist only when their feature and
provider requirements are satisfied; the [feature guide](feature_gates.md)
defines their assembly rules.

| Target | Surface |
| --- | --- |
| `esy_common` | Aggregate common types, utilities, device helpers and enabled common modules. |
| `esy_common_defs` | Shared public types and errors. |
| `esy_common_utils`, `esy_common_device` | Platform-independent utility and device helpers. |
| `esy_common_ds` | Fixed-capacity data structures. |
| `esy_common_protocol_codec` | Wire-format codecs and TLV helpers. |
| `esy_event_bus` | Local OS-specific publish/subscribe routing with static per-subscriber queues and nonblocking task/ISR fanout; public `esy_os` dependency, no internal task or hardware ownership. |
| `esy_hal` | Public hardware APIs; the backend and board remain implementation dependencies. |
| `esy_hal_backend` | Private hardware adaptation target, not a middleware/application hardware API. |
| `esy_logger_core` | Frontend formatting, filtering and backend binding; common types only, no HAL, OS, data-structure or SEGGER dependency. |
| `esy_logger_backend_uart_core`, `esy_logger_backend_printf_core`, `esy_logger_backend_rtt_core` | Independently linked caller-driven engines, each carrying only its own port, buffering and provider dependencies. |
| `esy_logger_backend_uart`, `esy_logger_backend_printf`, `esy_logger_backend_rtt` | Optional FreeRTOS adapters over their corresponding engines. |
| `esy_logger` | Interface aggregate of the frontend and consumer-selected backend execution targets. |
| `esy_interrupts` / `Esy::interrupts` | Optional registered backend/OS vector objects linked to the executable; existing STM32F4 ports provide them. See [interrupt ownership](hardware_layers.md#stm32f4-interrupt-ownership) and [port registration](porting.md#hal-backend-os-and-interrupts). |
| `esy_bootloader` | OS-independent image validation, selection, trial state and confirmation. |
| `esy_dual_flash_store` | Synchronous transactional storage of one latest binary record in two caller-reserved Flash regions; no OS dependency. |
| `esy_file` | Caller-owned bound-file facade and volatile RAM provider; common types only, no filesystem/HAL/OS. Capability discovery distinguishes create, atomic replacement and removal. |
| `esy_file_flash` | Optional single-region Flash file supporting create/read/remove through public HAL; no atomic replacement and no hidden partition. |
| `esy_file_dual_flash_store` | Optional adapter exposing one initialized transactional dual Flash store as a file with atomic replacement; preserves existing two-region layout and tombstone semantics. |
| `esy_settings` | String-keyed binary configuration in caller-owned RAM with explicit snapshot commits through a borrowed atomic-replace file; no direct HAL or dual Flash store dependency. |
| `esy_diagnostics` | Caller-owned named health sources and optional synchronous self-tests; common types only, with no automatic tests, task, storage or timebase. |
| `esy_coredump_core` | Bounded retained-RAM capture, validation and explicit clearing; no HAL, Logger or OS dependency. |
| `esy_coredump_logger` | Caller-paced CD1 hexadecimal chunk export through the Logger frontend; no delivery acknowledgment and no automatic clearing. |
| `esy_coredump_file` | Synchronous first-record persistence/restore and identity-checked discard through a borrowed file; no direct Flash dependency. Capture remains in the separate retained-RAM core. |
| `esy_ota_core` | Encoding-independent image transactions over a caller-provided storage provider; common types only, no HAL, bootloader, protocol or OS dependency. |
| `esy_ota_flash` | Optional Flash/bootloader provider selected by `ENABLE_ESY_OTA_FLASH`; checked image writing and verification with separate activation. |
| `esy_remote_manager` | Caller-driven authorized management operations through normalized owner callbacks, including typed diagnostics, typed statistics and a caller-owned live RAM table; common types only, without implicit collection tasks or persistence. |
| `esy_remote_manager_proto_bin` | Optional management payload encoding selected by `ENABLE_ESY_REMOTE_MANAGER_PROTO_BIN`; no framing, sender or session ownership. |
| `esy_remote_manager_ota` | Optional adapter selected by `ENABLE_ESY_REMOTE_MANAGER_OTA`; borrows an initialized `esy_ota_core`, owner INFO callback and optional independent slot/confirm/rollback provider without selecting Flash, bootloader, protocol or OS support. |
| `esy_remote_manager_diagnostics` | Optional adapter selected by `ENABLE_ESY_REMOTE_MANAGER_DIAGNOSTICS`; borrows one initialized diagnostics instance for ENUMERATE/READ/RUN, independently of the wire codec. |
| `esy_proto_bin_contract` | Header-only borrowed ProtoBin sender and frame/event types; common codecs only, no HAL/OS/session runtime. |
| `esy_common_protocol_json` / `Esy::protocol_json` | Optional common JSON codec with a private coreJSON v3.3.1 parser provider, borrowed input views and caller-owned output/workspace. Independent of ProtoBin/TLV/Modbus codecs, HAL, OS, transport and management schemas. |
| `esy_proto_json_tcp_channel` / `Esy::proto_json_tcp_channel` | Independent caller-driven JSON frames over a borrowed connection, with four-byte big-endian document lengths, caller-owned RX/TX buffers and bounded polling. Public common JSON/network contracts only; no HAL/system, DS, OS, native stack or other protocol-family dependency. |
| `esy_proto_json_usb_channel` / `Esy::proto_json_usb_channel` | Independent caller-driven JSON frames over an initialized borrowed CDC interface, with caller-owned RX/TX buffers and observed disconnect-session isolation. Public common JSON/USB contracts only; the channel neither selects network/other protocol families nor creates an OS owner or pumps the USB stack. Native USB provider requirements remain consumer-owned. |
| `esy_proto_bin_session` | Typed reliable ProtoBin session selected by `ENABLE_ESY_PROTO_BIN`; caller-owned buffers and state, HAL tick. |
| `esy_proto_modbus_manager` | Independent Modbus transaction owner selected by `ENABLE_ESY_PROTO_MODBUS`. |
| `esy_proto_modbus_uart_channel` | Caller-driven RTU UART channel; no OS dependency. Requires the explicitly selected UART TX scheduler. |
| `esy_proto_modbus_uart_worker` | Explicit optional OS owner for one channel, with separate handle/storage and typed callbacks. |
| `esy_proto_modbus_tcp_channel` | Caller-driven master/slave over one borrowed network connection; MBAP reassembly, bounded TX staging and explicit session/EOF handling, without UART or OS dependencies. |
| `esy_proto_bin_uart_channel` | Explicit UART/DMA ProtoBin adapter; requires the selected UART TX scheduler, without an OS dependency. |
| `esy_proto_bin_usb_channel` | Explicit USB CDC ProtoBin session adapter. |
| `esy_mqtt` / `Esy::mqtt` | Caller-driven MQTT 3.1.1 client; public common types, private coreMQTT provider, caller-owned storage and injected TCP/TLS/time interfaces. |
| `esy_http_client` / `Esy::http_client` | Caller-driven HTTP/1.1 client; public common/network stream types, private llhttp v9.4.3 parser provider and caller-owned request/response storage. No endpoint, TLS provider or OS target dependency. |
| `esy_ble` / `Esy::ble` | Portable Bluetooth LE GAP, GATT server and security facade; public common types, no HAL or OS target dependency. Radio operation, callbacks and durable bonds belong to the injected software-stack provider. |
| `esy_ble_btstack` / `Esy::ble_btstack` | Optional BTstack software adapter with a private `esy_ble_btstack_provider` dependency; borrows a consumer-initialized runtime and durable bond store. |
| `esy_network` | Basic TCP/TLS byte stream plus a separate whole-write/session/peer-ACK connection extension; no HAL or OS dependency. |
| `esy_network_lwip` | Raw lwIP client/server TCP and DNS provider, with explicit caller storage and execution hooks. |
| `esy_credentials` / `Esy::credentials` | Caller-owned borrowed CA/client identity bundles; leases prevent replacement or deinit while a TLS session uses them. No storage, HAL, OS or native crypto dependency. |
| `esy_credentials_store` / `Esy::credentials_store` | Optional Credentials persistence selected by `ENABLE_ESY_CREDENTIALS_STORE`; borrows a READ/ATOMIC_REPLACE file and caller RAM for whole confirmed/candidate snapshots. Requires the Credentials and File feature gates, without adding HAL, OS or native crypto dependencies. |
| `esy_crypto` / `Esy::crypto` | Mbed TLS 3.6.x AES-256 CTR_DRBG, with explicit assessed entropy injection and public provider/header propagation. |
| `esy_tls` / `Esy::tls` | Caller-driven authenticated TLS 1.2 client over an existing stream, mandatory CA/hostname/date checking, optional mutual TLS and copied TX staging. Requires the three explicit network/credentials/crypto feature gates. |
| `esy_proto_bin_tcp_channel` | ProtoBin framing over a borrowed network connection descriptor; no endpoint lifecycle. |
| `esy_lcd_device`, `esy_touch_device` | Optional device registration and GPIO PM integration, separate from the facades. |
| `esy_freq_synth`, `esy_freq_synth_device` | Synthesizer facade and selected chip drivers; separate optional device registration and GPIO PM integration. |
| `esy_lvgl_touch` | Optional touch-to-pointer adapter; base LVGL runtime accepts a generic input callback. |

Select each protocol family independently with `ENABLE_ESY_PROTO_BIN` and
`ENABLE_ESY_PROTO_MODBUS`. Choose channels explicitly with
`ENABLE_ESY_PROTO_BIN_UART_CHANNEL`, `ENABLE_ESY_PROTO_BIN_USB_CHANNEL` or
`ENABLE_ESY_PROTO_BIN_TCP_CHANNEL`; existing hardware/provider targets do not enable
them. Pure `ENABLE_ESY_COMMON_PROTOCOL` codec consumers need no HAL or OS target.
Channel owners obtain a typed session through `get_session()` and lend only an
acquire/commit/cancel sender to OTA or other producers. No protocol aggregate or
runtime transport facade is assembled.

Protocol public headers are flat under `esy_middleware/protocol/`; internal
sources/tests are grouped by `bin`, `modbus` and `json`. All concrete protocol
targets use `esy_proto_<family>_*`. Both ProtoBin UART and Modbus UART channels
require `ENABLE_ESY_IO_SCHEDULER` and `ENABLE_ESY_UART_TX_SCHEDULER` selected by the
consumer. The schedulers live in `esy_middleware/io_scheduler` and retain their
concrete `esy_spi_scheduler` and `esy_uart_tx_scheduler` target names.
Modbus UART channels use `ENABLE_ESY_PROTO_MODBUS_UART_CHANNEL`. Only
`ENABLE_ESY_PROTO_MODBUS_UART_WORKER` additionally assembles an OS worker; a
channel never creates a task according to an implicit runtime mode.

`ENABLE_ESY_PROTO_MODBUS_TCP_CHANNEL` selects the Modbus TCP channel and requires
`ENABLE_ESY_PROTO_MODBUS` and `ENABLE_ESY_NETWORK`. No native network provider, endpoint
or listener is selected by that channel. Its consumer establishes a connection,
borrows it into one channel, polls the channel and owns ordered shutdown. Session
replacement or a terminal channel fault requires connection retirement and a
fresh session before recreating the channel.

`ENABLE_ESY_COMMON_PROTOCOL_JSON` requires `ENABLE_ESY_COMMON_PROTOCOL` and
selects `esy_common_protocol_json` / `Esy::protocol_json`. Before platform or
direct module assembly, supply an `esy_json_parser_provider` target exposing
coreJSON v3.3.1, or call `esy_configure_corejson_provider()` from
`cmake/esy_corejson_provider.cmake` with `ESY_COREJSON_PATH` pointing to an
existing checkout. The helper accepts the same environment variable when the
CMake variable is undefined and never downloads sources. The optional project
importer calls it only when JSON support is enabled. Generic JSON public headers
hide the provider types; the existing `esy_common_protocol_codec` target does
not link the JSON target or its provider. An existing provider target must
expose the compatible coreJSON v3.3.1 validation/iteration API, permit scalar
roots (`JSON_VALIDATE_COLLECTIONS_ONLY` must remain undefined), and introduce
no heap or OS dependency. Its owner declares the configured nesting limit;
the supplied helper uses the upstream default of 32 containers. Any provider
override of `JSON_MAX_DEPTH` must agree with the consumer's documented resource
budget. See the
[common protocol contract](../../esy_common/protocol_codec/README.md).

`ENABLE_ESY_PROTO_JSON_TCP_CHANNEL` selects
`esy_proto_json_tcp_channel` / `Esy::proto_json_tcp_channel` and requires
`ENABLE_ESY_COMMON_PROTOCOL`, `ENABLE_ESY_COMMON_PROTOCOL_JSON` and
`ENABLE_ESY_NETWORK`. It borrows one established `esy_network_connection_t`
through prefix send/receive and status callbacks; `send_all` is unused and
provider progress is optional. The consumer supplies fixed buffers, polls from
one non-ISR owner and controls connection establishment, shutdown and timeouts.
Four-byte unsigned big-endian lengths delimit complete JSON documents, including
scalar roots and formatted documents containing newlines. This is independent
of HTTP, WebSocket and any management request schema. Send success reports local
staging; retirement of accepted TX bytes does not prove peer application receipt.
No HAL/system, DS or OS target is needed. See the
[protocol contract](../../esy_middleware/protocol/README.md).

`ENABLE_ESY_PROTO_JSON_USB_CHANNEL` selects
`esy_proto_json_usb_channel` / `Esy::proto_json_usb_channel` and requires
`ENABLE_ESY_COMMON_PROTOCOL`, `ENABLE_ESY_COMMON_PROTOCOL_JSON`, `ENABLE_ESY_USB`
and `ENABLE_ESY_USB_CDC`, plus an already assembled `esy_usb` target. Select the
CDC backend/provider explicitly; the channel does not force a particular backend
or enable TCP/network, ProtoBin, Modbus, DS, HAL or OS targets. Initialize the
borrowed CDC with `discard_on_disconnect = true`, keep it initialized until
channel shutdown, and keep RX callbacks from consuming channel bytes. The consumer's
USB stack owner calls `esy_usb_cdc_task()` separately before polling all channels
sharing that stack; channel polling never pumps the backend. Provider OS mode
and hardware initialization still govern the stack's scheduling contract.
Disconnected instances retain their channel handles while new observed session
generations reset local partial frames, staged messages and prior errors.
The call observing a generation change returns `ESY_ERR_IO` once and performs
no new-session I/O; subsequent calls may proceed with the new peer.
The endpoint owner must observe physical unplug before reconnect, and native
bus reset must discard provider endpoint/FIFO state. See the
[protocol contract](../../esy_middleware/protocol/README.md).

`ENABLE_ESY_HTTP_CLIENT` selects `esy_http_client` and requires
`ENABLE_ESY_NETWORK`. Before platform assembly, supply an
`esy_http_parser_provider` target exposing llhttp v9.4.3 or call
`esy_configure_llhttp_provider()` from `cmake/esy_llhttp_provider.cmake` with
`ESY_LLHTTP_PATH` pointing to the pre-generated `release/v9.4.3` checkout.
The optional project importer calls that helper only while HTTP client support
is enabled. The helper does not download or generate sources. The parser stays
private; application code borrows an already connected TCP or authenticated TLS
stream and supplies the client's buffers, clock and scheduling. See the
[HTTP client contract](../../esy_middleware/http/README.md).

Additional middleware targets are documented by their module READMEs. Prefer
explicit target links over global include paths or compile definitions.

TLS and crypto consume one explicit `esy_mbedtls_provider` target. Consumers may
wrap compatible native targets or call `esy_configure_mbedtls_provider()` from
`cmake/esy_mbedtls_provider.cmake` after `project()` and before platform assembly,
with `ESY_MBEDTLS_PATH` pointing to a complete existing Mbed TLS 3.6.x release.
The optional helper requires CMake 3.21 or later, preserves normal configuration
inputs across upstream cache handling, and does not download sources or select
a product entropy source. Older CMake consumers define the provider target directly. Set
the upstream `MBEDTLS_CONFIG_FILE` before calling it to select a provider profile;
the [TLS module](../../esy_middleware/tls/README.md) includes a portable example.
Disabling these gates adds no native TLS sources or provider dependency. The
existing common SHA-256 implementation and bootloader verifier contract remain
independent of this software provider.

## Contracts

Preserve early import, the consumer's `project()`, library assembly and firmware
setup ordering. Explicit project import, when needed, follows the consumer's
`project()`. Override pre-import SDK/toolchain/component inputs before SDK
import. Platform/device and features remain consumer selections. Defaults are
activated by explicit early-helper inclusion and preserve supplied values.

## Resource Model

The helpers create interface targets, optional version output and selected
SDK/provider targets during configuration. They allocate no firmware runtime
resources. Consumers retain their files and third-party sources throughout the
build tree's lifetime.

## Verification

From the platform repository root:

```sh
python3 -B tools/checks/test_cmake_integration.py
```

To include real Pico SDK, GNU Arm compiler and STM32 CMSIS/startup checks:

```sh
ESY_TEST_PICO_SDK_PATH=/path/to/pico-sdk \
ESY_TEST_PICO_TOOLCHAIN_PATH=/path/to/arm-toolchain \
ESY_TEST_ARM_TOOLCHAIN_ROOT=/path/to/arm-toolchain \
ESY_TEST_STM32_SUPPORT_ROOT=/path/to/stm32-support \
python3 -B tools/checks/test_cmake_integration.py
```

The regression configures and builds temporary consumers, including no override
file, explicit empty/CLI overrides, nonstandard roots, provider ownership, helper
ordering, ESP-IDF component conventions and firmware slots. Generated default
version headers compile as C11 and C++17, including omitted and zero-padded
components; custom template precedence, output paths and disabling generation are
checked.
Network fixtures cross the library assembly boundary, including explicit
providers defined after the helper and separate consumers that did not opt in.
STM32 checks cover explicit CMSIS mappings, rejected unknown/ambiguous model
inputs and, when the vendor checkout is supplied, the actual device headers and
startup filenames. Logger tests retain the cached RTT child gate while disabling
its parent and verify that unrelated RTT providers are preserved.
Host SDK fixtures establish build-interface behavior. Real embedded builds
establish SDK/toolchain integration. Board procedures cover hardware
initialization and runtime behavior.

## Known Limits

A build adapter does not establish support for every MCU, peripheral, FreeRTOS
port or SDK release. Product board code, linker geometry and required provider
configuration remain necessary. Changing toolchains in an existing build cache is
unsupported. Host fixtures do not establish memory placement, IRQ/DMA behavior or
runtime timing.
