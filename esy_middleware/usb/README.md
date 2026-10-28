# USB Middleware

## Purpose

`esy_middleware/usb` provides reusable USB middleware adapters for firmware
projects that already select and initialize a USB device stack in their product
integration layer. The current component is a USB CDC adapter with a module
private backend boundary. The default backend uses TinyUSB CDC class APIs.

The module is intentionally thin: it does not allocate memory, create tasks,
own descriptors, or introduce an extra buffering layer between the application
and backend CDC FIFOs.

## Public Surface

- `esy_usb_cdc_default_config()` fills a CDC instance configuration with safe
  defaults.
- `esy_usb_cdc_init()` binds a caller-owned handle to one CDC interface index.
- `esy_usb_cdc_deinit()` detaches a handle and releases its interface binding.
- `esy_usb_cdc_connected()` reports whether the backend sees the interface as
  connected and DTR is asserted.
- `esy_usb_cdc_get_session_id()` identifies closed-session boundaries when
  `discard_on_disconnect` is enabled; otherwise it returns zero.
- `esy_usb_cdc_available()`, `esy_usb_cdc_read()`, `esy_usb_cdc_write()`, and
  `esy_usb_cdc_flush()` forward directly to backend CDC FIFO operations.
- `esy_usb_cdc_task()` runs the backend USB device task pump.
- `tud_cdc_rx_cb()` and `tud_cdc_line_state_cb()` are implemented by the module
  and dispatch TinyUSB CDC callbacks to the registered instance.

## Integration

Feature selection belongs to the consuming project. Enable the module from the
consumer feature import file, then link the `esy_usb` target through the normal
`esy_middleware` aggregation target.

```cmake
set(ENABLE_ESY_USB ON)
set(ENABLE_ESY_USB_CDC ON)
set(ENABLE_ESY_USB_BACKEND_TINYUSB ON)
```

The consuming project must also provide TinyUSB device integration, descriptors,
board bring-up, a `tinyusb_device` CMake target, and a valid `CFG_TUD_CDC` value. The CDC interface index passed
to `esy_usb_cdc_init()` must be less than both `CFG_TUD_CDC` and
`ESY_USB_CDC_MAX_IFACES`. The public `esy_usb` target propagates its common-type
header dependency; consumers need no manual common include paths.

When both CDC and backend gates are OFF, no `esy_usb` target, CDC source, TinyUSB
dependency, or protocol USB channel is assembled, even if `ENABLE_ESY_USB` is ON.
The top-level module gate OFF also excludes USB regardless of child gates.

Integration checklist:

- Select `ENABLE_ESY_USB` and `ENABLE_ESY_USB_CDC` from the consumer feature gate.
- Select exactly one CDC backend feature. The currently available backend is
  `ENABLE_ESY_USB_BACKEND_TINYUSB`.
- Provide TinyUSB device configuration, descriptors, endpoint memory, and board
  USB bring-up.
- Decide where `esy_usb_cdc_task()` runs and keep that scheduling compatible
  with the backend USB stack.
- Register RX and line-state callbacks only after the owning handle and callback
  context are fully initialized.
- Keep stdio-over-CDC ownership explicit. If another subsystem owns the same CDC
  interface, do not bind that interface to this adapter.
- Override `ESY_USB_CDC_MAX_IFACES` if the product exposes more than four CDC
  interfaces. The supported registry capacity is 1..255; out-of-range values
  are rejected at compile time. Set layout overrides such as
  `ESY_USB_CDC_PRIV_BYTES` consistently for the library and every consumer.

### Usage Example

After successful `esy_usb_cdc_init()`, the single stack owner can poll one RX
chunk per iteration. This example borrows the initialized CDC handle and calls
the USB pump itself; select a provider whose task pump returns within the owner's
execution budget. The consumer has already initialized TinyUSB and supplied its
descriptors. No other task or callback may consume RX from this interface.

