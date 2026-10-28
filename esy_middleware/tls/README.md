<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Authenticated TLS client

## Purpose

Adapt a consumer-provided Mbed TLS 3.6.x library to the shared network stream.
The adapter is a TLS 1.2 client with mandatory CA, DNS hostname
and certificate-date validation, plus optional mutual TLS. MQTT remains a
protocol consumer and does not own cryptographic or connection policy.

## Public Surface

- `esy_middleware/tls/esy_tls.h`: transport, time and RNG inputs; init, process,
  state query, authenticated stream, orderly close and deinit.
- `esy_middleware/tls/esy_tls_mbedtls.h`: caller-owned native client storage.
- `esy_tls` / `Esy::tls`: explicit Mbed TLS software adapter target. Its native
  types and version coupling are intentional; the generic header has no native
  types. Provider compile definitions and headers propagate through this target.

## Integration

Declare all gates in the consumer's selected feature file using ordinary sets:

```cmake
set(ENABLE_ESY_NETWORK ON)
set(ENABLE_ESY_CREDENTIALS ON)
set(ENABLE_ESY_CRYPTO ON)
set(ENABLE_ESY_TLS ON)
```

Before module assembly, call the helper below with an explicit `ESY_MBEDTLS_PATH`,
or provide
`esy_mbedtls_provider` linked to compatible native TLS, X.509 and crypto targets
with one consistent configuration. The helper never downloads sources.

