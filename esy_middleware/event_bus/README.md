<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# esy_event_bus

## Purpose

`esy_event_bus` distributes local, value-based messages from multiple publishers
to independent subscriber queues. Acquisition, GUI, recording and communication
tasks can consume the same event without competing for one queue entry.

This OS-backed software middleware uses
public OS queues and short task/ISR critical sections. It creates no worker task,
invokes no business callbacks, and accesses no hardware. Existing SPSC rings
cannot provide the required multiple-publisher and independent-consumer semantics.
There is currently no bare-metal execution mode.

## Public Surface

Include `esy_middleware/event_bus/esy_event_bus.h`:

- `esy_event_bus_init()` / `esy_event_bus_deinit()` bind and release bus storage.
- `esy_event_bus_subscribe()` / `esy_event_bus_unsubscribe()` manage independent
  static subscriber queues.
- `esy_event_bus_publish()` and `esy_event_bus_publish_from_isr()` copy one event
  to matching subscribers and report individual acceptance counts.
- `esy_event_bus_receive()` receives one message in its subscriber's task.

There are 32 consumer-assigned topic IDs, from 0 through 31. A subscription mask
matches one or several IDs. Each event carries a payload length and up to 64
inline bytes; unused payload bytes in queued copies are zeroed. These are library
format limits, not product topic assignments or queue-size defaults.

## Integration

Declare the capability in the consumer's selected feature entry:

```cmake
set(ESY_RTOS "FreeRTOS")
set(ENABLE_ESY_OS_QUEUE ON)
set(ENABLE_ESY_EVENT_BUS ON)
```

Link `esy_event_bus` directly or use the `esy_middleware` aggregate. The public
target propagates `esy_os` and `esy_common_defs`. Disabled assembly creates no
event-bus target or source; enabled assembly rejects a missing OS/queue provider.
The library supplies no default feature selections.

### Usage Example

This single-task example subscribes, publishes one value, receives its copy and
cleans up. The caller supplies a persistent, zero-initialized `example_bus_t`;
the OS and queue provider are already initialized. No other publisher, receiver
or lifecycle caller uses this instance during the example.

```c
#include "esy_middleware/event_bus/esy_event_bus.h"

#define EXAMPLE_QUEUE_DEPTH 4u

typedef struct {
  esy_event_bus_t bus;
  esy_event_bus_subscription_t slots[1];
  ESY_ALIGNED_STORAGE(queue, ESY_EVENT_BUS_QUEUE_BYTES(EXAMPLE_QUEUE_DEPTH));
  esy_event_bus_subscription_t *subscription;
} example_bus_t;

esy_err_t example_bus_once(example_bus_t *owner,
                           esy_event_bus_event_t *received) {
  if (owner == NULL || received == NULL) return ESY_ERR_INVALID_ARG;
  const esy_event_bus_static_resources_t storage = {owner->slots, 1u};
  const esy_event_bus_config_t config = {&storage};
  esy_err_t status = esy_event_bus_init(&owner->bus, &config);
  if (status != ESY_OK) return status;
  const esy_event_bus_subscription_static_resources_t queue = {
      owner->queue.bytes, sizeof(owner->queue.bytes)};
  const esy_event_bus_subscription_config_t subscriber = {
      .topic_mask = ESY_EVENT_BUS_TOPIC_BIT(0u),
      .queue_depth = EXAMPLE_QUEUE_DEPTH, .static_resources = &queue};
  status = esy_event_bus_subscribe(&owner->bus, &subscriber,
                                   &owner->subscription);
  if (status == ESY_OK) {
    const esy_event_bus_event_t event = {.topic = 0u, .size = 1u, .data = {42u}};
    esy_event_bus_publish_result_t delivery;
    status = esy_event_bus_publish(&owner->bus, &event, &delivery);
    if (status == ESY_OK) {
      status = esy_event_bus_receive(owner->subscription, received, 0u);
    }
    esy_err_t cleanup = esy_event_bus_unsubscribe(owner->subscription);
    if (cleanup != ESY_OK) return cleanup;
    owner->subscription = NULL;
  }
  esy_err_t cleanup = esy_event_bus_deinit(&owner->bus);
  return cleanup == ESY_OK ? status : cleanup;
}
```

