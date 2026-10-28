<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# LVGL runtime

## Purpose

`esy_lvgl` owns one GUI task that serializes UI requests, LVGL rendering, pointer
input, pixel conversion and GUI cleanup. It adapts the public LCD facade and an optional pointer-input
provider;
it does not own a physical display, bus or transport worker. It remains separate
from the SPI scheduler targets.

This runtime is intentionally OS-only and exempt from the caller-driven
core/OS-adapter split. Its GUI task owns rendering and request delivery, and OS
notifications and waits let transport workers progress during asynchronous
flushing. FreeRTOS remains a required dependency; no baremetal runtime or
separate caller-driven core target is provided. This is the `esy_lvgl` runtime
contract, not a requirement for the upstream LVGL library to use a native OS port.

The verified provider is **LVGL v9.6.0**, the latest stable upstream release
checked on 2026-09-21 (released 2026-09-16). The implementation intentionally
rejects other versions until their contracts and tests have been reviewed.

## Public Surface

`include/esy_middleware/lvgl/esy_lvgl.h` exposes:

- `create`: validate/copy configuration and reserve static queue/context storage;
- `start`: create the task asynchronously;
- `get_status`: observe initialization readiness and the first runtime error;
- `send_event` / `send_event_from_isr`: nonblocking asynchronous submission;
- `request_stop`: close admission and request cleanup;
- `destroy`: confirm task parking and release resources without executing LVGL;
- `default_config` and caller-owned storage declaration/binding helpers;
- `esy_lvgl_pointer_input_t`: a copied synchronous pointer-read callback and a
  borrowed argument. A NULL `config.pointer_input` creates no input device.

The public runtime configuration borrows an `esy_lcd_t` defined by the LCD
facade. The `esy_lvgl` target propagates `esy_lcd` as a public dependency, so
consumers obtain the complete LCD type through the runtime header.

Optional `esy_lvgl_touch.h` / `esy_lvgl_touch_input_init()` initialize that
provider from an attached touch handle. Link `esy_lvgl_touch`; the adapter uses
the touch facade and creates no task or persistent resource. There is no direct
touch handle field in `esy_lvgl_config_t`.

No LVGL enums are synthesized in the public header. UI callbacks may include the
real provider headers propagated by the public target dependency.

## Integration

The consumer owns feature selection and supplies a `lvgl` CMake target before
adding the module directly; the module's own CMake does not import `LVGL_PATH`.
The optional `cmake/esy_project_import.cmake` helper imports the consumer's
`LVGL_PATH` only when `ENABLE_ESY_LVGL` is enabled and no `lvgl` target exists.
The consumer retains its local LVGL/FreeRTOS source and submodule selections.

