<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Adding Device and Platform Support

EsyMcuPlatform keeps platform facts in the selected port, hardware implementations
in the HAL backend and native RTOS bridges in the OS port. Shared CMake files
control build ordering, feature validation and reusable software providers.
Adding a platform follows these contracts without adding platform-name branches
to the shared build entry points.

Paths in this guide are relative to the library root. A consuming firmware or
independent example owns its board, feature selection, SDK installation, native
RTOS provider and final executable. Porting the library does not require a
particular product repository or its private scripts.

## Choose the Scope

For a new part in an existing family, first check
`cmake/ports/<port>/device_map.cmake`. An accepted part may reuse the existing build
port and backend. Extend that descriptor only when the actual part is missing.
Verify the selected SDK's device define and startup file, peripheral instances,
DMA/IRQ routes, SRAM and Flash geometry against the part being supported.

Device recognition and peripheral support are separate. For example, the
STM32F4 descriptor recognizes more devices than the audited Flash, fault and
CAN implementations support. Expand each implementation and its tests before
changing its accepted device set. Keep unsupported configurations rejected.

A new hardware family adds its three build files under
`cmake/ports/<port>/`, its backend under `esy_hal_backend/src/<port>/`, and any
native RTOS bridge under `esy_middleware/os/src/freertos/port/<port>/`.
The build-port directory key is a lowercase identifier such as `stm32f4`.
Device names, platform labels, board paths and specs paths remain distinct:
Pico profiles share the `pico` port while selecting different board paths.

## Device Descriptor Contract

The shared `cmake/esy_target_device.cmake` normalizes the consumer's
`ESY_TARGET_DEVICE`, discovers and sorts `ports/*/device_map.cmake`, and requires
exactly one matching port. Zero matches fail as unsupported; multiple matches
fail as ambiguous. Add a descriptor directory and run CMake configuration again
to discover a new port. Use a new build directory when changing architecture or
toolchain.

Every descriptor declares `ESY_PORT_EXPORT_FIELDS`, including an empty list
when it has no extra outputs, and defines these namespaced functions:

```cmake
set(ESY_PORT_EXPORT_FIELDS "")

function(esy_port_acme_match device output)
    if(device STREQUAL "ACME_M4")
        set(${output} TRUE PARENT_SCOPE)
    else()
        set(${output} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(esy_port_acme_resolve device result_prefix)
    set(${result_prefix}_ESY_TARGET_DEVICE "${device}" PARENT_SCOPE)
    set(${result_prefix}_ESY_TARGET_PLATFORM ACME PARENT_SCOPE)
    set(${result_prefix}_ESY_BUILD_PORT acme PARENT_SCOPE)
    set(${result_prefix}_ESY_BOARD_PLATFORM_PATH acme_board PARENT_SCOPE)
    set(${result_prefix}_ESY_SPECS_PLATFORM_PATH acme_specs PARENT_SCOPE)
endfunction()
```

This fictional `acme/device_map.cmake` illustrates only the descriptor contract;
its SDK and hardware implementations must be supplied separately. The function
name uses the descriptor's directory key. The matcher receives the normalized
uppercase device and must return an explicit boolean. The resolver fills every
required result through `PARENT_SCOPE`, preserving that device and returning its
own directory key as `ESY_BUILD_PORT`.

Extra output names use the owning port's `ESY_<UPPERCASE_PORT>_` namespace and
are explicitly listed in `ESY_PORT_EXPORT_FIELDS`. Values must be scalars without
newlines or semicolons. The shared resolver exports every declared field and
clears extras owned by unselected ports. It recomputes derived fields and removes
their stale cache entries. Consumer build overrides may set board/specs locations,
SDK roots and toolchain inputs, but must not change or cache any derived device metadata
or its export registry.

Descriptors only declare metadata and pure matching/parsing functions. They
must run before compiler detection and in `cmake -P` mode without an SDK,
toolchain, target creation, network access or parent-project metadata.
Consumer feature and OS choices remain outside this descriptor.

Test the same resolver used by build and shell tooling:

```sh
cmake -DESY_TARGET_DEVICE=STM32F407ZG \
  -DESY_TARGET_MAP_FILE=/tmp/esy-target-map.txt \
  -P cmake/esy_target_device.cmake
```

## Build Entries and Hooks

Every port has exactly three build files:

```text
cmake/ports/<port>/
  device_map.cmake
  pre_project.cmake
  post_project.cmake
```

