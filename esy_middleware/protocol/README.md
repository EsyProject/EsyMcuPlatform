<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Protocol cores, channels and workers

## Purpose

This module owns protocol-specific cores, I/O channels and optional OS workers.
ProtoBin, Modbus and JSON are independent families. Pure wire-format codecs live in
`esy_common/protocol_codec`; protocol-independent I/O schedulers live in `io_scheduler`.

The module separates these responsibilities:

- codec: build, parse, and verify protocol frames;
- session: own ProtoBin framing, reliable delivery and byte-stream I/O state;
- manager: own Modbus transactions, request scheduling, timeout, retry, and dispatch.
- channel: bind one protocol core to an I/O provider, advanced by its caller.
- worker: optionally own an OS task and drive the same channel implementation.

Implementation and unit-test files are grouped under `src/bin`,
`src/modbus` and `src/json`, with `tests/bin`, `tests/modbus` and the independent
`tests/json` suite. Public headers remain flat;
consumers do not encode those implementation directories in includes:

```c
#include "esy_middleware/protocol/esy_proto_bin_uart_channel.h"
#include "esy_middleware/protocol/esy_proto_modbus_uart_channel.h"
#include "esy_middleware/protocol/esy_proto_modbus_uart_worker.h"
#include "esy_middleware/protocol/esy_proto_json_tcp_channel.h"
#include "esy_middleware/protocol/esy_proto_json_usb_channel.h"
```

## Public Surface

Public headers live under
`esy_middleware/protocol/include/esy_middleware/protocol/`.

Pure codec headers live under
`esy_common/protocol_codec/include/esy_common/protocol/`.

### JSON TCP channel

`esy_proto_json_tcp_channel.h` provides one caller-driven, full-duplex channel
over an exclusively borrowed `esy_network_connection_t`. It validates complete
JSON through the common codec, reconstructs fragmented/coalesced TCP input and
stages one copied outgoing frame. It requires prefix `stream.send`, immediate
`stream.recv` and `get_status`; progress is optional and `send_all` is unused.
The provider must already expose an OPEN or EOF peer with a nonzero session ID.
Client/listener setup, DNS, TLS policy, close and reconnect stay with its owner.
No HAL, system clock, DS target, ProtoBin, Modbus or OS is required.

The APIs are `init`, `send`, `poll`, `get_status` and `deinit`.
The synchronous message callback receives a validated root whose input and
descendant views remain valid only until callback return. It may submit a copied
reply and query local status. Recursive poll/init/deinit is rejected with BUSY.
Provider callbacks may not reenter the borrower. All operations share one
serialized non-ISR owner, whether an existing task or a baremetal main loop.

### JSON USB channel

`esy_proto_json_usb_channel.h` provides the same length-prefixed JSON format
over one exclusively borrowed `esy_usb_cdc_t`. Its APIs are `init`, `send`,
`poll`, `get_status` and `deinit`, with caller-owned buffers and synchronous
JSON-view callbacks. It is independent of the TCP/network and other protocol
families. Shared private prefix/storage primitives keep the TCP and USB wire
format aligned without introducing a generic runtime transport facade.

The CDC owner initializes session isolation, owns the USB stack pump and retains
the CDC handle through channel deinit. Channel poll performs CDC I/O only; it
never invokes the global stack task. Handles can remain initialized across
DTR/USB peer changes, with local framing and errors reset only on an observed
new CDC generation. This is a generation boundary, not TCP EOF/peer-ACK state.

### Shared ProtoBin contracts

`esy_proto_bin_types.h` declares DATA/event callbacks and byte-stream I/O hooks.
`esy_proto_bin_sender.h` declares `esy_proto_bin_sender_t`, a borrowed context and
three required `acquire`, `commit` and `cancel` callbacks. It owns no storage,
session lifecycle, polling or backend dispatch. Its provider may submit directly
to a session or post payloads through an existing bounded owner queue.

A successful acquire reserves writable storage. Commit must receive that exact
pointer and an in-range length; success consumes the reservation and returns a
nonzero transaction ID. Failure retains the reservation until commit or cancel
succeeds. Submission accepts work without promising peer receipt. Calls execute
on the declared serialized non-ISR owner and never drive session progress or
synchronously invoke application DATA/completion callbacks. Detach borrowers
before releasing their provider context.

### ProtoBin session

Files:

- `esy_proto_bin_session.h`
- `esy_proto_bin_session.c`

Responsibilities:

- enqueue payloads;
- package payloads as ProtoBin `CMD_DATA` frames;
- send ACK for received DATA frames and suppress duplicate application delivery;
- handle ACK, NACK, timeout, retry, and CRC-error events.

The session uses a byte-stream I/O adapter and `esy_hal_system_get_tick()`.
It implements a stop-and-wait reliability model. Only an explicit matching ACK
completes an in-flight transmission; peer DATA uses an independent sequence
space and never acts as an implicit acknowledgement. Retransmitted DATA is
acknowledged again but delivered to the application callback only once.

Reliability uses two independent deadlines. `tx_completion_timeout_ms` starts
after a successful send returns and the channel is observed busy; it ends when
that channel reports the physical transmission complete.
`acknowledgement_timeout_ms` starts when the channel is observed idle after
acceptance or during a later poll, and ends when the matching ProtoBin ACK
arrives. Each attempt samples the current tick at its own phase transition;
time spent in earlier RX callbacks or in the send operation does not consume a
new phase's timeout. Callback time still counts against an already active
deadline, which is checked using the current tick after RX handling. A
transmission timeout emits `ESY_PROTO_BIN_TP_EVENT_TX_TIMEOUT` and does not retry a frame
whose physical completion is unknown. An acknowledgement timeout retries the
frame up to `retries` times, with a new transmission-completion phase for every
attempt.

A matching NACK received before physical completion records a retry request
without replacing the active TX deadline. The work buffer remains borrowed and
cannot be rebuilt while the channel is busy. If physical TX never retires, the
original deadline still reports TX_TIMEOUT, including when no retries remain.
After idle is observed, the pending NACK triggers retry without waiting for an
ACK deadline; zero remaining retries reports TIMEOUT. Every accepted retry
clears the previous request and receives fresh TX/ACK deadlines.

Each poll consumes at most `rx_byte_budget` bytes before advancing TX and its
deadlines, even while input remains continuously readable. The default is
`ESY_PROTO_BIN_DEFAULT_RX_BYTE_BUDGET` (256 bytes); zero selects the same default.
Parser state remains intact between polls, so frames may span several polls.
The byte limit also bounds how many complete-frame callbacks can run in one
poll. Physical channels use this default. Direct session users can configure
the budget for their owner loop's throughput and latency needs.

