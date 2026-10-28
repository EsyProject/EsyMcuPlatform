<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Generic Input

## Purpose

`esy_input` provides caller-driven button debounce, short/long/double-click
recognition and quadrature encoder decoding through a shared event sink. The
consumer owns source IDs, key/axis codes, hardware, scheduling and event routing.
The core supports baremetal main loops and existing owner tasks without an OS,
HAL, LVGL, queue or worker dependency. An optional GPIO provider reads through
public HAL APIs.

## Public Surface

- [`esy_input.h`](include/esy_middleware/input/esy_input.h): caller-owned button
  and encoder handles, configuration, init/deinit, logical sample entry points
  and the common `esy_input_event_t` callback.
- [`esy_input_gpio.h`](include/esy_middleware/input/esy_input_gpio.h): optional
  button and encoder polling providers borrowing initialized public GPIO handles.

`KEY` reports a debounced level with value 1 for press and 0 for release.
`CLICK`, `DOUBLE_CLICK` and `LONG_PRESS` report gestures; `RELATIVE` reports an
encoder step with value +1 or -1. Every event includes a consumer-defined source
ID, code and recognition timestamp. Callbacks receive one complete event, whose
address is valid only until the callback returns.

## Integration

Set `ENABLE_ESY_INPUT` in the consumer's `esy_feature_import.cmake`, then link the
core target:

```cmake
target_link_libraries(your_target PRIVATE esy_input)
```

With `ENABLE_ESY_INPUT` and `ENABLE_ESY_GPIO` enabled, `esy_input_gpio` is also
available. Link that target when using GPIO polling; its public dependencies
provide the core and HAL include paths. No input target is created when the
input feature is disabled. No OS feature is required.

The following owner-local example counts recognized single clicks. The chosen
durations are examples; set them from the consumer's switch and interaction
requirements. Call `esy_input_button_sample()` regularly with the logical
pressed level and the owner's wrapping monotonic millisecond timestamp.

```c
#include "esy_middleware/input/esy_input.h"

static void count_clicks(const esy_input_event_t *event, void *arg) {
  uint32_t *clicks = arg;
  if (event->type == ESY_INPUT_EVENT_CLICK) {
    *clicks += 1u;
  }
}

/* Storage and callback context stay alive until deinit returns. */
esy_input_button_t button = {0};
uint32_t clicks = 0u;
const esy_input_button_config_t config = {
    .source_id = 1u,
    .code = 1u,
    .debounce_ms = 10u,
    .long_press_ms = 800u,
    .double_click_ms = 250u,
    .on_event = count_clicks,
    .event_arg = &clicks,
};
esy_err_t err = esy_input_button_init(&button, &config);
/* Check err before starting the owner loop. Inside that loop: */
err = esy_input_button_sample(&button, pressed, now_ms);
/* Stop the loop before releasing the input or callback context. */
err = esy_input_button_deinit(&button);
```

For a GPIO button, keep the existing GPIO lifecycle owner and replace the
logical sample call with this provider. `gpio` must be an initialized input:

```c
#include "esy_middleware/input/esy_input_gpio.h"

const esy_input_gpio_button_t provider = {
    .gpio = gpio,
    .active_low = true,
};
err = esy_input_gpio_button_poll(&button, &provider, now_ms);
```

Encoders use a separate `{0}` `esy_input_encoder_t`, an
`esy_input_encoder_config_t` with `edges_per_step` equal to 1, 2 or 4, and
`esy_input_encoder_init()`. Submit AB directly with
`esy_input_encoder_sample()`, or use a GPIO provider with distinct initialized
inputs:

```c
const esy_input_gpio_encoder_t encoder_provider = {
    .phase_a = phase_a,
    .phase_b = phase_b,
    .inverted_a = false,
    .inverted_b = false,
};
err = esy_input_gpio_encoder_poll(&encoder, &encoder_provider, now_ms);
```

An application can map `KEY` to navigation keys and accumulate `RELATIVE` steps
for its GUI input reader. Copy events into the GUI owner's existing bounded
request/queue interface when the input owner is a different task, and define
overflow handling there. Only the GUI owner accesses LVGL. This module does
not create LVGL keypad/encoder devices or change the existing touch/pointer
integration; the consumer selects the GUI mapping and routing policy.

## Contracts

### Buttons and encoders

Buttons start logically released. A button already held on the first sample
must satisfy the configured debounce interval before emitting `KEY` press.
A changed raw candidate must remain stable for `debounce_ms`; zero commits it
immediately. Long-press time starts at the debounced press and emits once per
hold. Release exactly at its threshold is a long press. That press still emits
`KEY` release, but does not emit a click gesture.

Without double-click recognition, a short release emits `KEY` release followed
by `CLICK`. With it enabled, single-click delivery waits for the window to
expire. The second short release must occur strictly before
`double_click_ms` from the first short release to emit `DOUBLE_CLICK` instead.
A second release exactly at the boundary starts a new click sequence. A pending
first click can expire while another press is held. Zero `long_press_ms` or
`double_click_ms` disables that gesture independently.