| File | Put new platform behavior here |
| --- | --- |
| `device_map.cmake` | Device matching, derived metadata and pure CPU/ABI data shared by build stages. It runs without an SDK or compiler, including in script mode. |
| `pre_project.cmake` | Work required before the consumer's `project()`: SDK import, toolchain selection and compiler-detection settings. Preserve an explicitly supplied `CMAKE_TOOLCHAIN_FILE`. |
| `post_project.cmake` | Namespaced hooks for SDK initialization, providers, target options and firmware linking/output. Including the file only declares functions/macros; callers run the hooks when their inputs and targets exist. |

Start with the device descriptor, put compiler/SDK preparation in the pre-project
file, and put target work in hooks in the post-project file. Keep private helper
functions beside their callers in these files. Additional platform behavior does
not require another build-entry file or a shared dispatcher branch.

When an SDK provides its own toolchain, the pre-project file selects/imports it.
For STM32F4, the pre-project file also serves as the default
`CMAKE_TOOLCHAIN_FILE`. CMake loads that same file during `project()` and compiler
probes. Keep its toolchain work self-contained and safe to repeat without
consumer targets or assumptions about source/build directory layout. An explicit
consumer toolchain takes precedence and is not loaded by early import.

STM32F4 keeps shared CPU/ABI data in its device descriptor so target options and
compiler probes use the same values. Pure helpers must work without consumer
device selection when their data is independent of a particular part.

These files support the optional library integration helpers. A direct consumer
can still supply its SDK, targets and final linking itself.

`cmake/esy_port.cmake` supplies `esy_get_port_file(entry output)`, returning the
selected port's absolute entry filename or an empty result for an absent entry.
Missing required entries are configuration errors. The
`esy_call_port_hook(hook ...)` macro loads the selected `post_project.cmake`
and calls the optional `esy_port_<port>_<hook>` function or macro. Implement only
hooks that have actual behavior; do not add a registry, feature source or
top-level setup work to the hook file.

The usual target-assembly hooks are listed below. Implement only the ones that
the new port needs; there is no requirement to implement the full hook list.

| Hook suffix | Invocation and responsibility |
| --- | --- |
| `configure_project_options` | After final consumer features and creation of `esy_project_options`; add platform-specific target settings. |
| `initialize_sdk_dependencies` | After shared project options are assembled; initialize the SDK and select/import the native RTOS and platform dependencies. |
| `configure_firmware(target)` | When the firmware helper runs after target creation; validate the executable/component and apply linker scripts, slot rules and output formats. |

Use the following hooks only when the port needs their specific behavior:

| Hook suffix | Invocation and responsibility |
| --- | --- |
| `configure_build_defaults` | After consumer build overrides and before SDK import; fill only unspecified platform-specific defaults. Must remain usable in script mode. |
| `configure_network_providers` | Deferred until library assembly; supply missing defaults after the consumer can provide network targets before or after project import. |
| `export_sdk_variables` | During automatic project integration inside the library directory; export SDK state to the consumer's scope. Explicit project import already executes there. |
| `configure_runtime_targets` | After common and board targets exist, before backend/HAL assembly; create prerequisites such as an independent native context bridge. |
| `select_atomic_backend` | Only when the shared native 32-bit always-lock-free probe fails; validate and select a real runtime fallback. |
| `configure_coredump_target(target)` | After the coredump core target and common options exist; add private architecture options without changing other targets. |

Use functions for target-only work. Use macros when a hook must update caller
variables or export them with `PARENT_SCOPE`: the dispatcher macro introduces no
function scope. An extra function around SDK initialization or variable export
would absorb outputs or redirect their destination. In a function, use
`CMAKE_CURRENT_FUNCTION_LIST_DIR` for files beside its definition; a macro must
not assume `CMAKE_CURRENT_LIST_DIR` names its definition directory.

The pre-project entry is included directly so native SDK scope and `return()`
behavior remain intact. A `return()` inside a hook macro
can exit its caller's file or function; use guarded bodies instead of early
returns in those macros. Loading `post_project.cmake` must remain safe at any
hook call site, including before `project()` and in script mode.
Its name identifies where SDK and target implementation lives; the pure
`configure_build_defaults` hook can be called earlier. For example, STM32F4 fills
its conventional support-root path there, including during dependency discovery.

Shared software assembly for JSON, HTTP, MQTT, LVGL and RTT stays reusable.
The dependency-initialization hook supplies platform additions; common assembly
consumes `esy_port_lvgl_libraries` and `esy_port_segger_rtt_root` only when it
creates the corresponding provider. Those inputs are reset before each
dependency-initialization hook invocation.