Poll often enough that the byte budget can drain incoming traffic and parse
acknowledgements within the configured timeout. A frame larger than the budget
needs multiple polls. I/O hooks and application callbacks execute synchronously;
their duration still contributes to poll latency, so the byte budget is not a
hard real-time guarantee. No task or scheduling policy is created by this limit.

### Physical Channels

- `esy_proto_bin_uart_channel` adapts an asynchronous UART stream and owns continuous
  RX stream recovery. TX uses `esy_uart_tx_scheduler`, shared with Modbus UART.
  Its polling entry advances a scheduler slice before session processing and,
  only after a new submission, a second slice to start that frame. Submission
  queues one physical frame, with no additional protocol payload queue.
  A failed submission that retains a HAL borrow prevents TX work-buffer reuse
  until polling or successful abort retires it. Cleanup errors retain the
  channel and are returned for owner retry; ACK timing starts after physical
  transmission retirement. Direct HAL users remain responsible for their own
  polling cadence.
- `esy_proto_bin_usb_channel` adapts an initialized USB CDC instance and pumps the
  USB device task from its owner task.
- `esy_proto_bin_tcp_channel` borrows an exclusive `esy_network_connection_t`. The
  consumer owns connection, listening, reconnection and shutdown. The channel
  retains only framing and protocol state, and waits for peer TCP acknowledgement
  before completing the physical transmission phase.

UART reports completion after DMA and the UART FIFO/shift register become idle.
USB reports completion after the CDC write queue releases the submitted bytes.
TCP reports completion after the connection extension reports peer acknowledgement
of the accepted bytes; the lwIP provider derives this evidence from its sent
callback. All three channels therefore start the ProtoBin acknowledgement timer
after channel-specific transmission completion, not after the send call merely
accepts data.

The UART owner reports retired transmissions through
`esy_proto_bin_session_on_tx_complete()` before the next session poll. DATA
failures produce IO_ERROR for the matching live transaction; late completion
after ACK/timeout cannot fail a later request. Failed control ACKs are retried
without discarding queued DATA or RX duplicate-detection state. Providers using
only the existing busy hook keep their completion-observation contract.

Stop every borrowed sender/session user before UART channel deinit. Deinit closes
admission, requests scheduler abort and advances one cleanup slice. Abort or RX
stop failure retains storage; poll/deinit may retry cleanup, while `get_session()`
returns NULL. Existing borrowed pointers must not be used during shutdown. The
default UART opaque storage is 1280 bytes.

All channels expose a borrowed `esy_proto_bin_session_t` through their
`get_session()` API. The execution owner drives that typed session through the
channel and may obtain a copied sender descriptor with
`esy_proto_bin_session_get_sender()`. OTA and other payload producers borrow only
the sender capability; they cannot replace session storage or poll the channel.
USB products normally configure zero retries because USB already provides
reliable delivery; framing remains necessary because CDC preserves no message
boundaries.

### Modbus Manager

Files:

- `esy_proto_modbus_manager.h`
- `esy_proto_modbus_manager.c`

Responsibilities:

- master request scheduling;
- priority-based request handling;
- timeout/retry at manager level;
- passive slave request handling and response generation.

This layer is frame-oriented and does not own UART/TCP I/O.

### Modbus UART channel and worker

`esy_proto_modbus_uart_channel.h` exposes caller-driven RTU master/slave channels.
The channel owns protocol and UART TX state, observes HAL RX idle gaps and calls
the independent manager. It uses the public UART TX scheduler and creates no
OS task, timer or queue. Call its bounded `poll()` from one serialized owner.

`esy_proto_modbus_uart_worker.h` exposes separate master/slave worker handles,
storage, configuration and typed callbacks. A worker owns one private channel,
an OS task, command admission and status snapshots. It calls the same public
channel implementation rather than selecting another channel execution mode.
Channel APIs always execute on their caller; cross-task request/abort semantics
are explicit worker APIs. Only worker creation starts a task. No ProtoBin worker
or Modbus TCP worker is assembled by a channel selection.

### Modbus TCP channel

`esy_proto_modbus_tcp_channel.h` exposes separate master/client and slave/server
channels over a borrowed `esy_network_connection_t`. Both reuse the existing
Modbus manager in TCP mode, with no UART, OS or native networking-stack dependency.
The endpoint owner establishes or accepts a connection before creating a channel.
Each channel exclusively uses one already connected peer session. Multiple
connections use independent channel instances and caller-owned resources.

