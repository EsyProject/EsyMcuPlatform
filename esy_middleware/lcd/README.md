<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# LCD

## Purpose

`esy_lcd` provides a small display facade and optional controller middleware for MCU
applications. Upper layers, such as LVGL integration, hold an `esy_lcd_t`
handle and submit rectangular pixel flushes through the handle's flush hook.

The current concrete backend is `esy_ili9488`, which drives an ILI9488 LCD
controller through the HAL SPI and GPIO abstractions. The backend owns no
heap memory and stores its private state in the caller-owned `esy_lcd_t`
opaque storage.

## Public Surface

Public headers live under `include/esy_middleware/lcd/`.

- `esy_ili9488.h` defines the ILI9488 configuration, default initializer,
  panel profile types, init/deinit APIs, direction API, display/sleep control
  APIs, readiness query, and optional backlight API.
- `esy_lcd.h` owns the complete `esy_lcd_t` runtime handle,
  `ESY_LCD_PRIV_BYTES`, and the facade APIs for attachment checks, detach and
  rectangular flush requests.
- `esy_lcd_device.h` and `esy_ili9488_device.h` belong to `esy_lcd_device` and
  expose optional graph registration and controller-specific GPIO PM integration.
- `esy_common/defs/display/esy_lcd_defs.h` defines only the shared display orientation
  and pixel format enums. Include the LCD facade for runtime objects.
- `esy_common/defs/peripheral/esy_spi_defs.h` defines the prepared SPI transaction
  provider contract shared with scheduler implementations.

## Integration

Link `esy_lcd` for the facade. Without a concrete controller it has no HAL or
GPIO dependency and supports custom providers. Logical-device registration and
auxiliary GPIO power policy are supplied separately by `esy_lcd_device`, which
is assembled only when the consumer enables `ENABLE_ESY_GPIO`. Link that target
explicitly when using generic or ILI9488 registration APIs. It links `esy_lcd`;
the facade never links the optional device integration.

Enable the ILI9488 backend through the consumer's feature gate:

```cmake
target_link_libraries(<your_target> PRIVATE esy_lcd)
```

Enable `ENABLE_ESY_LCD` and `ENABLE_ESY_LCD_ILI9488` in the consumer feature
configuration, with GPIO and SPI enabled. ILI9488 physically uses GPIO even when no logical
device is registered. A scheduler-backed provider also needs
`ENABLE_ESY_IO_SCHEDULER` and `ENABLE_ESY_SPI_SCHEDULER`; link `esy_spi_scheduler` for
a worker or `esy_spi_scheduler_core` for caller-driven processing. LCD itself has
no scheduler dependency.
A custom provider may complete inline while honoring the prepared transaction
contract. Initialize the SPI and GPIO resources first, then configure
the LCD while the bus is exclusively available. Typical setup:

```c
esy_lcd_t lcd = {0};
esy_ili9488_config_t cfg;

esy_ili9488_default_config(&cfg);
cfg.spi = &lcd_spi;
cfg.cs_gpio = &lcd_cs;
cfg.dc_gpio = &lcd_dc;
cfg.rst_gpio = &lcd_rst;
cfg.width = 320u;
cfg.height = 480u;
cfg.color_format = ESY_COLOR_FORMAT_RGB565;
cfg.dir = ESY_LCD_DIR_L2R_U2D;
cfg.backlight_initially_enabled = false; /* Match the actual board state. */
cfg.panel_profile = NULL; /* use the built-in default profile */
cfg.spi_submit_cb = esy_spi_scheduler_submit_prepared_with_arg;
cfg.spi_submit_cb_arg = spi_scheduler;

esy_err_t err = esy_ili9488_init(&lcd, &cfg);
```

Flush users should call the generic facade after initialization:

```c
err = esy_lcd_flush(&lcd, x0, y0, x1, y1, color_buf, flush_done_cb, user_arg);
```

When `esy_lcd_flush()` returns `ESY_OK`, the backend accepted the request and
will invoke `flush_done_cb` when the pixel transfer finishes. When it returns a
negative error, the request was rejected and the callback will not be invoked
for that request. The `color_buf` storage must remain valid until
`flush_done_cb` runs for an accepted request.

Stop producers and detach through the facade:

```c
err = esy_lcd_detach(&lcd);
```

On `ESY_ERR_BUSY`, retain the handle, pixel buffer, provider and callback state;
let the accepted flush complete, unregister its logical device if present, and
retry detach. Release caller-owned storage only after `ESY_OK`. The scheduler
must likewise drain prepared requests before destruction.

`esy_lvgl` integrates through `esy_lcd_flush()` rather than calling the handle
function pointer directly. Applications that enable LVGL must enable and link
`esy_lcd` as well.

Products linking `esy_lcd_device` may register the ILI9488 logical device with an optional
adapter-specific auxiliary GPIO PM profile. Runtime suspend selects optional
per-pin `idle` overrides, system suspend selects optional `sleep` overrides,
and resume restores each GPIO configured state. A missing override also
restores that configured state. SPI controller pads remain board-owned.

