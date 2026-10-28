<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Network Byte Streams

## Purpose

`esy_network` defines a borrowed TCP/TLS byte stream for MQTT and a connection
extension for framed consumers such as ProtoBin and Modbus TCP.
The optional lwIP provider owns raw TCP connection/listener resources. Network
interfaces, authentication, reconnection pacing, shutdown deadlines and product
policy remain consumer-owned. The module provides no framing, QoS, task or
universal UART/SPI I/O framework.

## Public Surface

- `esy_network_stream.h`: prefix send, immediate receive, explicit progress and
  a borrowed context; it contains no connection state or acknowledgement fields.
- `esy_network_connection.h`: embeds the basic stream and adds whole writes plus
  observed session/peer-ACK status for reliable framed consumers. Both extension
  callbacks share `connection.stream.context`.
- Target `esy_network` / `Esy::network` exports both C11 header contracts without
  native stack or OS dependencies.
- `esy_network_lwip.h`: caller-owned raw lwIP 2.x TCP client or single-peer server.
  Target `esy_network_lwip` privately consumes a configured lwIP provider.

No public header exposes lwIP, Pico SDK or native RTOS types. TLS implementations
can supply the generic stream without linking this lwIP implementation.

## Integration

The consumer's feature import supplies the only feature selection:

```cmake
set(ENABLE_ESY_NETWORK ON)
set(ENABLE_ESY_NETWORK_LWIP ON)
set(ENABLE_ESY_COMMON_DS ON)
```

Create `esy_network_lwip_provider` before adding the library. It supplies the
lwIP 2.x raw TCP API and the consumer's `lwipopts.h`/native configuration. A Pico
polling consumer can link its SDK's `pico_cyw43_arch_lwip_poll` and its own
configuration target through this provider. The project import helper can
assemble that existing SDK combination; it neither downloads nor initializes it.
The lwIP endpoint implementation adapts software APIs. Native hardware-driver
servicing and SDK execution guards remain behind the public HAL.

For Pico polling, the application obtains an `esy_wifi_network_runtime_t` from
`esy_hal_wifi_get_network_runtime()` and injects its `progress`, `enter` and
`leave` callbacks into `config.progress`, `config.owner_enter` and
`config.owner_leave`. Use its `context` for both callback context fields. This
wiring belongs to the owner; the network module neither includes the Wi-Fi HAL
nor selects a hardware provider. There is no separate Pico network target or
gate. Network startup and cleanup must retain the initialized Wi-Fi provider
until all endpoints and stream borrowers have stopped using these callbacks.
The runtime descriptor does not initialize Wi-Fi, create a task or start an
endpoint. The current Wi-Fi backend qualifies the SDK polling execution model;
its entry/exit hooks do not add cross-task or multicore safety to that model.

For an independently driven lwIP stack, provide owner/core callbacks where its
native raw API requires them; use NULL progress only if input/timers continue
independently in the same documented core execution model. Optional owner entry
and exit callbacks must be supplied as a pair. They do not make concurrent calls
on the same endpoint safe.

Initialize an endpoint with `{0}` and exclusive receive storage:

```c
esy_network_lwip_t endpoint = {0};
uint8_t receive_bytes[256];
esy_network_lwip_static_resources_t resources = {
    .rx_buffer = receive_bytes,
    .rx_buffer_size = sizeof(receive_bytes),
};
esy_network_lwip_config_t config = {.static_resources = &resources};
/* Select provider progress/core callbacks before init as applicable. */
esy_err_t result = esy_network_lwip_init(&endpoint, &config);
esy_network_stream_t stream = {0};
if (result == ESY_OK) result = esy_network_lwip_get_stream(&endpoint, &stream);
/* ProtoBin uses the connection extension rather than the basic MQTT stream. */
esy_network_connection_t connection = {0};
if (result == ESY_OK) result = esy_network_lwip_get_connection(&endpoint, &connection);
```

