<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Streaming HTTP Client

## Purpose

`esy_http_client` implements caller-driven HTTP/1.1 transactions for REST calls
and streaming downloads over an existing TCP or authenticated TLS connection.
The caller receives decoded body blocks without buffering the complete response.
The module adapts the llhttp response parser and owns request encoding, bounded
I/O progression, transaction deadlines and consumer backpressure. It creates no
task, endpoint, allocator, retry loop or product update policy.

## Public Surface

- `esy_middleware/http/esy_http_client.h`: caller-owned client storage, request
  types, static resources, body/header callbacks, configuration and status.
- `esy_http_client_init`, `esy_http_client_submit`,
  `esy_http_client_process`, `esy_http_client_get_status`,
  `esy_http_client_cancel` and `esy_http_client_deinit`.
- Targets: `esy_http_client` / `Esy::http_client`.

The public header exposes no llhttp, native network, hardware SDK or OS types.
The shared `esy_network_stream_t` contract comes from
[network](../network/README.md). HTTPS uses the stream returned by the existing
[authenticated TLS client](../tls/README.md).

## Integration

Select the module in the consuming project's sole feature import:

```cmake
set(ENABLE_ESY_NETWORK ON)
set(ENABLE_ESY_HTTP_CLIENT ON)
```

Supply pre-generated llhttp **v9.4.3** through an existing
`esy_http_parser_provider` target or an explicit `ESY_LLHTTP_PATH` CMake or
environment input.