```c
#include "esy_middleware/usb/esy_usb_cdc.h"

esy_err_t example_usb_read_step(esy_usb_cdc_t *cdc, uint8_t *buffer,
                                uint32_t capacity, uint32_t *received) {
  if (cdc == NULL || buffer == NULL || capacity == 0u || received == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  *received = 0u;
  esy_usb_cdc_task();
  if (!esy_usb_cdc_connected(cdc)) {
    return ESY_ERR_STATE;
  }
  *received = esy_usb_cdc_read(cdc, buffer, capacity);
  return *received == 0u ? ESY_ERR_BUSY : ESY_OK;
}
```

On `ESY_OK`, process exactly `*received` bytes before reusing the buffer. `BUSY`
means no bytes are ready; schedule another iteration. A read is a byte chunk,
not a complete application message. Keep the handle and any callback context
resident, stop all callbacks/I/O, then call `esy_usb_cdc_deinit()` before releasing
them. The stack remains consumer-owned after adapter deinit.

## Contracts

Each `esy_usb_cdc_t` handle is caller-owned storage. First init accepts
uninitialized storage: it consults the registry without reading handle contents.
A handle may be initialized, deinitialized, and initialized again. Initializing an
already-bound handle or binding another handle to its occupied interface returns
`ESY_ERR_STATE` without changing either binding or handle. All init failures leave
storage unchanged and acquire no new resources. Failed first init may be retried
or cleaned up with deinit; do not use query/I/O APIs on uninitialized storage.

Deinit accepts first-use storage, including storage from failed init, and clears
it to the detached state. It cannot fail, is synchronous and idempotent, and does
not stop the backend stack or discard FIFO data. Keep handles at stable addresses
and callback arguments alive until deinit returns. Do not copy or clear a bound
handle to bypass cleanup. The init configuration is copied and need not outlive
init. Lifecycle calls must not run in an ISR or race with callbacks/I/O.

```c
esy_usb_cdc_t cdc;
esy_usb_cdc_config_t cfg;
esy_usb_cdc_default_config(&cfg);
esy_err_t status = esy_usb_cdc_init(&cdc, &cfg);
/* After success, pump the stack and poll available/read on the owner task. */
/* Stop callbacks and I/O before cleanup, including after failed first init. */
esy_usb_cdc_deinit(&cdc);
```

The public handle contains opaque private storage. Downstream code must not
modify handle fields directly. The `ctx` pointer is observable only as an
initialized/detached sentinel.

Read and write APIs are non-blocking and directly reflect backend FIFO behavior.
They may short-read or short-write. NULL handles, detached handles, NULL buffers,
and zero lengths return `0` or no-op according to the API return type.

`discard_on_disconnect` defaults to false, preserving the backend FIFO contents
across DTR closure. Set it true before init for a session-oriented byte stream,
including the protocol USB channel. DTR deassertion clears software RX/TX FIFOs
and advances a nonzero session generation before invoking the line callback.
The USB task also observes physical disconnects before and after pumping the
provider. RX notifications and task pumps discard bytes received while closed.
Reopening does not clear RX, so bytes already sent by the new peer survive.
The generation changes even if DTR closes and reopens between owner polls;
unchanged DTR with only an RTS change leaves it unchanged. Compare generations
for inequality, not ordering; they wrap while skipping zero and reset on init.
Physical unplug without a DTR callback must be observed by the task before
reconnect, and the provider must discard endpoint state on bus reset. The policy
cannot recall bytes already submitted to an endpoint or cancel USB transfers.
It does not alter deinit's FIFO-preserving behavior.

The default NULL RX callback preserves incoming FIFO data for polling reads.
Applications must drain it regularly to avoid backend RX backpressure. To discard
input, explicitly consume it.

RX and line-state callbacks run in TinyUSB callback context. Callback
implementations must stay short and non-blocking; heavy work should be handed to
an application-owned task or event path.