Choose outgoing `begin(host, port)` or `listen(port, backlog)` explicitly.
Progress DNS/input/timers from the owner and inject the descriptor required by
one protocol consumer. MQTT accepts `stream`; ProtoBin accepts `connection`. A listener keeps its native listener while a peer is connected. Retire
that peer through `close_peer` before accepting another; `close` also releases
the listener. Retry failed cleanup without reusing storage.

### Usage Example

Borrow an already connected stream from its initialized TCP/TLS owner. Start a
caller-owned offset at zero for one message and retain the unsent bytes between
calls. This function makes at most one progress and send call on that owner;
`ESY_ERR_BUSY` requests another iteration without resending accepted bytes.

```c
#include "esy_middleware/network/esy_network_stream.h"

esy_err_t network_example_send_step(const esy_network_stream_t *stream,
                                   const uint8_t *data, size_t size,
                                   size_t *offset)
{
    if (stream == NULL || stream->send == NULL || offset == NULL ||
        (data == NULL && size != 0u) || *offset > size) return ESY_ERR_INVALID_ARG;
    if (*offset == size) return ESY_OK;
    if (stream->progress != NULL) {
        esy_err_t result = stream->progress(stream->context);
        if (result != ESY_OK) return result;
    }
    size_t remaining = size - *offset;
    if (remaining > INT32_MAX) remaining = INT32_MAX;
    int32_t accepted = stream->send(stream->context, data + *offset, remaining);
    if (accepted < 0) return (esy_err_t)accepted;
    if ((size_t)accepted > remaining) return ESY_ERR_IO;
    *offset += (size_t)accepted;
    return *offset == size ? ESY_OK : ESY_ERR_BUSY;
}
```

`ESY_OK` proves local acceptance, not delivery or peer acknowledgement. Handle
other errors through the provider owner's recovery policy. Keep the provider,
its context and buffers alive while borrowed; after submissions stop, progress
any queued output and close/deinit through the provider's public lifecycle API.
Retry failed cleanup before releasing storage. This borrowed-stream example
creates no endpoint or socket and performs no hidden reconnect.

## Contracts

### Ownership and execution

One task or bare-metal context serializes endpoint APIs, stream callbacks and
native raw callbacks. No API is ISR-safe. Every referenced buffer/provider/callback
context remains alive until successful deinit. Init borrows no native resources,
rejects repeated init and preserves input storage on failure. Deinit first closes
resources and clears storage only on success; repeated deinit returns STATE.

`stream.progress` explicitly executes one configured provider iteration. Prefix
send, whole send, receive and status never progress input/timers implicitly.
MQTT advances stream progress before each synchronous upstream send/recv call;
ProtoBin advances it once per channel poll. Neither bounded byte work nor the
callback count proves a wall-clock bound: SDK/stack/provider latency contributes
to execution time. Progress cannot recursively drive the endpoint or borrower.

### Acceptance, acknowledgments and sessions

Endpoint setup belongs to `esy_network_lwip.h`: its `esy_network_state_t` reports
uninitialized, idle, resolving, connecting, listening, connected, closing, closed
and error states.
The connection extension observes only `INACTIVE`, `OPEN` or `EOF`: lwIP maps
CONNECTED to OPEN, CLOSED to EOF and every other endpoint state to INACTIVE.
It does not expose listener or connection-establishment policy. A changed
session or INACTIVE invalidates all prior-session TX evidence, while retained
native resources still require explicit endpoint cleanup.


Prefix `send` accepts at most the current send capacity and `UINT16_MAX` bytes.
The connection extension's `send_all` accepts the entire request or returns zero for backpressure;
requests above `UINT16_MAX` reject. Accepted bytes are copied before return.
`tcp_output` failure does not undo successful `tcp_write`, so accepted bytes are
reported once and never resubmitted merely because output failed. Connection status counts
accepted bytes in `unacked_bytes` until the native sent callback reports peer
acknowledgment; local queue acceptance is never reported as peer ACK.

