<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# esy_hal_backend

## Purpose

`esy_hal_backend` implements hardware operations for the HAL, selected by the
consuming project's `ESY_TARGET_DEVICE` and feature variables. The shared target
resolver derives the platform before backend selection. The module is
intentionally close to vendor SDKs, but its public headers are still reusable
EsyMcuPlatform contracts.

## Public Surface

Public headers live under `esy_hal_backend/include/esy_hal_backend/`.

Platform implementations live under:

- `src/pico/`
- `src/stm32f4/`
- `src/esp32/`

Pico multicore assembly explicitly links `pico_multicore`. Queue init takes
`buf_size` and rejects backing storage smaller than `item_size * (depth + 1)`;
the extra slot does not increase usable depth. Pico native SPI rejects LSB-first
before ownership or pin changes. DMA IRQ affinity selects a core before first
prepare; that core must perform first assembly, while later submissions may run
on either core. Generic XIO program/SM objects require `{0}` before first use;
side-set counts name physical pins and exclude the optional enable bit. The
[Pico contract HIL plan](../esy_hal/tests/hil/pico_multicore_watchdog_xio_hil_plan.md)
covers these contracts and watchdog/PIO relocation behavior.

Pico DMA and XIO use the native SDK claim tables, including reservations held
by the built-in CYW43 Wi-Fi provider. A private nonblocking assembly reservation
serializes fixed-ID HAL claims against the built-in provider's lazy allocation
during station enable/join, including retries. Contention returns `BUSY` for
caller retry; no IRQ mask or waiting spinlock spans native SDK work. Ordinary
CYW43 polling does not allocate resources. Consumers using raw SDK allocation
outside the built-in provider must serialize those allocation phases with HAL
DMA/XIO claims: SDK fixed-ID claims panic on a race, and an availability query
is not an atomic try-claim. Independent HAL DMA owners are still arbitrated
atomically. Existing native claims return `BUSY` before
programming that resource, and a HAL claim is visible to subsequent SDK
allocators until successful retirement/release. XIO instruction memory is
reserved by the SDK's atomic add operation. Do not call raw SDK unclaim for a
resource owned by HAL. Shared DMA IRQ handlers acknowledge only their own
configured channels, leaving native-provider pending flags intact.

The Wi-Fi port also supplies `esy_hb_wifi_get_network_runtime()`: a side-effect-free
descriptor query available before native initialization. Pico supplies network
polling and paired core-region hooks with a NULL context. Its hooks are defensive
no-ops outside native lifetime, which does not extend borrower lifetime. ESP32
supplies no-op progress because native tasks drive networking; paired hooks use
the nonrecursive TCP/IP core lock and require both native core-locking and
thread-safety options. The consumer keeps that native TCP/IP stack initialized.
Call endpoints from their app owner without an outer core lock, never from native
TCP/DNS callbacks. Neither port's hooks run station connection/deadline bookkeeping
or permit concurrent app callers. Quiesce and detach every network borrower before
native deinit or replacement. Public types contain no SDK, lwIP or middleware
configuration types. See the [network runtime HIL plan](../esy_hal/tests/hil/pico_wifi_runtime_hil_plan.md)
for native progress, state-separation and teardown acceptance.

Pico USB exposes physical controller index zero only. All USB handle lifecycle
and TinyUSB callbacks share one serialized owner context. A second handle
returns `BUSY` without replacing the current callback owner; that reservation
ends only when its owner deinitializes. See the
[USB ownership HIL plan](../esy_hal/tests/hil/pico_usb_owner_hil_plan.md).

Duty updates require enabled PWM output. On ESP32 this check precedes LEDC
calls because native duty update also enables the physical output. Disabled
updates return `STATE`, preserve the accepted duty and do not produce edges.

STM32F4 watchdog enable before any successful init applies the public 1000 ms
default; feed before initialization is harmless. Successful IWDG init starts
the non-stoppable watchdog, so later enable preserves its active policy.
Explicit init applies both debug policies: true sets the native IWDG freeze bit,
and false clears it, including a later true-to-false reconfiguration.
ESP32 reset acknowledgement is boot-local software state and leaves the
read-only SDK reset reason and watchdog configuration intact. The
[watchdog HIL plan](../esy_hal/tests/hil/watchdog_lifecycle_hil_plan.md) covers
these sequences.

Each native feature source must close its matching public `esy_hb_*` API.
Direct boot combines `src/esy_hb_boot_direct.c` runtime queries with the
port-owned `src/<platform>/esy_hb_boot.c` CPU handoff.

### Platform Capability Matrix

| Feature | Pico | STM32F4 | ESP32 | Notes |
|---|---|---|---|---|
| System | Implemented | Implemented | Implemented | Tick, delay, IRQ control, CPU queries and reset semantics are platform-specific. SDK random bytes are available on Pico/ESP32; STM32F4 fails closed. |
| Boot | Direct image handoff | Direct image handoff | Managed ESP-IDF OTA partitions | Activation commits the next partition without reset; explicit handoff restarts. Rollback must be enabled and Flash encryption is unsupported by this update path. |
| GPIO | Implemented | Implemented | Implemented | IRQ and wakeup behavior depend on board routing and wake-source configuration. |
| DMA | Implemented | Implemented | Unsupported public DMA | ESP32 DMA API returns `ESY_ERR_NOT_SUPPORTED` for operations with status returns. |
| UART | Automatic TX/RX DMA; IRQ RX stream | Automatic finite DMA; IRQ RX stream | Synchronous I/O | DMA IRQ completion is distinct from UART TX/TC IRQ. Pico TX callback retires source storage; busy covers physical drain. |
| SPI | Implemented | Implemented | Implemented with bounded transfer lengths | DMA remaining queries are unsupported on STM32F4/ESP32. ESP32 RX above four bytes requires a multiple of four. |
| I2C | Blocking/combined transfers; DMA unsupported | Blocking/transfer APIs; DMA unsupported | Master blocking/combined transfers | Pico/STM32F4 DMA submissions fail before bus activity. ESP32 requires board-completed SDA/SCL pins; slave mode and runtime speed changes are unsupported. Bus recovery depends on board wiring and external devices. |
| CAN | Configure-time rejection | Classical bxCAN on STM32F405/407/415/417 | Classical TWAI with SDK RX queue and owner-driven TX/RX | ESP32 uses exact bounded software filters and bus-off stop/deinit/init recovery. STM32F4 uses hardware filters and explicit bus-off recovery. FD/BRS are unsupported. See the [CAN contract](../esy_hal/docs/can.md). |
| Timer | Implemented | Implemented | Implemented | ESP32 SDK-task callbacks support cancel/start replacement; failed cleanup reserves its native slot for retry. |
| Timer Capture | Unsupported | 1 MHz hardware capture | MCPWM hardware capture | ESP32 captures a 32-bit APB tick counter and converts to microseconds. STM32F4 uses 32-bit TIM2/TIM5 or a supported 16-bit timer. Consecutive edges must be less than one native counter wrap apart. |
| PWM | A/B sharing with per-slice ownership | Implemented | Implemented | Pico A/B and ESP32 channel pairs require a common requested frequency. Complementary/dead-time entry points are unsupported on these ports. |
| Watchdog | Implemented | Implemented | Implemented | Reset behavior follows the native watchdog and reset-domain configuration. |
| ADC | Configure-time rejection | Implemented | ADC1 synchronous oneshot | ESP32 uses a fixed 12-bit/12 dB raw profile with DEFAULT sampling time; DMA, scan and external triggers are unsupported. |
| Flash | Implemented; custom hook supported | STM32F405/407/415/417 single-bank layouts; custom hook unsupported | Implemented; custom hook unsupported | Geometry comes from the selected hardware/SDK. Board smoke tests should cover erase/write/read. Non-NULL hook registration fails explicitly on unsupported ports. |
| Low Power | CPU idle plus caller-selected hardware alarm | Resumable Sleep/Stop plus explicit Standby; wakeup alarm not provided | Explicit task-context Light-sleep and reset-class Deep-sleep | ESP32 generic atomic idle/tickless is unsupported; manual entry requires system quiescence and does not settle OS tick deadlines. See the [ESP32 sleep contract](../esy_hal/docs/esp32_low_power.md). |
| Multicore | Implemented | Configure-time rejection | Configure-time rejection | Pico-only; `ENABLE_ESY_MULTICORE` requires a Pico platform. |
| USB Device | Shared-stack event binding | Configure-time rejection | Configure-time rejection | Pico-only; connect/disconnect, suspend/resume and SOF events. Generic endpoint/address/EP0 operations return `NOT_SUPPORTED`; no SETUP/endpoint-completion events. |
| Wi-Fi | Pico W/Pico 2 W only | Configure-time rejection | ESP-IDF station and DHCP | One station backend; Pico uses polling, ESP32 uses SDK-driven networking with one application owner. |
| XIO | Implemented | Configure-time rejection | Configure-time rejection | Pico PIO extension; `ENABLE_ESY_XIO` requires a Pico platform. |

