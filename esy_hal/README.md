<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# esy_hal

## Purpose

`esy_hal` is the public hardware abstraction layer for EsyMcuPlatform. It provides
handle-oriented wrappers for system, GPIO, device GPIO PM, DMA, UART, SPI, I2C, CAN,
PWM, ADC, timers, flash, watchdog, USB device, Wi-Fi, low-power, multicore, and
XIO-backed peripherals.

The [calendar RTC](rtc.md) adds provider-backed UTC reads/writes, one-shot
alarms and optional calibration, including a portable software provider.

The [CAN facade](docs/can.md) adds owner-driven frame transfer, mask filters,
error diagnostics and explicit bus-off recovery. `ENABLE_ESY_CAN` currently
selects the STM32F4 Classical CAN backend without OS, DMA or native CAN-driver
dependencies; other platform selections fail configuration. One handle reserves
the complete shared CAN1/CAN2 resource domain. Hardware verification follows the
[CAN plan](tests/hil/can_hil_plan.md).

Pico queue initialization takes an explicit backing `buf_size` and requires
`item_size * (depth + 1)` bytes for `depth` usable entries. Pass the actual
capacity to initialization. Pico watchdog enable preserves
its configured timeout/debug policy; reset acknowledgement changes only the
reported status for this boot. Generic backend XIO program/SM objects require
`{0}` before first use. See the
[Pico multicore/watchdog/XIO plan](tests/hil/pico_multicore_watchdog_xio_hil_plan.md)
for queue wrap, IRQ-core, nonzero PIO placement and trigger-waveform acceptance.

HAL APIs are intentionally thin: they validate public inputs, assemble
backend ops tables, route board resource configuration, and hide vendor SDK
details behind EsyMcuPlatform-owned types.

## Public Surface

Public headers live under `esy_hal/include/esy_hal/`.

The module exposes wrapper APIs for enabled hardware features. Public headers
expose EsyMcuPlatform and standard C types only; vendor SDK, board and backend
headers stay behind implementation boundaries. Middleware uses HAL for hardware
access and must not depend on board or backend interfaces directly.

## Integration

Include public headers from `esy_hal/include/esy_hal/` and link the
`esy_hal` CMake target.

The consuming project selects platform features through its own
`esy_feature_import.cmake`. `esy_hal` consumes `ENABLE_ESY_*` variables; it does
not define feature options.

Sources are selected in `src/CMakeLists.txt`:

```cmake
if(ENABLE_ESY_UART)
    list(APPEND MODULE_SRCS esy_hal_uart.c)
endif()
```

XIO-backed HAL sources are selected in `src/xio/CMakeLists.txt` when
`ENABLE_ESY_XIO` is enabled. TX-only and RX-only UART builds select a disabled
implementation for the other direction and do not link that direction's HAL backend.
Shared UART and SPI I/O sources are assembled when either their native driver
or their XIO transport is enabled. In particular, XIO-only SPI links the generic
blocking, DMA, poll and abort APIs without enabling the native SPI driver.
Manual source lists must include `src/esy_hal_spi_io.c` for either SPI transport.
`ENABLE_ESY_TIMER` requires `ENABLE_ESY_SYSTEM`. `ENABLE_ESY_DMA` selects the
generic `esy_hal_dma_*` controller API only. UART, SPI, I2C, PWM, ADC and XIO
can be enabled independently; their native DMA support and shared physical-resource
arbiter are assembled with the peripheral implementation. Disabling the generic
facade does not disable peripheral DMA entry points or change capability masks.
Boot hardware layout assembly requires Flash and system support. ADC currently
requires the STM32F4 backend. Invalid combinations fail during configuration.
All-disabled builds retain linkable library targets without exporting dummy APIs.

## Contracts

- `esy_hal_wifi_get_network_runtime()` returns native network progress and paired
  entry/exit hooks through `esy_wifi_network_runtime_t`, without exposing SDK,
  lwIP or network-middleware types. The descriptor query is legal before native
  initialization and performs no initialization, allocation or station-state
  update. Copy its callbacks/context into the network provider configuration.
  Use them only on the provider's serialized non-ISR owner while native Wi-Fi
  remains alive; they do not make network borrowers safe for multiple tasks.
  Progress dispatches native work and callbacks without running station
  connection/retry bookkeeping. Quiesce and detach every network borrower before
  Wi-Fi stop/deinit. Pico hooks defensively do nothing outside native lifetime;
  this does not extend the lifetime of borrowed endpoints.
- UART, SPI, I2C, PWM, ADC, timer capture, USB device and inter-core queues bind
  a shared `const` provider-operations table through `handle.ops`. The HAL owns
  this binding; applications use public HAL APIs rather than editing or directly
  dispatching the table. Custom providers keep their immutable table and borrowed
  context alive for the complete attachment lifetime. Optional NULL hooks retain
  their documented unsupported-operation results, including on an empty handle.
- Peripheral PM links, pin teardown flags, trigger configuration and DMA callback
  ownership live in opaque `hal` storage. GPIO configuration, wake ownership and
  PM state are opaque as well. These bytes are not an integration interface;
  only the HAL and its own tests include the corresponding private headers.
  Backend `ctx` and `backend` storage remain a separate port contract, not
  application state. HAL, backend and consumer translation units must use
  consistent public handle layouts.
- `esy_hal_gpio_is_initialized()` and `esy_hal_gpio_get_config()` provide
  serialized lifecycle/configuration observations. Configuration snapshots
  describe the original normal state even while a PM profile selects another
  electrical state; obtaining a snapshot does not authorize I/O.