For library host tests, `./setup.sh` prepares seven pinned official providers,
including LVGL; the Python/Ruby test helpers discover their cache automatically.
Firmware CMake does not select sources from this test cache or its lock. See
[software provider setup](../../tools/README.md#software-provider-setup).
Direct module assembly still requires the consumer to assemble the `lvgl` target,
as below. Both integration paths require the same provider version and configuration:

```cmake
set(ENABLE_ESY_LVGL ON)
set(ENABLE_ESY_LCD ON)
set(ESY_RTOS FreeRTOS)
set(ENABLE_ESY_OS_QUEUE ON)
set(ENABLE_ESY_SYSTEM ON)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h")
add_subdirectory("${LVGL_PATH}" "${CMAKE_CURRENT_BINARY_DIR}/lvgl-provider")
# Add EsyMcuPlatform after the provider and the normal board/platform inputs.
target_link_libraries(firmware PRIVATE esy_lvgl)
```

Only the LCD facade is required. A no-input runtime with a custom LCD provider
requires neither `esy_touch` nor GPIO. Missing provider/OS/system/LCD dependencies
fail configuration. Concrete display controllers retain their real hardware
dependencies. Feature OFF adds neither this runtime nor its provider dependency;
the consumer independently controls whether other modules use that provider.

For an existing touch device, enable `ENABLE_ESY_TOUCH`, link `esy_lvgl_touch`, and
initialize a descriptor before creating the runtime:

```c
esy_lvgl_pointer_input_t pointer_input;
esy_err_t result = esy_lvgl_touch_input_init(&pointer_input, &touch);
if (result != ESY_OK) return result;
config.pointer_input = &pointer_input;
```

Create copies the descriptor; its original storage can then be reused. Keep the
touch handle, driver callback context and any asynchronous transport resources
alive until destroy succeeds. The consumer owns touch initialization, graph
registration and transport progress. GPIO is needed only by selected hardware or
optional `esy_lcd_device` / `esy_touch_device` integration. The `esy_lvgl_touch`
target is assembled when `ENABLE_ESY_TOUCH` is enabled.
A custom pointer callback can be supplied directly without that target.

Required LVGL configuration is RGB565
(`LV_COLOR_FORMAT_DEFAULT=LV_COLOR_FORMAT_RGB565`, yielding `LV_COLOR_DEPTH=16`), packed rows
(`LV_DRAW_BUF_STRIDE_ALIGN=1`), one software draw unit, `LV_USE_OS=LV_OS_NONE`,
`LV_USE_STDLIB_MALLOC=LV_STDLIB_BUILTIN`, and `LV_MEM_ADR=0`. No external native
RTOS port is used by LVGL. The caller must
not replace draw-buffer handlers, add hardware/parallel renderers, change display
format/render mode/rotation/offsets, or replace this adapter's callbacks. The
fixed LVGL pool must be sized for the complete UI and its transient allocations.

### Usage Example

Use the public LVGL 9.6.0 provider headers and enable `LV_USE_LABEL` for this
example. These hooks create a label, update its text and release it on the GUI
task. The runtime invokes the stop hook even after partial startup failure.

```c
#include "lvgl.h"
#include "esy_middleware/lvgl/esy_lvgl.h"

typedef struct {
    lv_obj_t *label;
} example_ui_t;

static esy_err_t create_ui(void *arg) {
    example_ui_t *ui = arg;
    if (ui == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    ui->label = lv_label_create(lv_screen_active());
    if (ui->label == NULL) {
        return ESY_ERR_NO_MEM;
    }
    lv_label_set_text(ui->label, "Ready");
    lv_obj_center(ui->label);
    return ESY_OK;
}

static void destroy_ui(void *arg) {
    example_ui_t *ui = arg;
    if (ui != NULL && ui->label != NULL) {
        lv_obj_delete(ui->label);
        ui->label = NULL;
    }
}
```

Keep the zero-initialized UI state and runtime/drawing storage in the persistent
application owner. These hooks use LVGL's configured fixed pool. Other tasks
must submit UI work through `esy_lvgl_send_event()` with a terminal completion
callback, rather than call the hooks or access the label directly.

Example persistent application context (drawing buffer alignment must meet the
chosen provider's `LV_DRAW_BUF_ALIGN`):

```c
example_ui_t ui_state = {0};
ESY_LVGL_STATIC_STORAGE(gui_storage, ESY_LVGL_DEFAULT_EVENT_QUEUE_DEPTH, 4096u);
esy_lvgl_static_resources_t resources;
esy_lvgl_config_t config;
esy_lvgl_t *gui = NULL;

ESY_LVGL_BIND_STATIC_RESOURCES(&resources, gui_storage);
resources.draw_buffer1 = draw_buffer;
resources.draw_buffer_size = sizeof(draw_buffer);
esy_lvgl_default_config(&config);
config.width = display_width;
config.height = display_height;
config.lcd = &lcd;
config.on_start = create_ui;   /* esy_err_t create_ui(void *arg) */
config.on_stop = destroy_ui;  /* void destroy_ui(void *arg) */
config.ui_arg = &ui_state;
config.static_resources = &resources;
config.task_priority = gui_priority;
/* Check each result. Resources must outlive successful destroy. */
esy_err_t result = esy_lvgl_create(&config, &gui);
if (result == ESY_OK) result = esy_lvgl_start(gui);
/* Poll get_status: successful start means STARTING, not necessarily RUNNING. */
```

`create` reserves the singleton without calling LVGL. `start` may create the task
before the scheduler runs, but RUNNING cannot be reached until it is scheduled.
All pointer reads, UI setup/cleanup hooks and queued operations execute on the
GUI task. Hooks
must be bounded and must not block on work needing that same task. The worker's
OS task notification channel is private to the runtime.

The runtime uses the [selected atomic backend](../../docs/integration/atomic_backends.md).
Native ports require always-lock-free 32-bit integers. RP2040 builds use Pico
SDK IRQ-masked hardware spinlocks. Maskable ISR submission may briefly spin on
another core; it performs no RTOS wait. NMI/fault callers and core halt/reset
while the SDK atomic lock is owned are excluded. Qualify ISR latency, concurrent
publishers and shutdown under contention on the target. The consumer still owns
LCD/SPI adaptation, framebuffer capacity and supported board wiring.

Create the runtime, call `start`, and observe RUNNING before submitting work.
Drawing buffers and stack capacity come from `static_resources`. The supported
display path is RGB565/PARTIAL. Every event requires a terminal `complete`
callback. Shutdown uses request-stop/poll/destroy. RP2040 consumers use the SDK
IRQ-spinlock backend described above; include its contention and ISR latency in
target measurements.

## Contracts

### Ownership and state

First-use handles require `{0}`; the storage macro supplies it. Backing OS and
pixel buffers need no clearing. Never reset, copy or move live storage. The
configuration and resource descriptor are copied; their pointed-to buffers,
LCD handle, pointer-provider argument and callback contexts remain borrowed
until destroy succeeds.
Storage objects must be distinct; drawing buffers must not overlap.

Create/start/destroy have one externally serialized owner. Stop may also be
requested from a GUI callback; other tasks serialize stop with start. Submit is
MPSC from tasks and ISR, using the OS queue and the selected 32-bit atomic backend. Status readers may run
concurrently, but the owner must exclude all API calls before destroy. Accepted
publishers retain admission references through queue publication and notification;
stopping waits for those references before finishing cancellation. Callers must
not start a new API call through a handle after successful destroy.

States are INITIALIZED -> STARTING -> RUNNING -> STOPPING -> STOPPED/FAILED.
Allocation or UI initialization failure in the task ends at FAILED after partial
GUI cleanup. `last_error` latches the first asynchronous error. State/error are
individual atomic observations; terminal state publication precedes actual task
parking, so destroy checks the OS as well. A stopped/failed instance is recreated
only after successful destroy. Queue creation failure retains nothing. Task
creation failure leaves INITIALIZED and permits start retry. Duplicate create
returns STATE without overwriting the output handle.

### Request delivery and scheduling

Submission copies the descriptor and borrows its argument. Rejection retains
nothing. Each accepted request gets exactly one GUI-task terminal callback:
EXECUTED after its operation returns, or CANCELLED without invoking it. Completion
can occur before submit returns. The completion callback may release the argument.
No synchronous cross-task LVGL API is provided. Callbacks must not invoke destroy.

Each iteration processes at most `event_budget` requests before rendering. A
budget-exhausted iteration also delays one OS tick interval (via a 1 ms request)
to give lower-priority tasks CPU time. The timer deadline is computed after UI
operations. Task notifications wake idle processing; waits are capped at 20 ms,
including the LVGL no-timer sentinel and a failed notification after acceptance.
A failed wakeup does not turn a committed request into a rejected request.

### Display and input

The adapter creates one RGB565/PARTIAL display and optional pointer input. Each
writable draw buffer must hold at least one complete display row and satisfy
`LV_DRAW_BUF_ALIGN`. Display coordinates and capacity are validated before pixel
access. The adapter converts native RGB565 to the configured packed LCD byte order in place
and restores them on actual completion or immediate rejection. No second byte
swap is permitted. The concrete LCD must be configured to accept RGB565.
`rgb565_byte_order` defaults to `ESY_LVGL_RGB565_MSB_FIRST`; select
`ESY_LVGL_RGB565_LSB_FIRST` for modules requiring low-byte-first pixel payloads.
The choice is independent of CPU endianness, SPI bit order and RGB/BGR channel
selection. Invalid values are rejected by create before resources are reserved.
Use the default initializer or zero initialization for the configuration.

LCD completion runs inline or on a transport task, never ISR. It only wakes the
GUI and publishes completion as its last context access. The GUI alone restores
pixels and calls flush-ready. An LVGL flush-wait callback blocks using OS APIs,
allowing the transport worker to progress even when it has lower priority.

An accepted transfer owns its buffer until its actual callback. A timeout latches
TIMEOUT and closes admission but does not invent completion or release the buffer.
The runtime remains owned and destroy returns BUSY if the transport never retires.
The LCD completion contract reports buffer retirement, not physical display
success. Transport failures after acceptance require the transport owner's own
status/recovery policy. Pointer-provider read failures map to a released pointer.

### Stop, cleanup and OTA

The consumer first stops external producers, then requests stop. The runtime
finishes an already executing operation, cancels unstarted accepted requests,
waits for accepted transfers, runs `on_stop`, deletes LVGL input/display objects,
and cooperatively parks. `on_stop` also runs after a failed `on_start`; it must
remove any application timers and external callbacks referencing UI objects.
Only successful checked task deletion permits storage reuse.

The GUI task must stop before OTA operations requiring display/SPI silence. Keep
transport workers/DMA completion machinery running until GUI stop finishes. The
consumer uses a bounded deadline; timeout aborts that OTA transition, preserving
all outstanding resources. Physical transport shutdown belongs to its owner.
After successful destruction, OTA failure/cancellation can recreate the GUI once
resources are available; a partially stopped instance must first finish cleanup.
No product watchdog or Flash policy is embedded here. A callback that does not
return can prevent shutdown and requires the consumer's system recovery policy.

## Resource Model

One caller-owned context, task stack/control block, OS queue/control block and
one or two drawing buffers are used. No adapter OS/libc heap allocation occurs.
The default event budget is 8 and flush error deadline is 1000 ms; the consumer
may override both. Measure actual GUI callback stack requirements on the target.

LVGL core is exclusively initialized once on the GUI task and retained for process
lifetime across GUI restart. Other code must not call `lv_init`, `lv_deinit` or
mutate global LVGL allocator/rendering handlers. LVGL dynamically allocates its
objects from its fixed built-in pool; this is explicitly different from a claim
that every UI object is statically allocated. Its pool has a fixed upper bound;
no allocator expansion or OS/libc allocator is enabled by this adapter.

## Verification

Run `./setup.sh` once from the platform repository root to prepare the cached
software providers, including LVGL. The paths and commands below are relative to
this module.

- `tests/project.yml`: public API lifecycle, failure rollback, MPSC stop boundary,
  cancellation, event fairness, idle wakeups, copied generic input providers and
  inline/deferred/rejected/timed-out flushing using reusable fakes. The optional
  touch descriptor has focused validation, facade-read and error tests.
- `python3 tests/test_cmake_consumer.py`: disabled feature (including RP2040
  and GPIO OFF), missing runtime dependencies, C/C++ public headers,
  STM32F4/PICO/PICO_W/PICO2/PICO2_W configuration acceptance, and rejection of
  unsupported provider versions/configurations. Fake-provider smoke builds,
  links without section garbage collection, and runs with GPIO disabled both
  without touch and with the separate touch adapter.
  Platform-name configuration cases do not replace target-toolchain validation.
- The same consumer suite compiles the cached pinned provider and exercises real
  partial rendering, native RGB565 output,
  asynchronous completion, UI requests and two complete create/start/stop/destroy
  cycles with single and double drawing buffers. The host OS fake models cooperative
  task/transport interleavings; it is not a native RTOS scheduler.
- CI runs the real pinned provider smoke separately from the deterministic fake
  unit suite. Host sanitizer configuration is provided in `tests/sanitizer.yml`.
- Source contracts and C/C++ public-header checks apply. Embedded consumer builds
  with this feature disabled prove only the existing integration boundary.

Optionally set the `LVGL_PATH` environment variable or pass
`--lvgl-path /path/to/lvgl-v9.6.0` to test another compatible LVGL checkout.

Both byte orders have regression coverage for deferred completion, inline
completion and rejection, including native draw-buffer restoration. Invalid
byte-order configuration is rejected without creating a task.

## Known Limits

One runtime/display, RGB565/PARTIAL and the pinned LVGL provider are supported.
Native always-lock-free or verified RP2040 SDK atomic operations are required,
including for task-only use with a custom provider. Native SMP/ISR/DMA,
cache coherency, priority interactions, stack bounds, memory-pool exhaustion and
OTA timing depend on the selected target and provider. Drawing memory must be
accessible to the chosen transport, with cache maintenance provided by that
transport contract.
LVGL's internal allocation/assertion policy is not converted into universal
recoverable errors; an exhausted provider pool can still invoke its configured
assertion handler. No transport cancellation or forced GUI-task deletion exists.
