<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Touch

## Purpose

`esy_touch` provides a small controller-neutral touch input handle and concrete
touch controller backends. Upper layers, such as LVGL integration, hold an
`esy_touch_t` handle and call the `esy_touch_read_input()` facade to poll the
current touch state.

The current concrete backend is `esy_xpt2046`, which reads an XPT2046
resistive touch controller through caller-provided SPI scheduler callbacks and
uses the HAL GPIO abstraction for software CS and optional IRQ state.

## Public Surface

Public headers live under `include/esy_middleware/touch/`.

- `esy_touch.h` owns the complete `esy_touch_t` runtime handle,
  `ESY_TOUCH_PRIV_BYTES`, touch SPI reservation callback contract, and the
  driver-agnostic APIs for attachment checks, detach and input reads.
- `esy_touch_device.h` and `esy_xpt2046_device.h` declare the optional
  `esy_touch_device` graph/PM integration.
- `esy_xpt2046.h` defines the XPT2046 configuration, calibration structure,
  defaults, and init API.
- `esy_common/defs/display/esy_touch_defs.h` defines only the shared touch sample value.
  Include the touch facade for runtime objects and provider callbacks.

## Integration

Link the `esy_touch` target and enable the XPT2046 backend through the CMake
feature gate:

```cmake
target_link_libraries(<your_target> PRIVATE esy_touch)
```

Modules that include `esy_middleware/touch/esy_touch.h`, such as `esy_lvgl_touch`,
must link `esy_touch` explicitly and require `ENABLE_ESY_TOUCH=ON` in the feature
gate.
Public common type/device headers propagate through this target; consumers do
not need to add their include paths manually. The facade itself has no HAL or GPIO dependency. Concrete XPT2046 support uses
GPIO for CS/IRQ and therefore requires `ENABLE_ESY_GPIO`. HAL and compiler
settings remain private implementation dependencies of the concrete backend.

Link `esy_touch_device` explicitly for `esy_touch_register_device()` or
`esy_xpt2046_register_device()`. This target is assembled only when the consumer
enables `ENABLE_ESY_GPIO`; it contains graph registration and auxiliary GPIO PM
policy and links the facade. The facade retains device references around reads
and rejects detach while registered. Device integration is never a reverse
dependency of the facade.

Typical setup (the caller has initialized `touch_cs` as a GPIO resource):

```c
esy_touch_t touch = {0};
esy_xpt2046_config_t cfg;

esy_xpt2046_default_config(&cfg);
cfg.cs_gpio = &touch_cs;
cfg.irq_gpio = NULL;
cfg.width = 320u;
cfg.height = 240u;
cfg.dir = ESY_LCD_DIR_R2L_D2U;
cfg.calibration.mode = ESY_XPT2046_CALIBRATION_LINEAR;
cfg.calibration.linear.x_scale_q16_16 = ESY_XPT2046_Q16_16_ONE;
cfg.calibration.linear.y_scale_q16_16 = ESY_XPT2046_Q16_16_ONE;
cfg.touch_detect.mode = ESY_XPT2046_TOUCH_DETECT_ALWAYS;
cfg.spi_get_write_buffer_cb = esy_spi_scheduler_get_read_write_buffer_with_arg;
cfg.spi_get_write_buffer_cb_arg = spi_scheduler;
cfg.spi_submit_result_cb = esy_spi_scheduler_submit_result_with_arg;
cfg.spi_submit_cb_arg = spi_scheduler;

esy_err_t err = esy_xpt2046_init(&touch, &cfg);
```

Upper layers should poll through the facade:

```c
uint16_t x;
uint16_t y;
bool pressed;

err = esy_touch_read_input(&touch, &x, &y, &pressed);
```

Asynchronous users may split sample progress from sample retrieval:

```c
esy_touch_data_t sample;

err = esy_touch_poll(&touch);
if (err == ESY_OK) {
    err = esy_touch_get_sample(&touch, &sample);
}
```