### Public Capability Masks

ADC, I2C, SPI, UART, PWM and timer-capture queries use public
`ESY_<peripheral>_CAP_*` constants from their `esy_common/defs` headers. Bit
meanings are identical across ports. They describe implemented operations for
an initialized handle's role, subject to board routes, timer selection and
runtime errors. They neither reserve resources nor promise every configuration.
Ignore unknown bits; zero is valid. Use the public constants when testing bits.

The following entries use each peripheral's public constant suffixes:

| Query | Pico | STM32F4 | ESP32 |
|---|---|---|---|
| ADC | Unavailable | DMA, SCAN, EXTERNAL_TRIGGER, CIRCULAR_DMA | Zero |
| CAN | Unavailable | LOOPBACK, LISTEN_ONLY, FILTER, RECOVERY | LISTEN_ONLY, FILTER |
| I2C master | MASTER_TRANSFER, REPEATED_START | MASTER_TRANSFER | MASTER_TRANSFER, REPEATED_START, RECOVERY |
| SPI | DMA_TRANSFER, TRANSFER16, DMA_REMAINING | DMA_TRANSFER, TRANSFER16 | TRANSFER16 (master only) |
| UART | TX_DMA, RX_DMA, RX_STREAM, DMA_REMAINING | TX_DMA, RX_DMA, RX_STREAM | None |
| PWM | DMA_OUTPUT, WRAP_IRQ, POLARITY | DMA_OUTPUT, WRAP_IRQ, POLARITY | POLARITY |
| Timer capture | Unavailable | EDGE_CALLBACK, INTERVAL, HARDWARE_LATCH | EDGE_CALLBACK, INTERVAL, HARDWARE_LATCH |

Pico I2C slave handles report zero: callback support is not implied by controller
initialization. STM32F4 and ESP32 reject slave initialization before claiming or
routing pins. STM32F4 is master-only; unsupported combined repeated-START
requests are rejected before any write or other bus activity.
Pico/STM32F4 I2C ordinary DMA read/write returns
`ESY_ERR_NOT_SUPPORTED` without starting a transaction or borrowing storage.
Use blocking/combined master transactions explicitly. Raw FIFO DMA cannot by
itself provide the required command servicing, final STOP, error handling and
callback retirement. See the [Pico I2C HIL plan](../esy_hal/tests/hil/pico_i2c_hil_plan.md).
I2C controller claims are exclusive and precede HAL board-pin configuration.
Blocking and DMA-named target addresses use `uint16_t` without truncation:
7-bit mode accepts 0..0x7f; ESP32 10-bit mode accepts 0..0x3ff. Pico and STM32F4
continue to reject 10-bit configuration. Combined transfers use one total
`timeout_ms` budget across both phases, including ESP32's two transactions when
a STOP separates them. Zero selects `ESY_I2C_DEFAULT_TIMEOUT_MS` (1000 ms),
also used by blocking read/write. Values above `INT32_MAX` are invalid.
Expiry returns `ESY_ERR_TIMEOUT`; native timebase resolution and bounded SDK
retirement affect return latency. Keep Pico's timer, STM32F4's HAL tick and
ESP32's microsecond clock running during calls. A stuck external bus may still
require board-approved recovery before another transaction.
Suspend releases native controller resources while retaining the same handle's
claim; resume cannot be overtaken by a second owner. Status-returning I2C deinit
releases the claim only after native cleanup succeeds. ESP32 device and bus
deletion are separate retryable stages, and STM32F4 propagates native deinit
failure. Failed cleanup keeps the handle and remaining resources alive for
retry; never clear a retained handle to bypass it.

UART also claims its physical controller before HAL board configuration.
Native cleanup retains the claim and context until HAL resets board pins and
calls the backend release operation. Suspend preserves the claim. A failed
native cleanup retains resources for retry. ESP32 installs its driver before
applying UART parameters and rejects an SDK driver already owned elsewhere.
Final cleanup returns BUSY for live RX/callbacks or undrained TX; there is no
platform-owned worker to retire. HAL and backend UART deinit return status.
STM32F4 UART data_bits counts payload bits, excluding parity. Supported framing
is 7E/7O and 8N/8E/8O; unsupported framing, including 9N, is rejected before
claiming or routing pins. 8E/8O uses a native nine-bit word. DMA and IDLE cursor
publication is serialized with a short local IRQ mask; callbacks run afterward.
STM32F4 I2C ordinary DMA calls reject without retaining buffers or callbacks.
Consumers must query capabilities and select blocking/transfer APIs explicitly;
no automatic fallback is performed. See the [STM32F4 I2C HIL plan](../esy_hal/tests/hil/stm32f4_i2c_hil_plan.md).
ESP32 I2C supports repeated START. Its recovery operation resets the native bus;
it does not
promise physical SCL pulses to release an external device. SPI `TRANSFER16`
names the blocking uint16_t-buffer APIs and does not specify wire byte order.
DMA bits identify supported native DMA transfer entry points. ESP32 public
SPI/UART/I2C DMA operations reject without retaining buffers or invoking a
callback. ESP32 UART's empty backend DMA abort succeeds on an owned handle so
the HAL can retire rejected submissions and release temporary device references;
it leaves synchronous FIFO traffic untouched. Public HAL abort returns STATE
after a rejected submission because no tracked DMA request remains.
SPI may use SDK-internal DMA within a synchronous call. Pico UART TX/RX
DMA and RX stream delivery use their native IRQs. UART TC and DMA TC are distinct.
STM32F4 remaining-count queries follow their existing provider capability masks.
Capture uses native hardware latches on supported ports. Pico rejects capture
initialization before board setup; GPIO interrupts remain independently usable.
No software GPIO timestamp substitutes for a native capture provider.

### Native capability validation

Capabilities describe the initialized hardware instance. A DMA capability
requires native asynchronous DMA; a blocking copy cannot advertise it.
Unsupported configuration is rejected before hardware changes:

- STM32F4 TIM9 supports ordinary PWM, polarity and wrap IRQ, but neither PWM
  DMA nor TRGO. The DMA bit is absent and TRGO returns `NOT_SUPPORTED`.
- On STM32F405/407/415/417, ADC temperature, VREFINT and VBAT channels belong
  to ADC1 only. ADC2/3 reject those channels in both single and scan requests.
- STM32F4 master SPI rejects frequencies below its input clock divided by 256;
  accepted dividers do not exceed the requested frequency.
- Pico DMA rejects non-NULL half-transfer callbacks. Unrequested half events
  remain NULL through HAL and scheduler wrappers; finite completion/error
  delivery remains supported. PWM scheduler terminal-only requests use
  `on_progress = NULL`.
- ESP32 GPIO wake accepts explicit `HIGH_LEVEL` or `LOW_LEVEL` only. Edge wake
  requests return `NOT_SUPPORTED`; they are never replaced with level wake.
  Input-only GPIO34..39 reject internal pull-up/down requests.