- `esy_hal_spi_get_device()` borrows the registered parent identity until
  unregister. A child PM callback that must write its active SPI parent uses
  `esy_hal_spi_write_blocking_for_pm()` with its own transitioning device.
  This entry checks the same registry, a declared direct dependency, an active
  parent and absence of outstanding asynchronous ownership before dispatch.
  It does not acquire a new graph reference. Calls remain synchronous and
  serialized by the graph owner, with provider latency included in the PM
  budget; no ISR, RTOS wait or ordinary-operation bypass is authorized.
- An RX stream exclusively borrows reception until stop succeeds, including
  retained ownership after a failed start or stop. Blocking RX, ordinary RX DMA,
  deinit and registration reject that borrow even on unregistered handles.
  Independent TX remains available. The lifecycle/I/O owner serializes these
  operations; callbacks do not modify the HAL stream-ownership flag.
- The caller owns all public handle storage and configuration objects. Initialize
  peripheral and wakeup handles with `{0}` before their first init. Repeated init
  returns `ESY_ERR_STATE`; never clear a live handle to bypass cleanup. Successful
  cleanup after failed initialization permits retry; preserve a handle whose
  cleanup reports failure and retry its deinit. Successful
  deinit returns peripheral handles to zero; registered handles must first be
  unregistered. Serialize lifecycle operations and stop asynchronous work first.
- Every `default_cfg` writes the entire configuration, including callbacks and
  board metadata. Apply overrides **after** calling it. It does not preserve
  existing field values. Board completion uses a private copy of const configs.