Products linking `esy_touch_device` may register the XPT2046 logical device with an optional
adapter-specific auxiliary GPIO PM profile containing per-pin `idle` and
`sleep` overrides. Resume, or a missing override, restores each GPIO configured
state. SPI controller pads remain board-owned. Products without PM configure
only the normal GPIO mode and initial output value.

## Contracts

- No heap allocation is used by the XPT2046 backend.
- Initialize `esy_touch_t` and `esy_touch_device_binding_t` with `{0}` before
  first use. Initializing an attached or registered handle again returns
  `ESY_ERR_STATE` without changing its binding, sample cache or pending transfer
  contexts. Stop producers, drain accepted transfers and callbacks, unregister
  and detach before changing configuration through another init.
- The backend context is stored in `esy_touch_t::priv.bytes`; the backend
  verifies the storage size with `_Static_assert`.
- The caller owns and initializes the SPI scheduler before `esy_xpt2046_init()`.
- `cs_gpio` is required and must point to an initialized GPIO resource.
- `width` and `height` must be non-zero.
- Supported touch detection modes are:
  - `ESY_XPT2046_TOUCH_DETECT_ALWAYS`: completed X/Y samples are reported as
    pressed. This is the default and does not require an IRQ pin.
  - `ESY_XPT2046_TOUCH_DETECT_IRQ`: active-low `irq_gpio` decides pressed state;
    `irq_gpio` must point to an initialized GPIO resource.
  - `ESY_XPT2046_TOUCH_DETECT_PRESSURE_THRESHOLD`: the backend reads X, Y, Z1,
    and Z2, estimates touch resistance from `x_plate_ohms`, and reports pressed
    when the estimate is less than or equal to `pressure_max_ohms`.
- Calibration is board/display specific and must be supplied through
  `esy_xpt2046_config_t`; the middleware does not carry board-specific
  hard-coded calibration constants.
- Calibration scale factors use signed Q16.16 fixed-point format, so coordinate
  transform does not require runtime floating-point support.
- Supported calibration modes are:
  - `ESY_XPT2046_CALIBRATION_LINEAR`: scale/offset model using
    `calibration.linear`.
  - `ESY_XPT2046_CALIBRATION_RANGE`: raw min/max model using
    `calibration.range` plus optional swap/invert flags. Each raw range spans its
    final logical axis: with `SWAP_XY`, raw X maps to height and raw Y to width
    before swapping. Non-square screens retain their complete coordinate range.
    Reversed raw endpoints and samples outside those endpoints are supported.
  - `ESY_XPT2046_CALIBRATION_AFFINE`: two-axis affine transform using
    `calibration.affine`, suitable for three-point or least-squares five-point
    calibration generated outside the driver.
- `ESY_XPT2046_CALIBRATION_FLAG_SWAP_XY`,
  `ESY_XPT2046_CALIBRATION_FLAG_INVERT_X`, and
  `ESY_XPT2046_CALIBRATION_FLAG_INVERT_Y` are applied after range/affine
  mapping.
- `esy_xpt2046_calibration_export()` and
  `esy_xpt2046_calibration_import()` provide a versioned persistent blob with
  magic, size, and CRC32 validation.
- SPI callbacks return `esy_err_t`; read operations propagate buffer reserve
  and transfer submit failures.
- The buffer callback receives `minimum_size` before any write. XPT2046 requests
  three writable bytes in both TX and RX buffers. Providers must check both
  capacities before reserving a request, return `ESY_ERR_NO_MEM` when either is
  too small, and leave no new reservation on failure. Use the scheduler's sized
  `esy_spi_scheduler_get_read_write_buffer_with_arg` adapter.
- The `esy_touch_spi_get_write_buffer` callback takes `size_t minimum_size`
  before its context argument. Custom callbacks validate this size before
  returning a buffer.
- `spi_submit_result_cb` is required and delivers an explicit terminal status
  for every accepted transfer. RX is valid only when status is `ESY_OK`.
- Any failed or NULL-RX accepted transfer invalidates the whole pending sample.
  No coordinates or pressed state are published from that sample. The last
  successfully completed sample remains cached, but `get_sample` returns any
  pending error without modifying its output. The next poll can start a retry
  and clear the old error; while an accepted asynchronous retry is pending,
  retrieval can return the previous completed sample.
