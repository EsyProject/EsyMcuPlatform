<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Hello World: Static Tasks and RTT Logger

Start with the maintained
[`smp_hello` example](https://github.com/EsyProject/EsyMcuPlatformDemo/tree/main/demos/smp_hello)
in [EsyMcuPlatformDemo](https://github.com/EsyProject/EsyMcuPlatformDemo).
It includes application source, board adapters, explicit CMake/feature files,
FreeRTOS configuration and caller-owned task/logger storage. No project
generation step is required.

The same application runs on both supported demo boards:

| Device | Scheduler configuration | Expected actual core IDs |
| --- | --- | --- |
| Pico 2, RP2350 Arm | Native FreeRTOS SMP, two cores with affinity | `hello0`: 0; `hello1`: 1 |
| STM32F407ZGT6, 1 MiB Flash | Single-core FreeRTOS | Both tasks: 0 |

STM32F407 is a single-core MCU. Its build demonstrates reuse of the task, HAL
and logger APIs; only the Pico 2 build demonstrates SMP.

## Prerequisites

Both targets require Git, CMake **3.24 or later**, Ninja, an Arm GCC toolchain,
and an **RTT-capable SWD debug probe and reader**. RTT uses the debug connection;
USB power or UF2 loading does not create a serial log port.

Pico 2 additionally requires Pico SDK **2.1.1** with its dependencies and matching
picotool **2.1.1**. STM32 uses the pinned CMSIS/HAL providers and internal 16 MHz
HSI oscillator. Its example does not require a Pico SDK installation, external
crystal or particular LED wiring. The example README describes probe connections
and host-tool requirements.

## Get the example

```sh
git clone https://github.com/EsyProject/EsyMcuPlatformDemo.git
cd EsyMcuPlatformDemo
git submodule update --init external/EsyMcuPlatform external/FreeRTOS-Kernel
```

The submodule pins select compatible platform and kernel revisions. Application
code uses public `esy_hal`, `esy_os` and logger APIs; the native SDK and kernel
interfaces stay behind the board/provider boundary.

## Build for Pico 2

Replace the two installation paths, then configure and build:

```sh
cmake -S demos/smp_hello -B build/hello-pico2 -G Ninja \
  -DESY_TARGET_DEVICE=PICO2 -DCMAKE_BUILD_TYPE=Release \
  -DPICO_SDK_PATH=/path/to/pico-sdk \
  -DPICO_TOOLCHAIN_PATH=/path/to/arm-toolchain
cmake --build build/hello-pico2
```

The Pico port reuses matching host-tool packages from a recognized SDK installer
layout while retaining the SDK's standard search and version checks. An ordinary
SDK checkout may still download/build picotool during configuration. For an
explicit offline installation, install picotool 2.1.1 and append
`-Dpicotool_DIR=/path/to/picotool/cmake-package`; that directory must contain
`picotoolConfig.cmake`.

Hold BOOTSEL while connecting USB and copy
`build/hello-pico2/smp_hello.uf2` to the mounted drive. For the RTT reader's
symbols, use `build/hello-pico2/smp_hello.elf`.

## Build for STM32F407ZGT6

Initialize the STM32 and logger providers, then use a separate build directory:

```sh
git submodule update --init external/CMSIS_5 external/cmsis-device-f4 \
  external/stm32f4xx-hal-driver external/SEGGER_RTT external/printf
cmake -S demos/smp_hello -B build/hello-stm32 -G Ninja \
  -DESY_TARGET_DEVICE=STM32F407ZGT6 -DCMAKE_BUILD_TYPE=Release \
  -DESY_TOOLCHAIN_ROOT=/path/to/arm-toolchain
cmake --build build/hello-stm32
```

Load `build/hello-stm32/smp_hello.elf` through SWD using the example's
[complete build/load instructions](https://github.com/EsyProject/EsyMcuPlatformDemo/blob/main/demos/smp_hello/README.md#stm32f407zgt6-build-and-load).
This is a standalone image at `0x08000000`; it replaces a bootloader at that
address. The separate boot/update examples require their own provisioning.

The STM32 build explicitly selects a static buffer formatter for logger output;
the Pico build uses its SDK formatter. This keeps the selected hello builds
free of retained libc/RTOS allocator functions without assuming that every
possible provider has the same memory behavior.

## Observe the tasks

Leave the board running and open RTT up-channel **0**. Use automatic control-block
discovery, or locate `_SEGGER_RTT` in the matching ELF. Reset with the reader
connected if you need the startup message.

Expected Pico 2 output, illustrated rather than captured from hardware:

```text
[boot][I] Two static tasks; scheduler cores=2
[hello0][I] Hello from core 0, sequence 0
[hello1][I] Hello from core 1, sequence 0
```

On STM32F407, the scheduler count is `1`, and both workers report core `0`.
Each task increments its sequence counter once per logging attempt. They have
a 500 ms initial offset, but logger contention and RTT buffer exhaustion can
drop lines; exact alternation and delivery are not guaranteed.

The complete source makes the startup order visible: HAL, RTT backend/logger,
OS initialization, static task creation, affinity, then scheduler start. Each
worker owns a 4096-byte stack and static task control storage; the application
owns the shared logger and its 256-byte formatting buffer. FreeRTOS dynamic
allocation is disabled.

Native cross-builds and host checks are recorded in the demo's
[verification record](https://github.com/EsyProject/EsyMcuPlatformDemo/blob/main/VALIDATION.md).
For board observations, capture RTT output and inspect actual core IDs,
advancing sequences and stack margins under the intended workload.

## Continue with firmware updates

The companion [bootloader](https://github.com/EsyProject/EsyMcuPlatformDemo/tree/main/demos/bootloader)
and [OTA update](https://github.com/EsyProject/EsyMcuPlatformDemo/tree/main/demos/ota_update)
examples support both boards. They demonstrate signature verification, inactive
slot writing, Flash readback, trial boot and application confirmation over wired
UART. The updater uses one scheduled core on both targets; Pico 2 keeps core 1
parked for Flash operations. The bootloader runs before scheduling starts.
Follow their full provisioning instructions and sample-key limits rather than
adding an updater to the hello firmware unchanged.

For your own firmware build, see the
[build integration contract](build_integration.md#existing-or-custom-build-systems),
[OS integration requirements](../../esy_middleware/os/README.md#integration) and
[memory model](memory_model.md).