- ESP32 periodic logical timers reject intervals below 50 microseconds rather
  than allowing the pinned SDK to clamp the period. Non-default clock sources
  are unsupported; rejection retains no timer or HAL wake deadline.
- ESP32 watchdog counts use the selected RTC slow clock's nominal frequency,
  rounded up to whole ticks. Zero-frequency or overflowing requests fail before
  native configuration. Keep the clock source fixed until deinit; nominal
  duration does not certify an oscillator-independent lower bound.

## Integration

The consuming project selects `ESY_TARGET_DEVICE`; the resolver derives
`ESY_TARGET_PLATFORM`. It enables
HAL backend sources through `ENABLE_ESY_*` feature variables.
Enabling Multicore, USB Device or XIO on STM32F4/ESP32 fails during CMake
configuration, before unresolved HAL backend symbols can reach final linking.
This applies to direct backend assembly and full library assembly. Disabled or
undefined gates add no corresponding sources; XIO child gates remain inactive
when `ENABLE_ESY_XIO` is disabled or undefined.
The same assembly boundary accepts `ENABLE_ESY_ADC` on STM32F4 and classic
ESP32, and rejects it on Pico. ESP32 ADC requires the native `idf::esp_adc` target; it does not require the generic DMA facade
or an Esy OS/middleware target.
`ENABLE_ESY_DMA` selects the generic HAL DMA controller facade. Peripheral gates
independently assemble the native DMA arbiter, backend queries and IRQ vectors
required by those implementations. A peripheral can therefore use blocking I/O
or its advertised DMA API with `ENABLE_ESY_DMA=OFF`; this does not assemble
`esy_hal_dma.c` or enable the generic controller API. I2C-only builds do not
require the shared DMA backend on the current ports.
On STM32F4, `ENABLE_ESY_FLASH` requires an explicit `ESY_TARGET_DEVICE` from
the verified STM32F405/407/415/417 families. Other devices are rejected while
Flash is enabled; disabling Flash leaves their other backend features available.
ESP32 PWM privately links the native `idf::esp_driver_ledc` and
`idf::esp_driver_gpio` components. Consumers include
these components in native SDK discovery; disabling PWM adds neither source nor
provider dependency.
ESP32 CAN privately links native `idf::driver` (the ESP-IDF 5.3.4 TWAI provider). Its portable config explicitly supplies TX/RX pins and
native RX queue capacity; no product board routing is inferred. ESP32 explicit
sleep requires system support, `idf::esp_hw_support`,
`idf::esp_timer` and `idf::esp_pm`. Disabling either gate omits its sources and
native dependencies. The sleep provider does not enable generic atomic tickless.
ESP32 I2C uses `driver/i2c_master.h` from the native `idf::esp_driver_i2c`
component, `idf::esp_driver_gpio` for pin validation and `idf::esp_timer` for
the total transaction deadline. Consumers include all three components in SDK
discovery; enabling I2C without any required target fails during
configuration, and disabling I2C adds neither its source nor its dependencies.
The verified provider is ESP-IDF 5.3.4 for the classic ESP32; the provider must
implement `i2c_master_execute_defined_operations` and addressless master devices.
One serialized owner controls each handle and native controller. Initialization
creates one SDK bus and one addressless device; all transfers use bounded stack
operation lists and retain no buffers or callbacks after returning. SDK resource
allocation is limited to initialization; no per-address or per-transfer device
allocation occurs. Master frequency must be nonzero and at most 1 MHz, subject
to the board, peer and native controller's lower limits. Runtime `set_speed`
returns `ESY_ERR_NOT_SUPPORTED`; deinitialize and initialize to select a new
frequency. Slave mode and cancellation are unsupported. Separate native phases
receive only the remaining total transaction budget. Native execution contributes
to the caller's execution time; no I/O completion callback is installed or invoked.

Pico SPI initialization always supplies a valid SDK divider, including slave
mode with `speed_hz=0`. Slave initialization derives that divider from the
peripheral clock; the external master supplies the actual SCK. SPI privately
links `hardware_clocks` as well as `hardware_spi`. Qualify slave startup with
`PARAM_ASSERTIONS_ENABLED_HARDWARE_SPI=1` using the
[SPI buffer HIL plan](../esy_hal/tests/hil/spi_buffer_contract_hil_plan.md).

Run the HAL backend checks from the EsyMcuPlatform repository root:

```sh
tools/checks/check_source_contracts.sh esy_hal_backend
tools/checks/check_esy_hal_backend_contracts.sh
```

Platform compile verification belongs to each consuming firmware project's CI
because SDK paths, board ports, linker scripts, feature gates, and toolchain
files are owned by the consumer. A consumer project should build its configured
firmware target, or the `esy_hal_backend` target when it exposes one, after changing
HAL backend sources or platform integration.

## Atomic Runtime Provider

Build integration probes 32-bit C11 atomics using the configured toolchain.
RP2040 builds without native always-lock-free integers require the SDK
`pico_atomic` target. The Pico HAL backend privately links that runtime and compiles
its hardware-lock contract checks. Common and middleware retain standard C11
operations, storage and headers. No RTOS or extra feature selection is required.
See [atomic backends](../docs/integration/atomic_backends.md) for IRQ masking,
contention, reserved locks and target test procedures.

## Contracts

Pico Flash writes accept a source range only when every byte lies in the main
on-chip SRAM mapping. Flash/XIP, external memory and SRAM aliases return
`ESY_ERR_INVALID_ARG` before the flash-safe hook. Copy such sources into
caller-owned SRAM and retain that buffer until write returns; there is no hidden
staging allocation. See the
[Flash/system HIL plan](../esy_hal/tests/hil/pico_flash_system_hil_plan.md).

Pico core-1 startup has one serialized core-0 startup/task owner. It can launch
core 1 once per boot; repeats return `ESY_ERR_BUSY`, and other-core/ISR calls return
`ESY_ERR_STATE`. Do not reset or launch it through another provider. Unattainable
Pico system-clock configurations return `ESY_ERR_NOT_SUPPORTED`.

Direct ADC and USB backend handles require `{0}` before first init. ADC init
failure may retain resources: preserve the handle and finish deinit before retry.
USB init/deinit manage only the exclusive event binding for controller zero.
The separate TinyUSB startup owner controls class dispatch and final hardware
shutdown. Detaching the handle leaves the stack, controller, pull-ups, interrupts
and clocks running; quiesce dispatch before releasing callback/handle storage.
Only the shared stack's standard mount/unmount, suspend/resume and SOF callbacks
feed this binding. It does not implement a generic TinyUSB class driver or emit
SETUP/endpoint-completion events; class traffic uses its own software adapter.

Generic Pico XIO mappings and pin helpers accept only chip GPIOs within 0..31;
ranges cannot wrap. Masked writes preserve outputs outside the range and require
a stopped, exclusively owned SM. Direct XIO SPI/UART handles require complete
`{0}` initialization. Repeated init returns `STATE` without disturbing PIO/DMA;
failed first init releases temporary PIO resources and permits retry. Let native
terminal IRQ completion retire active DMA or abort it successfully before deinit;
failed stop retains the borrow for task-side abort. UART TX source retirement
does not establish physical wire idle: observe its FIFO/PIO drain separately.
Successful SPI cleanup clears its context, and UART direction cleanup preserves
the other direction.