- Initialize configuration using `esy_xpt2046_default_config()` or zero
  initialization before setting fields.
- SPI providers may invoke transfer callbacks synchronously before submit
  returns, or asynchronously after returning `ESY_OK`.
- A failed SPI submit means the transfer was not accepted and callbacks for that
  failed submit must not be invoked later.
- SPI providers must keep submitted TX/RX buffers valid until the accepted
  transfer completes and its terminal callback has returned.
- `esy_touch_poll()` starts or advances sampling, while
  `esy_touch_get_sample()` returns the latest completed sample without starting
  new hardware I/O.
- XPT2046 sampling is asynchronous-safe for one handle: a poll while a sample
  is already in flight returns the latest completed sample and does not enqueue
  another overlapping sample transaction.
- Late callbacks from superseded transactions are ignored by a per-transaction
  generation check.

## Error Model

`esy_xpt2046_init()` returns:

- `ESY_OK` on success;
- `ESY_ERR_INVALID_ARG` for invalid pointers, missing callbacks, invalid pins,
  invalid dimensions, invalid orientation, or invalid calibration;
- `ESY_ERR_STATE` when the handle is already attached or bound. Every init
  failure leaves the handle unchanged, so a detached handle may retry after
  correcting configuration.

`esy_touch_register_device()` rejects an already bound handle or live binding
with `ESY_ERR_STATE` before claiming GPIOs or changing graph state. A binding
cannot be reused for another touch until unregister succeeds. Failed graph
registration releases its newly claimed GPIOs and clears the binding for retry.
If rollback release fails, ownership remains linked to the handle and binding;
restore default GPIO PM state and successfully unregister before retrying or
detaching. The original graph error is returned after successful rollback;
rollback failure returns the cleanup error.

`esy_touch_read_input()` returns:

- `ESY_OK` when a sample request is accepted and the current state is returned;
- `ESY_ERR_INVALID_ARG` for invalid output pointers or context arguments;
- `ESY_ERR_STATE` when the touch handle is not initialized;
- any error returned by the SPI buffer, submit, completion or CS/IRQ GPIO access.

Failed samples never replace the last completed sample. XPT2046 `get_sample`
reports the outstanding sample error without modifying its output; a successful
later poll/sample clears it. A failed CS release must be retried through polling
until CS can be deasserted before detaching or lending the physical SPI bus to
another owner. Linear transforms keep signed 64-bit intermediates through screen
clamping, including extreme signed calibration offsets. Failed GPIO PM selection
restores the DEFAULT profile explicitly; failed restoration faults the device graph.

Backends must provide split `poll` and `get_sample` hooks.

## Thread Safety

The module does not serialize access to a handle. A single `esy_touch_t`
instance must be accessed from one execution context at a time, or the caller
must provide external serialization.

Do not call init and read operations concurrently on the same handle.

### Shutdown and reuse

Stop every polling/input producer and let all accepted SPI transfers and their
callbacks finish before detaching. Drain completion through the provider's
completion/quiescence contract; `esy_touch_detach()` does not cancel or wait for
transfers. Serialize cleanup with all other handle access and provider callbacks.
If the touch handle has a logical-device binding, successfully unregister it
before detach. Before graph start, unregister removes its graph node directly.
For a started graph, stop producers and drain callbacks, deinitialize the
registry, then unregister the typed binding and detach the handle. Registry
teardown leaves the typed handle link and GPIO claims attached. GPIO release
failure preserves this ownership; restore the GPIO default PM state and retry
unregister before reusing any of the resources.

`esy_touch_detach()` returns `ESY_OK` after clearing the handle, including an
already detached handle. A registered handle returns `ESY_ERR_STATE` and retains
its resources. NULL returns `ESY_ERR_INVALID_ARG`. Reclaim the handle, GPIO,
provider resources and callback contexts only after successful detachment.
The same handle may then be initialized again.

## Verification

Host unit tests live under `tests/`.