For library host tests, `./setup.sh` prepares seven pinned official providers;
the Python/Ruby test helpers discover their cache automatically. Firmware CMake
uses the consumer's explicit providers, independently of this test cache and
lock. See [software provider setup](../../tools/README.md#software-provider-setup).
For the consumer's checkout, use the upstream
[`release/v9.4.3` tag](https://github.com/nodejs/llhttp/tree/release/v9.4.3),
tested at commit `0e815792b167a9bd8ace259b95b7da953776c288`. This checkout has
`include/llhttp.h`, `src/api.c`, `src/http.c`, `src/llhttp.c` and `LICENSE-MIT`.
The ordinary source tag is not a substitute for the pre-generated release.
Upstream sources and their MIT license remain in that external provider checkout;
the platform does not fetch them or invoke Node.js during configuration.

The project importer creates `esy_http_parser_provider` only when HTTP is
enabled. Direct module consumers must call the helper below or supply the
provider target before module assembly:

```cmake
include("${ESY_MCU_PLATFORM_DIR}/cmake/esy_llhttp_provider.cmake")
esy_configure_llhttp_provider()
add_subdirectory("${ESY_MCU_PLATFORM_DIR}" platform)
target_link_libraries(application PRIVATE Esy::http_client)
```

An existing consumer-provided `esy_http_parser_provider` target takes precedence.
It must supply the compatible llhttp sources, include paths and configuration.
The adapter uses the parser's strict framing behavior; unsupported or relaxed
provider configurations need separate qualification.

The connection owner follows this sequence:

1. Establish TCP and, for HTTPS, authenticate TLS with the intended hostname,
   trust anchors, qualified RNG and trusted UTC. Obtain its stream. HTTP has no
   insecure-TLS switch, DNS resolver or connection establishment API.
2. Initialize `esy_http_client_t client = {0}` with that stream, explicit budgets,
   clock and exclusive workspaces. Initialization performs no network I/O.
3. Submit one request. Supply an encoded origin-form target such as
   `/firmware.bin?version=2`, a Host authority and optional custom headers.
4. Call `esy_http_client_process` regularly from the stream's serialized owner.
   Consume body blocks or return `ESY_ERR_BUSY` to apply backpressure. Read the
   terminal status to distinguish HTTP status codes from transaction failures.
5. After completion/failure, deinit HTTP, then stop/deinit TLS and close TCP in
   their documented order. Cancel HTTP first if it is still active. A new
   request requires a fresh connection and an explicit new initialization.

Declare aggregate workspace using `ESY_HTTP_CLIENT_STATIC_STORAGE` and bind it
using `ESY_HTTP_CLIENT_BIND_STATIC_RESOURCES`. Capacities are consumer inputs;
they do not scale with the total response body size. Configure `max_body_bytes`
for the application's accepted response size, rather than the receive capacity.

GET, HEAD, POST, PUT, DELETE and PATCH are supported. Requests have a known body
length. The client generates Host, Content-Length and `Connection: close`;
callers cannot override framing, connection-management or upgrade headers.
URL encoding, authorization credentials and content serialization remain with
the consumer. Redirect responses expose their status and Location header to the
owner; no redirect or cross-origin credential forwarding happens automatically.

### Usage Example

The caller supplies a `{0}` client and prepared configuration with a connected
TCP/authenticated TLS stream, clock, workspaces, budgets and body callback. This
example starts a GET using caller-supplied Host and encoded path. The function
copies request metadata; the handle and referenced configuration resources stay
alive until successful cleanup.

```c
#include "esy_middleware/http/esy_http_client.h"

esy_err_t http_example_get(esy_http_client_t *client,
                          const esy_http_client_config_t *config,
                          esy_http_string_t host, esy_http_string_t target)
{
    esy_http_request_t request = {
        .method = ESY_HTTP_GET, .host = host, .target = target
    };
    esy_err_t result = esy_http_client_init(client, config);
    if (result != ESY_OK) return result;
    result = esy_http_client_submit(client, &request);
    if (result == ESY_OK) return ESY_OK;
    esy_err_t cleanup = esy_http_client_deinit(client);
    return cleanup == ESY_OK ? result : cleanup;
}

esy_err_t http_example_stop(esy_http_client_t *client)
{
    esy_err_t result = esy_http_client_cancel(client);
    return result == ESY_OK ? esy_http_client_deinit(client) : result;
}
```

After a successful start, call `esy_http_client_process` from the owner and query
`esy_http_client_get_status` until COMPLETE or FAILED. Check both transaction
errors and the HTTP status code. On completion, failure or cancellation, call
`http_example_stop`; retire the underlying TLS/TCP providers in order only after
it succeeds.

## Contracts

### Ownership and lifecycle

One task or bare-metal main loop owns the client and all provider calls.
Every API is non-ISR. Callbacks run within that owner's current process call;
they may query status but must not mutate or recursively drive the client.
Reentry checks do not provide cross-task synchronization. Different clients may
run independently only if their providers permit it.

The client requires `{0}` on first use. Buffers need no initial contents. Init
copies configuration/resource descriptors and borrows the referenced buffers,
stream and callback/clock contexts through successful deinit. Workspaces must be
pairwise disjoint and must not overlap the handle. Repeated init returns STATE;
failed init preserves the handle and acquires no resources. Live handles must
not be copied, moved or cleared.

Submit validates and copies request headers without network I/O. Request strings
and header arrays need live only through submit; the body remains borrowed until
a terminal state. All inputs must be disjoint from the handle and workspaces.
Invalid arguments, insufficient request capacity or a failed clock sample leave
the client IDLE so the request can be corrected. Each initialized client accepts
one request; terminal states require deinit and a new connection before reuse.

Cancel performs no provider I/O, releases the request-body borrow and disables
transaction waits. It is idempotent on terminal initialized clients. Deinit
accepts IDLE/terminal or zero-state handles, detaches references and restores
`{0}`; deinit while active returns STATE, retaining resources. Neither operation
closes or frees the network/TLS provider.

### Streaming and execution budget

Each process call advances the provider at most once and performs at most one
send or receive limited by `io_budget_bytes`. A receive is additionally bounded
by the raw receive buffer. Decoded bytes from that input are staged in a separate
body buffer; its capacity must be at least the receive buffer capacity.

The body callback receives at most one nonempty block per call. OK accepts the
entire block. BUSY accepts none: identical bytes are retained and retried before
further provider/input progress. No response bytes are discarded to make space.
Other consumer errors terminate the transaction. Header callbacks receive a
complete name/value pair and may reject it, but BUSY is not retryable for header
events. Body/header views expire when their callback returns; copy anything that
must outlive it. Header events identify informational responses and trailers.

Native TLS, transport and callbacks remain synchronous. Limiting input bytes or
provider calls does not establish a wall-clock or hard realtime bound. Budget
native cryptography, callback work and network progress against the owner's
watchdog and scheduling obligations. Do not call Flash-writing code from native
network callbacks or assume an HTTP callback itself provides Flash arbitration.

### Framing, status and failure

Responses use explicit Content-Length or chunked transfer coding, including
trailers; HEAD and applicable no-body statuses complete without a body.
Informational responses are consumed until the final response. Headers are
bounded both per complete field by `header_buffer_size` and in aggregate by
`max_header_bytes`, which counts status-reason and field-name/value bytes across
informational responses and trailers, plus one accounting unit per message and
field. `max_body_bytes` limits decoded body bytes.

Close-delimited bodies are rejected: the shared stream reserves zero for
temporary lack of data, and its error return alone cannot distinguish orderly
EOF from failure. A premature close, malformed/ambiguous framing, unsupported
transfer/content coding, protocol upgrade or extraneous bytes accompanying the
final response fails the transaction. Completion requires the full length or
complete chunked framing and acceptance of every pending body block.

HTTP status and transport/parser status are independent. A complete 404 or 500
response may have `state == ESY_HTTP_COMPLETE` and `error == ESY_OK`; the consumer
decides what those HTTP codes mean. Terminal failures are sticky and preserve
diagnostics. Previously delivered body bytes are provisional until completion;
a streaming sink must abort its operation if the transaction subsequently fails.
No POST or other business request is retried automatically.

### Time and cancellation

The injected clock supplies continuous unsigned 64-bit microseconds modulo
2^64, a period of about 584,542 years. It must not reset or change rate during a
transaction and must advance during provider/callback work and any sleep that
should count toward deadlines. Total and idle budgets are each positive
uint32 values, at most `UINT32_MAX` microseconds.

Submit arms both waits. Process observes them at least once within the smaller
budget and before a complete counter cycle, while also meeting the provider's
shorter progress cadence. Elapsed time at or beyond the budget expires the
transaction, including time spent in a call that just completed. Accepted I/O
or body progress renews only idle after checking the previous deadline; BUSY
does not renew it. The total budget also covers backpressure. Completion,
failure and cancellation permanently disarm waits, including across later full
counter cycles. Only explicit reinitialization and submit start fresh waits.
Cancel before an unbounded pause or clock discontinuity.

## Resource Model

The platform adapter uses caller-owned opaque handle storage and four exclusive
workspaces: encoded request headers, raw input, a pending decoded body block and
one complete response header field. It creates no heap allocation, socket,
task, queue, timer or singleton connection state. The llhttp parser is held
inside the handle and keeps incremental state between process calls.

Network/TLS providers can allocate native resources and block. Static HTTP
storage does not establish an allocation-free whole stack. Measure actual
parser code size, TLS handshake/record peaks, Flash usage, stack and execution
time on the intended target. No fixed product memory profile is prescribed.

## Verification

From the platform repository root, run `./setup.sh` once to prepare the cached
software providers, then run:

```sh
ruby tools/quality/run_module_tests.rb esy_middleware/http/tests \
  /tmp/esy-http-tests test:all
ruby tools/quality/run_module_tests.rb esy_middleware/http/tests \
  /tmp/esy-http-gcov gcov:all
ruby tools/quality/run_module_tests.rb esy_middleware/http/tests \
  /tmp/esy-http-sanitizer test:all --sanitizer
python3 -B esy_middleware/http/tests/test_cmake_consumer.py
python3 -B esy_middleware/http/tests/test_http_smoke.py
bash tools/checks/check_no_dynamic_alloc.sh esy_middleware/http
```

Optionally set `ESY_LLHTTP_PATH` or `ESY_MBEDTLS_PATH` in the environment to test
another compatible pre-generated llhttp or Mbed TLS checkout.

The deterministic suite uses real llhttp parsing with a controlled byte stream,
clock and consumers. CMake consumers check C11/C++17 public-header use, private
provider isolation, feature on/off behavior, provider diagnostics and dependency
closure without an OS target.

The loopback smoke script downloads a 196,613-byte deterministic body through
257-byte workspaces, with Content-Length and chunked framing/trailers, fragmented
socket I/O and consumer backpressure. It also checks a JSON POST exchange.
With the cached Mbed TLS provider and OpenSSL installed, the same download/REST
paths run through `esy_tls`, including rejection of an incorrect TLS hostname.
Without those prerequisites, HTTPS tests skip explicitly while ordinary HTTP
smoke tests remain available. CI prepares both providers.

Host evidence does not establish target network, entropy, trusted-time or OTA
scheduling safety. Run the embedded consumer build and qualify the real provider
under slow links, disconnects, Flash pauses and the intended watchdog budget.

## Known Limits

- HTTP/1.1 client only; no HTTP server, HTTP/2, proxy tunnel, WebSocket upgrade,
  connection pool, pipelining, automatic redirects/retries, decompression or
  chunked request upload.
- Request targets are already encoded origin-form strings; URL parsing, endpoint
  selection, authorization, cookies and application content belong to the owner.
- Response processing starts after the complete request has been accepted by
  the stream. Early final responses during an upload are not processed in
  parallel with request transmission; the transaction deadlines still apply.
- Responses require supported explicit framing; a server using only connection
  closure to delimit a body is unsupported by this adapter.
- The client provides body blocks suitable for a download sink, not an OTA pull
  service. The consumer must retain one OTA/Flash owner, enforce admission and
  signature policy, and arbitrate push/pull operations. HTTP Range does not
  establish durable resume offsets or recovery across resets.
- This module owns no task. An integrating consumer must declare its OTA run
  policy and coordinate provider quiescence, bounded stop, Flash/ISR/DMA safety
  and watchdog handling before pauses that affect live resources.