Pico XIO state-machine initialization validates the exact clock-divider ratio
before touching native configuration. The supported range is 1 through 65536,
quantized down to the hardware's 16:8 divider; unsupported ratios return
`INVALID_ARG`. UART baud rates must also permit an overflow-free eight-cycle
conversion. Failed child initialization releases its PIO resources for retry.
`ESY_XIO_PROG_PRIV_BYTES` and `ESY_XIO_SM_PRIV_BYTES` may be overridden globally.
All translation units must agree on the resulting object layouts, including
embedded storage controlled by `ESY_UART_PRIV_BYTES` and `ESY_SPI_PRIV_BYTES`.
Rebuild all consumers after changing these sizes; backend assertions reject
insufficient storage.
The default SPI backend reserve includes the Pico XIO SPI DMA notification and
atomic event state. Its increase from 256 to 264 bytes adds eight bytes to each
default SPI handle on Pico; rebuild common, HAL, backend, middleware and consumer
objects together.
The default UART backend reserve includes both XIO directions' notification and
atomic event state. Its increase adds 32 bytes to each default UART handle;
rebuild common, HAL, backend, middleware and consumer objects together. XIO splits
that reserve into aligned direction slots and checks each context's capacity at
compile time.

An active or cleanup-retained RX stream excludes blocking RX and ordinary RX
DMA. Pico/STM32 direct blocking receivers also return `BUSY` before reading
FIFO/DR. Independent TX remains available; serialize all RX mode changes.

`esy_hal_backend` is explicitly exempt from host unit tests. Do not add an
`esy_hal_backend/tests/` unit-test project for this module. The module is primarily a
hardware SDK adaptation layer, and mocking vendor SDK calls would mostly test
mock call ordering instead of real HAL backend behavior.

Functions that are not implemented on a platform must return
`ESY_ERR_NOT_SUPPORTED` when their signature allows an error code. They must not
return `ESY_OK` as a placeholder.

STM32F4 GPIO owns EXTI per encoded pin and masks only the departing line.
Registrations in EXTI9_5 or EXTI15_10 share a priority; conflicting priorities
or attempts to steal the same line on another port return `ESY_ERR_STATE`
before hardware changes. A disabled line retains ownership and its priority.
Only the last detached registration disables the group NVIC IRQ. Task-side
register and callback publication uses a short PRIMASK window; callbacks run
outside it and cannot reenter lifecycle/configuration. Deinit and wake detach
retire the old callback before returning.

Pico/ESP32 GPIO IRQ lifecycle is serialized in task context on the core of the
first attachment. Cross-core attach returns `STATE`; void IRQ operations ignore
cross-core calls. An occupied pin returns `BUSY` without replacing its callback.
ESP32 does not adopt an externally installed GPIO ISR service. Do not modify
native IRQ registrations outside this backend or call lifecycle from a callback.
Same-core detach retires borrowed callback context before returning.
ESP32 GPIO electrical initialization returns `BUSY` while an IRQ/wake binding
exists, before `gpio_config` can replace its trigger. This includes HAL temporary
state, configured-state restore and GPIO PM profile selection. Detach first;
rejected transitions preserve the native IRQ/wake state and callback argument.
Pico and ESP32 ordinary IRQs support native edge and level triggers; STM32F4
EXTI rejects level triggers. ESP32 Light-sleep wake supports levels only.
An already asserted level can wake immediately or prevent sleep, and a level
IRQ handler must promptly clear the external condition to avoid repeated IRQs.
GPIO34..39 on classic ESP32 have no internal pulls: pull requests fail before
GPIO setup or IRQ service allocation rather than relying on SDK return status.

ESP32 PWM maps channels 0/1, 2/3, 4/5 and 6/7 to four shared low-speed LEDC
timers. One externally serialized owner coordinates all channels. A timer tracks
its accepted frequency and live channel count. Matching requests coexist;
conflicting init or shared-timer frequency changes return `ESY_ERR_BUSY` before
SDK reconfiguration. The final channel stops before timer pause/deconfigure.
Failed cleanup retains the handle and claim for retry, blocking new timer
claimants while final cleanup is pending. Native SDK users must not share these
resources outside this ownership model.
Runtime polarity changes reconnect only the selected channel's GPIO-matrix
inversion. A stopped channel stays stopped; its physical idle level follows
`start_low`. Suspend stops that channel while preserving its enable intent,
duty and timer claim. Resume restores the waveform only if it was enabled
before suspend. Neither operation pauses the shared timer or changes a peer.
Data/enable operations reject suspended channels; failed resume remains
suspended for retry. These channel transitions do not enter system sleep.

Interrupt save/restore tokens preserve the entry mask, including nested calls
and already-masked entry. On ESP32 the backend uses the SDK's interrupt-level
token APIs in SDK-supported task/ISR contexts; unconditional enable/disable
operations are not a nesting mechanism.
Tokens remain on their saving core and are restored in reverse order. This
protects short local ISR/task handoffs, not objects accessed by another core.
The [ESP32 IRQ HIL plan](../esy_hal/tests/hil/esp32_irq_hil_plan.md) covers nested
restoration and interrupts pending until the outer mask is restored.

ESP32 UART idle suspension retains the SDK driver and RX ring, gating RX
interrupts while preserving configuration. Undrained TX rejects suspend with
BUSY. Synchronous calls are externally serialized; send_timeout has a finite
64-bit native microsecond deadline and leaves no caller-buffer borrow after
return. Already copied FIFO bytes may continue after timeout. Native public DMA
and stream callbacks are unsupported. The backend creates no task/event queue
and has no OS abstraction dependency. Callback scheduling belongs to middleware.
UART assembly privately links `idf::esp_driver_uart` and `idf::esp_hw_support`.
HAL/backend neither include nor link an OS or middleware target. ESP32 CPU IRQ
tokens use native Xtensa instructions. The system integration owner supplies
exact runtime checks through `esy_hal_system_set_context_callbacks()`; the OS
adapter installs its binding during its own initialization. Before a binding is
available, IRQ queries conservatively classify masked CPU contexts as ISR and
explicit sleep rejects entry. Native drivers may still use their SDK kernel internally.
This system-only path does not require `idf::app_update`. That native component
is required and privately linked only when `ENABLE_ESY_BOOTLOADER` selects the
boot backend, including image activation and handoff.
System random bytes additionally require and privately link native
`idf::esp_hw_support` for `esp_fill_random` from `esp_random.h`. ESP-IDF's random
source is entropy-dependent: RF/ADC entropy-source state affects output quality.
The backend does not enable, disable or reconfigure these sources, and makes no
cryptographic or collision-free-identity promise.
ESP32 SPI accepts only complete 8-bit or 16-bit frames; other widths are rejected
before controller claims, board routing or native calls.
Runtime speed changes remain unsupported and preserve the current native and
saved configuration on failure.
The [ESP32 UART lifecycle HIL plan](../esy_hal/tests/hil/esp32_uart_lifecycle_hil_plan.md)
covers driver retention, synchronous I/O, repeated resume and busy rejection.

STM32F4 system initialization enables and verifies DWT CYCCNT without resetting
its value or changing other trace settings. It propagates vendor HAL init failure
as `ESY_ERR_IO` and returns `ESY_ERR_NOT_SUPPORTED` if the counter cannot advance.
`esy_hb_system_delay_us()` then busy-waits using
the current HCLK, rounds fractional cycles upward, and splits large requests
into wrap-safe chunks. It needs no TIM peripheral, tick interrupt or OS service,
and does not change the IRQ mask. Call it only after successful system init;
zero returns immediately. The clock owner must maintain `SystemCoreClock` and
keep HCLK fixed per call. Profilers and debuggers must not reset/disable the
shared counter while initialized. STOP restore preserves an already enabled
counter, but sleep/clock transitions must not overlap a delay. Keep requests
short, especially in ISR/masked context; preemption and call overhead extend the
wait. This is not an exact waveform generator or an always-on clock.
Delay support is unconditional and does not change the STM32F4 system timestamp's
millisecond resolution, even when `enable_us_tick` is set. See the
[microsecond delay HIL plan](../esy_hal/tests/hil/stm32_delay_us_hil_plan.md).

