<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Classic ESP32 Native Consumer

## Purpose

Compile and link production HAL, backend and middleware sources against the
official classic ESP32 SDK. The fixed baseline is ESP-IDF v5.3.4, commit
`1b459d9c4950395dec12ce19256c73f9a41e306f`, and Xtensa GCC
`esp-13.2.0_20240530`. The build entry rejects a different SDK or compiler.

## Public Surface

`build_native_consumer.py --build-root PATH [--profile PROFILE] [--jobs N]`
builds and inspects the selected consumer. `PROFILE` is `core`, `peripherals`,
`managed`, `radio`, `power`, or `all` (the default). It does not flash a device.

| Profile | Selected library capabilities |
| --- | --- |
| `core` | System, native FreeRTOS task/core-affinity and fatal registration APIs; optional peripherals disabled. |
| `peripherals` | Core, GPIO, synchronous UART/SPI/I2C with public DMA rejection probes, TWAI CAN, PWM, timers/capture, watchdog, ADC1 oneshot, Flash and all optional OS wrappers. |
| `managed` | Peripherals, managed boot/OTA, native panic capture, RAM coredump and SPI/UART TX scheduler OS adapters. |
| `radio` | Peripherals, native Wi-Fi station/manager and the raw lwIP network adapter with SDK core locking. |
| `power` | Peripherals, explicit task-context Light-sleep and reset-class Deep-sleep with native PM enabled; generic atomic tickless remains unsupported. |

## Integration

Install the pinned SDK and its recommended classic ESP32 tools once:

```sh
git clone --branch v5.3.4 --depth 1 --recursive --shallow-submodules \
  https://github.com/espressif/esp-idf.git /path/to/esp-idf-v5.3.4
/path/to/esp-idf-v5.3.4/install.sh esp32
source /path/to/esp-idf-v5.3.4/export.sh
python3 -B esy_hal/tests/native/esp32/build_native_consumer.py \
  --build-root /tmp/esy-esp32-native --profile all
```

Run the final command from the platform checkout. The build root must be outside
that checkout. Use a fresh root initially; subsequent runs accept only profile
directories carrying this consumer's matching ownership marker.

## Contracts

The consumer owns its feature files, board mappings, SDK configuration and
partition table. Production targets do not depend on these test assets. The
selected feature file uses ordinary final `set()` values. A profile is also
published as an IDF build property so dependency discovery and actual component
assembly select the same native providers.

`app_main()` retains probe functions through a volatile pointer. The probes
contain real public API calls, keeping their native call closures in the final
ELF; the entry point does not execute peripheral or image-management operations.
The script checks required final symbols, disabled backend source isolation and
the managed profile's panic-wrapper IRAM placement. Relevant library targets and
consumer sources compile with warnings treated as errors.

Each run recreates its owned generated `sdkconfig` from the fixed defaults.
Radio adds `sdkconfig.radio`, enabling `CONFIG_LWIP_TCPIP_CORE_LOCKING` and
`CONFIG_LWIP_CHECK_THREAD_SAFETY`. The default network provider requires both;
an explicitly supplied consumer provider retains its own integration contract.
Power adds `sdkconfig.power` to compile the PM-enabled explicit sleep path. It
does not execute sleep, enable native automatic Light-sleep or certify tick
settlement. CAN uses native `driver` discovery; the peripheral probe retains its
send/receive/filter/status closures. SPI/UART public DMA and UART RX stream probes
retain the explicit unsupported paths; synchronous UART send_timeout is linked.
The UART probe checks that rejected TX/RX DMA leaves both directions idle,
public abort reports no tracked request, and bounded TX plus deinit still work.
These checks execute only when the peripheral probe is explicitly run on
hardware; the standard entry retains their call closures without executing them.
Capture retains native MCPWM timer/channel creation, ISR registration, capability
and timestamp/interval query closures. Its six logical reservations map three
slots to each capture group; IDF selects the native channel. The probe uses
reservation 4, group 1, slot 1. Hardware timestamps use the 32-bit APB tick domain
(80 MHz: 53.6870912 s per native wrap), converted to whole microseconds; intervals
subtract native ticks before conversion. Capture configures floating GPIO inputs
with internal pulls disabled and owns its selected native capture groups; a
running consumer supplies external bias and does not create competing native
capture drivers.

A running application must bind execution-context predicates before ESP32 capture
lifecycle calls. `esy_os_init()` installs them in the OS configuration used by
this consumer. An SDK-only application can instead enable System and call
`esy_hal_system_set_context_callbacks()` with system-lifetime native context
predicates; the capture provider has no Esy OS target dependency. This compile/link
entry retains those startup closures without executing them.

Managed additionally retains the optional middleware UART TX worker and SPI
scheduler targets. HAL/backend has no platform OS target dependency; native SDK
components keep their own kernel implementation.

## Resource Model

The board profile and partition CSV belong to this consumer. Managed boot uses
two 1 MiB OTA partitions, native `otadata` and two separate 4 KiB Esy metadata
partitions. The source contains no product configuration or shared SDK patch.
Build products, generated SDK configuration and evidence stay under the explicit
external build root.

## Verification

Each successful profile produces `esy-native-evidence.json`, the application
ELF/map, bootloader ELF, SDK configuration and compiler command database. Evidence
records the SDK commit, compiler, platform commit, dirty state, tracked-diff
SHA-256, complete source SHA-256, backend source population and verified symbols.
An edit during compilation invalidates that run's evidence and requires a rerun.
The GitHub Actions native ESP32 job runs these same profiles and uploads their
artifacts.

## Known Limits

This is a compile/link consumer for classic ESP32 and the pinned provider
versions. It does not define an executable application workflow. Other ESP32
families, generic DMA, ADC2, Flash encryption and unrelated module combinations
are outside its profile selection.