On `ESY_OK`, `received` contains topic zero and the one-byte value 42. These
values and the queue depth are examples, not reserved product assignments.
Resource descriptors may be temporary; bus, slots and queue bytes remain
resident until cleanup succeeds. A failed subscribe retains no queue. If
unsubscribe fails, retain the owner, retire outstanding users and retry
unsubscribe before bus deinit. Retry failed bus deinit before reusing storage.
Do not call the example again on a still-live bus.

For multiple subscribers, allocate a separate queue buffer per subscription
and keep each receive operation on that subscriber's own task.

For publication, populate `esy_event_bus_event_t.topic`, `.size` and `.data`, then
pass a separate `esy_event_bus_publish_result_t`. Use `memcpy` between typed
payload values and the byte array; do not cast the byte array to an arbitrary
payload type. On `ESY_ERR_BUSY`, inspect `matched`, `delivered`, `failed` and
`first_error` before deciding whether recovery or a later update is needed.

The subscriber's own task calls `esy_event_bus_receive()` with a finite timeout
and performs its own business processing. Application integration must retain
narrow request/post interfaces; exposing service contexts, queue handles or
another task's main loop is unnecessary. GUI messages must be consumed by the
GUI execution owner before updating controls.

## Contracts

### Delivery and ordering

Each matching subscriber receives a separate complete copy. Publisher storage
may be reused after the publish call returns. The bus attempts every matching
open subscriber even after a queue failure; it never waits for space. A full
queue rejects that subscriber's new message while leaving other subscribers
independent. `ESY_ERR_BUSY` reports any delivery failure, with the first raw OS
error in `first_error`. No matching subscriber is a successful zero-delivery call.

Acceptance means enqueued, not processed. Partial delivery is not rolled back.
Retrying the same publication can duplicate copies at subscribers that already
accepted it. Membership is observed per slot during traversal, not as a global
snapshot. Concurrent publications may interleave differently across queues;
there is no global ordering. Each individual queue preserves its accepted FIFO.

All topics use the same reject-new policy. There is no retained state, automatic
coalescing, persistence, retry or guaranteed lossless delivery. A GUI may discard
intermediate states in its own bounded consumer; reliable recording requires a
separately specified capacity and overflow/recovery policy. Do not depend on this
best-effort distribution for all-or-nothing commands or stop acknowledgements.

### Concurrency and ISR use

Multiple task and OS-API-safe ISR publishers may use a bus concurrently. Each
subscriber has exactly one task consumer. One external task lifecycle owner
serializes subscribe, unsubscribe and deinit, including retries. Init/reinit
requires every user stopped. The registry lock protects admission, references
and publication of queue state; queue operations and payload copies run outside
it. This follows the port's task/ISR and SMP critical-section guarantees.

Only `publish_from_isr` may run in an ISR. It clears its optional `need_yield`
output and ORs wake requests from all sends. It never yields internally. The IRQ
owner must combine this result with any other notification results and yield
once after all attempts. Native OS interrupt-priority restrictions apply.

Publish scans the configured slot capacity and copies one fixed-size event per
match. Bound both capacity and message rate against the actual interrupt/task
budget; the API alone is not a hard-real-time latency guarantee. Prefer the
acquisition owner task publishing completed business data when other consumers
do not need the raw interrupt. Hardware callback registration retains its unique
external owner; the bus does not register or acknowledge IRQs.

### Lifecycle and time

The bus handle must initially be `{0}`; slot and queue backing bytes may be
arbitrary. Slot and data buffers must be exclusive, correctly aligned and sized,
non-overlapping, and unused by other objects. A repeated bus init returns `STATE`
without modifying the live binding. Init failure retains no resources. Handles
and slots must never be copied, moved, manually cleared or reused while live.

Unsubscribe closes that subscriber's admission before checking in-flight
send/receive references. `BUSY` means cleanup is incomplete: retain all storage
and retry after outstanding operations finish. No new receive enters a closing
subscription. Successful unsubscribe deletes the queue, discards pending messages
and invalidates returned slot aliases. Stop prospective receivers before
releasing or reusing storage; a new subscription may reuse a freed slot.