`esy_hb_system_random_bytes()` uses Pico SDK `pico_rand` through a private
system-feature dependency. SDK-generated 128-bit words are copied explicitly
in little-endian order, including partial final blocks. Pico SDK 2.1.1 uses its
configured entropy sources and lazy PRNG seeding; default RP2350 assembly uses
hardware TRNG/per-boot entropy, while RP2040 has different SDK entropy sources.
The consumer must preserve valid source configuration and assess its suitability.
SDK seeding/generation may wait, so normal serialized startup/task context is
required; terminal fault/ISR/Flash-unavailable capture must never call it.
Zero-length requests succeed without touching SDK state and permit NULL output.
NULL/nonzero is INVALID_ARG. STM32F4 nonempty requests return NOT_SUPPORTED and
leave output untouched until a real supported source is provided. No tick/UID
fallback, new heap allocation, task or persistence write is supplied.

`esy_hb_flash_set_safe_execute()` returns `esy_err_t`. Pico accepts a custom
hook and returns `ESY_OK`; STM32F4 and ESP32 return `ESY_ERR_NOT_SUPPORTED` for
non-NULL hooks. NULL restores the port default and succeeds on all three ports.
Failed registration preserves prior behavior. Serialize registration with all
flash operations and check its result before relying on the hook.

Flash geometry queries return status and preserve output values on failure;
they are available before initialization. Pico uses SDK board capacity and Flash
constants. STM32F405/407/415/417 use the factory Flash-size register for 512 KiB
or 1 MiB capacity and a private single-bank sector map. ESP32 uses the default
SDK Flash chip's accessible size and erase-sector metadata, with byte-granular
unencrypted programming and offset addresses. The ESP32 port privately includes
the SDK's `spi_flash_chip_driver.h` for that metadata; it does not expose SDK
types through public headers. See [Flash geometry](../docs/integration/flash_geometry.md)
for address, erase and hardware verification contracts.

Low-power ports report resumable hardware modes and conservative entry, exit,
and minimum-residency timing through
`esy_hb_low_power_get_capabilities()`. Their prepare/enter/cancel lifecycle
also owns any vendor-specific early restore state. STM32F4 snapshots and
restores RCC state inside `esy_hb_low_power`; after STOP wake it uses the
HSI-driven DWT cycle counter plus a finite polling budget instead of the halted
HAL tick. The advertised STM32F4 exit bound includes the configured HAL HSE
startup timeout and bounded HSI, PLL, and clock-switch waits. The generic
HAL wrapper contains no hardware-specific branch. A restore failure is fatal to
normal execution. Kernel tick ownership stays in the OS adapter, and Standby
remains a separate explicit shutdown operation.

`esy_hb_wakeup_timer` is a documented hardware-alarm abstraction, not a
private HAL hook. Pico implements it with a caller-selected SDK hardware
alarm and the same 64-bit timer used for absolute deadlines. STM32F4 reports
`ESY_ERR_NOT_SUPPORTED` until a reusable RTC/LPTIM contract is supplied.

HAL backends expose hardware lifecycle, busy state, abort/stop, and terminal callback
primitives; they do not register with the device graph and do not choose system
power states. Registration is performed by caller-owned typed adapters in
`esy_hal`, because only the system assembler knows the parent bus, DMA,
power-domain dependencies, and product policy. A HAL backend that accepts one-shot
DMA callbacks must dispatch exactly one completion or error callback after a
successful submission so the HAL adapter can release its usage reference.
Pico DMA distinguishes bus errors from successful completion and stores both
callback kinds. Prepare/start-single/abort are serialized task operations; DMA
callbacks must be bounded and must not reenter those operations. Abort disables
channel IRQs, retires peer-core dispatch, and detaches callbacks before success.
STM32 abort failure retains ownership while the stream remains enabled.

Direct generic HAL DMA callbacks run automatically in IRQ context. Native
terminal retirement is bounded; failed stop retains the claim and storage for
explicit task-side abort. Half/circular callbacks retain ownership. Pico ISR
retirement caches SDK channel reservations to avoid native allocation locks in
interrupt context; task release/deinit flushes them. No HAL process hook exists.

Generic DMA routing has an explicit physical identity query. STM32F4 resolves
the controller/stream from `dreq`; the `channel` field selects the peripheral
request. Pico resolves the physical channel directly. HAL start masks select
configured-route indices and are translated to these backend resource indices;
backend callers use only the resolved physical mask. STM32 batch start validates
every selected stream before enabling any of them.
Pico start and triggering address updates validate the reload count in
`CHx_DBG_TCR`, not the live `TRANS_COUNT`. Preparing or updating a count programs
the reload value; the live counter may remain zero until the channel is triggered
and reaches zero again on completion. Remaining-count queries still read the
live counter. A completed prepared channel can be triggered again by its
serialized backend owner after restoring the required memory endpoints.

Physical DMA ownership is shared by generic HAL transfers and native peripheral
adapters. Every mutating backend DMA entry point takes a non-NULL stable
`owner` token as its first argument. Prepare atomically claims the resolved
physical channel/stream before changing its configuration. A second prepare,
including the same token, returns `BUSY` until the previous request retires.
Start validates the complete physical mask and matching owner before enabling
anything; foreign abort/address/count/chaining calls cannot alter the request.
Generic HAL uses its controller handle as the token; native SPI/PWM/ADC and UART
direction/stream adapters use their own stable context storage. Initialization
does not reserve routes, so idle handles may describe overlapping resource lists;
only the prepared transaction owns the hardware. Keep owner storage alive until
retirement. Custom backend implementations and direct callers use the same
owner-first signatures.
UART busy queries use the UART handle's own DMA/stream state and physical TX
drain. Activity owned by another request on an overlapping configured route does
not make an idle UART busy or prevent its baud-rate change or PM suspension.

Hardware idle and terminal IRQ publication keep the claim intact. Successful
task abort retires hardware and callback access, then releases the claim; failed
retirement retains it for retry. An unclaimed idle abort is idempotent, while a
foreign claim returns `BUSY` without affecting its owner. The native
`esy_hb_dma_release()` operation is reserved for a provider that has already
retired peripheral requests, DMA and pending callback access. STM32 finite ADC
completion uses it only after the client callback returns; failed ISR retirement
keeps the claim until task-side abort succeeds. Claim/release uses atomic owner
publication, and request metadata is detached before availability is published.
The existing serialized-owner requirement still covers configuration and
lifecycle; atomic claims do not permit concurrent operations on one transaction.
Pico additionally reserves first-time shared DMA IRQ assembly under callback
metadata exclusion, performs SDK handler installation outside that exclusion,
and then publishes readiness. A competing prepare on that IRQ may return `BUSY`
without retaining its channel claim and may be retried. Core binding uses the
same metadata guard and returns `BUSY` once IRQ assembly has begun.
All shared DMA IRQs assigned to the same Pico core must use the same configured
priority, preventing an IRQ from preempting a peer that holds callback exclusion.
Conflicting prepare requests return `BUSY` and release any newly acquired channel
claim before changing native IRQ configuration. Installed handlers retain their
priority until reset, even after all transfers retire. A first assembly must run
on the selected core. Native code must not change these IRQ priorities.
Native SDK users outside these adapters must not touch reserved DMA resources.
Pico native release is task-only; an ISR call returns `NOT_SUPPORTED` before
mutation. It does not wait for hardware, but metadata exclusion may await a
bounded peer IRQ dispatcher. The exclusion spans SDK unclaim and owner
publication so a dispatcher cannot carry a stale channel mask into the next
SDK owner's transfer. Native callbacks only notify; lifecycle runs on the task.
External raw SDK allocation must follow the serialization contract above;
native providers may keep their claimed channels idle without losing their
reservation. Native unclaim occurs only after hardware/IRQ retirement and as
the final mutation of that physical resource, so release does not wait for the
allocation reservation.

Address/count setters and chaining return status. Validate unsupported or
invalid operations before changing hardware; STM32F4 chaining returns
`ESY_ERR_NOT_SUPPORTED`. The public generic HAL also rejects chaining on Pico
until chained destination ownership and terminal retirement are modeled; the
private Pico primitive remains available to hardware adapters. Custom backend
implementations return the actual provider outcome through `esy_err_t`.