The facade still holds an admitted asynchronous flush's device reference until
completion, and rejects detach while registered. Splitting the registration
implementation does not bypass runtime PM or transfer-drain requirements. Before
cleanup, stop producers, drain callbacks, resume registered devices, deinitialize
the registry and unregister bindings; detach only after unregister succeeds.

## Contracts

- No heap allocation is used by the ILI9488 backend.
- The backend context is stored in `esy_lcd_t::priv.bytes`; the backend
  verifies the storage size with `_Static_assert`.
- The caller owns and initializes the SPI handle before `esy_ili9488_init()`.
- `cs_gpio`, `dc_gpio`, and `rst_gpio` must refer to initialized GPIO resources.
- Initialize the first handle with `{0}`. Reinitializing a live handle returns
  `ESY_ERR_STATE`. Failed init is retryable unless CS cleanup failed; in that
  case the attached faulted handle requires deinit recovery first.
- `width` and `height` must be non-zero.
- `panel_profile` may be NULL to use the built-in default profile. Custom panel
  profiles must provide a non-empty init command sequence and valid data
  pointers for entries with non-zero data length.
- The current ILI9488 backend supports `ESY_COLOR_FORMAT_RGB565`.
  `ESY_COLOR_FORMAT_RGB666` is a public enum value, but this backend rejects it
  with `ESY_ERR_NOT_SUPPORTED` until RGB666 flush sizing and controller setup
  are implemented.
- `flush` coordinates use inclusive `x1` and `y1` bounds.
- A flush region must satisfy `x0 <= x1`, `y0 <= y1`, `x1 < width`, and
  `y1 < height`.
- Only one flush may be in flight for a handle. A reentrant or overlapping
  flush request returns `ESY_ERR_BUSY` and does not overwrite the in-flight
  callback.
- `esy_lcd_flush()` validates backend-independent arguments and returns
  the concrete backend's flush result. Backend-specific region bounds,
  busy-state, and format validation remain the concrete backend's
  responsibility.
- SPI providers may invoke transfer callbacks synchronously before submit
  returns, or asynchronously after returning `ESY_OK`.
- A failed SPI submit means the transfer was not accepted and callbacks for
  that failed submit must not be invoked later.
- One flush submits exactly one prepared transaction. Its prepare callback sends
  column and row windows using the configured parameter encoding, followed by
  memory-write; the
  provider then transmits pixels before invoking completion. The provider must
  own the bus across this entire sequence, including CS cleanup in completion.
- A failed prepare skips pixels. Every accepted transaction completes exactly
  once, including transport failure, so its resources can be released.
- No per-command queue buffers or generation tokens are required. Detach returns
  `ESY_ERR_BUSY` while a flush or registered logical device retains the handle.
- Public calls are task-context operations and must be externally serialized.
  Completion may run on another task or inline, and may submit another flush.
  Serialize lifecycle operations with user callbacks as well; ISR callbacks
  are unsupported. Blocking init/control operations require exclusive SPI bus
  access outside the scheduler queue.
- `esy_ili9488_set_direction()` returns a status, requires an idle handle, and
  propagates GPIO/SPI failures. Initialization also checks its final direction
  setup before publishing the handle.
- `esy_ili9488_display_on()`, `esy_ili9488_display_off()`,
  `esy_ili9488_sleep_in()`, and `esy_ili9488_sleep_out()` send controller
  commands synchronously and require an idle handle.
- `esy_ili9488_set_backlight()` calls the optional backlight callback. It
  returns `ESY_ERR_NOT_SUPPORTED` when no callback was configured.
- System resume invokes the backlight callback only in the late recovery phase,
  after interrupts and the OS tick are enabled. The FreeRTOS scheduler may
  still be suspended by tickless idle, so the callback must not wait on an RTOS
  object or delay.

### Recovery and power management

Initialize both `esy_lcd_t` and `esy_lcd_device_binding_t` with `{0}`. A live
binding cannot be registered against another LCD. After normal operation, stop
producers, drain completion callbacks and resume suspended devices before
registry deinitialization. Then unregister LCD bindings to release GPIO claims,
and detach the LCDs. Before registry start, unregister can remove the graph
node directly. If GPIO release fails, retain the binding, restore its profile
to DEFAULT and retry. Do not submit LCD operations after registry deinit.

Final CS release failure still completes the accepted buffer, but latches a
fault instead of making the LCD idle. The completion owner can call
`esy_ili9488_get_status()` and must exclude other bus users while recovering.
Deinit retries CS release and retains the handle if recovery fails. Init failures
with failed CS cleanup also retain an attached faulted handle; deinit it before
retrying init. A software driver cannot guarantee physical bus isolation after
a GPIO hardware failure; shared-bus owners must provide a fault containment policy.

The GPIO PM profile supports only the configured CS, DC and reset handles.
Reset must remain output-high in all states, and panel power must be retained.
Reset/power-loss recovery requires normal task-context reinitialization, outside
system-resume callbacks. PM restores the last successful backlight setting,
starting with `backlight_initially_enabled` (default false). Backlight callbacks
must preserve the previous state on failure. Failed suspend rollback returns
`ESY_ERR_STATE` so the registry becomes faulted rather than accepting changed
hardware as READY.

