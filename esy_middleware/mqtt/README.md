<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# MQTT Client

## Purpose

`esy_mqtt` adapts coreMQTT v2.3.1 into a caller-owned MQTT 3.1.1 client.
It provides publish/subscribe, QoS 0/1, retained messages, last wills,
acknowledgment events, keep-alive and explicit connection cleanup. It does not
create a task, require an RTOS, or define application topics or cloud credentials.
Packet encoding, decoding and QoS state remain upstream; EsyMcuPlatform owns the
adapter, keep-alive progression and connection-fatal error policy.

## Public Surface

- `esy_mqtt.h`: client storage, transport callbacks, configuration, connect,
  publish, subscribe/unsubscribe, process, disconnect, abort and deinit.
- `esy_network_stream_t`, owned by [network](../network/README.md), supplies the
  borrowed TCP/TLS byte-stream contract shared with other protocol consumers.
- Targets: `esy_mqtt` / `Esy::mqtt`.

The public header requires no coreMQTT, lwIP, hardware SDK or OS types.
coreMQTT is a private software provider, not a public HAL extension.

## Integration

The consumer's feature import is the only source of feature selection:

```cmake
set(ENABLE_ESY_NETWORK ON)
set(ENABLE_ESY_MQTT ON)
```

Supply coreMQTT v2.3.1 through an existing `esy_mqtt_provider` target or an explicit
`ESY_COREMQTT_PATH` CMake or environment input. The optional project importer
assembles the provider only when MQTT is enabled; configuration never downloads
dependencies.