For library host tests, `./setup.sh` prepares seven pinned official providers;
the Python/Ruby test helpers discover their cache automatically. Firmware CMake
uses the consumer's explicit providers, independently of this test cache and
lock. See [software provider setup](../../tools/README.md#software-provider-setup).
Explicit firmware source assembly:

```cmake
set(ESY_MBEDTLS_PATH "/path/to/mbedtls-3.6.x")
# Optional portable reference profile; products may supply their own profile.
set(MBEDTLS_CONFIG_FILE "${platform}/esy_middleware/tls/config/esy_tls_mbedtls_config.h")
include("${platform}/cmake/esy_mbedtls_provider.cmake")
esy_configure_mbedtls_provider()
```

Run this optional helper with CMake 3.21 or later, after the consumer's `project()`
and before library assembly. Older CMake consumers provide the target directly.
An existing SDK provider may instead be wrapped explicitly. It must be 3.6.x;
the old Mbed TLS 2.28 bundled with some SDKs cannot supply this adapter. Use a
maintained provider patch release and the same configuration on every target.

When `MBEDTLS_USE_PSA_CRYPTO` is enabled, the consumer's process-wide provider
owner must successfully call `psa_crypto_init()` before the first TLS init or
other native X.509/PK/TLS operation, and keep PSA available until all native
consumers have stopped. The adapter does not initialize or free global PSA
state. Configure any PSA entropy source and hardware drivers through the native
provider; the per-session RNG does not configure PSA's independent RNG.

Initialize `esy_crypto_random_t random = {0}` with an explicitly qualified
entropy callback. Supply `{esy_crypto_random_fill, &random}` as the TLS RNG.
Initialize `esy_credentials_t credentials = {0}` with CA bytes and optional
paired client certificate/key. Initialize `esy_tls_t tls = {0}` from the adapter
header with an already connected plain stream, expected DNS hostname, a
monotonic clock, trusted UTC and exclusive TX buffer. Call `esy_tls_process()`
until status is READY, obtain its stream and lend it to MQTT. MQTT's progress
callbacks then also advance pending encrypted writes. Before any reconnect,
stop borrowers, deinit TLS and retire its underlying connection.

Generic HAL SDK random bytes do not automatically meet the entropy contract.
The application owns clock establishment and freshness; an SNTP validity flag
alone does not authenticate time. Never install a process-global native clock
hook for individual sessions: this adapter verifies each chain certificate
against its injected UTC while preserving all other native verification flags.
At init it parses the complete CA bundle, rejects malformed/partially parsed
bundles and copies only currently valid trust anchors into the session's native
CA list. Expired or future copies of a renewed root therefore cannot displace
an eligible copy during native path selection. If none is valid, init returns
`ESY_ERR_HASH_FAIL`, releases the credential lease and restores the zero handle.
This selection uses the init-time UTC snapshot. Reinitialize after a UTC
correction or certificate-validity-window change to select different anchors;
each certificate is still checked against current UTC during authentication,
so expiry after init fails closed.

### Usage Example

The caller owns a stable `{0}` adapter handle and a complete configuration with
an already connected plain stream, initialized credentials/RNG, trusted UTC,
monotonic clock and TX workspace. These resources outlive the TLS session.
Call `tls_example_begin` once, then `tls_example_poll` on the same owner until it
returns `ESY_OK` with the authenticated stream; `ESY_ERR_BUSY` means call again.

```c
#include "esy_middleware/tls/esy_tls.h"

esy_err_t tls_example_begin(esy_tls_t *tls, const esy_tls_config_t *config)
{
    return esy_tls_init(tls, config);
}

esy_err_t tls_example_poll(esy_tls_t *tls, esy_network_stream_t *stream)
{
    esy_tls_status_t status;
    esy_err_t result = esy_tls_process(tls);
    if (result == ESY_OK) result = esy_tls_get_status(tls, &status);
    if (result != ESY_OK) return result;
    if (status.state == ESY_TLS_HANDSHAKING) return ESY_ERR_BUSY;
    return status.state == ESY_TLS_READY ? esy_tls_get_stream(tls, stream)
                                         : ESY_ERR_STATE;
}

esy_err_t tls_example_stop(esy_tls_t *tls)
{
    return esy_tls_deinit(tls);
}
```

On a handshake error, require successful `tls_example_stop` before retiring its
providers. After success, stop every stream borrower before `tls_example_stop`;
it aborts pending I/O. For orderly shutdown, call `esy_tls_close` and drive
`esy_tls_process` under the owner's close deadline first. TLS cleanup never closes
the plain connection.

## Contracts

One externally serialized normal-context owner controls a handle and every
borrowed provider. No ISR, terminal fault, callback reentry, task or native OS
dependency is introduced. Both baremetal loops and RTOS owner tasks can drive
the same API. Native/provider execution is synchronous and may allocate or
block; one step limits native operation count, not elapsed cryptographic work.

First initialization requires `{0}`; handles and leases cannot be copied or
moved while live. Init acquires a credential lease and prepares native state;
malformed certificates, partial CA parsing, no currently valid trust anchor,
mismatched keys and provider errors release everything. Credentials cannot be
replaced or destroyed until TLS
deinit, including after CLOSED or FAILED. Other borrowed inputs and their data
must remain valid and disjoint from client state/workspace.

Each process call advances the underlying provider once, then takes one native
handshake step, application write or close step. Handshake time uses unsigned
64-bit microseconds modulo 2^64, with a positive uint32 budget. The clock must
continue across sleep when the deadline should. Process must observe a wait
before a full counter cycle; polling within the timeout is expected. Elapsed
>= timeout wins, including provider/crypto runtime. Completion, failure and
deinit permanently disarm the wait until explicit reinitialization. Stream I/O
and close have no implicit timeouts; their owner budgets progress or aborts.

Send copies at most one workspace-sized prefix before accepting it. It returns
zero while that slot is pending. Stable copied bytes are retried with identical
native arguments after WANT_READ/WANT_WRITE; callers may immediately reuse
their original buffer. Receive never waits for input and defers while TX is
pending. A peer orderly close returns IO and marks CLOSED because the shared
stream reserves zero for temporary lack of data. Renegotiation is disabled;
post-handshake read operations needing control-write/asynchronous retries fail
closed. No application write may accidentally retire a pending native alert.

Close stops accepting application data, drains accepted bytes and sends
close_notify, without waiting for peer acknowledgement. Deinit aborts any
remaining I/O, securely clears staged pending bytes, frees native state and
releases the lease. It never closes the borrowed transport. A successful deinit
restores `{0}` and is idempotent. Failed/closed connections require fresh init.

## Resource Model

The consumer owns the typed native handle, one TX buffer, credential bundle,
plain stream, RNG and clocks. Parsed certificates, private keys, record buffers
and native sessions may allocate inside Mbed TLS. No direct platform allocator,
hidden singleton, task, socket, timer or global clock/RNG is created. Native
resources are configured/freed at explicit lifecycle operations; handshake and
record internals retain their provider-specific allocation and latency costs.
Init temporarily owns the complete parsed CA bundle while copying eligible
anchors, then frees that candidate list before returning; budget this transient
provider memory as well as the retained active anchors.
Measure actual RAM, Flash, stack and watchdog budgets on the target board.

## Verification

From the platform repository root, run `./setup.sh` once to prepare the cached
software providers, then run:

```sh
python3 esy_middleware/tls/tests/test_cmake_consumer.py
python3 esy_middleware/tls/tests/test_tls_smoke.py
ruby tools/quality/run_module_tests.rb esy_middleware/tls/tests \
  /tmp/esy-tls-tests gcov:all
ruby tools/quality/run_module_tests.rb esy_middleware/tls/tests \
  /tmp/esy-tls-sanitizer test:all --sanitizer
```

Optionally set the `ESY_MBEDTLS_PATH` environment variable to test another
compatible Mbed TLS 3.6.x checkout. The loopback smoke tests also require OpenSSL.

The loopback OpenSSL tests generate temporary certificates and exercise
authenticated TLS 1.2, mutual TLS, hostname/trust/date rejection, injected UTC
and RNG errors, source-buffer reuse under deterministic transport backpressure,
state cleanup and timeout wrap/boundary behavior.
Renewed-root tests use fixed certificate dates and injected UTC to check both
bundle orders, expired/future alternatives and failure when no anchor is valid.
PSA-enabled provider consumers initialize the subsystem before native use.
These host tests establish adapter behavior; they do not establish board entropy,
network runtime budgets or hardware retention. Ceedling and sanitizer
configurations cover deterministic
API failures and lifecycle behavior. See the neighboring test configuration for
the provider source requirement.

## Known Limits

No DTLS, TLS 1.3, server, ALPN selection, session resumption, renegotiation,
encrypted private-key passwords, hardware/nonexportable key adapter, CRL/OCSP,
certificate enrollment or network reconnection is provided. PEM lengths include
the final NUL; DER certificate blobs hold one certificate. Software key bytes
are borrowed from the credential owner and parsed into native provider memory.
Secure storage and rotation policy remain explicit consumer/provider concerns.
Peer-provided intermediate path selection remains the Mbed TLS algorithm; this
adapter does not add general alternate-path search for ambiguous intermediate
chains. The active trust-anchor set is fixed at init, so crossing a validity
boundary during a handshake can require aborting and reinitializing to use an
anchor that has just become valid.

The adapter owns no task. A consumer integrating these handles must stop
borrowers before Flash/OTA operations that
invalidate their code, buffers or providers, account for ongoing synchronous
cryptography and watchdog windows, and decide whether to abort or resume the
connection after its bounded stop confirmation.