DMA submission length is always bytes. STM32F4 validates element width, alignment
and whole beats, then programs at most 65,535 beats into NDTR. ADC performs its
separate native sample-count conversion only at the native ADC start call.
Missing peripheral-path memory endpoints use a fixed aligned dummy word without
memory increment; they do not imply circular mode. Memory-to-memory transfers
increment both endpoints, use the required FIFO mode, and reject circular mode.
STM32F4 accepts memory-to-memory only on DMA2. DMA1 rejects it with
`ESY_ERR_NOT_SUPPORTED` before claiming or changing stream, clock or IRQ state.
The [DMA/SPI HIL plan](../esy_hal/tests/hil/dma_spi_hil_plan.md) covers width/count
canaries, physical routing, Pico reload/live-count startup and retriggering,
finite dummy streams and failure retirement.

STM32F4 ADC DMA borrows the generic DMA owner's prepared native handle and links
it to the ADC; only the native ADC API starts the transfer. ADC MSP setup owns
clocks and analog pins, not a second DMA handle. DMA lengths are bytes containing
complete, aligned 16-bit sample groups. Finite completion retires hardware before
the terminal callback; circular callbacks retain the buffer and device lease.
Callbacks only notify the serialized owner and must not restart or retire ADC
work. If ISR cleanup cannot disable the stream, busy state and ownership remain
until a successful task-side abort, which delivers the deferred error. ADC deinit
returns status and preserves the handle on cleanup failure for retry. The shared
ADC IRQ and active ADC DMA streams must use the same configured priority.
Consumers provide the SDK ADC/ADC extension sources and the ADC/DMA IRQ provider.
See the [ADC DMA HIL plan](../esy_hal/tests/hil/adc_dma_hil_plan.md) for scan,
circular, error, abort and graph-lifecycle acceptance checks.
One task/startup owner serializes lifecycle across every ADC handle and PM
transition. The read-only `esy_hb_adc_validate_config()` rejects unsupported
sample-time requests and invalid controller/channel values before HAL board setup.
The read-only `esy_hb_adc_check_available()` preflight checks physical
ownership before HAL board setup; native init rechecks it before changing its
slot. The query is not a reservation against unsynchronized lifecycle callers.

Classic ESP32 binds one exclusive ADC1 handle at physical index 1 and channel
0..7. It accepts only `ESY_ADC_SAMPLETIME_DEFAULT`, using the native oneshot
provider with 12-bit results and 12 dB attenuation. Results are uncalibrated raw
counts in 0..4095; no voltage-conversion accuracy is implied. Explicit sampling
cycle requests return `NOT_SUPPORTED`; DEFAULT resolves to the previous
15-cycle profile on STM32F4 without renumbering the existing cycle enums or
changing configuration layouts. The generic default initializer now returns
DEFAULT. Software trigger with edge NONE is accepted; DMA, scan, continuous
mode and external triggers return `NOT_SUPPORTED`, and the capability mask is
zero. A synchronous read makes one native attempt; contention/conversion timeout
returns `TIMEOUT` without exposing an invalid result or adding retries.
The backend owns one SDK unit from init until successful deinit. Native
creation/deletion may allocate or block internally; the sampling path creates
no native unit. A channel-configuration failure retains the acquired unit for
HAL cleanup, and a deletion error retains its claim for retry while refusing
new samples. Independent native ADC users must not share this controller or pin.

Native SPI claims its physical controller before HAL board setup. Each handle
keeps that reservation through PM suspension, failed initialization cleanup and
native teardown errors. Native deinit returns status and retains the context
and claim; HAL resets board routing before the backend release operation.
Do not reclaim handle storage or callback arguments before successful cleanup.
Supported active DMA retires through its native terminal IRQ or successful abort.
A failed native stop retains buffers/callback storage for explicit task-side
abort. Destructive PM cleanup errors fault the graph and retain resources for
system recovery/reset.
ESP32 SPI explicitly requires native `idf::esp_driver_spi` and
`idf::esp_driver_gpio` targets. External native SDK users must not own or change
a controller selected for this HAL, before initialization or during its lifetime.
Master-driver preflight is not a complete SDK bus/slave ownership query.
STM32F4 supports only 8-bit and 16-bit frames and rejects other valid
widths with `NOT_SUPPORTED` before board setup instead of rounding them.
Consumers check the status-returning SPI deinit API before reusing storage.

Built-in Pico/STM32 SPI combines both DMA directions and the native physical
tail in the interrupt path. It retires callback storage, releases CS and reports
one terminal result without a process/poll call. The tail has a fixed maximum register-inspection
budget derived from native clock dividers; configurations exceeding it reject DMA; a failed stop retains the transaction for explicit abort.
ESP32 public SPI DMA is unsupported; its synchronous implementation may use
SDK-internal DMA. Middleware handles queued task-context result callbacks.

On supported Pico/STM32 SPI DMA paths, descriptors require nonzero byte lengths,
aligned complete 8-bit or 16-bit elements and equal lengths/widths when both
directions are provided. Circular and 32-bit-element requests are rejected.
Missing directions use internal null-memory dummy descriptors, not caller-supplied
invalid endpoints. ESP32 rejects public SPI DMA without starting native I/O,
retaining buffers or invoking completion callbacks.

ESP32 blocking native transactions accept at most 4094 TX bytes. RX of one through
four bytes uses SDK inline storage and exact-length copy-back; larger RX must be
a multiple of four, at most 4092 bytes. Other RX lengths return `NOT_SUPPORTED`
before native I/O/CS assertion. This avoids SDK-internal DMA word writes beyond
the caller's declared buffer without imposing hidden scratch storage or assuming
extra caller padding. The synchronous SDK operation may allocate temporary DMA
buffers for unsuitable caller memory; device/bus creation remains native SDK
lifecycle allocation. Pico 16-bit full-duplex uses the native simultaneous
write/read primitive, with exactly the requested number of halfword frames. See
the [SPI buffer HIL plan](../esy_hal/tests/hil/spi_buffer_contract_hil_plan.md).

PWM DMA retires automatically from native IRQs on Pico/STM32F4. STM32F4 selects
the configured channel's CCx DMA request and routes it to timer update events,
so a zero compare does not stall transfer requests. The backend restores its
previous CCDS selection after DMA retirement. Finite completion leaves PWM
running at its final compare after the next hardware preload update; it is not
an end-of-period notification. The owner explicitly disables output before idle
suspension or another DMA submission. Error and explicit abort stop output.
STM32F4 supports circular DMA; Pico rejects circular mode. Both keep callback
storage borrowed until successful terminal IRQ retirement or task abort. PWM deinit returns status and
preserves ownership on failure. See the
[PWM HIL plan](../esy_hal/tests/hil/stm32_pwm_hil_plan.md) for request, retirement,
clock and waveform verification.

Pico PWM tracks each physical slice and A/B channel, including GPIO aliases,
accepted frequency, running outputs, polarity and wrap handlers. Only the first
owner initializes the slice. Same-frequency A/B handles coexist; a second
physical channel claim or conflicting frequency is rejected. Per-GPIO output
override keeps a disabled/suspended channel quiet while the peer runs; final
owner release permits new timing. DMA requires a sole owner until terminal IRQ completion or successful abort
retires its borrow. All Pico PWM calls share one serialized task/core owner;
wrap callbacks cannot reenter control or lifecycle. Logical-slot preflight runs
before HAL board setup on every port. Different logical indices must use
different board GPIOs. Pico explicitly links PWM, GPIO, clock, IRQ and sync
SDK targets when PWM is enabled. See the [Pico PWM HIL plan](../esy_hal/tests/hil/pico_pwm_shared_slice_hil_plan.md).