The channel reconstructs ADUs from fragmented or coalesced TCP input using the
MBAP length, validates protocol identifier zero, and passes complete frames to
the manager. The server echoes the request transaction identifier through the
common codec. MBAP and PDU bounds follow the
[TCP implementation guide](https://www.modbus.org/file/secure/messagingimplementationguide.pdf)
and [application specification](https://www.modbus.org/file/secure/modbusprotocolspecification.pdf).
The configured common codec limit remains authoritative; its default PDU limit
is 252 bytes, with a consistent build override supporting the 253-byte protocol
maximum. A connection descriptor supplies `send_all`, `get_status` and immediate
`stream.recv`; `stream.progress` is optional.

## Integration

Declare features in the consumer's `esy_feature_import.cmake` and link the narrow
target actually used. No protocol aggregate or runtime transport facade exists.

```cmake
set(ENABLE_ESY_COMMON_PROTOCOL ON)
set(ENABLE_ESY_COMMON_DS ON)
set(ENABLE_ESY_SYSTEM ON)
set(ENABLE_ESY_PROTO_BIN ON)
set(ENABLE_ESY_PROTO_MODBUS OFF)
set(ENABLE_ESY_PROTO_BIN_TCP_CHANNEL ON)
set(ENABLE_ESY_NETWORK ON)
target_link_libraries(<your_target> PRIVATE esy_proto_bin_tcp_channel)
```

The module is added only when `ENABLE_ESY_PROTO_BIN` or `ENABLE_ESY_PROTO_MODBUS` is enabled.
Each family independently validates system tick, common codec/data-structure/
utility, HAL and build-option dependencies. The library never enables those
features for the consumer. `ENABLE_ESY_COMMON_PROTOCOL` alone selects pure codecs
without protocol session, HAL or OS requirements.

| Target | Consumer selection and dependencies |
| --- | --- |
| `esy_common_protocol_codec` | `ENABLE_ESY_COMMON_PROTOCOL`; pure ProtoBin, TLV and Modbus wire codecs. |
| `esy_proto_bin_contract` | `ENABLE_ESY_PROTO_BIN`; header-only sender/types contract with common types and codecs, no session/HAL/OS dependency. |
| `esy_proto_bin_session` | `ENABLE_ESY_PROTO_BIN`; reliable typed session with caller-owned state and the HAL tick. |
| `esy_proto_modbus_manager` | `ENABLE_ESY_PROTO_MODBUS`; independent master/slave transaction owner, with no ProtoBin dependency. |
| `esy_proto_bin_uart_channel` | `ENABLE_ESY_PROTO_BIN` and `ENABLE_ESY_PROTO_BIN_UART_CHANNEL`; requires UART/DMA and an explicitly selected `esy_uart_tx_scheduler`; no OS dependency. |
| `esy_proto_bin_usb_channel` | `ENABLE_ESY_PROTO_BIN` and `ENABLE_ESY_PROTO_BIN_USB_CHANNEL`; requires an assembled USB CDC provider target. |
| `esy_proto_bin_tcp_channel` | `ENABLE_ESY_PROTO_BIN` and `ENABLE_ESY_PROTO_BIN_TCP_CHANNEL`; requires the shared network contract target. |
| `esy_proto_modbus_uart_channel` | `ENABLE_ESY_PROTO_MODBUS` and `ENABLE_ESY_PROTO_MODBUS_UART_CHANNEL`; requires UART/DMA/system and an explicitly selected `esy_uart_tx_scheduler`; no OS dependency. |
| `esy_proto_modbus_uart_worker` | `ENABLE_ESY_PROTO_MODBUS_UART_WORKER`, the UART channel, `ESY_RTOS=FreeRTOS` and `esy_os`; optional task and request adapter. |
| `esy_proto_modbus_tcp_channel` | `ENABLE_ESY_PROTO_MODBUS`, `ENABLE_ESY_PROTO_MODBUS_TCP_CHANNEL` and `ENABLE_ESY_NETWORK`; borrowed connected peer, caller-driven master/slave, no UART or OS dependency. |
| `esy_proto_json_tcp_channel` / `Esy::proto_json_tcp_channel` | `ENABLE_ESY_PROTO_JSON_TCP_CHANNEL` with common protocol, common JSON and network gates; one borrowed connected peer, no HAL/system/DS or OS dependency. |
| `esy_proto_json_usb_channel` / `Esy::proto_json_usb_channel` | `ENABLE_ESY_PROTO_JSON_USB_CHANNEL` with common protocol, common JSON, USB and CDC gates, plus an assembled `esy_usb` target; no TCP/network or other protocol family is selected. |

Hardware support or an available provider never enables a channel implicitly.
Enabling a channel without its required codec/family or transport dependencies
fails configuration.
Modbus manager-only selection adds neither ProtoBin nor a UART channel or worker.
A UART consumer explicitly enables `ENABLE_ESY_IO_SCHEDULER` and
`ENABLE_ESY_UART_TX_SCHEDULER`; selecting a channel never enables those inputs.
The I/O scheduler targets are assembled before protocol targets. The manager remains
the single request scheduling, matching, timeout and retry owner.

To select Modbus TCP without UART channels or OS workers:

```cmake
set(ENABLE_ESY_PROTO_MODBUS ON)
set(ENABLE_ESY_NETWORK ON)
set(ENABLE_ESY_PROTO_MODBUS_TCP_CHANNEL ON)
target_link_libraries(firmware PRIVATE esy_proto_modbus_tcp_channel)
```

The endpoint remains consumer-owned. Obtain its initialized connection descriptor,
assign it to the master or slave configuration, provide master queue resources
or a slave request callback, then create and poll the channel from that owner.
No listener, DNS lookup, reconnect policy, default socket or worker is created.

To select JSON TCP, declare these values in the consumer's feature entry:

```cmake
set(ENABLE_ESY_COMMON_PROTOCOL ON)
set(ENABLE_ESY_COMMON_PROTOCOL_JSON ON)
set(ENABLE_ESY_NETWORK ON)
set(ENABLE_ESY_PROTO_JSON_TCP_CHANNEL ON)
```

Supply coreJSON v3.3.1 through the existing JSON provider input
`ESY_COREJSON_PATH` or an explicitly compatible `esy_json_parser_provider` target.
Link the narrow `Esy::proto_json_tcp_channel` target. A direct module consumer
assembles the [common JSON codec](../../esy_common/protocol_codec/README.md#optional-json)
and network contract before adding the protocol directory. Turning the channel
off excludes its source; a separately enabled JSON codec remains available.

Initialize `esy_proto_json_tcp_channel_t channel = {0}`, provide independent
RX body storage and TX prefix-plus-body storage through
`esy_proto_json_tcp_channel_static_resources_t`, and assign an already-connected
descriptor and required receiver to its config. Init copies both descriptors.
The owner calls poll and handles its return status. Send copies complete JSON
bytes into one slot; it does not drive I/O or execute the target's business flow.

### Usage Example: JSON TCP

The owner supplies a persistent zero-initialized channel and a config containing
an OPEN connection, the required message callback and exclusive RX/TX storage.
All referenced buffers, callback contexts and the connection provider remain
alive until channel deinit succeeds. These helpers run on that same non-ISR
task/main-loop owner; the provider and network connection are already running.

```c
#include "esy_middleware/protocol/esy_proto_json_tcp_channel.h"

esy_err_t example_json_start(esy_proto_json_tcp_channel_t *channel,
                             const esy_proto_json_tcp_channel_config_t *config) {
  esy_err_t result = esy_proto_json_tcp_channel_init(channel, config);
  if (result != ESY_OK) return result;
  const char request[] = "{\"request\":\"status\"}";
  result = esy_proto_json_tcp_channel_send(channel, request, sizeof(request) - 1u);
  if (result != ESY_OK) {
    esy_err_t cleanup = esy_proto_json_tcp_channel_deinit(channel);
    return cleanup == ESY_OK ? result : cleanup;
  }
  return ESY_OK;
}

esy_err_t example_json_step(esy_proto_json_tcp_channel_t *channel,
                            esy_proto_json_tcp_channel_status_t *status) {
  if (status == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t result = esy_proto_json_tcp_channel_poll(channel);
  if (result != ESY_OK) return result;
  return esy_proto_json_tcp_channel_get_status(channel, status);
}
```

After successful start, call step regularly; `BUSY` permits another owner slice.
Read `status` only when step returns `ESY_OK`. `status->tx_pending == false`
means the staging slot is free, not that the peer processed the request. Before
the receive callback returns, copy required content into owner-owned storage;
copying a view does not extend the RX buffer lifetime. Stop polling and all
submitters before
`esy_proto_json_tcp_channel_deinit()`, retaining storage if cleanup fails.
The connection owner still closes its endpoint; after a fatal or abandoned
partial exchange it retires residual traffic and creates a fresh session before
reuse. Never automatically replay a request merely because a later poll fails.

### JSON USB integration

To select JSON USB, declare these values in the consumer's feature entry and
choose a supported CDC backend/provider separately:

```cmake
set(ENABLE_ESY_COMMON_PROTOCOL ON)
set(ENABLE_ESY_COMMON_PROTOCOL_JSON ON)
set(ENABLE_ESY_USB ON)
set(ENABLE_ESY_USB_CDC ON)
set(ENABLE_ESY_USB_BACKEND_TINYUSB ON)
set(ENABLE_ESY_PROTO_JSON_USB_CHANNEL ON)
```

Supply coreJSON as above and the selected USB provider/descriptors/configuration,
then link `Esy::proto_json_usb_channel`. The channel itself does not choose a
USB backend or initialize native USB. The [CDC integration contract](../usb/README.md)
owns provider threading, OS configuration, descriptors, endpoints and bring-up.
Baremetal support depends on that actual provider; an OS-free channel target
does not make every USB provider OS-free.

Initialize CDC with `discard_on_disconnect = true` and a NULL or notification-only
RX callback, then initialize the zeroed JSON channel. Both backing buffers are
described by `esy_proto_json_usb_channel_static_resources_t`; TX storage includes
the four-byte prefix. A disconnected CDC is a valid initial binding. From the
same existing owner, run the stack pump followed by channel polls:

```c
/* The stack owner calls this once, then advances every channel it owns. */
esy_usb_cdc_task();
esy_err_t result = esy_proto_json_usb_channel_poll(&channel);
/* Handle result and owner-managed pending responses before the next slice. */
```

Do not call the pump recursively or from the JSON callback. Serialized I/O must
follow the native USB provider's execution model; use its documented stack task
when its pump cannot run within a main-loop latency budget. Another logger or
protocol cannot share this CDC byte stream without a separate explicit framing
owner; use a separately configured interface where appropriate.

### Execution and channel selection

All concrete protocol headers, functions, types, macros and targets use
`esy_proto_<family>_*`. ProtoBin transport targets use
`esy_proto_bin_{uart,usb,tcp}_channel`; select the corresponding
`ENABLE_ESY_PROTO_BIN_*_CHANNEL` inputs. Select `ENABLE_ESY_PROTO_MODBUS` for
Modbus. `ESY_PROTO_MODBUS_MANAGER_MAX_PDU_LEN` controls the manager-specific
request-storage limit independently of the codec limit.

Modbus UART channels use the caller-driven
`esy_proto_modbus_uart_{master,slave}_channel_*` APIs. Optional OS execution uses
`esy_proto_modbus_uart_{master,slave}_worker_*`, with distinct
handle, storage, configuration and callback types. Enable the channel and worker
separately. Each API has its own blocking and concurrency contract. Worker
storage includes its own channel storage; use the public capacity macros when
declaring caller-owned resources.

The TCP consumer initializes a network provider independently and passes its
exclusive connection in `esy_proto_bin_tcp_channel_config_t.connection`. The
connection extension embeds the basic `esy_network_stream_t` and adds whole
writes and status with session IDs and peer-acknowledgement evidence. All
callbacks use `connection.stream.context`. The basic stream remains only
send/recv/progress/context; endpoint establishment/listening states remain in
the selected provider's control API. Connection establishment, listening,
reconnection and shutdown remain with the consumer. The channel never
selects lwIP, Wi-Fi, a hardware SDK or connection role. The optional
[network module](../network/README.md) supplies caller-driven lwIP endpoints;
the consumer injects native progress/core-region hooks. A Wi-Fi consumer
obtains those hooks through the public Wi-Fi HAL runtime descriptor.

OTA core is independent of protocol. Select `ENABLE_ESY_OTA` for `esy_ota_core`
and, when native Flash installation is needed, `ENABLE_ESY_OTA_FLASH` for its
provider. Management image transactions use `ENABLE_ESY_REMOTE_MANAGER` and
`ENABLE_ESY_REMOTE_MANAGER_OTA`, which assemble `esy_remote_manager` and
`esy_remote_manager_ota`. Select `ENABLE_ESY_REMOTE_MANAGER_PROTO_BIN` separately
for the `esy_remote_manager_proto_bin` payload codec. The consumer owns framing,
duplicate suppression, response submission through its borrowed sender, session
cleanup and activation delivery confirmation/reset. See the
[management contract](../remote_manager/README.md) and
[OTA provider contract](../ota/README.md) for integration and lifecycle details.

## Contracts

All handles must be initialized with `{0}` before first use. Active reinitialization
returns `ESY_ERR_STATE`; finish cleanup successfully before reuse. Caller buffers
do not require prior clearing. Core/channel APIs and callbacks are serialized on
one owner and are not ISR-safe. Worker APIs use their explicit command and
lifecycle contracts below. Callbacks may enqueue/cancel TX, or submit/abort Modbus requests,
but cannot recursively poll or change lifecycle. Session-reset callbacks cannot
enqueue. UART IRQ callbacks only publish atomic stream events to the owner task.
UART deinit returns errors without discarding the active handle, so retain all
storage and retry after hardware cleanup. It aborts active TX DMA and waits by
returning `ESY_ERR_BUSY` if physical TX has not yet become idle.

ProtoBin payload limits are 1..65535 bytes; the TCP channel restricts TX payloads
to `UINT16_MAX - ESY_PROTO_BIN_OVERHEAD_BYTES`. A network provider must support
whole writes large enough for the complete configured frame. The lwIP provider
additionally limits each write to its configured send capacity and 16-bit length.
Millisecond timeouts are
1..INT32_MAX; manager zero selects its configured/default value. Oversized RX
frames are rejected even when the supplied work buffer has spare capacity.
Lost UART bytes discard the partial parser. TCP or USB disconnect ends the peer session:
queued transactions receive IO_ERROR and neither payloads nor deduplication state
carry into the next connection. A connection session change is observed before new
input or TX. Temporary send backpressure retains the queued payload, sequence
and retry budget. An expired ACK wait stays inactive while a retry waits for
capacity; successful acceptance starts its next timeout phase. Accepted network
bytes remain accounted until peer acknowledgement
or terminal session loss; local acceptance is not completion evidence.

Connection status exposes only `INACTIVE`, `OPEN` and `EOF`, independently of
endpoint resolving, connecting or listening states. `EOF` represents receive
half-close. The channel continues bounded polls
to drain buffered final DATA/ACK frames and transport acknowledgements. Its
`esy_proto_bin_tcp_channel_is_peer_drained()` query identifies completed drain;
the endpoint owner supplies an explicit deadline and closes stalled peers.
The owner must not close immediately upon observing connection `EOF` (or the
lwIP endpoint's `CLOSED`), as complete input
may still await delivery. A new session clears the prior peer's drain result.

USB channel consumers must initialize CDC with `discard_on_disconnect = true`;
channel init rejects CDC handles without this policy. Keep CDC initialized until
the channel has been deinitialized. CDC records DTR closure
even when the host reopens between channel polls. Old software RX/TX FIFOs are
discarded at closure; reconnect preserves new bytes already in RX. The channel
then resets partial parser state, RX deduplication, pending ACK, TX reservations
and queued/in-flight transactions before processing the new peer. Reset callbacks
cannot reserve or commit TX. Polling while disconnected does not consume RX or
advance TX timeouts. Physical disconnect without a DTR-close callback must be
observed by the USB task before reconnect; the provider must clear its endpoint
state on bus reset. Software FIFO cleanup cannot recall an endpoint packet
already submitted to USB.

ProtoBin `send`, `receive` and `is_rx_ready` hooks are mandatory. `is_tx_busy`
may be NULL only when send copies or finishes using the complete frame before
returning. Async senders provide busy tracking until their borrow ends; a failed
send must not retain the frame buffer.

Modbus completion statuses are OK, TIMEOUT, TRANSPORT_ERROR, BAD_FRAME, ABORTED,
and EXCEPTION. CRC failures report BAD_FRAME. Submission failures return errors
synchronously without invoking the completion callback: an uninitialized handle
returns `ESY_ERR_INVALID_ARG`, and exhausted request storage returns
`ESY_ERR_NO_MEM`.

Modbus responses match unit/function and TCP transaction ID. RTU broadcasts
complete after acceptance by default and are never retried. With the optional
`defer_tx_completion` master setting, the owner reports each accepted send through
`esy_proto_modbus_master_on_tx_complete()` after releasing its asynchronous borrow.
Broadcast success and response timing then wait for physical TX completion;
asynchronous errors complete with TRANSPORT_ERROR. The UART channel uses this
mode; synchronous senders leave it disabled and start response timing when the
send callback returns successfully. Async manager senders must set
`is_tx_busy`; NULL explicitly means the send callback copies/completes before
returning. Borrowed TX storage is untouched until busy becomes false, including
after abort. Master/slave deinit returns BUSY while that storage is borrowed.
Modbus complete frames and slave responses use the codec's PDU limit
`ESY_PROTO_MODBUS_MAX_PDU_LEN` (default 252, configurable in 1..253). The optional
master request-storage limit `ESY_PROTO_MODBUS_MANAGER_MAX_PDU_LEN` must be in 1 through the codec
limit; invalid combinations fail compilation. Build the codec, manager, and
consumers with matching definitions. Spare workspace capacity does not permit
over-limit requests or responses; the codec rejects them before dispatch/send.
Master abort notifies all requests present on entry with ABORTED; new callback
submissions survive and nested abort returns BUSY. Successful master deinit also
notifies outstanding requests and rejects submissions during those callbacks.
Initialize the master configuration with default_cfg or zero and build all
consumers against the same public headers.
Slave requests while busy are dropped; the peer can retry after its timeout.

Initialize session configuration with `esy_proto_bin_session_default_cfg()` and
supply `static_resources` containing queue, TX work and RX work buffers with their
capacities. The session copies this descriptor; the buffers remain borrowed until
successful deinit. Queue block count must be a power of two of at least two;
usable capacity is count minus one. Workspaces include frame overhead and must
not overlap. The typed session owns fixed opaque storage, independent of the
borrowed sender descriptor. Manager deinit returns a status; retain resources on
failure and build consumers with matching public configuration definitions.


- Core/channel public headers expose no OS resources. Worker headers use public
  OS wrapper types; no protocol public header exposes native RTOS or vendor SDK types.
- Keep all buffers caller-owned and sized explicitly.
- Codecs live in `esy_common/protocol_codec` and are transport-agnostic.
- ProtoBin sessions own protocol state but do not own the physical I/O device.
- RTU silent interval detection and TCP stream reassembly belong to the I/O
  adapter below the Modbus manager. The manager is the sole response-matching,
  timeout and retry owner for each request.

### Modbus UART ownership, timing and shutdown

Master/slave channels borrow an initialized exclusive UART with HAL RX stream
and DMA TX support. Zero opaque storage before create; a failed create with a
non-NULL output retains resources and requires destroy retry. Cleanup must stop
RX before releasing its buffer/context. A failed stop or restart preserves that
borrow. Master queues, payloads and callbacks stay alive until destroy succeeds.
Configuration is copied. No callback may recursively poll or destroy its owner.

One channel poll consumes at most `ESY_PROTO_MODBUS_MANAGER_MAX_PDU_LEN + 16`
RX bytes, dispatches at most one frame, ticks the manager once and takes two TX
scheduler slices. Manager queue scans and synchronous providers/callbacks add to
elapsed time. Submit enqueues immediately; abort/destroy can synchronously invoke
up to the configured queue depth in completion callbacks. Per-request completion
and context fields remain NULL; the channel's typed completion callback receives
each terminal result.

RTU idle gaps use the modulo-32-bit HAL millisecond tick. Scheduling gaps and
deadlines must stay below 2^31 ms. Zero gap selects 10 ms; master gaps are below
INT32_MAX and slave gaps may equal it. Poll often enough to observe every gap;
missed byte arrival times cannot be reconstructed. RX loss discards partial
frames until recovery. Response timeouts begin after physical TX completion.
Broadcasts wait for the larger of the RTU gap and `broadcast_turnaround_ms`
(zero selects 100 ms), plus one clock tick, before the next TX.

A worker owns a separate channel and adds one task with a nominal 1 ms poll
period. Master submit/abort use one copied command slot; concurrent commands
return BUSY. An unclaimed command is withdrawn after 100 one-millisecond waits;
a claimed command must finish on its owner. Scheduling and callback cost can
extend elapsed time. Owner completion callbacks may submit/abort directly but
cannot destroy their worker. Queries expose independent atomic snapshots.

Worker destroy closes admission, lets the owner clean its channel, and confirms
cooperative parking before deleting the task. Each deletion attempt allows up
to 100 one-millisecond waits; BUSY and cleanup errors retain all resources.
Failed creation without a task is cleaned by the lifecycle caller. Stop API
producers before destruction, retry retained cleanup and reclaim storage only
after ESY_OK. The consumer defines each worker's OTA policy: confirm destruction
before Flash operations requiring UART silence, or explicitly establish safe
continued execution. No OTA detection or system-wide stop policy is implicit.

### Modbus TCP framing, deadlines and peer lifetime

Create requires zero-initialized channel storage and a connected OPEN or EOF
provider with a nonzero session identifier. Configuration is copied; provider,
callback context and master queue arrays remain borrowed until destroy returns
success. A channel pins that session. INACTIVE state, a different session,
malformed MBAP, truncated EOF input or a fatal provider error closes admission
and permanently fails the channel. Master requests receive one terminal result:
BAD_FRAME for malformed/truncated framing, TRANSPORT_ERROR for other transport
failures. Normal abort/destruction reports ABORTED. After a terminal fault, the
endpoint owner must retire that connection and establish a fresh session before
creating another channel; reusing the same stream can misassociate stale replies.

Keep one channel for the peer session across transactions. Ordinary destroy also
requires retiring the old connection before recreation if provider-accepted
requests remain unanswered or RX is unread/partial. Reusing the same session
requires proving there is no residual or late traffic: recreating the manager
resets transaction identifiers and cannot distinguish stale replies. Destroy
releases local channel resources but does not establish that connection-level
condition.

Each poll progresses the provider at most once, attempts at most one whole-frame
write, consumes at most `rx_byte_budget` bytes and dispatches at most one ADU.
The default RX budget is 256 bytes. Partial input persists between polls; extra
complete frames remain in the provider for later polls. PID must be zero and
LEN must include one UID plus a nonempty, codec-bounded PDU. Invalid lengths are
terminal rather than triggers for guessed stream resynchronization.

Manager TX copies the complete ADU into channel-owned staging before returning.
Zero or BUSY from `send_all` retains it, and slave backpressure prevents consuming
another request whose response would otherwise be lost. Positive prefix writes
and receive overreports violate the connection contract and fail with IO. A
successful whole write is never repeated while awaiting peer-session confirmation.
Provider calls copy or retire submitted storage before return; channel destroy
does not revoke bytes already accepted by TCP and never closes the endpoint.

The local TX deadline starts on staging and includes whole-write acceptance and
confirmation that the same peer session still owns the connection. Its default
is 1000 ms; configured waits are 1..INT32_MAX ms. Once accepted and confirmed,
the manager starts the response timeout (default 100 ms). TCP peer ACK is needed
only for the final drain query. Local TX timeout and response timeout are separate
phases; provider BUSY cannot suppress their observations. Completed, cancelled
or failed local waits remain disarmed until an explicit new transmission.

These waits use the modulo-32-bit HAL millisecond tick. The counter must not
reset, and owner observation intervals must remain below 2^31 ms. A stopped tick
freezes waits; coordinate cancellation before a pause when elapsed-time policy
requires otherwise. Synchronous provider/callback duration adds to poll latency.
The work budget is not a hard wall-clock guarantee. Poll observes manager expiry
before dispatching a received response, including provider time spent that poll.

EOF permits draining buffered complete frames and sending final slave responses,
while preventing new master transmissions. If `recv` first discovers EOF and
returns IO, a fresh same-session EOF observation with no readable bytes resolves
that result as half-close; other provider errors still propagate. Partial final
frames fail. Outstanding
master requests fail once the peer's buffered responses are exhausted. A healthy
peer is drained only with no readable/partial input, staged TX, pending master
request or unacknowledged provider bytes. The endpoint owner supplies its own
bounded drain/close policy; an unacknowledged peer must not keep resources forever.

Each master has one in-flight transaction with an independent priority queue.
Request Unit Identifier and function code, plus TCP Transaction Identifier, must
match the response. Slaves serve exactly their configured UID (default 0xff);
all UIDs 0..255 are valid and TCP UID zero is not an RTU broadcast. Direct TCP
devices that rewrite UID need an explicitly compatible configuration. The module
does not implement wildcard unit routing or multiple in-flight transactions on
one connection. Application register maps, function-code handling and retry
idempotence policy remain consumer-owned.

### JSON TCP framing and execution

JSON TCP uses this platform wire convention in both directions:

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 4 bytes | Unsigned big-endian JSON byte length, excluding this prefix. |
| 4 | Declared length | Exactly one complete UTF-8 JSON document. |

The length must be positive, fit uint32_t and stay within the receiver's body
buffer. There is no C terminator, newline delimiter, native structure, CRC,
request ID, application ACK or implicit retry. Objects, arrays and scalar roots
are supported, as are formatting whitespace and literal newlines outside JSON
strings. Empty, malformed or truncated documents fail. This is not raw
concatenated JSON, NDJSON, HTTP or WebSocket; peers must agree on this framing.
JSON field schemas, binary value representation and JavaScript-safe integer
representations belong to the consuming application/management adapter.

For example, a Python socket peer sends one document as follows:

```python
import json
import struct

def send_json(sock, value):
    body = json.dumps(value, ensure_ascii=False, allow_nan=False).encode("utf-8")
    sock.sendall(struct.pack("!I", len(body)) + body)

def receive_json(sock, maximum_bytes):
    def read_exact(count):
        result = bytearray()
        while len(result) < count:
            block = sock.recv(count - len(result))
            if not block:
                raise EOFError("Incomplete JSON TCP frame")
            result.extend(block)
        return bytes(result)

    length, = struct.unpack("!I", read_exact(4))
    if length == 0 or length > maximum_bytes:
        raise ValueError("JSON TCP length outside receiver limit")
    return json.loads(read_exact(length).decode("utf-8"))
```

Choose socket deadlines in the host owner. The firmware owner similarly bounds
input length, unfinished frames, send stalls and EOF/ACK drain; no channel clock,
timeout or automatic reconnect is installed. There is no timeout/rollover state
to rearm. A stopped polling owner makes no progress; poll count does not measure
elapsed time. The channel owns no task; its execution owner decides the
OTA stop/continue policy and provider/Flash coordination. A TCP stream does not
provide authentication; use an appropriate authenticated transport provider when
required by the consuming product.

One poll calls provider progress at most once, accepts at most one TX prefix of
`tx_byte_budget` bytes and consumes at most `rx_byte_budget` bytes, delivering
at most one JSON message. Zero budgets select the named default of 256 bytes
per direction. Zero or BUSY from send/recv defers work. Partial sends retain
their accepted offset even when subsequent status observation is BUSY; those
bytes are never sent twice. Provider/callback latency and full-document JSON
validation still contribute to call time; byte limits are not a hard-time bound.

The single outgoing slot remains occupied until every header/body byte has been
copied by the provider. This local acceptance is not peer TCP ACK or application
completion. RX is independent of that slot, so a receiver callback's reply may
return BUSY while an earlier TX remains staged. The owner must retain and retry
any required response using its own bounded work storage; the channel neither
drops that response automatically nor supplies an application response queue.

Init binds one peer session permanently. A session change, inactive peer,
fatal provider error, invalid length/JSON or over-limit body closes admission
and latches its first error; subsequent poll/send do not replay work. No
resynchronization is attempted. EOF drains buffered frames and permits replies;
a partial final prefix/body fails. A completed body waiting for transient status
confirmation remains pending and is not mistaken for truncated EOF.

`get_status` is a local snapshot and does not progress input or query the
provider. `peer_drained` reflects a successfully observed same-session EOF with
no buffered/partial input, staged TX or unacknowledged bytes. It covers only
channel/provider work: the owner must also check its pending response work
before close. An explicit successful send on the still-live EOF peer clears
that observation and permits an owner-delayed reply. Init/deinit or a changed
clock cannot silently recreate old traffic.

Deinit is idempotent, cancels local framing/staging and releases borrowed
buffers without closing the endpoint or invoking callbacks. Provider sends copy
bytes before return, so pending peer ACKs retain provider resources rather than
channel buffers. After failure or abandonment of partially processed traffic,
retire the old connection and use a fresh peer session before init again;
resetting local buffers does not discard residual or late TCP bytes safely.

### JSON USB session and delivery

USB uses the exact [four-byte big-endian JSON framing](#json-tcp-framing-and-execution)
described above. The prefix counts UTF-8 bytes only, including any JSON formatting
whitespace and excluding the prefix and a C terminator. One host serial read is
not one message; hosts must accumulate a complete prefix and declared body.
The same JSON encoder/decoder can therefore serve TCP and USB while each channel
retains its own connection/session behavior. No UART JSON channel is provided.

All JSON channel operations and native CDC calls are serialized on one non-ISR
owner. USB poll calls no stack pump, consumes at most `rx_byte_budget` bytes and
delivers at most one root; it attempts at most one CDC write of
`tx_byte_budget` bytes and flushes after positive acceptance. Zero budgets select
the named default of 256 bytes. Short/zero reads and writes preserve offsets,
and a message can exceed the CDC FIFO capacity because TX is accepted in bounded
prefixes. Full-document parsing and provider/callback runtime still contribute
to latency. The channel owns no task, IRQ, DMA, watchdog, clock or timer; the
existing execution/USB owner decides OTA policy, Flash safety and bounded stall
or incomplete-frame recovery. There is no timeout arithmetic or rollover state.

Send success means the document was validated and copied into one local staging
slot. `tx_pending` becomes false after all bytes have been copied to CDC, without
claiming USB endpoint completion or host application receipt. There is no TCP
half-close, peer-ACK drain or automatic retransmission. Application acknowledgments,
request IDs and mutation idempotence remain the consuming protocol's contract.
RX proceeds independently of staging, so callbacks must retain any required
response rejected with BUSY in the owner's bounded work storage for retry.

CDC session isolation is mandatory. The session generation advances when CDC
observes DTR closure or a physical disconnect; the CDC owner clears old software
FIFOs before line notifications and does not discard new-peer RX on open. Channel
poll/send compares generations before considering an old sticky error or occupied
TX slot. A changed generation cancels old local header/body/TX state, clears that
session's error, updates local status and returns `ESY_ERR_IO` once without
performing new-peer I/O. A later call can consume bytes already sent by the new
peer. Status error is `ESY_OK` after such a reset: distinguish this one-time
discontinuity from an impossible I/O count that latches IO for the current
generation. No old document is replayed automatically.

Initial closed polling returns OK and send returns STATE without staging.
Malformed JSON, zero/oversized length, impossible backend counts or a close
observed without a generation change latch the first current-session error.
Poll/send keep returning it until a new generation is observed or the channel is
deinitialized. There is no resynchronization within a damaged byte stream. RTS
changes alone do not create a new session. Compare generations for inequality;
they are wrapping identities, not ordered timestamps.

The stack owner must observe physical disconnection through a pump before
reconnection when no DTR-close callback is delivered. Native bus reset must
retire endpoint state; neither channel deinit nor software FIFO discard can
recall already submitted USB packets. Keep the same CDC handle initialized
through the channel lifetime: CDC reinit reuses generation values and cannot be
treated as a safe continuation. After local deinit abandons partial traffic,
close/retire that old stream before recreating rather than merely zeroing state.
Channel deinit is idempotent, calls no callback/pump/CDC lifecycle function, and
leaves FIFO and endpoint management with the owner.

## Resource Model

The module uses caller-owned buffers and typed fixed-capacity session/manager
handles. No runtime backend storage or polling capability exists in a sender.
Sessions and managers create no task and allocate no heap storage. The TCP channel borrows its network connection and does not allocate native
connection resources. The network provider separately documents its PCB, pbuf,
TLS and copied-send resource costs. Channel buffers and provider RX storage have
independent ownership and must remain alive through their respective cleanup.
ProtoBin channel handles, configuration/resource descriptors and every backing buffer
must occupy disjoint, nonwrapping address ranges. UART RX stream storage must
also remain disjoint from all protocol buffers. Invalid storage is rejected
before provider I/O or handle modification.

JSON channel storage initially requires `{0}`; init failure preserves it and
successful deinit returns it to zero. Its config, resource descriptor, handle,
RX body buffer and TX frame buffer must occupy pairwise disjoint, nonwrapping
ranges. No backing buffer clearing is required. They remain borrowed until
successful deinit. JSON callback views cannot outlive callback return; sending
an echo may borrow those RX bytes because send copies into disjoint TX storage.

JSON USB applies the same caller-owned model and additionally requires the CDC
handle to be disjoint from the channel, configuration/resource descriptors and
both buffers. It retains no native endpoint or task object. RX callback views
expire at callback return; the CDC context and every buffer outlive successful
channel deinit. No backing buffer zeroing is required.

Modbus UART channel and worker handles are different opaque types. Channel storage
contains no task, command bridge or OS execution hooks. Worker storage includes
one private channel plus its task/command state; the caller separately supplies
queue backing, stack and task-control storage. Public storage capacities are
4096/3072 bytes for master/slave channels and 5120/3328 bytes for master/slave
workers. Build-time assertions check private size and public alignment. These
capacities do not replace target stack and latency measurements.

Modbus TCP master/slave storage is independently caller-owned and contains the
manager, one bounded RX frame and one copied TX frame. Masters additionally
borrow priority slots and request payload arrays. No provider memory, native
socket, task, callback registration or heap allocation is created. See the
public TCP storage macros for capacities and alignment requirements.

## Verification

JSON TCP uses its own provider-backed suite so ProtoBin and Modbus tests do not
require a JSON checkout. From the platform root, with `ESY_COREJSON_PATH` set:

```sh
ruby tools/quality/run_module_tests.rb esy_middleware/protocol/tests/json \
  esy_middleware/protocol/tests/json/build test:all
ruby tools/quality/run_module_tests.rb esy_middleware/protocol/tests/json \
  esy_middleware/protocol/tests/json/build/sanitizer test:all --sanitizer
python3 -B esy_middleware/protocol/tests/test_json_tcp_cmake_consumer.py
python3 -B esy_middleware/protocol/tests/test_json_usb_cmake_consumer.py
```

The suite covers fragmented/coalesced frames, complete JSON/Unicode/integer
validation, budgets, partial-send backpressure without replay, callback reply
and reentry, peer/session failures, EOF truncation/drain, delayed replies and
overlapping resources. C11/C++17 consumer probes link the actual codec/channel
with no OS/HAL/DS and verify independent feature assembly, provider isolation and
enabled-to-disabled reconfiguration. The quality gate merges this suite's
coverage with the separate channel and common codec reports. Native
consumer or HIL procedures measure actual network-stack/provider timing and
exercise reconnect and OTA coordination.

The same suite also links the real CDC wrapper and TinyUSB backend against the
shared TinyUSB fake. USB cases cover DTR quick cycles, initial closed binding,
physical disconnect through an external pump, RTS-only changes, cancellation of
old partial frames, preservation of new-peer input, true short/zero I/O,
overreported counts, callback reply/reentry and resource alias rejection. The
USB CMake probe verifies C11/C++17 direct/aggregate consumers, independence from
TCP/network, explicit backend/feature errors and enabled-to-disabled isolation.
Native USB consumer/HIL procedures exercise hardware enumeration, physical
unplug/reset, real endpoint completion and host interop; the fake cannot establish
those properties.

Run module tests from this directory:

```sh
cd external/EsyMcuPlatform/esy_middleware/protocol/tests
ceedling test:all
ceedling gcov:all
ceedling --mixin=sanitizer.yml test:all
python3 -B test_cmake_consumer.py
python3 -B test_coverage_gate.py
```

Coverage reports are generated under:

```text
external/EsyMcuPlatform/esy_middleware/protocol/tests/build/artifacts/gcov/gcovr/
```

The coverage gate requires every production C file to reach 85% line coverage
and retains the aggregate floor. It resolves complete source paths and rejects
duplicate aliases or foreign files, including same-name files from another
directory. Local internal-header rows are permitted but cannot replace a source.
This line gate does not claim branch coverage or hardware acceptance.

The current tests cover:

- executable C11/C++17 family/channel consumers, header-only sender/types
  consumers without HAL/OS includes, missing dependency diagnostics and pure
  codec-only assembly;
- borrowed-connection TCP framing, whole-write backpressure, peer acknowledgement,
  session replacement and terminal-session cleanup without native stack calls;
- Modbus wrong-peer/transaction rejection, send failure, asynchronous buffer
  borrow, broadcast, callback reentry, timeout/retry through tick wrap and the
  maximum legal wait; completed/cancelled requests remain inactive after a full
  counter period, and a new request explicitly starts a new wait;
- UART stop failure/retry and partial-parser recovery;
- Typed ProtoBin session fake-I/O behavior, including ACK, NACK, CRC error,
  separate transmission and acknowledgement timeout phases, retry, duplicate
  suppression, independent TX/RX sequences, TX busy, and invalid
  configuration;
- slow RX callbacks, send acceptance and idle observations, fresh deadlines for
  retries, and exact timeout boundaries through tick wrap, including INT32_MAX;
- NACK before physical idle, repeated NACK without deadline extension, stuck TX
  with zero/nonzero retries, borrowed-buffer preservation, delayed idle retry,
  zero-retry completion, and pending control ACK alongside an early NACK;
- default/custom RX budgets, complete-frame delivery across polls, bounded
  callback dispatch, and TX/timeout progress under continuously readable input.
- USB DTR close/reopen between polls, physical disconnect, sequence reuse,
  partial-frame and FIFO cleanup, pending ACK/TX cancellation, and reset-callback
  TX rejection; mandatory RX hooks and synchronous senders without busy hooks.
- caller-driven Modbus UART channels, independent typed worker handles, command
  handoff, callback reentry, parked deletion, partial-create cleanup and retained
  UART resources across failed stop/restart.
- Modbus TCP fragmented/coalesced MBAP input, byte and callback budgets, copied
  TX backpressure, session isolation, half-close drain, typed callbacks, timeout
  boundaries and lifecycle cleanup using injected connection providers; the real
  lwIP adapter also runs against the fake stack for split/coalesced input, final
  replies, ACK drain, native backpressure and reconnect isolation.

## Known Limits

- TCP channels require an exclusive connection extension with honest peer-ACK evidence;
  providers exposing only local send acceptance cannot satisfy this contract.
  Connection policy and provider progress are owned by the consumer. See the
  network module for lwIP and Pico polling constraints.
- USB CDC must be exclusively written by this channel; configure the CDC TX FIFO
  for at least `tx_payload_max + ESY_PROTO_BIN_OVERHEAD_BYTES`. The CDC public API
  exposes current free bytes, not configured capacity, so the integrator verifies
  this capacity prerequisite. Temporary FIFO exhaustion only delays sending.
- In target smoke/HIL tests, measure DMA/IRQ timing and exercise network
  interoperability and long-duration operation.

### Uncommitted TX cancellation

`esy_proto_bin_session_cancel_tx()` releases an uncommitted reservation without
changing queued or in-flight traffic. The session sender exposes the same
operation through its required cancel callback. Cancellation is idempotent for
an initialized session; inactive/resetting sessions reject reservation changes.
Calls share the session's serialized owner and are never ISR-safe. Resolve each
reservation and detach sender borrowers before releasing provider resources.

UART channel initialization preflights RX ownership before starting the stream.
A pre-existing stream is left untouched. A failed start that may retain receive
storage is stopped before cleanup completes; failed cleanup retains the channel
and backing storage for `esy_proto_bin_uart_channel_deinit()` retry. Error recovery
likewise stops a retained failed restart before starting again. Unit regressions
cover both initial failure and recovery with retained callbacks and failed stop.