Deinit closes publication/subscription admission and returns `BUSY` until every
subscription and admitted publisher has retired. Unsubscribe remaining slots and
retry. A failed teardown stays closed. Successful deinit permits reinit, and
repeated deinit of the same resident handle is harmless. Stop all prospective
callers before reclaiming the bus or slots, including callers not yet admitted.
Memory remains owned by the caller on every path.

Receive accepts zero for polling and finite millisecond waits. `UINT32_MAX` is
rejected because it means an indefinite native wait. Blocking receive holds a
reference until the OS wait completes; select a finite bound that permits the
required stop acknowledgement. Tick width, wrap, rounding, saturation and
suspended-tick behavior are inherited from the public OS queue wrapper. The bus
adds no timestamp arithmetic, periodic work, delayed retries or local deadlines.
After receive returns, no timer or waiting state remains to reactivate on wrap.

For OTA/Flash operations requiring silence, the application must stop relevant
publishers/IRQs and consumers, confirm their required bounded acknowledgements,
then retire subscriptions and the bus. Closing the bus alone does not prove that
hardware, DMA, pending IRQs or a consumer's business work have stopped. The bus
owns no task, watchdog registration, DMA, hardware timer or OTA policy.

## Resource Model

All resources are caller-owned. There is no singleton, heap allocation or task
stack. A bus reserves one handle plus the configured array of opaque subscriber
slots. Each slot includes queue control storage; each active subscriber additionally
borrows `ESY_EVENT_BUS_QUEUE_BYTES(depth)` data bytes. Use `ESY_ALIGNED_STORAGE`
for queue buffers; creation validates capacity and the public OS storage alignment,
and the OS provider may impose additional native alignment constraints.

Control-storage budgets are opaque ABI reservations. Calculate memory from the
public `sizeof` values and queue-byte macro rather than relying on private field
layout. Current private byte overlays require GCC/Clang-compatible `may_alias`
support, checked at compilation. No hardware/RTOS-native types appear in the
public interface. Inline pointer bytes never extend external buffer lifetimes or
provide DMA cache maintenance.

## Verification

From the platform root:

```sh
(cd esy_middleware/event_bus/tests && ceedling test:all)
(cd esy_middleware/event_bus/tests && ceedling gcov:all)
python3 -B esy_middleware/event_bus/tests/test_cmake_consumer.py
ruby tools/quality/run_module_tests.rb esy_middleware/event_bus/tests \
  esy_middleware/event_bus/tests/build/sanitizer test:all --sanitizer
tools/checks/check_source_contracts.sh esy_middleware/event_bus
```

Public API tests cover independent copies, mask filtering, FIFO, nested
publications, partial delivery, ISR wake aggregation, empty/full queues, input and
storage validation, queue creation failures, receiver-output preservation,
close-before-retire races, failed cleanup retries and reinitialization. Reusable
OS fakes inject deterministic send/receive interleavings; these do not replace
native scheduler, interrupt-priority, SMP or latency validation.

CMake tests cover feature on/off and invalid provider combinations, enabled
aggregate source assembly, C11/C++17 public headers and consumers, matching public
storage layout, optimized aliasing behavior, and lifecycle/fanout smoke. Embedded
consumer/toolchain checks exercise the enabled module in its target configuration.

## Known Limits

- OS-only, fixed inline payload size and 32 mask-addressable topics.
- No worker, direct subscriber callbacks, ISR receive, wildcard hierarchy,
  automatic overwrite/coalescing, retained state or zero-copy buffer ownership.
- No transactional broadcast, consumer acknowledgement, persistence, replay or
  globally ordered publication.
- No automatic cancellation of a blocked receiver; finite waits and caller-owned
  stop orchestration determine teardown latency.
- Lifecycle operations have one serialized owner; dynamic membership is not an
  atomic snapshot of an entire publication.
- No target ISR/SMP timing or hardware/OTA safety guarantee from host tests.