The adapter does not add locks. Lifecycle calls (`init` and `deinit`) are
single-owner operations and must not race with callbacks or I/O calls. `read`
may be called from the RX callback for the same interface when the backend
supports that usage. `write`, `flush`, and `task` must follow the backend stack's
threading rules. When multiple tasks may use the same CDC interface, the
consumer must serialize those calls above this module.

## Backend Model

The public CDC adapter depends only on the internal
`src/esy_usb_cdc_backend.h` contract. `esy_usb_cdc_backend_tinyusb.c` is the
default implementation and is the only production file in this component that
includes TinyUSB headers directly.

`ENABLE_ESY_USB_CDC` and `ENABLE_ESY_USB_BACKEND_TINYUSB` are intentionally
separate: the first selects the CDC adapter, while the second selects the
concrete backend and links `tinyusb_device`. Enabling CDC without a backend, or
enabling the TinyUSB CDC backend without the CDC adapter, is a CMake error.
A missing `tinyusb_device` provider also fails at configure time.

Compatibility is defined by the TinyUSB CDC `tud_cdc_n_*` functions used in the
backend and `tud_task()`, rather than a claimed range of untested releases. The
consumer supplies the TinyUSB version, configuration and OS port through the
provider target. Firmware builds and HIL procedures exercise that combination.
The adapter does not initialize TinyUSB. Its task pump calls `tud_task()` directly:
blocking and callback execution follow that provider's OS mode. Use one stack
owner; only integrate the pump into a periodic application loop when the selected
provider returns within that loop's latency budget. Otherwise use a dedicated
stack task and backend-compatible serialization for I/O. Do not call the pump
from an ISR or recursively from callbacks.

Backend responsibilities:

- report the backend CDC interface count;
- pump the USB stack;
- expose connection and line-state queries;
- forward CDC RX/TX FIFO operations.

The backend boundary is module-private. Downstream applications should not
include or call backend headers directly.

## Resource Model

The module uses no dynamic allocation. Runtime state consists of:

- caller-owned `esy_usb_cdc_t` handles;
- one module-private registry that maps CDC interface indexes to handles;
- backend-owned CDC RX/TX FIFOs.

The module does not own USB descriptors, endpoint memory, task stacks, RTOS
objects, or board resources.

## Verification

Host unit tests live in `esy_middleware/usb/tests/` and use the reusable
TinyUSB fake under `esy_test/fake/third_party/tinyusb/`.

Run from the EsyMcuPlatform repository root:

```sh
python3 -B esy_middleware/usb/tests/test_cmake_consumer.py
(cd esy_middleware/usb/tests && ceedling test:all && ceedling gcov:all)
```

Run the source contract checks after modifying this module:

```sh
tools/checks/check_source_contracts.sh esy_middleware/usb
```

The quality gate runs these source checks and CMake tests in addition to module
unit tests. Regressions cover first init with uninitialized/nonzero storage,
failed-init cleanup/retry, duplicate binding preservation, polling RX and short
I/O. CMake tests compile and run C/C++ consumers through direct and aggregate
targets. Session regressions cover opt-in FIFO cleanup on DTR/physical closure,
default FIFO preservation, generation tracking, RTS-only changes, closed RX and
preserving new-peer RX after reopening. CMake tests also cover all feature
combinations, missing providers, disabled protocol
channel assembly and registry capacity boundaries. The TinyUSB fake does not
model IRQ timing, OS blocking or physical transfers.

Consumer-project builds remain the final integration check because TinyUSB
descriptors, board bring-up, and USB IRQ/task scheduling are product-specific.
Use `tests/smoke/usb_cdc_hil_checklist.md` to record board-level CDC enumeration,
open/close, DTR/RTS, RX/TX, unplug/replug, and multi-interface smoke results.

## Known Limits

- Only the TinyUSB CDC backend is currently implemented.
- The adapter does not provide a middleware-owned RX/TX ring buffer.
- The adapter does not serialize concurrent callers; applications must keep
  access compatible with the backend and their own execution model.
- USB descriptors and `CFG_TUD_CDC` are supplied by the consuming project.