### Pixel and control contracts

`parameter_format` defaults to `ESY_ILI9488_PARAMETER_U8`, preserving one-byte
command parameters. Modules requiring zero-extended 16-bit parameters select
`ESY_ILI9488_PARAMETER_U16_BE`: each logical parameter is transmitted as
`00 value`, under its own CS assertion. This applies to initialization,
direction and window coordinates. Commands remain one byte. Invalid enum values
are rejected before I/O. Pixel payloads are never padded or reordered here.

RGB565 flush buffers are packed row-major in the module's required wire byte
order. `esy_lvgl` defaults to MSB-first and supports LSB-first via
`rgb565_byte_order`; it restores native pixels before notifying LVGL.
Direct callers must supply wire-order bytes themselves.
`bgr=true` sets MADCTL's BGR bit; false selects RGB. Width and height describe
the physical panel at zero degrees; 90/270-degree directions exchange logical
bounds without changing those physical dimensions.

Sleep In waits 120 ms before returning, satisfying both the next-command
and subsequent Sleep Out deadlines. Sleep Out retains its post-command delay.
These blocking controls require exclusive bus access and are not PM callbacks.

## Resource Model

The caller owns:

- the `esy_lcd_t` handle;
- the SPI handle;
- the SPI scheduler or transport backing storage;
- each flush color buffer until its done callback runs.

The backend owns only state stored inside `esy_lcd_t::priv.bytes`. It does not
allocate memory, create tasks, or keep hidden singleton state.

## Platform Capability Matrix

| Backend | RGB565 | RGB666 | Rotation | Async Flush | Blocking Flush |
| --- | --- | --- | --- | --- | --- |
| ILI9488 | yes | rejected | yes | provider-dependent | provider-dependent |

The ILI9488 backend uses synchronous HAL SPI writes during controller
initialization and direction changes. Pixel flushes require the prepared-transaction
`spi_submit_cb` provider; the provider may complete inline or asynchronously.
The scheduler uses DMA for larger payloads and blocking writes for small payloads
or when DMA is unsupported.

## Verification

Host unit tests live under `tests/`.

Run module tests:

```sh
cd external/EsyMcuPlatform/esy_middleware/lcd/tests
ceedling test:all
ceedling gcov:all
```

The current tests cover:

- generic LCD facade attachment, detach, argument validation, and flush
  dispatch;
- default config sentinel values;
- init argument validation;
- custom panel profile initialization;
- padded parameter initialization/direction/window encoding, unchanged pixel
  payloads, invalid encoding and per-write failures including short writes;
- RGB666 rejection;
- reset timing and initialization command sequence;
- direction register values for all supported orientations;
- display on/off, sleep in/out, and backlight control;
- flush coordinate and NULL buffer rejection;
- deferred flush completion until the pixel transfer post callback;
- preparation failures, GPIO failures and CS cleanup;
- overlapping flush requests without callback overwrite;
- flush done callback reentry;
- submit failure cleanup and retry;
- repeated busy detach without losing the accepted callback;
- init failure retry and live-handle reinit rejection;
- malformed dependency arrays, live binding reuse, registry teardown and retryable GPIO release;
- final CS release failure, failed init cleanup and readiness queries;
- BGR selection, rotated coordinate bounds and Sleep In command spacing;
- backlight retention, failed suspend rollback and reset-profile rejection;
- real scheduler integration with a one-usable-slot queue, deferred DMA, drain,
  quiesce and destroy sequencing.

## Known Limits

- The generic facade covers attachment checks, detach, and flush dispatch.
  Controller-specific setup and direction changes are still concrete backend
  APIs.
- The init sequence is table-driven and can be overridden through a panel
  profile, but only one built-in profile is currently provided.
- Pixel flush currently supports RGB565 only.
- Flush result propagation is available through `esy_lcd_flush()`, but the
  callback still carries no status payload for transfers that were accepted and
  later fail in a lower transport layer.


### Provider and binding requirements

Use an `esy_spi_submit_prepared_t` provider (scheduler:
`esy_spi_scheduler_submit_prepared_with_arg`) that implements acceptance, exclusive
bus ownership and exactly-once completion. Check results from detach/deinit and
direction changes.

Zero-initialize bindings before first use. Supply physical zero-degree dimensions
and panel wire-order RGB565 bytes. Configure `esy_lvgl::rgb565_byte_order` for
panels requiring LSB-first; the adapter owns that conversion.
Use the default initializer or zero initialization for the configuration.
A zero `parameter_format` selects U8 parameters; `bgr` selects the intended
color order. Recovery after init failure must check whether the handle remains
attached and clean it up before retrying.

Run `python3 esy_middleware/lcd/tests/test_cmake_consumer.py` from the platform
repository root for disabled, facade-only, backend and SPI-only provider builds.
Every public LCD header is also compiled independently as C11 and C++17 through
the facade target, including the complete handle and optional device interfaces.
All generated consumer artifacts are temporary.
