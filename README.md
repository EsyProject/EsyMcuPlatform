<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# EsyMcuPlatform

An EsyProject library. **Embedded software for you.**

**Build portable FreeRTOS firmware with familiar tools and explicit resource ownership.**

EsyMcuPlatform is a reusable C11 library for teams working with C, CMake, and
FreeRTOS. Share hardware APIs, OS wrappers, logging, protocols, boot/update,
and GUI integration across supported MCUs. Your application selects the
modules, supplies their storage, and controls startup.

[Try the examples](#try-the-examples) · [Quick start](#quick-start) · [Hardware support](#hardware-support) · [Explore the modules](#explore-the-modules)

**Apache-2.0 licensed.** See [License](#license) for dependency attribution.

## Why EsyMcuPlatform?

- **One HAL across supported MCUs.** Use consistent public hardware APIs in
  application and middleware code. Ports contain the vendor SDK details,
  reducing platform-specific changes when moving between supported targets.
- **No heap for library-owned resources.** Module state, buffers, and OS objects
  use static or caller-owned storage, including task stacks and control blocks.
  SDK/provider allocations and fixed-pool behavior have explicit boundaries in
  the [memory model](#memory-model).
- **Use native FreeRTOS SMP.** Access supported native ports through one OS
  interface, including core affinity and cross-core yield where available.
  Single-core projects use the same public OS surface; check
  [chip-level support](#smp-on-specific-mcus) for dual-core scheduling.
- **OTA with image verification and boot confirmation.** Write updates to an
  inactive image slot, verify the image read back from Flash with SHA-256, and
  use trial boot with application-controlled confirmation. Reuse the
  [OTA](esy_middleware/ota/README.md) and
  [bootloader](esy_middleware/bootloader/README.md) modules across supported targets.
- **Build on familiar tools.** Use C APIs and standard CMake targets. Teams
  familiar with C, CMake, and FreeRTOS can reuse that experience when adopting
  the library.
- **Choose modules and keep integration explicit.** Select features and link
  the targets you need. Your application owns startup and resource lifetimes;
  optional sources and providers stay out of builds that do not use them.
- **Reuse firmware infrastructure.** Build on logging, diagnostics, protocols,
  bootloader, OTA, watchdog, and GUI modules, sharing their integration across
  products instead of assembling each capability from scratch.
- **Test behavior on a host.** Reusable HAL/OS fakes and module-owned Ceedling
  tests support early verification. Target smoke/HIL procedures cover native
  timing and peripheral behavior.

## Try the Examples

[EsyMcuPlatformDemo](https://github.com/EsyProject/EsyMcuPlatformDemo) contains
three independently buildable examples. Each supports **Pico 2 (Arm)** and
**STM32F407ZGT6**, using the same application source with device-specific
configuration and board adapters.

| Example | Pico 2 | STM32F407ZGT6 |
| --- | --- | --- |
| [smp_hello](https://github.com/EsyProject/EsyMcuPlatformDemo/tree/main/demos/smp_hello) | Two static tasks pinned to separate cores; RTT logs actual core IDs. | The same task and logger APIs on one core; both tasks report core 0. |
| [bootloader](https://github.com/EsyProject/EsyMcuPlatformDemo/tree/main/demos/bootloader) | SHA-256 and ECDSA P-256 verification, dual-slot selection and trial boot. | The same boot lifecycle using internal Flash. |
| [ota_update](https://github.com/EsyProject/EsyMcuPlatformDemo/tree/main/demos/ota_update) | Receive a signed image over UART, verify Flash readback, reboot and confirm startup. | The same update flow through the public HAL and middleware APIs. |

**Start with `smp_hello`, then run `bootloader` and `ota_update` together.**
The examples include board configuration, static resources, CMake files,
build/flash instructions and expected output. No project generation step is
required. Select the device with `ESY_TARGET_DEVICE` and follow its documented
SDK/toolchain prerequisites.

STM32F407 is single-core. The OTA application uses one scheduled core on both
boards; Pico 2 keeps core 1 parked during Flash writes. The transport is wired
UART. The included signing key is public demonstration material.
See the demo [verification record](https://github.com/EsyProject/EsyMcuPlatformDemo/blob/main/VALIDATION.md)
for recorded cross-build and host-check results.

## Hardware Support

| `ESY_TARGET_DEVICE` | Hardware | Scope |
| --- | --- | --- |
| `PICO`, `PICO_W` | Pico / Pico W, RP2040 | Pico SDK backend; onboard Wi-Fi on W targets. |
| `PICO2`, `PICO2_W` | Pico 2 / Pico 2 W, RP2350 Arm | Pico SDK backend using `rp2350-arm-s`; onboard Wi-Fi on W targets. |
| `STM32F407ZG`, other recognized STM32F4 models | Concrete chip model | Per-feature coverage; Flash layouts cover STM32F405/407/415/417 with 512 KiB or 1 MiB single-bank Flash. |
| `ESP32` | Classic ESP32 | Partial backend; check peripheral coverage before selecting it. Other ESP32-series targets are outside the selection contract. |

Check the [backend capability matrix](esy_hal_backend/README.md#platform-capability-matrix)
and [Flash/boot/OTA limits](docs/integration/flash_geometry.md) for your target.
Choose the board profile or concrete chip through `ESY_TARGET_DEVICE`; the
platform is derived internally. Per-feature support follows the backend
capability matrix and the selected board resources.

## Hello World: Static Tasks and Logger

The [`smp_hello` task](https://github.com/EsyProject/EsyMcuPlatformDemo/blob/main/demos/smp_hello/src/main.c)
logs its actual core ID through the shared RTT logger. Pico 2 pins the two
workers to separate cores; STM32F407 runs both on core 0. The task loop is the
same on both targets:

```c
static void worker_main(void *arg) {
  const demo_worker_config_t *config = arg;
  uint32_t sequence = 0u;
  if (config == NULL) {
    for (;;) esy_os_task_delay_ms(DEMO_LOG_PERIOD_MS);
  }
  if (config->start_delay_ms != 0u)
    esy_os_task_delay_ms(config->start_delay_ms);
  for (;;) {
    ESY_LOG_I(config->name, "Hello from core %u, sequence %lu",
              (unsigned)esy_hal_system_get_core_id(), (unsigned long)sequence++);
    esy_os_task_delay_ms(DEMO_LOG_PERIOD_MS);
  }
}
```

Expected Pico 2 output on RTT channel 0, illustrated rather than captured:

```text
[boot][I] Two static tasks; scheduler cores=2
[hello0][I] Hello from core 0, sequence 0
[hello1][I] Hello from core 1, sequence 0
```

The complete example supplies the types and configuration omitted here, static
task stacks/control storage, logger initialization, task creation, affinity and
scheduler startup. Each worker attempts one greeting per second, with a 500 ms
startup offset; logger and RTT buffer contention can drop lines. RTT requires
an SWD debug probe and reader; the USB power/UF2 connection is not a log port.

## Quick Start

For the Pico 2 SMP example, install an Arm toolchain, CMake 3.24 or later,
Ninja, Pico SDK **2.1.1** with its dependencies, and matching picotool. Replace
the SDK/toolchain paths below with your installation directories:

```sh
git clone https://github.com/EsyProject/EsyMcuPlatformDemo.git
cd EsyMcuPlatformDemo
git submodule update --init external/EsyMcuPlatform external/FreeRTOS-Kernel

cmake -S demos/smp_hello -B build/hello-pico2 -G Ninja \
  -DESY_TARGET_DEVICE=PICO2 -DCMAKE_BUILD_TYPE=Release \
  -DPICO_SDK_PATH=/path/to/pico-sdk \
  -DPICO_TOOLCHAIN_PATH=/path/to/arm-toolchain
cmake --build build/hello-pico2
```

Hold BOOTSEL while connecting USB, copy `build/hello-pico2/smp_hello.uf2` to the
mounted drive, then read RTT channel **0** through the SWD probe.
For STM32F407ZGT6, follow the same example's
[STM32 build and load instructions](https://github.com/EsyProject/EsyMcuPlatformDemo/blob/main/demos/smp_hello/README.md#stm32f407zgt6-build-and-load).
The [Hello World guide](docs/integration/hello_world.md) covers prerequisites,
both device choices and what to observe.

### Add to an existing project

Use the library as a Git submodule:

```sh
git submodule add https://github.com/EsyProject/EsyMcuPlatform.git external/EsyMcuPlatform
```

With your board, features, SDK, and FreeRTOS provider configured, link the
selected modules through CMake:

```cmake
add_subdirectory(external/EsyMcuPlatform)
target_link_libraries(firmware PRIVATE esy_hal esy_os esy_logger)
```

See [build integration](docs/integration/build_integration.md) for the complete
inputs and optional helpers, or the
[direct integration contract](docs/integration/build_integration.md#existing-or-custom-build-systems)
for a custom build. Application hardware and RTOS calls use `esy_hal` and
`esy_os`; existing direct SDK calls need adaptation to those interfaces.

## Explore the Modules

| Need | Start here |
| --- | --- |
| Hardware access | [Public HAL](esy_hal/README.md) |
| CAN frames, acceptance filters and bus-off recovery | [CAN HAL](esy_hal/docs/can.md), with a caller-driven STM32F4 Classical CAN backend |
| Tasks, queues, synchronization, and SMP integration | [OS wrappers](esy_middleware/os/README.md) |
| Local one-to-many event delivery between tasks | [Event bus](esy_middleware/event_bus/README.md), with independent static subscriber queues and task/ISR publishing |
| Rings, pools, and fixed-capacity containers | [Data structures](esy_common/ds/README.md) |
| Flat and hierarchical entry/run/exit state machines | [State machine framework](esy_common/smf/README.md), driven by an existing task or baremetal main loop |
| Framing, transports, and protocol routing | [Wire codecs](esy_common/protocol_codec/README.md), [protocol middleware](esy_middleware/protocol/README.md) |
| Firmware activation and update flow | [Bootloader](esy_middleware/bootloader/README.md), [OTA](esy_middleware/ota/README.md) |
| Transactional binary-record persistence | [Dual Flash store](esy_middleware/dual_flash_store/README.md) |
| Bound file access over RAM, single-region Flash or transactional records | [File](esy_middleware/file/README.md), with explicit create/atomic-replace/remove capabilities |
| Volume, streaming file and directory access on Flash or sector media | [Filesystem](esy_middleware/fs/README.md), with explicit littlefs/FatFs providers and bound-file adaptation |
| String-keyed configuration over a replaceable file backend | [Settings](esy_middleware/settings/README.md) |
| Remote configuration, image upload/slot enumeration/confirmation/rollback, RAM statistics, structured diagnostics and fault-record collection | [Remote manager](esy_middleware/remote_manager/README.md), with normalized owner operations, a caller-owned statistics table, optional provider adapters and ProtoBin payload conversion |
| Sensor channels, units, sampling and events | [Sensors](esy_middleware/sensor/README.md) |
| Named health queries and explicit self-tests | [Diagnostics](esy_middleware/diagnostics/README.md), with caller-owned sources and Remote Manager typed results |
| Logging and crash capture | [Logger](esy_middleware/logger/README.md), [coredump](esy_middleware/coredump/README.md) |
| GUI, physical input and display transport | [LVGL](esy_middleware/lvgl/README.md), [input](esy_middleware/input/README.md), [LCD](esy_middleware/lcd/README.md), [touch](esy_middleware/touch/README.md), [I/O schedulers](esy_middleware/io_scheduler/README.md) |
| USB and network connectivity | [USB CDC](esy_middleware/usb/README.md), [Wi-Fi manager](esy_middleware/wifi/README.md), [network streams](esy_middleware/network/README.md) |
| Bluetooth LE | [Portable GAP, GATT server and security facade](esy_middleware/ble/README.md), with an optional BTstack adapter over a consumer-owned runtime and persistent bond store |
| MQTT broker messaging | [MQTT client](esy_middleware/mqtt/README.md), using a consumer-provided coreMQTT v2.3.1 provider, shared network streams, including optional lwIP TCP or injected TLS providers |
| HTTP downloads and REST requests | [HTTP/1.1 client](esy_middleware/http/README.md), with incremental llhttp v9.4.3 response parsing, caller-owned buffers and borrowed TCP/TLS streams |
| JSON data and framed TCP/USB messages | [Common JSON codec](esy_common/protocol_codec/README.md#optional-json), [JSON TCP channel](esy_middleware/protocol/README.md#json-tcp-channel) and [JSON USB channel](esy_middleware/protocol/README.md#json-usb-channel), with exact 64-bit integers, caller-owned buffers and independent feature gates |
| Authenticated TLS and cryptographic randomness | [TLS 1.2 client](esy_middleware/tls/README.md), [CTR_DRBG](esy_middleware/crypto/README.md), with an explicit consumer-provided Mbed TLS 3.6.x library and qualified entropy source |
| CA and client identity lifecycle | [Credential bundles](esy_middleware/credentials/README.md), with pinned certificate/key snapshots and optional whole-bundle persistence, candidate staging and confirmed-version recovery |
| Supervision and coordinated Flash access | [Watchdog monitor](esy_middleware/watchdog_monitor/README.md), [core lockout](esy_middleware/core_lockout/README.md) |
| Fixed-memory numerical helpers | [Numerics utilities](esy_middleware/numerics/README.md) |
| UTC calendars, RTC alarms and network time | [RTC facade](esy_hal/rtc.md), [UTC and SNTP](esy_middleware/time/README.md) |

Module READMEs describe public APIs, resource ownership, concurrency, and limits.
Boot/update components provide building blocks; the consumer supplies image
policy, verification/key providers where required, and board integration.

Image handoff uses `esy_hal_boot_jump_to_app()` under the boot feature. System
time, IRQ and reset operations do not acquire image-update dependencies.
LCD and touch modules own their complete handles; common headers provide shared
value types. Frequency synthesizers use shared immutable operation tables and
private lifecycle state, with public operations limited to implemented tuning,
output control and diagnostics.

Logger frontend and output engines have separate targets, so selecting an
engine does not add its port/provider dependencies to frontend-only consumers.
Synthesizer device registration and GPIO PM similarly use a separate optional
target; ordinary tuning and chip-driver consumers can link the synthesizer
without that adapter.

The shared network stream contains only byte send/receive, optional progress
and a borrowed context. ProtoBin uses a separate connection descriptor for
whole writes, session identity and peer-ACK observations; connection-establishment
states belong to the endpoint. Network streams and lwIP endpoints remain software
adapters. Native Wi-Fi
driver/stack progress and access guards come from the public Wi-Fi HAL; the
startup owner connects that runtime descriptor to the existing lwIP owner hooks.
SDK calls stay in the hardware backend, sharing the station provider lifetime.

Protocol consumers select ProtoBin sessions and Modbus independently through
`ENABLE_ESY_PROTO_BIN` and `ENABLE_ESY_PROTO_MODBUS`. ProtoBin UART, USB and TCP channels
have explicit feature gates. The session owns framing, acknowledgments and
retry state; a borrowed sender exposes buffer acquisition, submission and
cancellation without polling or backend storage. Remote management uses the
same sender for versioned configuration, image transactions, commands and fault
records. Typed OTA core operations remain independent of protocol encoding;
Flash installation is an explicitly selected provider.

Modbus TCP master/client and slave/server channels are selected separately by
`ENABLE_ESY_PROTO_MODBUS_TCP_CHANNEL`. They borrow an established network
connection, reconstruct MBAP frames and reuse the Modbus transaction manager.
Channels are caller-driven and do not create tasks, sockets or listeners. See
the [protocol contracts](esy_middleware/protocol/README.md#modbus-tcp-channel).

`ENABLE_ESY_PROTO_JSON_TCP_CHANNEL` independently selects length-prefixed JSON
over an established network connection. It reuses the optional common JSON
codec/coreJSON v3.3.1 provider, supports fragmented input and partial output and
creates no task or endpoint. Its four-byte big-endian framing and owner
contracts are documented in the [JSON channel contract](esy_middleware/protocol/README.md#json-tcp-framing-and-execution).

`ENABLE_ESY_PROTO_JSON_USB_CHANNEL` selects the same wire format over an exclusive
session-isolated CDC instance, with bounded partial transfers and DTR/session
recovery. The existing USB owner pumps the stack separately; the channel creates
no worker and does not select a TCP/network provider. See the
[USB JSON contract](esy_middleware/protocol/README.md#json-usb-session-and-delivery).

## Memory Model

**Pico and STM32F4 library-owned resources require no libc or FreeRTOS heap.**
Module state and OS wrapper objects use static or caller-owned storage. A
complete firmware's memory policy also includes its SDK, providers, and
application code.

LVGL and lwIP allocate objects at runtime inside reserved pools or private
heaps. On ESP32, native ESP-IDF drivers allocate SDK-owned resources even when
the wrapper storage is caller-owned. See the
[memory and provider contracts](docs/integration/memory_model.md) for allocation
paths and the scope of `configSUPPORT_DYNAMIC_ALLOCATION=0`.

## SMP on Specific MCUs

For dual-core scheduling, check both the chip port and the native kernel
configuration.

| Target | Vendor FreeRTOS integration |
| --- | --- |
| Pico / RP2040 | Raspberry Pi provides a [dual-core SMP port](https://github.com/raspberrypi/FreeRTOS-Kernel/blob/main/portable/ThirdParty/GCC/RP2040/README.md). |
| Pico 2 / RP2350 Arm | Raspberry Pi provides a [dual-core SMP port](https://github.com/raspberrypi/FreeRTOS-Kernel/blob/main/portable/ThirdParty/GCC/RP2350_ARM_NTZ/README.md). |
| Classic ESP32 | ESP-IDF provides [dual-core FreeRTOS SMP](https://docs.espressif.com/projects/esp-idf/en/v5.3.4/esp32/api-reference/system/freertos_idf.html). |

## Host Verification

With the [host prerequisites](tools/README.md#entry-point) installed, run from
the library root:

```sh
./setup.sh
tools/quality/ci_quality_gate.sh test:all
```

This is the development setup for contributors extending or fixing the library.
Setup prepares seven pinned host-test providers in the shared user cache;
subsequent library tests and quality gates discover them automatically, without
shell sourcing or a consuming firmware project. Firmware dependencies remain
consumer-owned, including LVGL and FreeRTOS submodules. See
[software provider setup](tools/README.md#software-provider-setup) for existing
checkout overrides, offline verification and coverage commands.

## Documentation and Contributing

- [Runnable examples: tasks, bootloader and OTA](https://github.com/EsyProject/EsyMcuPlatformDemo)
- [Hello World: static tasks and RTT logger](docs/integration/hello_world.md)
- [Architecture and repository scope](docs/integration/hardware_layers.md)
- [Build inputs and CMake targets](docs/integration/build_integration.md)
- [Adding device and platform support](docs/integration/porting.md)
- [Feature selection](docs/integration/feature_gates.md)
- [Memory model and SDK allocations](docs/integration/memory_model.md)
- [Atomic backends](docs/integration/atomic_backends.md)
- [Contributing and verification](CONTRIBUTING.md)
- [Quality tools and CI](tools/README.md)
- [Release checklist](docs/release/release_tag_checklist.md), [changelog](CHANGELOG.md)

## License

Original EsyMcuPlatform source, documentation, and tools are licensed under the
[Apache License, Version 2.0](LICENSE). See [NOTICE](NOTICE) for attribution.

The SPI and UART RX PIO adapters retain Raspberry Pi `pico-examples` notices and
[BSD-3-Clause terms](LICENSES/BSD-3-Clause.txt) alongside Apache-2.0 for project
contributions. External SDKs, RTOS kernels, and other providers retain their
own licenses.