Each successful connection has a distinct nonzero 64-bit session ID until deinit.
The counter never wraps; new connections reject at exhaustion. This lets a
borrower detect a disconnect/reconnect even if it did not observe the intermediate
state. Status is an owner-context observation, not a cross-task synchronization
primitive or send-capacity reservation. `send_capacity` observes native current
capacity; a whole write must also obey the provider's `UINT16_MAX` request limit.

### Receive, EOF and cleanup

RX uses a caller-owned power-of-two ring plus at most one accepted native pbuf
chain. Additional chains return ERR_MEM and remain provider-owned for redelivery.
Only consumed bytes are acknowledged through `tcp_recved`. Peer FIN enters
`CLOSED` (connection `EOF`), a receive half-close: buffered bytes remain readable, empty receive
reports EOF and sending remains valid until explicit close. The native PCB is
still owned; CLOSED does not authorize reclaiming endpoint storage.

ProtoBin drains buffered input and transport acknowledgments over bounded polls
before retiring the peer session. Existing queued output may also progress while
draining. Its `is_peer_drained` query tells an owner when normal EOF cleanup can
close the endpoint. The owner bounds drain time and explicitly closes stalled
connections; an absent peer ACK must not become an unbounded shutdown wait.

Close discards RX and detaches callbacks only when native close succeeds. Native
ERR_MEM returns BUSY; other errors return IO. Both retain the PCB, callbacks,
receive storage and provider for retry. Pending DNS cannot be cancelled natively:
close remains BUSY until its callback retires. No failed close forces tcp_abort.
Only failed initial connection/listener setup or refusal of an extra peer uses
native abort to retire an unborrowed control object. Stop protocol borrowers
before explicit provider cleanup, then release their borrowed references before
endpoint deinit.

The provider imposes no timebase, timer, connection deadline or automatic retry.
The consumer supplies deadlines and keeps input/timers advancing through cleanup.
Wi-Fi, OTA and system-power coordination remain outside this byte-stream layer.

## Resource Model

Handles and the receive ring are caller-owned. The stream and connection descriptors own no memory and borrow the same provider
context. Getter outputs are written only on success. Detach both kinds of
borrower before successful endpoint deinit invalidates their context.
The lwIP adapter directly allocates no heap, but native DNS/TCP lifecycle and
send/close operations may allocate PCBs, pbufs and copied segments through lwIP's
configured pools/heap. Static handle capacity does not prove an allocation-free
network stack or bound provider timing. Injected execution hooks borrow their
provider and context until successful endpoint deinit; hardware provider
initialization and shutdown belong to the consumer.

## Verification

Run `ceedling test:all` and `ceedling gcov:all` from `lwip/tests`; the suite covers
prefix/whole writes, peer ACK status, sessions, DNS cancellation, receive
backpressure, EOF, server lifecycle, retained close failures, generic execution
hooks, connection-state folding and getter/status failure output preservation.
`python3 tests/test_cmake_consumer.py` builds generic C11/C++17 consumers and runs
a real raw endpoint with an injected software progress callback, without HAL,
Pico SDK or OS symbols. Negative C11/C++17 probes require the basic stream to
exclude connection callbacks, status types and endpoint states. These fixture providers do not replace native toolchain or network acceptance.

Qualify the selected stack on hardware with DNS/link loss, native memory limits,
TCP backpressure, last data plus FIN, close retry, sustained RX, suspend and
OTA/Flash quiescence. Core execution/locking and timer progress must match the
actual native provider configuration.

## Known Limits

- Raw lwIP provider is plain TCP, one active peer per endpoint, no TLS or socket
  portability layer. MQTT/TLS authentication policy belongs to another provider.
- No reconnect, connection timer, application framing, durable delivery or task.
- Owner hooks serialize native raw API entry only as their provider specifies;
  they do not provide cross-task endpoint ownership.
- A stopped provider cannot complete DNS, native close or peer acknowledgments;
  retain resources and follow the consumer's recovery policy.