ESP32 LEDC does not provide period-wrap notifications. A non-NULL wrap handler
returns `NOT_SUPPORTED` without retaining callback storage or installing an ISR;
NULL performs idempotent unregister/cleanup. Its zero `WRAP_IRQ` capability is
consistent with that behavior. See the
[ESP32 PWM HIL plan](../esy_hal/tests/hil/esp32_pwm_shared_timer_hil_plan.md).

Pico UART RX start/stop and ISR entry share lifecycle exclusion across cores;
the ISR never waits and processes at most 32 bytes per entry. Stop disables the
peripheral source and detaches stream pointers while retaining the static IRQ
dispatcher. RX callbacks must be bounded and must not reenter stream lifecycle.
All RX-stream UART IRQs installed on one Pico core must share their configured
priority. The first successful stream start reserves that core's UART priority
until reset; a conflicting start returns `BUSY` before borrowing caller storage
or changing native configuration. Native code must not change these priorities.
STM32 RX errors stop new peripheral DMA requests and retire supported access in ISR;
a failed native stop retains storage for task-side abort. Only successful stop releases caller storage, and failed stop must
be retried before deinit/reuse.

UART exposes native abort and direction-busy operations. STM32 finite RX
completes in DMA IRQ; TX retires DMA source access and finishes in UART TC IRQ.
Pico native TX/RX DMA completes automatically from DMA IRQ. TX certifies source
retirement; physical UART busy remains separately observable. Ordinary
native DMA rejects circular requests, which use the separate RX stream API.
Pico XIO DMA callbacks retire source storage automatically; TX busy also covers
queued PIO bytes and its final stop-bit drain. No UART process function exists.
Callbacks are ISR-safe and cannot reenter lifecycle. Retained errors require
explicit task-side abort before buffers or handles are reused. ESP32 exposes
synchronous operations and no DMA/stream callbacks.

The Pico XIO SPI backend implements three-wire write-then-read as one atomic
operation. It releases the shared data output after the final transmit sampling
edge and before the next clock edge, and leaves the data pin as an input until
the caller ends the transaction. Its blocking byte APIs support 2-bit to 8-bit
frames, and its blocking half-word APIs support 9-bit to 16-bit frames. Each
frame occupies the least-significant configured bits of its buffer element;
the configured bit order determines only its order on the wire. Three-wire
write-only blocking transfers remain on the configured PIO state machine to
avoid repeated pin-mux and state-machine restarts. Atomic write-then-read uses
an SIO transfer path for the direction turnaround. Three-wire mode cannot be
combined with `pin_trigger` because that turnaround path is not trigger-aware;
initialization rejects the configuration with `ESY_ERR_NOT_SUPPORTED`.
Slave mode requires an explicit CS input; an unused CS is rejected before any
PIO or DMA resource is claimed. Slave MISO additionally requires a distinct
`slave_cs_sm_index` in the same PIO instance. A two-instruction monitor runs
at `clk_sys` and writes only MISO output enable; the data SM writes only MISO
value. The [RP2040 datasheet, section 3.5.6.1](https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf)
defines independent arbitration for value and direction. Thus the monitor can
release MISO even while the data SM is blocked on SCK or an empty/full FIFO,
without a CPU/IRQ handler. CS uses its input synchronizer. Allow six system-clock
cycles plus pad propagation for deselection to high impedance and selection to
enable, including bus handoff and first-clock timing; the exact board limit
requires measurement. Both SMs and programs retire on failed init, suspend and
deinit, with MISO disabled before its monitor is released. Master and MISO-less
slave modes reserve no monitor. Slave MISO configurations assign the monitor SM
and use a consistent configuration layout across all translation units.
Blocking slave operations consume exactly one RX FIFO word per requested frame,
also for TX-only calls, so completion is retained after owner preemption and CS
deassertion without waiting for a second transaction. DMA accepts only 8-bit frames with 8-bit elements
or 16-bit frames with 16-bit elements, preserving the same frame values for both
bit orders. Other frame/element combinations return `ESY_ERR_NOT_SUPPORTED`
before DMA preparation; this restriction does not change blocking frame support.
Finite slave DMA likewise uses its completed RX count, including dummy RX for
TX-only, and an empty TX FIFO rather than a future CS/TXSTALL. Master DMA retains
its physical wire-drain check. The [SPI buffer HIL plan](../esy_hal/tests/hil/spi_buffer_contract_hil_plan.md)
covers completion accounting and wire timing.

`esy_hb_boot_jump_to_app()` receives only the application image/vector
base address. Any platform-specific handoff details, including vector-table
register updates, stack setup, cache/memory remapping, or bootloader handoff
mechanisms, belong inside the active HAL backend implementation.

STM32F4 direct handoff requires privileged Thread mode with MSP selected and a
caller-validated vector table. The port completes all C work before a stack-free
native trampoline installs the application's MSP and enters its Thumb reset
handler. IRQs stay masked until application startup restores its policy. This
does not reset CPU/peripheral state. See the
[STM32 handoff HIL plan](../esy_hal/tests/hil/stm32_system_handoff_hil_plan.md),
including native assembly checks at three optimization settings.

ESP32 interprets the image address as a physical app-partition offset. Its
handoff selects the corresponding native OTA partition and calls `esp_restart()`;
it never calls an application image as a function pointer. Status-returning
activation belongs to the boot backend: it validates native image contents,
updates `otadata`, and leaves reset to the consumer. Repeated ordinary resets
must not reselect the same partition, because selection restarts native trial
state. `esy_hb_system_get_pc()` continues to report an execution address.