For library host tests, `./setup.sh` prepares seven pinned official providers;
the Python/Ruby test helpers discover their cache automatically. Firmware CMake
uses the consumer's explicit providers, independently of this test cache and
lock. See [software provider setup](../../tools/README.md#software-provider-setup).
Direct module consumers must call the helper below or supply the provider target
before module assembly:

```cmake
include("${ESY_MCU_PLATFORM_DIR}/cmake/esy_coremqtt_provider.cmake")
esy_configure_coremqtt_provider()
add_subdirectory("${ESY_MCU_PLATFORM_DIR}" platform)
target_link_libraries(my_application PRIVATE esy_mqtt)
```

An existing consumer-provided `esy_mqtt_provider` target takes precedence. It
must provide the v2.3.1 sources and their include/configuration requirements;
coreMQTT v5 is a different API/protocol and is not accepted by the checkout
helper. The tested upstream commit is
`2beef04725328923e05e576b884212d53ec97af7`. Upstream source and its MIT license
stay in that checkout; they are not copied into this module.

By default, the helper sets `MQTT_DO_NOT_USE_CUSTOM_CONFIG`. To customize timeouts
or logging, create an existing configuration target and select it with
`ESY_COREMQTT_CONFIG_TARGET`. That target must expose `core_mqtt_config.h` or
explicit upstream definitions. The helper propagates it to both coreMQTT and
this adapter. Configuration must be consistent across the provider and adapter.
Protocol configuration is separate from consumer-owned feature selection.

The client borrows the basic `esy_network_stream_t` through `config.transport`
and uses only send, recv, optional progress and context. It does not require
whole-write, session-state or peer-acknowledgement extensions. A TCP or TLS
provider owns connection establishment, closure, authentication and native
stack resources. For the optional raw lwIP provider with injected native
progress/core-region hooks, follow
[network integration](../network/README.md#integration). Obtain the stream with
`esy_network_lwip_get_stream`, initialize MQTT with it, and call MQTT connect only
after the endpoint reaches `ESY_NETWORK_CONNECTED`. Close/deinit failures retain
the network provider's storage until its explicit cleanup succeeds.

The MQTT adapter calls stream progress before each upstream send/recv callback.
This allows synchronous coreMQTT waits to advance a polling stack. The stream's
send/recv callbacks themselves do not implicitly progress it. Progress and I/O
share the provider's documented owner context and cannot recursively drive MQTT.

The application sequence is:

1. Initialize the client and its packet buffer using `esy_mqtt_init`.
2. Establish a TCP connection, or a TLS connection with certificate and hostname
   validation. The consumer owns DNS, trust anchors, credentials and TLS policy.
3. Call `esy_mqtt_connect`, then submit subscriptions. Track SUBACK events before
   relying on a subscription.
4. Drive the network provider and call `esy_mqtt_process` from the same owner.
   Copy incoming topic/payload data if it must outlive its callback.
5. On a fatal error, close the old stream, apply consumer-selected retry pacing,
   establish a fresh stream, reconnect and restore subscriptions. Reuse the
   initialized client; a clean session discards old protocol state.
6. Before suspend or a long OTA/Flash pause, quiesce submissions, disconnect or
   abort the client, close the stream, then stop the network provider. Resume
   through a new connection and subscription setup. Do not pause an active
   client across a stopped clock or an unbounded scheduling gap.

The existing Wi-Fi manager can own station connectivity. It does not own MQTT
sessions. Existing ProtoBin TCP channels add their own framing and cannot serve
as an MQTT byte stream.

### Usage Example

Supply a `{0}` client, prepared packet storage/configuration, an event sink and
an already connected TCP/authenticated TLS stream. The connect parameters and
outbound message are borrowed only during the calls below; packet storage and
provider/event contexts remain alive through deinit. The example uses the
caller's topic, payload, QoS and broker settings.

```c
#include "esy_middleware/mqtt/esy_mqtt.h"

esy_err_t mqtt_example_start(esy_mqtt_t *client,
                            const esy_mqtt_config_t *config,
                            const esy_mqtt_connect_config_t *connection,
                            const esy_mqtt_message_t *message)
{
    uint16_t packet_id;
    esy_err_t result = esy_mqtt_init(client, config);
    if (result != ESY_OK) return result;
    result = esy_mqtt_connect(client, connection);
    if (result == ESY_OK) result = esy_mqtt_publish(client, message, &packet_id);
    if (result == ESY_OK) return ESY_OK;
    esy_err_t cleanup = esy_mqtt_abort(client);
    if (cleanup == ESY_OK) cleanup = esy_mqtt_deinit(client);
    return cleanup == ESY_OK ? result : cleanup;
}

esy_err_t mqtt_example_stop(esy_mqtt_t *client)
{
    esy_err_t result = esy_mqtt_disconnect(client);
    esy_err_t cleanup = esy_mqtt_deinit(client);
    return cleanup == ESY_OK ? result : cleanup;
}
```

After success, call `esy_mqtt_process` regularly from the same owner and handle
its errors. QoS 1 completes through PUBLISHED events; QoS 0 proves transport
acceptance. Stop or abort MQTT before retiring the stream. After successful
deinit, let the transport owner drain/close TLS and TCP according to its budget;
retain all resources if cleanup fails.

## Contracts

### Ownership and lifecycle

Initialize each client with `{0}`. Backing packet storage need not be cleared.
Initialization performs no network I/O and preserves the client on argument
failure. Reinitializing an active client returns STATE; mutating callback
reentry returns BUSY. Never copy, move, clear or free a live client.

The config and static-resource descriptors are copied. Their referenced packet
buffer, stream context and event context remain borrowed until successful
deinit. Connect strings and outbound topic/payload data are borrowed only during
the corresponding synchronous call. A transport send must copy or finish using
accepted bytes before it returns; a successful send does not permit an
asynchronous provider to retain the caller's payload.

`deinit` requires a disconnected/aborted client. Failure retains resources;
success clears the client and permits a fresh init. Repeated deinit returns
STATE. `disconnect` sends the protocol packet and cancels pending commands even
if that send fails. `abort` performs no I/O. Both stop local protocol processing,
but neither closes the borrowed network stream. Disconnect/abort on an already
stopped initialized client is idempotent.

### Execution and events

One task or bare-metal loop exclusively owns a client and serializes all API
calls, including queries. These APIs are not ISR-safe. Callbacks run in that
owner's current call. They may query connectivity but must not drive or mutate
the client. The reentry check is not a mutex and provides no cross-task safety.
Different clients are independent only if their providers permit it.

Each `process` call handles at most one MQTT packet and checks a fixed number of
pending commands. The packet buffer limits inbound packet size. During connected
processing, a current packet whose decoded header exceeds that capacity returns
NO_MEM and ends the session without draining the remaining payload. Packets
already buffered after the current packet are checked on subsequent calls.
Provider calls and callbacks remain synchronous; byte budgets alone do not bound
elapsed time.
With the upstream defaults, send may wait **20 seconds**, receive polling may
wait 10 ms, and a PINGRESP is allowed 5 seconds. The consumer must select a
configuration compatible with its task/watchdog and provider latency budgets.
Do not call a blocking connect from a GUI, ISR or task with an incompatible
deadline.

QoS 0 success means the bytes were accepted by the stream. QoS 1 success means
the command was submitted; `PUBLISHED` reports PUBACK. SUBACK reports the granted
QoS or NOT_SUPPORTED for broker rejection. A rejected subscription leaves the
connection usable. Other malformed/unmatched acknowledgments, transport errors,
buffer exhaustion, acknowledgment expiry or keep-alive failure end the session.
Pending operations receive their corresponding failure events before the
DISCONNECTED event. Explicit stop cancels them with STATE.

Incoming message views expire when the callback returns. QoS 1 consumers must
tolerate duplicate delivery, including a message delivered before a later PUBACK
send failure. Payloads are arbitrary bytes. Topic names/filters and text
credentials must be valid UTF-8; wildcard placement is checked. Passwords are
binary. QoS 2, persistent sessions, offline message spooling and automatic
publish replay are not exposed. The application decides whether retrying an
unacknowledged business operation is safe.

### Time and cancellation

The injected time function returns a continuously advancing `uint32_t`
millisecond clock. Its period is 2^32 ms (about 49.7 days). It must advance during
blocking provider/library operations and must not stop/reset during a session.
Connection/acknowledgment waits are in 1..INT32_MAX ms; upstream timeout
configuration must fit the checked clock range. An acknowledgment timer starts
after command transmission completes.

Call `process` at least every `min(1000, acknowledgement_timeout_ms)` ms, and
more often if required by the keep-alive interval or network stack. This bounds
observation gaps well below a full counter period. Modulo subtraction is used
only for active waits. ACK, disconnect, abort and fatal error retire those waits;
later full wraps cannot revive them. A fresh explicit connect starts new state.
Expiry is checked before input processing and at ACK dispatch: an ACK dispatched
at or after its deadline is late, even if its bytes were already buffered.
Zero keep-alive disables automatic PINGREQs, including receive-inactivity pings.

For nonzero keep-alive, the adapter advances keep-alive on every `process` call,
including calls that receive data. It retains the provider's configured transmit
and receive inactivity limits and uses `MQTT_Ping` to start a response wait.
Receive inactivity starts at the successful CONNACK. A response wait remains
active until PINGRESP, abort, disconnect or a fatal error; other inbound traffic
does not extend it. The wait uses the same advancing uint32_t millisecond clock
and observation limits as command waits. To preserve the provider's boundary
semantics, expiry occurs strictly after `MQTT_PINGRESP_TIMEOUT_MS`; a response
dispatched at that boundary is accepted, and a later one is rejected. Completed
waits are inactive, so old ping timestamps cannot reactivate after a counter
wrap. A later PINGREQ explicitly starts a fresh wait.

The network provider owns DNS/TCP cancellation and native resource retirement.
Its explicit connection deadlines and cleanup contract are independent of MQTT
acknowledgment deadlines; see the [network contracts](../network/README.md#contracts).

## Resource Model

The module allocates no heap memory. Each client contains fixed private protocol
storage and up to eight outstanding QoS 1 publish/subscription commands. The
caller supplies the network packet buffer. There are no hidden tasks, queues,
timers, interrupts or singleton connection state.

Network and TLS providers may allocate their own native resources. Neither
static client storage nor the direct-allocation checker proves an allocation-free
network/TLS stack. Provider memory, handshake costs and failure cleanup depend on
the selected native provider and its configuration.

## Verification

From the platform repository root, run `./setup.sh` once to prepare the cached
software providers, then run:

```sh
cd esy_middleware/mqtt/tests
ruby ../../../tools/quality/run_module_tests.rb "$PWD" "$PWD/build" test:all
ruby ../../../tools/quality/run_module_tests.rb "$PWD" "$PWD/build" gcov:all
python3 -B test_cmake_consumer.py
python3 -B test_broker_smoke.py
```

Optionally set the `ESY_COREMQTT_PATH` environment variable to test another
compatible coreMQTT checkout.

The tests use real upstream encoding/state logic and a deterministic byte stream.
They exercise connection flags/credentials/will, QoS 0/1 traffic, SUBACK rejection,
partial transfers, backpressure, invalid data, operation capacity, callback
reentry, keep-alive, timeout boundaries, wrap/cancellation and reinitialization.
Regressions cover keep-alive during continuous QoS 0 traffic and oversized
packets with fragmented headers, queued packets and exact-capacity boundaries.
CMake regressions check on/off isolation, provider diagnostics and public C/C++
consumers without an OS target. The broker smoke script requires Mosquitto,
OpenSSL development libraries and the `openssl` command (or `MOSQUITTO_BIN` for
an explicit broker executable). It uses loopback listeners and temporary
certificates to verify QoS 0/1, subscribe/unsubscribe, reconnect, authenticated
TLS, and rejection of an incorrect TLS hostname. It skips clearly if the broker
is unavailable; CI installs the broker and requires it before invoking the test.
The network module's `lwip/tests` suite separately covers the raw TCP provider
with a deterministic lwIP fixture. CI also runs sanitizer builds.

For network-enabled consumer integration, test a real broker with its intended
TLS credentials, connection loss, DHCP/DNS changes, long runs, OTA/suspend and
the actual RAM/watchdog budgets. Host regressions are not hardware acceptance.

## Known Limits

- MQTT 3.1.1 client, clean sessions and QoS 0/1 only; no broker implementation,
  MQTT 5, WebSocket, persistent-session storage or cross-reset delivery guarantee.
- Secure deployments inject an authenticated TLS byte stream through the shared
  network contract; this module does not implement a TLS stack.
- Retry pacing, subscription restoration, application idempotency and OTA policy
  remain consumer-owned; this module owns no application task.
- No onboard network/TLS/OTA HIL result is implied by a successful host or
  embedded compile. Public storage capacity is not an ABI stability promise.