Run module tests:

```sh
cd external/EsyMcuPlatform/esy_middleware/touch/tests
ceedling test:all
ceedling gcov:all
```

From the platform repository root, run the independent CMake consumer check:

```sh
python3 -B esy_middleware/touch/tests/test_cmake_consumer.py
```

It compiles, links and runs C11/C++17 consumers that link only `esy_touch`, with
the XPT2046 backend enabled and disabled, and checks that the disabled module
creates no target. Public facade, device-binding and backend headers are checked
individually in both languages without manually supplied dependency include
paths, including the complete handle and touch SPI callback. Real common/touch targets
use shared HAL fakes; no board SDK, OS target or hardware acceptance is implied.

The current tests cover:

- generic touch facade attachment, detach, argument validation, and backend
  error propagation, including read-after-detach behavior;
- registered-handle detach refusal and successful detach after unregister;
- XPT2046 registration rejecting attached foreign providers before inspecting
  their context, including single-byte and unaligned storage under sanitizers;
- first-init failure/retry, repeat-init rejection with a completed sample or
  registered handle, and preservation of accepted asynchronous callback contexts;
- live-binding reuse rejection with unchanged graph and GPIO ownership,
  failed registration rollback/retry, and binding reuse after successful unbind;
- started-registry teardown, retry after GPIO release failure, and handle reuse;
- XPT2046 read-after-detach behavior without issuing new SPI I/O;
- split `poll` / `get_sample` facade behavior and XPT2046 integration;
- facade `read_input` integration through the XPT2046 split backend path;
- LCD facade and XPT2046 integration using a shared display orientation
  contract;
- default config sentinel values;
- init argument validation;
- read argument validation and uninitialized-handle state errors;
- X/Y SPI command order;
- Q16.16 fixed-point calibration scaling;
- raw min/max range calibration with swap/invert flags and reversed-range edge
  clamping;
- exact min/mid/max range calibration, all swap/invert combinations across
  landscape/portrait and single-pixel axes, reversed/outside-range corners and
  interior coordinates on a non-square screen;
- affine calibration and out-of-bounds clamping;
- persistent calibration blob export/import and CRC rejection;
- coordinate transform for all supported orientations and clamp behavior through
  public read;
- duplicate poll suppression while asynchronous callbacks are pending;
- late callback rejection through per-sample generation checks;
- always/IRQ/pressure-threshold pressed semantics, including threshold boundary
  behavior;
- pressure-threshold invalid Z boundaries, including equal and decreasing Z
  samples;
- SPI buffer reserve, NULL buffer, and submit error propagation;
- real scheduler TX/RX sizes of one or two bytes rejected before any payload write
  or request reservation, including asymmetric capacities and subsequent reuse;
- retry behavior after a failed Y transfer submit, including the case where the
  X transfer callback is still pending.
- late NULL RX callbacks from older generations and post-callback reentrant
  reads;
- real SPI scheduler transfer failures through terminal result callbacks,
  rejecting invalid sample publication and allowing a later successful sample.

Run sanitizers with an independent, complete Ceedling configuration:

```sh
cd external/EsyMcuPlatform
ruby tools/quality/run_module_tests.rb esy_middleware/touch/tests /tmp/esy-touch-sanitizer test:all --sanitizer
```

## Resource Model

The caller owns the touch handle, initialized CS/IRQ GPIO handles, SPI provider,
provider buffers and callback contexts. Keep them alive until accepted sampling
transfers and callbacks drain. No worker task or heap storage is created here.

## Known Limits

One owner context or external serialization must cover polling, sample retrieval
and completion callbacks. A scheduler worker does not automatically serialize a
separate application task accessing this touch handle. SPI failures preserve the
last completed sample; they do not establish a fresh hardware release event.
Host tests do not validate electrical timing, actual CS behavior or DMA/IRQ
retirement on a board.

Regression tests include extreme signed calibration values in every orientation,
CS-low/CS-high and IRQ read failures, and partial GPIO profile application with
successful or failed restoration. GPIO errors must preserve public output values.