Explicit consumer targets and documented source/configuration inputs retain
their precedence. Preserve explicit empty inputs, repeated-import behavior and
feature-off dependency isolation. A port must not create a second feature source
using cache options or compile definitions. SDK, toolchain and provider setup
must preserve the direct-consumer path, which can supply its own integration.

The SDK-dependency hook can use `esy_select_existing_freertos_provider()`
to retain an already supplied kernel target and `esy_require_freertos_sources()`
before importing a checkout. A native SDK such as ESP-IDF can instead select
its own component target. Use the actual CPU's kernel port and validate the
consumer's `FreeRTOSConfig.h`; the common wrapper is not a kernel port.

The `configure_firmware(target)` hook owns platform-specific executable/component
checks, linker scripts, slot rules and output formats. ESP-IDF component
integration delegates the final link to its SDK. Preserve those differences when
adding a new port.

## HAL Backend, OS and Interrupts

The shared backend entry selects `esy_hal_backend/src/${ESY_BUILD_PORT}`.
Its `CMakeLists.txt` selects supported `esy_hb_*.c` sources by the consumer's
features, links required SDK targets and rejects unsupported feature/device
combinations. Implement the existing backend headers and retain their ownership,
error, capability and ISR contracts. Missing support must not report success.
The public HAL and hardware-independent middleware normally remain reusable.

Pin routing, clocks chosen for a board, DMA bindings and interrupt priorities
belong to the board. See the board hooks in
`esy_common/defs/include/esy_common/defs/board/esy_board_defs.h` and
[hardware layers](hardware_layers.md). Hardware SDK headers remain private to
hardware adapters.

The FreeRTOS wrapper adds its selected
`src/freertos/port/${ESY_BUILD_PORT}/CMakeLists.txt` when present. That directory
owns native context/tick glue. A prerequisite needed by the backend must also be
available from `configure_runtime_targets`, with idempotent target creation.
Ensure a single tick/vector owner and verify task/ISR critical sections, static
TCB/storage compatibility, affinity and supported tickless behavior.

For strong vector entries, create an OBJECT library and register it with the
owning backend or OS target:

```cmake
add_library(acme_vectors OBJECT acme_irq.c)
target_link_libraries(acme_vectors PRIVATE esy_hal_backend esy_common_defs)
esy_register_interrupt_target(esy_hal_backend acme_vectors)
```

Registration validates OBJECT targets and deduplicates the owner's
`ESY_INTERRUPT_OBJECT_TARGETS` property. The library root aggregates backend and
OS registrations into `esy_interrupts` / `Esy::interrupts`. Link it to the final
executable, or use the firmware helper. Objects must reach the executable so
startup weak handlers cannot prevent their inclusion. Custom consumers can own
their vector entries explicitly.

Keep the shared atomic compiler probe. A failed probe requires a verified
implementation and SDK linkage, including ISR and multicore behavior; changing
a capability macro does not supply that implementation. See
[atomic backends](atomic_backends.md). Flash geometry belongs to the backend;
partitions and application slots belong to the board/product. See
[Flash geometry](flash_geometry.md).

## Verification and Delivery

Prepare library host-test dependencies and run focused build tests:

```sh
./setup.sh
python3 -B tools/checks/test_cmake_integration.py
python3 -B tools/checks/test_port_stages.py
tools/quality/ci_quality_gate.sh test:all
```

The port regression builds a fictional port with the same three build files,
backend and vectors, without editing shared
platform dispatchers. It covers automatic and explicit integration, direct
consumers, duplicate vector registration, weak-handler replacement and removal
of disabled vectors on reconfiguration.

Add regression cases for new device matches, invalid/ambiguous matches, complete
outputs, stale cache clearing, script-mode equivalence, provider precedence,
disabled features and repeated integration. Extend source/contract checks when
the new hardware contract requires it.

Supply an independent minimal firmware or example with its board hooks, startup
and linker inputs, features, real SDK/toolchain and native RTOS when enabled.
Build the affected baremetal/RTOS configurations with the actual target compiler.
Inspect linked vectors and required RAM sections. Record SDK/kernel versions,
device, enabled capabilities, commands and results.

Board smoke/HIL procedures cover timing, IRQ/DMA, Flash execution/cache safety,
watchdog, multicore and low-power recovery for the enabled capabilities. Host
fixtures cover contracts and build assembly. Keep the support matrix and
integration documentation aligned with the port's implemented capabilities.