The ESP32 boot backend derives the running image from
`esp_ota_get_running_partition()`, reports update eligibility, validates board
image/metadata regions against the partition table, and confirms the running
image through the native SDK. ESP-IDF owns boot selection and rollback; the
middleware's metadata cannot override an aborted native trial. A pending trial
rejects another update before image erasure. Confirmation rejects a different
already-selected next partition. Native rollback must be enabled, and Flash
encryption is unsupported by this update path. See
[ESP32 managed boot and OTA](../docs/integration/flash_geometry.md#esp32-managed-boot-and-ota)
for the required consumer configuration and failure semantics.

STM32F4 logical timers reserve complete TIM2/TIM5 peripherals, CC1 and their IRQ
lifecycles. Both counters run freely at exactly 1 MHz over their full 32-bit
range; TIM2 serves MS input and TIM5 serves US input. Eight logical slots each
retain an independent deadline. Only the earliest compare generates an IRQ;
there is no periodic software tick. Canceling a slot leaves other timers intact.
Periodic late deliveries are coalesced with phase preserved, and callbacks may
cancel or create timers without the returning dispatcher overwriting them.
Each IRQ dispatches at most one callback per slot outside local IRQ masking.
The clock must stay fixed/running; initialization derives APB1/TIMPRE frequency
and rejects inexact 1 MHz division. Both timer IRQs share a priority while any
slot or dispatcher is active; conflicting priorities fail without changing live
timers. This keeps their HAL callback publication from nesting. Maximum interval
and maximum unserviced IRQ delay are respectively `INT32_MAX` microseconds and
strictly less than 2^31 microseconds. Timer variants missing TIM2/TIM5 fail closed.
Do not share either resource with capture, PWM or another IRQ owner. STOP/clock
changes require owner-controlled retirement and reinitialization.

ESP32 logical timers deliver callbacks from the SDK timer task. One owner core
serializes lifecycle calls with that task. A one-shot captures its callback and
retires its native timer before notifying the user. No dispatcher reads or
writes the old slot after user callback return, so cancel/start replacement
cannot be undone by an older callback. Native stop/delete failure disables user
notifications and preserves the native handle, reserving the slot for cancel or
deinit retry. `is_active()` reports retained cleanup as active; init after deinit
returns `ESY_ERR_BUSY` until all native handles retire. Eight occupied slots
return `ESY_ERR_NO_MEM`. Periodic intervals below 50 microseconds and non-default
clock sources return `NOT_SUPPORTED` before allocation; the requested period is
never silently clamped. The minimum is not a callback-latency guarantee. See the
[ESP32 timer HIL plan](../esy_hal/tests/hil/esp32_timer_lifecycle_hil_plan.md).

Supported Timer Capture providers claim their index and physical GPIO/timer before board setup;
conflicting initialization cannot reset the current owner's pin. Board hooks
must validate and preserve the configured resource identity, returning false
without changes for unsupported mappings. Suspend retires native IRQs and resets
board routing while preserving the reservation. Final status-returning deinit
releases it only after successful cleanup. Pico has no native capture provider
and returns `NOT_SUPPORTED` before board configuration.
ESP32 uses native `idf::esp_driver_mcpwm` and `idf::esp_driver_gpio` targets;
it does not borrow the GPIO IRQ service or ESP Timer. Six logical reservations
map to MCPWM groups as `inner.tim_id = phys_index / 3` and
`inner.channel = phys_index % 3`; the SDK chooses the physical channel within
the group. Three reservations share one timer. Each group's lifecycle is
externally serialized on one core, with matching IRQ priorities; conflicting
claims fail before native setup. Cleanup is staged, retaining claims, callbacks
and native objects after failure for a same-owner retry. The final channel
stops and releases the shared timer. `CONFIG_MCPWM_ISR_IRAM_SAFE` is unsupported
because generic callbacks/storage have no cache-disabled placement contract.
Its lifecycle requires an ordinary schedulable context, checked through the
startup-installed system execution callbacks. `esy_os_init()` installs those
callbacks for its OS adapter; native-SDK-only startup supplies them through
`esy_hal_system_set_context_callbacks()`. Missing or nonblocking context returns
`STATE` before allocation; the backend has no OS API or target dependency.
Destructive PM cleanup failure faults the device graph, retaining reservations
and storage for system recovery/reset; the faulted registry cannot be unbound
locally. Unregistered cleanup failures permit ordinary deinit retry.
The ESP32 capture ISR owns timestamp history and atomically publishes 32-bit microsecond
values and intervals. Queries never combine two independently changing history
samples; separate timestamp and interval queries need not describe one edge.
Both queries initially return zero; the first edge still has zero interval.
It computes intervals by subtracting raw 32-bit ticks before conversion to
microseconds. At an 80 MHz APB clock the native counter wraps every
53.6870912 seconds; edges must be strictly less than one native wrap apart.
Converted timestamps wrap with that native domain, not every 2^32 microseconds;
use the interval query rather than subtracting converted timestamps across wrap.
The native timer may already run for another channel when initialized/resumed;
history resets, but the shared counter does not.

STM32F4 timer capture owns one complete timer and its IRQ lifecycle per handle.
The board supplies the timer clock and GPIO setup through HAL MSP hooks and
routes the IRQ to the configured physical capture index. Do not share that
timer or IRQ lifecycle with another driver. Initialization derives an exact
1 MHz counter from APB1/APB2 and, where present, TIMPRE; unsupported clock rates
fail before hardware initialization. Keep the clock tree fixed while capturing.
TIM2/TIM5 wrap every 2^32 microseconds; other supported timers wrap every 2^16
microseconds. Intervals are modulo that width, so consecutive edges must be
less than one wrap apart. The first capture has no interval and reports zero.
Initialization/resume resets the capture time domain. Consumers account for
these resource and range limits. Returned values and callback timestamps are
microseconds; no application-side timer-tick conversion is needed.

`check_esy_hal_backend_contracts.sh` checks each platform source file
against its matching public header. It also rejects status-returning placeholder
stubs that only discard parameters and return `ESY_OK`.

## Resource Model

HAL backend handles and configuration objects are caller-owned. Backend sources
do not directly call or alias heap allocators. Native provider APIs can still
allocate SDK-owned state; the source allocation gate cannot inspect or bound
that cost. Resource creation/retirement belongs to the documented owner lifecycle
or control operation and does not make that operation hard-real-time or ISR-safe.

| Provider resource | Creation and retirement boundary |
| --- | --- |
| ESP32 I2C bus/device | Init creates one bus and addressless device; deinit removes the device before the bus. Failed deletion retains the native owner for retry. Transfers create neither. |
| ESP32 UART driver | Init installs the native driver and its buffers; successful deinit retires I/O before deleting it. Failed cleanup retains the handle/reservation. |
| ESP32 SPI bus/device | Init creates the native master bus/device; deinit removes resources only after pending I/O has retired. SDK failure retains remaining ownership. |
| ESP32 logical timer | Start creates a native `esp_timer`; cancel/deinit retire it. A one-shot's SDK task callback also performs native retirement before invoking user code. This is a provider lifecycle-completion hook; failed deletion keeps the native handle for later cancel/deinit retry. |

Use task-context lifecycle calls as documented by each API, serialize their owner,
and keep callback context alive until retirement. Measure provider heap high-water
usage, repeated lifecycle fragmentation and worst-case create/delete latency in
the native consumer. Host coverage and source contract checks cover the wrapper
logic and source structure separately.

## Verification

The following behavior must be verified through source contract checks,
consumer-project compile jobs, hardware smoke/HIL tests, or an equivalent
documented review path:

- argument validation and error-code mapping;
- handle lifecycle and repeated init/deinit behavior;
- resource tables, busy state, and ownership tracking;
- callback dispatch and ISR-to-task handoff;
- GPIO interrupt registration release during GPIO deinit so the same line can
  be attached again;
- timeout behavior and remaining-count queries;
- three-wire direction turnaround timing and readback on hardware;
- unsupported capability reporting.

The [EXTI HIL plan](../esy_hal/tests/hil/stm32_gpio_exti_hil_plan.md) covers peer
delivery, pending events, line/priority conflicts and callback retirement.
The [ESP32 shared PWM timer HIL plan](../esy_hal/tests/hil/esp32_pwm_shared_timer_hil_plan.md)
covers conflicting requests, peer preservation, final-owner release and retry
after setup/cleanup failure. Capability qualification should query each enabled
peripheral and exercise advertised operations with valid board resources,
including role-dependent zero masks and unsupported optional operations. Record
callback context and measured timing separately from the capability mask.

The [timer capture HIL plan](../esy_hal/tests/hil/timer_capture_hil_plan.md)
covers ownership, PM cleanup, ISR publication, STM32F4 timebase and wraparound.
A successful target
compile establishes API/SDK compatibility, not measured capture accuracy.
The [PWM HIL plan](../esy_hal/tests/hil/stm32_pwm_hil_plan.md) covers actual
APB1/APB2 divider and TIMPRE clocks, frequency quantization, 16/32-bit period
limits and duty preservation. PWM derives its kernel clock before computing
prescaler/period values; invalid frequencies and unrepresentable compare values
are rejected before changing live registers. Keep the timer clock fixed and
reserve the complete physical timer when configuring its period.
The [logical timer HIL plan](../esy_hal/tests/hil/logical_timer_hil_plan.md) covers
independent periods, cancellation, callback reuse, compare races, full capacity,
32-bit wrap, APB/TIMPRE clocks and failure cleanup.

For hardware smoke tests, exercise GPIO, UART and SPI loopback, I2C scan, timer
callbacks, Flash erase/write/read, and watchdog reset. Review and exercise backend
state machines and callback dispatch, including ESP32 UART and ESP32 timer, with
the selected SDK and board. Check Flash geometry and erase boundaries on the
target. For ESP32 managed boot/OTA, test activation, confirmation, failed
diagnostics, unexpected reset and power-loss recovery. Source and consumer compile
checks establish API/SDK integration rather than those hardware outcomes.

## Known Limits

- ESP32 Flash metadata depends on the ESP-IDF chip-driver definition; consumer
  builds must verify that SDK boundary when upgrading ESP-IDF.
- Flash encryption is unsupported by the current raw Flash update path.

STM32F4 PWM wrap dispatch checks the owned UPDATE source and pending flag,
acknowledges before callback, and disables UPDATE/NVIC when unregistering or
deinitializing. The timer is exclusively owned by the backend PWM instance.
