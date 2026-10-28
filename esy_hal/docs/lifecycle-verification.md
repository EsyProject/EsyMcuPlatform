<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# HAL lifecycle verification

This document describes HAL lifecycle contracts and their verification coverage,
including the HAL backend and common interfaces used by those contracts.
Host and source checks cover software behavior; physical timing follows the
selected provider and board.

## Lifecycle contracts

Initialize handles with `{0}`, then call configuration defaults, then
set overrides. Defaults replace all fields. DMA descriptors are copied;
payloads and callback arguments remain borrowed until terminal retirement.

Registered ADC continuous mode and registered PWM loop/circular DMA retain
their device lease until successful owner abort or terminal error retirement.
The portable ADC/PWM abort operations retire native buffer and callback access
before releasing that lease. A failed abort retains ownership for retry. I2C
circular DMA and ADC circular descriptors passed to one-shot APIs are rejected.

Direct ADC and USB backend handles also require `{0}` before first init. ADC
init failure may retain native state until successful deinit; USB binding init
failure retains no new owner. Pico USB handle deinit releases only the event
binding. Its separate shared TinyUSB owner controls class users, dispatch and
hardware shutdown; handle detachment does not disconnect or stop the controller.

Pico Flash write accepts source bytes only from the main on-chip SRAM mapping.
It rejects XIP, external-memory and SRAM-alias sources before the safe hook runs.
The caller supplies any required copy buffer. Pico secondary-core launch belongs
to one core-0 startup/task owner and succeeds only once per boot; later starts
return BUSY without resetting the live core. An unattainable Pico clock request
returns NOT_SUPPORTED through the SDK's non-required clock-selection path.

## Contract verification coverage

| Contract | Verification coverage |
| --- | --- |
| XIO UART embeds its shared adapter and dispatches DMA through the generic wrapper. | Registered XIO generic DMA regression under ASan. |
| XIO SPI user callbacks run outside ISR after both DMA channels and PIO retire; abort retires without a callback. | HAL dispatch tests, Pico XIO cross-compilation and physical-retirement scenarios below. |
| XIO UART direction slots are initialized before assembly. | TX-only/RX-only and retry regression. |
| Configuration defaults initialize every byte, including null callbacks. | Different nonzero storage patterns produce identical defaults. |
| UART source assembly follows the enabled directions. | Real-target TX-only and RX-only consumer links detect disabled HAL backend symbol references. |
| Feature assembly validates dependencies and supports empty library targets. | All-disabled, missing system/DMA, supported timer and absent ADC backend cases. |
| Private hardware resume retains graph identity, including on failure. | Resume-failure graph fault regression. |
| DMA submission copies descriptors before adapting callbacks and metadata. | Submitted-descriptor fake, byte-for-byte caller comparison and descriptor reuse. |
| Accepted cyclic transfers retain their usage reference; I2C rejects circular DMA transactions. | Multiple cycles, busy second submit, abort and resubmit regression. |
| Board DMA mapping is validated before backend dispatch. | Mapping rejection across UART, I2C, ADC and PWM. |
| Repeated init rejects assembled or registered handles without replacing state. | Reinit, failed init, retry, deinit/reinit and registered ADC regression. |
| GPIO requires zero-initialized storage before first init. | GPIO lifecycle tests and public header checks. |
| Physical GPIO ownership excludes aliases from access and lifecycle changes. | Alias write, deinit and competing init are rejected while claimed. |
| Registered SPI/I2C resume uses the accepted runtime bitrate. | Fake observes changed bitrate on reinitialization. |
| Timer slots remain reserved during backend startup and dispatch. | Immediate one-shot callback starts another timer; cancellation removes the correct deadline. |
| UART direction assembly unwinds both directions on partial failure. | Injected RX failure, null context/ops checks and retry. |
| Destructive pin rollback and resume failures fault the device graph. | UART pin-state and resume failure injection. |
| PWM registration tracks output state and retains loop leases until retirement. | Registered loop, callback lease, failed/successful abort and disable-before-suspend regressions in `test_esy_hal_device_adapters.c`. |
| Continuous ADC retains its lease until successful abort or terminal error retirement. | Registered continuous, failed/successful abort and one-shot lease regressions in `test_esy_hal_device_adapters.c`. |
| A returned backend application jump is reported as a failed handoff. | Bootloader fallback/handoff tests. |
| Pico one-shot slots become inactive on terminal callback and remain reserved during startup/dispatch. | Pico HAL backend compilation and repeated physical alarm scenario below. |
| Wakeup reinit preserves the initialized alarm's claim. | Reinit preserves the original alarm; deinit/reinit selects another. |
| Timer Capture capability queries are part of the public contract. | NULL, missing operation and initialized query tests; C/C++ header checks. |

## Reproducible host checks

Run from the EsyMcuPlatform repository root:

```sh
python3 -B esy_hal/tests/test_cmake_consumer.py
python3 -B esy_hal/tests/test_coverage_gate.py
ruby tools/quality/run_module_tests.rb esy_hal/tests /tmp/platform-tests test:all
ruby tools/quality/run_module_tests.rb esy_hal/tests /tmp/platform-sanitizer test:all --sanitizer
ruby tools/quality/run_module_tests.rb esy_hal/tests /tmp/platform-gcov gcov:all
python3 -B esy_hal/tests/check_coverage.py /tmp/platform-gcov/artifacts/gcov/gcovr/coverage.json
tools/checks/check_source_contracts.sh esy_hal
tools/checks/check_esy_common_contracts.sh
tools/checks/check_esy_hal_backend_contracts.sh
```

A separate consumer target with real Pico SDK headers should enable system,
timer, low power, DMA and all XIO directions in its `esy_feature_import.cmake`
and compile both library targets. Host fakes cannot replace this port check.
The actual consuming firmware must also build with its own features and board.

## Hardware verification scenarios

- Pico XIO SPI: exercise TX-first/RX-first completion, master/slave and trigger
  mode, 8/16/32-bit DMA widths, DMA bus error, missing external clock and abort.
  Assert no callback while either channel accesses buffers; ISR never waits on
  the wire; poll reports BUSY until wire retirement; terminal counts are bytes.
  Verify CS hold/release and the first transaction following abort on a logic
  analyzer. Caller must poll or abort stalled transfers; no timeout is invented.
- Pico logical timers: execute more than eight successive one-shot alarms,
  check inactive status after each callback and exercise nested start/cancel
  before and after startup returns on the owner core.
- Wakeup alarms: reject reinit to another alarm, then abort/finish/deinit and
  verify both the original resource and a newly selected resource can be claimed.
- STM32F4: verify ordinary UART circular DMA rejection, dedicated RX-stream
  stop/error retirement and real peripheral initialization cleanup. ESP32:
  compile public types and wrappers against the selected native provider.
- Pico Flash/system: execute the source-range rejection and recoverable clock
  failure cases in [the Flash/system HIL plan](../tests/hil/pico_flash_system_hil_plan.md).
  Repeat core-start and USB callback-binding cases in their dedicated HIL plans.
- PM: check board pin restoration, physical PWM output and retained SPI/I2C
  bitrates through real sleep/resume. Host tests validate graph bookkeeping,
  not pad voltage, DMA retirement timing or low-power current.

Execution owners drain or abort DMA and quiesce callbacks before OTA Flash
operations or destruction. Host coverage measures the facade and lifecycle logic.