- Peripheral `get_caps` masks use the public `ESY_<peripheral>_CAP_*` enums in
  `esy_common/defs`; each bit has the same meaning across ports. Query an
  initialized handle under its lifecycle owner. Bits describe implemented
  operations for that role, subject to board routes and runtime limits. DMA
  bits name transfer APIs and do not promise native DMA or asynchronous return.
  Zero is valid and unknown bits must be ignored. Use the public constants
  when testing capabilities. See the
  [backend capability mapping](../esy_hal_backend/README.md#public-capability-masks).
- Pico/ESP32 GPIO IRQ and wake-source lifecycle, including PM detach and final
  deinit, runs on the core used by the first GPIO IRQ attachment. Keep the owner
  pinned to that core and serialize lifecycle calls. An occupied pin returns
  `BUSY`; cross-core attach returns `STATE`. ESP32 does not adopt an external
  GPIO ISR service. Do not change native registrations outside the backend.
- ESP32 electrical-state application, configured-state restore and GPIO PM
  selection return `BUSY` while that pin has an attached IRQ or wake binding.
  Rejection preserves native IRQ/wake configuration and callback ownership.
  Deinitialize an ordinary IRQ handle, or successfully disable its wake source,
  before reconfiguration; do not put attached IRQ pins into changing PM profiles.
- A GPIO wake-enable error can retain native wake/IRQ ownership and the callback
  argument when setup rollback fails. Keep the handle and argument alive until
  `esy_hal_gpio_disable_wakeup()` or `esy_hal_gpio_deinit()` returns `ESY_OK`.
  Deinit also handles attempts rejected before borrowing resources. Retry failed
  cleanup before enabling again; never clear the live handle to bypass it. See
  the [ESP32 wake ownership HIL procedure](tests/hil/esp32_irq_hil_plan.md#gpio-wake-ownership-procedure).
- STM32F4 GPIO EXTI registrations own individual lines and their callback
  arguments until deinit or wake-source detach returns. Temporary disable keeps
  ownership; cleanup leaves peer lines enabled. Lines in a shared NVIC group
  must use one priority, including temporarily disabled registrations. Line or
  priority conflicts return `ESY_ERR_STATE` without changing existing bindings.
  Lifecycle/configuration is task-only; callbacks only notify the owner. See
  the [EXTI HIL plan](tests/hil/stm32_gpio_exti_hil_plan.md).
- ESP32 PWM channel pairs share one LEDC timer. Equal requested frequencies may
  coexist; conflicting init or changing a shared timer's frequency returns
  `ESY_ERR_BUSY`. The final owner releases the native timer, permitting reinit
  at a new frequency. Failed cleanup retains ownership for deinit retry. One
  owner must serialize calls across all channels. See the
  [shared timer HIL plan](tests/hil/esp32_pwm_shared_timer_hil_plan.md).
- DMA submission copies descriptors. The descriptor may be reused after submit,
  but buffers and callback arguments remain borrowed through terminal completion
  or successful abort. Accepted circular callbacks report cycles and retain the
  graph reference until abort/error. I2C DMA transactions reject circular mode.
- Pico I2C supports blocking and combined master transactions. Its ordinary DMA
  read/write APIs return `ESY_ERR_NOT_SUPPORTED` without starting a transaction
  or retaining buffers/callbacks; the capability mask omits `DMA_TRANSFER`.
  Consumers must select blocking/combined transfers explicitly. See the
  [Pico I2C HIL plan](tests/hil/pico_i2c_hil_plan.md).
- STM32F4 I2C DMA also returns `ESY_ERR_NOT_SUPPORTED` without retaining storage
  or touching DMA/bus state. The capability mask omits `DMA_TRANSFER`; select
  blocking/transfer APIs explicitly. See the
  [STM32F4 I2C HIL plan](tests/hil/stm32f4_i2c_hil_plan.md).
- I2C has one handle owner per physical controller, including while its device
  adapter is suspended. A competing init returns `ESY_ERR_BUSY` before board
  pins or native state change. Serialize controller lifecycle and I/O on one
  owner; callbacks only notify that owner. I2C deinit returns `esy_err_t`:
  reset pins and reclaim handle storage only after success. Native cleanup
  failure preserves ownership for deinit retry, including partial init cleanup.
  A failed PM cleanup can leave partially released native state and faults the
  graph with `ESY_ERR_STATE`; retain storage and follow system recovery instead
  of resuming ordinary I/O. Check deinit results before reusing storage.
- All I2C transfer address parameters and provider vtable entries use `uint16_t`
  raw addresses: 0..0x7f for 7-bit mode, 0..0x3ff for supported 10-bit mode.
  Custom provider signatures use the same address width. Combined-transfer
  `timeout_ms` is one total TX/RX budget, with a finite 1000 ms default for zero;
  values above `INT32_MAX` reject and expiry returns `ESY_ERR_TIMEOUT`.
- Native UART controllers are exclusively claimed before board/native setup,
  including throughout PM suspension. Competing initialization returns `BUSY`
  without changing the current owner's pins or configuration. Native UART
  `esy_hal_uart_deinit()` returns status: preserve the handle and retry on
  failure. ESP32 has no platform-owned UART task; finite RX must retire through
  owner poll or abort. Successful cleanup resets board pins
  before releasing the controller claim. XIO UART keeps its own typed deinit.
- Timer Capture reserves its logical index and physical GPIO/timer before board
  configuration, retaining that reservation during PM suspension and failed
  cleanup. Supply the physical identity in `config.inner`; the board hook must
  preserve it and return false without hardware changes for a mismatched route.
  `esy_hal_timer_capture_deinit()` returns status; reclaim storage only after
  success. Pico/ESP32 IRQ lifecycle runs on the core used by the first GPIO IRQ
  attachment. Their ISR publishes complete 32-bit timestamp and interval values;
  separate query calls can observe different edges. Queries report zero before
  a first timestamp or second edge, respectively. Intervals require consecutive
  edges less than one counter wrap apart.
  A failed PM teardown faults the graph and requires system recovery/reset;
  retain storage and reservations. A faulted registry cannot be locally unbound.
  Pico/ESP32 privately link GPIO IRQ support even with public GPIO disabled.
- UART DMA ownership is tracked on every handle, independently of optional
  device registration. A terminal callback holds the handle and optional graph
  lease through its return. Abort retires the peripheral request, native DMA and
  pending callback access before releasing storage; cleanup failure retains
  ownership for retry. A failed submit can also retain storage: inspect busy
  state and continue poll/abort before reuse. Callbacks must not reenter UART
  I/O or lifecycle. Pico/STM32F4 ordinary TX/RX supports finite DMA only;
  circular submissions return `NOT_SUPPORTED`. STM32F4 enables DMAT/DMAR on
  acceptance and clears them on retirement. Continuous RX uses the stream API.
  Stream start requires a matching provider stop operation. `INVALID_ARG`,
  `NOT_SUPPORTED` and `NOT_IMPLEMENTED` leave storage/callbacks unborrowed.
  Other failed starts may retain them, even without device registration; keep
  storage and callback context alive and retry stream stop until it succeeds.
  For registered handles the facade first tries stop after such a failure:
  successful cleanup releases the graph reference and preserves the start error;
  failed cleanup returns its error and retains the reference through stop retry.
  Drive `esy_hal_uart_poll()` from the serialized owner while work remains.
  Pico/STM32 and Pico XIO IRQs publish events; polling retires DMA and delivers
  callbacks.
  STM32 TX success also waits for physical TC. SDK abort and callback duration
  contribute to each poll; it is not a time deadline. Pico XIO ordinary DMA
  rejects circular mode; its TX completion certifies source retirement while
  queued PIO bytes may still transmit. XIO TX busy includes that remaining wire
  activity: after FIFO empty, the provider discards old TXSTALL and requires a
  fresh stall at blocking PULL after the final stop-bit delay. Blocking TX waits
  for physical drain. Runtime PM skips undrained TX; system suspend returns BUSY,
  and deinit retains the complete handle until TX is idle. Keep clocks/SM running
  and serialize these checks with the I/O owner. ESP32 retains inline TX; finite RX is
  advanced by owner polling with a maximum 1024-byte budget and one terminal
  callback per poll. No platform task/event queue is created. Poll regularly
  and provide sender backpressure before the SDK ring fills. ESP32 callbacks
  run inside poll and cannot reenter UART; blocking calls busy-poll hardware.
  ESP32 finite RX reports SDK read failures through its error callback; native
  parity/frame/overflow event notifications are not exposed. Consumers must
  enforce protocol integrity, receive deadlines and sender flow control.
  Direct DMA consumers drive owner polling. Logger, UART TX scheduler and
  the UART protocol channel drive it from their processing paths. See the [UART DMA HIL plan](tests/hil/uart_dma_hil_plan.md).
- Direct generic DMA requires serialized owner calls to `esy_hal_dma_poll()`.
  Init validates the route list without reserving hardware. Prepare claims the
  physical resource globally across generic controllers and peripheral adapters;
  a competing prepare returns BUSY without replacing the descriptor or callbacks.
  Keep a live controller at a stable address. Failed start or retirement retains
  the claim; only successful abort/terminal retirement permits reuse. Aborting
  an inactive route of another controller is a no-op and cannot cancel its owner.
  IRQ callbacks only publish events; half/circular notifications and terminal
  callbacks run in the polling task, with the device reference held through
  callback return. Terminal completion retires hardware and
  pending callback access before returning borrowed buffers. Errors take priority
  over completion. Failed retirement retains the transfer and any device usage
  reference for another poll or abort attempt. This applies to registered and
  unregistered DMA controllers; every consumer drives owner polling.
  A rejected batch start retains its
  prepared routes and their ownership for retry or abort.
  Each poll visits at most the configured route count. SDK hardware abort,
  cross-core callback retirement and user callbacks contribute to its duration;
  there is no wall-clock execution bound. Qualify provider latency on the target.
- ADC DMA lengths are bytes in a two-byte-aligned, DMA-accessible 16-bit sample
  buffer; scans contain complete interleaved channel sequences. STM32F4 borrows
  the generic DMA owner's stream handle and starts it once through the native
  ADC provider. Finite completion retires hardware before callback; circular
  completion retains the buffer and registered graph lease until
  `esy_hal_adc_abort_dma()` or terminal error. Failed cleanup preserves ownership
  for task-side retry, including a busy result after failed submission. ADC
  lifecycle is task-only; callbacks only notify the owner. Trigger selection uses
  `esy_adc_trigger_edge_t` and `esy_adc_trigger_source_t` throughout the HAL and
  backend interfaces, and survives registered resume. A partial suspend cleanup
  failure faults the device graph.
  One-shot ADC APIs reject circular descriptors. ADC/PWM registration rejects
  active DMA.
  One owner serializes lifecycle across all ADC handles and PM transitions.
  Initialization checks physical-controller availability before board pin setup;
  an occupied controller returns `BUSY` without changing the existing owner.
- PWM DMA uses owner-driven `esy_hal_pwm_poll()` for notifications and terminal
  retirement. ISR callbacks only publish events; task-side polling retires the
  DMA callback before releasing storage or a graph reference. Circular cycles
  retain that reference until successful abort or terminal error. Check the
  status from `esy_hal_pwm_abort_dma()` and the status-returning PWM deinit;
  cleanup failure retains the handle and borrowed storage for retry. Continue
  polling every accepted transfer.
- PWM output state survives device registration. DMA start conservatively marks
  output enabled; explicitly disable it after completion before runtime suspend.
- PWM initialization checks logical-slot availability before board pin setup.
  Board mappings must assign different GPIOs to different logical indices.
  Pico additionally claims the physical `(slice, channel)`: A/B may share a
  slice at the same requested frequency, while conflicting frequency or channel
  aliases return `BUSY`. Enable, polarity, suspend and deinit preserve the peer.
  DMA requires exclusive slice ownership through retirement. All Pico PWM
  calls use one task/core owner; wrap callbacks only notify that owner. See the
  [Pico PWM HIL plan](tests/hil/pico_pwm_shared_slice_hil_plan.md).
- Native SPI reserves one handle per physical controller before configuring
  board pins or native hardware. Competing initialization returns `BUSY` without
  changing the existing owner. PM suspension retains this reservation.
  `esy_hal_spi_deinit()` returns status: finish polling/abort retirement and
  check cleanup success before reclaiming storage. Native cleanup failure keeps
  the handle, pins and claim available for retry. Board routing is reset before
  the reservation is released. XIO SPI keeps its own typed lifecycle.
  A destructive PM cleanup or failed rollback faults the graph and requires
  system recovery/reset; a faulted registry cannot be locally unbound.
  STM32F4 accepts only 8-bit and 16-bit frames; other nonzero widths up to 16
  return `NOT_SUPPORTED` before board or native changes. Width zero or above 16
  is invalid.
- Successful SPI/I2C speed changes update the registered resume configuration.
  UART pin-state rollback failure faults the device graph instead of reporting
  a usable active device.
- Logical timers and their IRQs share one owner core. Tasks serialize calls;
  callbacks may start/cancel timers, including immediate callbacks during start.
  Slot reservations survive that callback. Module init/deinit is task-only.
  STM32F4 owns complete TIM2/TIM5 peripherals and their CC1 IRQs, maintains eight
  independent deadlines on free-running 32-bit 1 MHz counters, and arms only the
  earliest compare. Canceling one slot leaves its peers running. Missed periodic
  deliveries are coalesced with phase preserved; intervals stop at `INT32_MAX`
  microseconds and IRQ service must stay within that half-counter horizon.
  Keep clocks running and fixed; do not share these timers with capture/PWM.
  TIM2/TIM5 must share an IRQ priority while timers or their dispatchers are
  active, so these callbacks cannot preempt each other's HAL state publication.
  Its HAL next-wake query retains the system clock's millisecond resolution.
- ESP32 logical timer callbacks run in the SDK timer task. Serialize lifecycle
  calls with that task on the owner core. A one-shot retires its native timer
  before user notification; no dispatcher accesses its old slot after the user
  callback returns, allowing cancel/start replacement. Failed native cleanup
  suppresses further notifications and reserves the handle/slot for cancel or
  deinit retry. `is_active()` includes this pending cleanup. Init returns `BUSY`
  while a handle remains after deinit. See the
  [ESP32 timer HIL plan](tests/hil/esp32_timer_lifecycle_hil_plan.md).
- XIO SPI and UART callers explicitly select `instance` and `sm_index` as direct
  configuration members; the HAL passes that binding to the board pin
  hook and the HAL backend without hiding it in board-private configuration.
  XIO UART data transfers use the generic UART API, including
  `esy_hal_uart_send_dma()` and `esy_hal_uart_receive_dma()`.
  `esy_hal_uart_is_readable()` forwards the configured XIO RX readiness query;
  TX-only and detached handles report false.
  XIO SPI slave mode requires an explicit CS input; master modes may leave CS
  externally managed. Slave MISO also requires a distinct `slave_cs_sm_index`
  in the same PIO instance. Initialize configurations with `default_cfg`, then
  select both SMs explicitly; missing/conflicting monitor selection rejects
  before board setup. Slave MISO configurations assign both SMs. All HAL,
  backend and consumer objects must use the same configuration layout.
  Master and MISO-less slave
  users retain one SM. The monitor consumes two instruction words and controls
  MISO output enable independently of data FIFO or SCK stalls at `clk_sys`.
  With synchronized CS, budget six system-clock cycles plus pad propagation
  between deselection and another slave driving MISO, and between selection
  and the first clock. Verify the board's timing through the
  [SPI buffer HIL plan](tests/hil/spi_buffer_contract_hil_plan.md).
  Slave blocking TX-only retires one received FIFO word per requested frame,
  retaining completion if the owner resumes after the master deasserts CS;
  the master must still clock all requested frames. Finite slave DMA uses the
  corresponding completed RX count, also for dummy RX, without waiting for
  a future CS/TXSTALL. Master DMA retains its physical wire-drain check.
  DMA supports only 8-bit frames with 8-bit elements or
  16-bit frames with 16-bit elements. Other frame/element combinations return
  `NOT_SUPPORTED` before DMA preparation; the blocking 2-to-16-bit frame API
  retains its documented least-significant-bit buffer representation.
- Board resource policy is delegated to `esy_board`.
- UART initialization lets `esy_board_uart_configure_pins()` complete the
  controller IRQ priority in a local configuration before HAL backend initialization.
  Board ports that own this priority replace the caller value; the caller's
  configuration remains unchanged. Pico/STM32F4 RX streaming applies the stored
  priority before enabling the UART interrupt. Boards must select a priority
  compatible with RTOS `FromISR` calls used by RX callbacks; zero is a real
  priority, not an automatic-default sentinel.
- Hardware backend behavior is delegated to `esy_hal_backend`.
- STM32F4 microsecond delays use the shared DWT cycle counter after successful
  system init. They busy-wait without reserving a peripheral timer or changing
  the IRQ mask; short masked/ISR waits do not depend on SysTick. Keep HCLK fixed
  per call and the shared counter enabled/unmodified. Preemption and call overhead
  extend the wait. System timestamps still have millisecond resolution; delay
  support is independent of the optional timestamp setting. See the
  [microsecond delay HIL plan](tests/hil/stm32_delay_us_hil_plan.md).
- `esy_hal_system_tick_from_isr()` lets the unique system-timebase ISR owner
  advance the hardware timebase exactly once per configured tick, including
  before scheduler startup. It does not advance the OS tick or yield. STM32F4
  forwards to the vendor HAL tick; Pico and ESP32 require no software forwarding
  for their independent clock sources. Preserve the backend's configured tick
  period and never install a second forwarding owner.
- `esy_hal_system_random_bytes()` obtains bytes from the SDK source during
  serialized normal startup/task execution. It never runs in terminal capture
  or an ISR, and adds no HAL-owned task, heap or persistent storage. Zero length
  succeeds without invoking the source; NULL with nonzero length is invalid.
  Output is authoritative only on OK. Pico uses `pico_rand`; ESP32 uses
  `esp_fill_random`, whose entropy quality depends on native source state.
  SDK configuration and entropy suitability remain with the owner: this is
  neither a cryptographic guarantee nor a collision-free ID allocator.
  STM32F4 returns NOT_SUPPORTED for nonempty requests rather than using ticks
  or its device UID as substitute entropy. Seeding and generation may wait;
  budget synchronous cost according to length and the selected SDK.
- Flash geometry comes from `esy_hal_flash_get_geometry()` and complete sectors
  from `esy_hal_flash_get_sector(address, &sector)`. Both are read-only,
  synchronous startup/task queries that can precede Flash init once the SDK and
  board configuration are ready. A failed query preserves the output. Serialize
  queries with the same owner as all other Flash operations; ISR calls are not
  supported. Geometry describes accessible capacity and required write address/
  length granularity; erase sizes may differ within one device. Pico/STM32F4
  addresses are mapped absolute addresses; ESP32 addresses are physical offsets
  from zero. Unknown geometry fails with `ESY_ERR_NOT_SUPPORTED`.
- Flash erase requires an exact range of complete sectors. It rejects partial
  sectors instead of expanding a request. Programming requires both address and
  length to be multiples of `geometry.program_size`. Use complete sector queries
  for erase boundaries and geometry queries for write granularity; a product
  boot layout does not define hardware programming units.
- `esy_hal_flash_set_safe_execute()` returns `esy_err_t` and propagates
  registration failures from the HAL backend without changing prior behavior. Pico
  supports custom hooks; STM32F4 and ESP32 reject non-NULL hooks with
  `ESY_ERR_NOT_SUPPORTED`. NULL restores the port default. Check registration
  status before relying on a hook. Registration is serialized against all flash access.
- `esy_hal_flash_get_geometry()` reports the accessible address range and
  programming unit; `esy_hal_flash_get_sector()` reports the erase sector
  containing an address. Both queries work before Flash initialization and
  preserve the caller's output on failure. Erase requests must cover complete
  sectors and writes must satisfy the reported programming unit; neither
  operation silently extends the caller's range. See
  [Flash geometry](../docs/integration/flash_geometry.md) for platform sources,
  address conventions and operation constraints.
- Public APIs validate arguments and route through the handle ops table when a
  runtime dispatch point is required.
- `esy_hal_spi_write_then_read_blocking()` keeps a half-duplex command and
  response in one backend operation so shared-data-line backends can switch
  direction inside the protocol turnaround window.
- Init functions assemble ops tables, ask `esy_board` for system configuration,
  pin configuration, or DMA metadata, and call the selected HAL backend.
- Deinit functions coordinate backend cleanup and board-resource release. Check
  status-returning cleanup before reclaiming storage or pins.
- Stateful peripheral and logical handles can be registered in an explicit
  `esy_device_registry_t`. Public operations acquire usage references; direct
  DMA uses a caller-owned `esy_dma_t` controller handle and holds its reference
  until terminal completion, error, or abort.
- Generic DMA start masks select positions in the configured `channels`
  routing array. Bit N selects entry N; the HAL resolves its physical resource
  through the backend. Different STM32 streams may share one peripheral request
  selector, while aliases of one physical resource are rejected during init.
  Chaining also selects its destination by configured-route index. Consumers
  use configured-route indices in these masks, not hardware channel numbers.
  Submission lengths remain bytes; transfer-count setters/queries use beats.
- DMA address/count setters propagate backend status. Unsupported
  operations return `ESY_ERR_NOT_SUPPORTED`. Generic HAL chaining currently
  returns that error on every port: a hardware-triggered destination needs an
  explicit transfer ownership contract before it can be exposed safely. Pico's
  private hardware backend retains its native chaining primitive; STM32F4 has
  no corresponding implementation.
  A failed control operation must not commit a new runtime state or release its
  prepared transfer. Custom backend setter/chaining operations return
  `esy_err_t` and report the actual provider outcome.
- Initialize every typed device binding with `{0}` before first registration.
  Registration rejects reusing an attached binding for another handle without
  changing the existing binding, including after registry deinit. Unregister
  successfully before reusing the binding; never clear live storage manually.
- XIO SPI registration can adopt an already active native DMA transfer. Its
  buffers remain borrowed, and runtime/system suspend returns `BUSY` until owner
  polling or successful abort retires it, even though it predates graph binding.
- Registry deinit invalidates graph nodes without destroying their typed
  bindings or hardware. After producers and callbacks stop, deinitialize the
  registry, then call each typed unregister to finish binding/GPIO release before
  handle deinit. A failed binding cleanup retains ownership and can be retried.
- A GPIO initialization config contains its only normal mode and initial output
  value. An optional adapter-specific GPIO PM profile references initialized
  auxiliary GPIO handles and contains only runtime `idle` and system `sleep`
  overrides. Registration claims every physical pin for one logical owner;
  duplicate aliases are rejected. Ordinary GPIO access is rejected while that
  owner has selected idle or sleep.
- `esy_hal_gpio_attach_irq()` takes an explicit input pull selection together
  with its IRQ mode and callback. Use `ESY_GPIO_PULL_NONE` to disable pulls.
- UART, SPI, and I2C controller pads are not GPIO PM profiles. Their PM adapters
  select board-owned `default`, `idle`, or `sleep` states. The board port owns
  alternate-function mux, pull, drive strength, slew rate, and analog mode.
  Unsupported states fail the device transition; wake-on-RX requires a
  separate controller contract.
- Low-power entry is a caller-owned resumable transaction. Begin delegates
  preparation to the documented HAL backend API; atomic entry includes any
  early clock restoration in the HAL backend; complete or abort closes the balanced
  transaction. The wrapper is platform-neutral and contains no platform
  branches.
- `esy_hal_wakeup_timer` wraps a caller-selected physical wakeup alarm. It
  is distinct from the logical callback timer service and exposes absolute
  alarm, capture/cancel, and shared-clock operations for power adapters.
- `esy_hal_system_always_on_time_us()` is the only elapsed-time source
  accepted across sleep. Pico and ESP32 provide a 64-bit hardware-backed
  implementation. STM32F4 returns `ESY_ERR_NOT_SUPPORTED` until its board port
  supplies a real RTC/LPTIM-class source; TIM6 is not used as a monotonic or
  retention timebase.
- Kernel tick phase sampling, stop, pending-tick observation, fractional
  settlement, and restart are owned by the OS adapter. The read-only phase
  query leaves SysTick running so the adapter can exclude elapsed current-tick
  time before power preparation. The low-power HAL backend has no global tick or
  consumer-supplied clock-restore callback. Standby is exposed only as an
  explicit reset-class shutdown request.

### Boot hardware boundary

`esy_hal_boot_get_layout()` copies the board-owned physical image/metadata layout
into a neutral public value type. The layout contains slot/metadata addresses and
capacities only; Flash geometry comes from the Flash queries rather than duplicated
board values. Managed ports also check that these regions agree with the SDK's
partition table and do not overlap SDK-owned boot records.

`esy_hal_boot_get_runtime()` returns an `esy_boot_runtime_t` with the boot mode,
current image address in the Flash API's coordinate system, and whether a new
update may begin. Direct ports identify the running image from execution;
managed ESP32 uses the native running application partition. The CPU program
counter remains an execution address and must not be used as a physical Flash
offset on ESP32. Query failures preserve the caller's output.

`esy_hal_boot_set_pending_image(address, image_size)` activates a completed
image for the next boot without resetting the device. ESP32 verifies the native
image and updates `otadata`. Direct ports return `ESY_ERR_NOT_SUPPORTED` for
native activation and confirmation; their middleware uses Esy metadata directly.
`esy_hal_boot_jump_to_app(address)` performs the hardware image handoff after
caller-owned validation and quiescence. It belongs to the boot boundary, alongside
layout, running-image and activation operations; the system API retains clocks,
interrupt control, CPU queries and reset. Successful handoff never returns; any
return means no transfer occurred. Direct ports preserve their documented CPU
entry sequence. ESP32 restarts through its native OTA selection, preserving an
already selected target's trial state.

`esy_hal_boot_confirm_running_image()` confirms native trial state after the
consumer's diagnostics. These synchronous startup/task operations require the
same serialized owner as boot metadata and Flash operations, and are not ISR
APIs. Their persistent effects are not rolled back when a later operation fails.

ESP32 managed updates require the native rollback configuration and an
unencrypted Flash device. A running image pending verification cannot start
another update. Confirmation must refer to the running partition: a different
partition already selected for the next reset causes confirmation to fail.
Image format, signature verification and update policy belong to the independent
[`esy_bootloader`](../esy_middleware/bootloader/README.md) middleware target.
Native ESP32 boot selection and rollback remain SDK-owned. The HAL public boot
header does not expose board declarations or native types. See
[Flash geometry](../docs/integration/flash_geometry.md#esp32-managed-boot-and-ota)
for partition setup, update order and persistent-state failure windows.

## Resource Model

Provider function tables occupy read-only shared storage once per implementation,
including distinct native and XIO UART direction combinations. Handles retain
only the table pointer and their own caller-owned state. Opaque HAL storage is
aligned and sized against its private runtime at compilation, independently of
backend storage overrides. No registry or side allocation is added to hide that
state, and handles must stay at stable addresses through successful cleanup.

Private runtime overlays require the GCC/Clang `may_alias` type attribute;
compilers without this capability fail compilation rather than silently using
incompatible typed access to byte arrays. The attribute applies to both the
outer UART runtime and its independently accessed DMA runtimes. GPIO state
snapshots are copied to typed local objects before passing their addresses to
other helpers. This compiler contract preserves the public layouts and UART
atomic publication protocol without disabling strict aliasing for other code.

All `esy_hal` production sources must remain dynamic-allocation free. Run:

```sh
external/EsyMcuPlatform/tools/checks/check_no_dynamic_alloc.sh external/EsyMcuPlatform/esy_hal
```

The only default dynamic-allocation exemption in EsyMcuPlatform is
`esy_middleware/os`, which owns RTOS/platform allocation semantics.

Direct Pico/STM32 hardware SPI and Pico XIO SPI DMA consumers must drive `esy_hal_spi_poll()` from
the serialized owner task. It delivers terminal completion only after both DMA
directions, pending callback access and wire activity retire. SPI scheduler workers
do this automatically. Every initialized SPI backend supplies the poll hook.
UART RX storage remains borrowed until stream stop succeeds;
a failed stop must be retried without clearing or releasing the handle/storage.
STM32 SPI rejects circular, mismatched-length and mismatched-width DMA. Its
single-direction API uses a fixed dummy memory endpoint for the missing
direction, with finite stream counts. A failed submission that still reports
busy retains its buffers and callbacks; continue owner polling or retry abort.

XIO UART embeds the shared UART adapter as `binding.base`; generic UART calls
use that exact layout. Custom UART
backends must supply `abort_dma` to support abort and may provide the remaining
count query; custom SPI backends may provide their own remaining count query.
No generic count query casts XIO storage to a hardware context.

## Verification

Host tests live under `esy_hal/tests/`. Each peripheral keeps its contract
tests in a dedicated test file and shares fake `esy_board` / `esy_hal_backend`
implementations through `hal_test_fakes`.

`python3 esy_hal/tests/test_hal_storage_aliasing.py` checks private storage
overlays with strict aliasing enabled at `-O2`, `-O3` and with LTO. It also covers
independent pointers to the nested UART DMA state. These representation tests
do not authorize resetting a live public handle or replacing atomic operations
with byte copies. The quality gate runs this regression alongside the module
tests.

Run HAL tests directly:

```sh
cd external/EsyMcuPlatform/esy_hal/tests
ceedling test:all
```

Or run all module tests:

```sh
external/EsyMcuPlatform/tools/quality/ci_quality_gate.sh test:all
```

The quality gate includes HAL source contracts, the CMake feature matrix,
AddressSanitizer/UndefinedBehaviorSanitizer, and a coverage inventory. Coverage
requires all executable sources selected for the host suite, at least 65% lines per
file and 85% overall. Wi-Fi is checked by its separate consumer suite; the empty
translation unit has no executable code; disabled UART directions are linked
and exercised by the CMake matrix. These floors are regression guards, not a
claim that every branch is tested. Bootloader behavior is verified separately by the middleware suite using real
CRC32/SHA-256 and a fake signature verifier.

CI uses Ceedling 1.1.8. When multiple gem versions are installed, reproduce its
coverage run with `ceedling _1.1.8_ clobber` followed by
`ceedling _1.1.8_ gcov:all` in `esy_hal/tests`. Production implementations
listed as test support must also be registered as sources so this version
instruments them, including the shared UART I/O and XIO UART TX/RX files.
Validate the generated inventory with
`python3 check_coverage.py build/artifacts/gcov/gcovr/coverage.json`.
The JSON inventory resolves local source identities and rejects foreign namesakes
or duplicate aliases; accepted path roots are documented in the
[quality tools guide](../tools/README.md).

See [lifecycle verification](docs/lifecycle-verification.md) for lifecycle
contracts, verification coverage and hardware scenarios.
See the [timer capture HIL plan](tests/hil/timer_capture_hil_plan.md) for ownership,
PM cleanup, Pico/ESP32 ISR publication, and STM32F4 counter/timebase checks.
See the [logical timer HIL plan](tests/hil/logical_timer_hil_plan.md) for independent
deadlines, compare races, callback reuse, capacity and clock-tree acceptance.
See the [ADC DMA HIL plan](tests/hil/adc_dma_hil_plan.md) for the shared DMA owner,
finite/scan/circular transfers, abort/error retirement, IRQs and graph leases.
See the [STM32 application handoff HIL plan](tests/hil/stm32_system_handoff_hil_plan.md)
for the native stack-free MSP switch, CPU entry context and application IRQ
policy. The quality gate runs `tests/test_stm32_handoff_native.py` when
`ESY_TEST_ARM_TOOLCHAIN_ROOT` and `ESY_TEST_STM32_SUPPORT_ROOT` identify real
providers; missing variables explicitly skip this target-specific check.
`tests/test_xio_storage_contract.py` checks public C11/C++17 storage overrides
and layout agreement, including the enclosing UART/SPI capacities.

The current suite covers:

- SPI ops binding, runtime dispatch, half-duplex write-then-read dispatch, DMA
  metadata completion, and deinit order;
- UART, I2C, and PWM argument validation and ops dispatch behavior;
- UART board IRQ priority forwarding before HAL backend initialization and caller
  configuration preservation; failed RX stream startup/cleanup, retained graph
  references through stop retry, and idle rejections without resource borrowing;
- ADC and timer capture ops binding, board configuration, DMA/query dispatch,
  and deinit order; ADC byte/scan validation, circular graph leases, failed-start
  and cleanup ownership, trigger restoration, and suspend fault propagation;
- DMA handle lifecycle, board IRQ metadata completion, transfer reference
  lifetime, and single-transfer start sequencing;
- GPIO HAL backend routing, explicit temporary-state application, board-provided IRQ
  priority metadata, GPIO PM ownership, bypass rejection, and state selection;
  attached-IRQ electrical-state rejection preserves callback delivery and PM
  rollback restores already changed peer pins;
- system board-before-HAL backend initialization and query forwarding;
- Flash query forwarding before init, invalid pointers, sector address
  forwarding, output preservation on failure, geometry-based address validity,
  and Flash/watchdog/timer/multicore lifecycle/dispatch contracts;
- USB device ops binding, endpoint dispatch, and query forwarding;
- resumable low-power capability forwarding, early restoration in the HAL backend,
  retention-entry cleanup, fatal restore reporting, explicit shutdown
  separation, and wakeup-timer lifecycle;
- board-provided boot layout, unavailable-layout failure, execution address
  forwarding, and hardware application handoff routing;
- XIO SPI/UART ops binding, board pin configuration, and DMA metadata
  completion; XIO UART RX readiness forwarding for RX-only/full-duplex and
  false readiness for TX-only/detached handles; explicit XIO SPI slave monitor
  preflight before board mutation; XIO SPI runtime/system suspend refusal while DMA accepted before
  graph binding is still active; XIO UART source retirement followed by retained
  physical TX busy, runtime-PM skipping, system-PM BUSY and deinit preservation;
- I2C blocking, DMA and combined entry points preserve 0x100..0x3ff addresses;
  invalid wider addresses and timeout budgets reject before provider dispatch;
- typed binding reuse rejection across configuring, started and deinitialized
  registry states, with original graph identity and controller I/O preserved;
- disabled PWM duty rejection before backend access, including after a prior
  enable/disable cycle.

Timer regression tests also cover a periodic callback delivered before start
returns canceling a peer whose native ID differs from its HAL slot. ADC tests
cover competing init before board mutation, retained cleanup and subsequent
owner reuse. Native GPIO/ADC source checks and their HIL procedures complement
these facade tests; fakes do not establish hardware interrupt or pin behavior.

Wi-Fi forwarding is also exercised through the real HAL target by
`esy_middleware/wifi/tests/test_cmake_consumer.py`, using a reusable fake HAL backend
facade. It validates all seven Wi-Fi forwarding APIs and C/C++ target consumers.
The additional network-runtime forwarding API is checked before station startup,
including descriptor/context propagation, all three hooks, NULL rejection and
unchanged output on a provider error. The backend's global ownership, credential
borrowing and task/core constraints are documented in `esy_hal_wifi.h`; manager
users must not call the direct station-control APIs.
`python3 esy_hal/tests/test_pico_wifi_runtime_contract.py` checks that the Pico
getter has no native side effects and the three hooks only call their guarded
SDK operations. The [network runtime HIL plan](tests/hil/pico_wifi_runtime_hil_plan.md)
covers real polling, station-state separation and quiescent shutdown.

## Known Limits

- Hardware behavior is only represented through fakes in host tests.
- Feature availability depends on both CMake feature gates and matching
  `esy_hal_backend` platform implementations.
- Board-specific policy belongs to the active `esy_board` port, not to this
  module.
- UART RX streaming blocks retention. Consumer feature assembly must explicitly
  disable persistent RX or accept CPU-idle-only behavior; ordinary UART pin
  configuration is never treated as wake-on-RX support.
- USB device adapters are CPU-idle-only until endpoint, PHY, bus-resume, and
  enumeration restoration semantics are standardized.
- GPIO PM claim capacity is fixed at 64 physical pins per process. Profile
  claim/release and device transitions are single-owner task operations.

## Fatal Capture Binding

`ENABLE_ESY_FAULT` adds the boot-lifetime `esy_hal_fault_init()` binding and
RAM-resident `esy_hal_fault_raise()` entry. It is optional and independent of
Logger, OS and the coredump record policy. Install it once during privileged
Cortex startup before peers/scheduler start, or during ESP32 native app startup
before application-owned tasks start. Preserve existing exception-vector owners. The callback, its entire call graph and ordinary context must remain in
internal RAM and may only copy a bounded snapshot into consumer-reserved RAM.
It must not log, allocate, access Flash, take locks or call the OS.

Cortex ports cover RP2040, secure-only Arm RP2350 without security transitions,
and verified STM32F405/407/415/417 main-SRAM layouts without an MPU. They reserve
one 4096-byte private state/stack block per core (including a 2048-byte emergency
stack). STM32 also owns an aligned 128-entry RAM vector table. RP2040 claims one
SDK hardware spinlock during startup and tries it once on fatal entry; RP2350
uses native lock-free admission. First fatal admission captures and resets;
other fatal cores park for that reset/watchdog. No task or heap is created.

Pico linkers must copy `.time_critical.*` and STM32 linkers must copy
`.RamFunc.*` into executable SRAM during startup. The consumer separately
reserves the same retained record range in its bootloader and every image;
these APIs do not allocate retained RAM or Flash partitions. STM32 excludes
CCM stack windows, and unknown/security-transition or stacking-error frames
are explicitly unavailable. Software assertions retain source details and a
partial entry snapshot rather than an interrupted hardware exception frame.

Classic ESP32 uses its native panic stack and reset/debugger policy instead of
the Cortex entry. It is version-pinned to ESP-IDF 5.3.4 with IRAM panic handling;
see [ESP32 fatal capture integration](docs/esp32_fault_integration.md). Native
verification scenarios cover provider limitations, RAM retention,
simultaneous/nested faults, capture while Flash is unavailable and watchdog
recovery on the consumer's target.