Encoder AB uses A in bit 1 and B in bit 0. The positive sequence is
`00 -> 01 -> 11 -> 10 -> 00`; `inverted` reverses the reported direction.
The first sample seeds AB without emitting motion. Adjacent reverse transitions
cancel accumulated edges. A two-bit jump returns `ESY_ERR_IO`, discards partial
motion and resynchronizes to the new AB state without an event. Contact bounce
that retraces adjacent edges cancels naturally; no time-based encoder debounce
or acceleration is applied.

### Time and progress

The caller supplies a monotonic `uint32_t` millisecond clock modulo 2^32, which
wraps approximately every 49.7 days. All configured button durations are in
`[0, INT32_MAX]` milliseconds. Every successful sample gap must be at most
`INT32_MAX` milliseconds, including idle periods. A pending timer is observed
within its duration plus that maximum gap, strictly less than a full cycle.
Completed and cancelled timers remain inactive through later wraps; only a new
gesture rearms them. Observably backwards/oversized sample gaps return
`ESY_ERR_STATE` without modifying the instance. A whole unobserved clock cycle
cannot be detected from two 32-bit timestamps.

Sample much faster than the shortest physical pulse or encoder edge; the
32-bit clock bound does not establish a usable physical sampling rate. Callback
delivery is delayed by sampling latency. If the clock advances during sleep,
elapsed sleep counts; if the clock pauses, gesture timing freezes. After a
clock reset or any pause whose duration cannot satisfy the sample bound, stop
the owner, deinitialize to cancel pending gestures and initialize again before
resuming. The module cannot recover unsampled input transitions.

### Lifecycle, concurrency and errors

Initialize each handle with `{0}` and keep its address stable. Init copies its
configuration and requires a non-NULL event callback. Init failure preserves
storage, repeated init returns `ESY_ERR_STATE`, and successful deinit restores
zero storage. Deinit is idempotent, emits no events, discards pending gestures
or partial movement, and permits reinitialization. Do not copy, move, clear or
modify a live handle.

One main-loop/task owner serializes input operations, GPIO lifecycle/PM changes
and callback-context lifetime. No API is ISR-safe. Callbacks run synchronously
inside sample/poll, must return promptly, and must not reenter the same input
instance. Reentrant sample/deinit returns `ESY_ERR_BUSY`; that guard does not
replace external serialization. Core work per sample is constant plus at most
three button callbacks or one encoder callback. GPIO poll additionally includes
one or two synchronous HAL reads. Provider and callback latency must be included
in the consumer's scheduling budget.

Invalid pointers/configuration return `ESY_ERR_INVALID_ARG`; inactive, copied
or discontinuously timed core handles return `ESY_ERR_STATE`. Apart from encoder
resynchronization on `ESY_ERR_IO`, core sample errors preserve input state.
GPIO adapters validate pointer arguments first, then read the GPIOs, and only
after successful reads delegate handle/time/reentry validation to the core.
They return the first HAL error without advancing input state or timers.
Repeated provider failures therefore count toward the successful-sample gap.
Encoder phases are read sequentially: no core sample occurs if either read
fails, and identical phase handles are rejected. A phase transition between
reads can produce an inconsistent AB pair; use a consumer-supplied coherent
snapshot and the core sample API when sequential reads are insufficient.

## Resource Model

Each consumer owns its input handles, copied configuration and borrowed sink
context. The core has no heap allocation, singleton, task, queue, timer object,
IRQ, DMA or watchdog registration. A GPIO provider borrows handles only for the
poll call and never initializes, reconfigures or deinitializes them. GPIO IRQs,
power states and wake policy remain with their existing owner.

There are no module-created tasks to stop for OTA. The consumer decides whether
its sampling owner can continue during flash operations, and accounts for GPIO
availability, callback destinations, event queues and watchdog obligations.
Where quiescence is required, stop that owner before the operation and cancel
pending gestures with deinit; resume from a new initialization after success,
failure or cancellation. Deinit is synchronous and has no stop handshake: the
caller must first ensure polling and callback delivery have finished.

## Verification

The host suites and CMake consumers live in [`tests/`](tests/). From that
directory, run:

```sh
ceedling test:all
ceedling gcov:all
python3 -B test_cmake_consumer.py
```

The tests exercise button bounce and gesture boundaries, encoder directions,
invalid transitions, lifecycle/reentry, wrapping clocks and provider errors.
CMake consumers verify feature-off, core-only and optional-GPIO assembly,
including public C/C++ header use without an OS dependency. Hardware validation
must measure the actual switch bounce, encoder edge spacing, GPIO latency and
owner/callback scheduling delay for the chosen polling rate.

## Known Limits

The module does not scan key matrices, perform key repeat/encoder acceleration,
register interrupt-driven input, create an event queue or automatically bind a
GUI input device. It cannot reconstruct missed pulses or full encoder cycles,
guarantee coherent sequential GPIO reads, or enforce queue capacity in a
consumer callback. Recognition timestamps are sample times rather than physical
edge timestamps. Host tests establish logic and integration contracts; they do
not establish board-level signal quality, timing or GUI responsiveness.
