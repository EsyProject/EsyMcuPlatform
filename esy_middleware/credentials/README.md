<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Credentials

## Purpose

`esy_credentials` holds one complete borrowed CA/client-certificate/private-key
bundle. Explicit consumer leases keep that bundle stable while a TLS adapter or
another consumer uses its bytes. Replacement publishes a new complete bundle
only after every consumer has released its lease.

The optional `esy_credentials_store` target keeps complete confirmed and candidate
bundles in one atomic file snapshot. It remains inside this module; consumers
using built-in or externally managed credentials need only the memory target.

## Public Surface

Include `esy_middleware/credentials/esy_credentials.h` and link `esy_credentials`.
`esy_credentials_blob_t` describes immutable DER or NUL-terminated PEM bytes; each
blob has its own encoding. `esy_credentials_config_t` groups required trust anchors
and an optional client certificate/private key pair.

Include `esy_middleware/credentials/esy_credentials_store.h` and link
`esy_credentials_store` / `Esy::credentials_store` for optional persistence.
That adapter publicly depends on the memory target and File, without a native
crypto, HAL or OS dependency. Common utilities supply its private CRC32 dependency.

| API | Behavior |
| --- | --- |
| `esy_credentials_init` | Validate and borrow a complete configuration. |
| `esy_credentials_acquire` | Copy descriptors and hold a rotation lease. |
| `esy_credentials_release` | End one lease and invalidate its snapshot. |
| `esy_credentials_replace` | Replace the bundle when no lease exists. |
| `esy_credentials_deinit` | End the borrow when no lease exists. |

| Optional store API | Behavior |
| --- | --- |
| `esy_credentials_store_init` | Bind a file/workspace and load an existing snapshot or confirmed absence. |
| `esy_credentials_store_get_info` | Query cached transaction, bundle IDs, sizes and READY/UNCERTAIN state. |
| `esy_credentials_store_load` | Copy confirmed or candidate data to independent consumer RAM. |
| `esy_credentials_store_stage` | Persist a complete candidate alongside the confirmed bundle. |
| `esy_credentials_store_confirm` | Promote exactly the identified candidate. |
| `esy_credentials_store_discard` | Remove exactly the identified candidate, preserving confirmed and ID history. |
| `esy_credentials_store_reload` | Reconcile authoritative file contents after an uncertain mutation. |
| `esy_credentials_store_deinit` | Clear borrowed work buffers and detach without changing the file. |

## Integration

Select `ENABLE_ESY_CREDENTIALS` in the consumer's feature import. The target
depends publicly only on `esy_common_defs`; it requires no HAL or OS target.

For persistence, select all three gates using ordinary sets in the same feature
file:

```cmake
set(ENABLE_ESY_CREDENTIALS ON)
set(ENABLE_ESY_FILE ON)
set(ENABLE_ESY_CREDENTIALS_STORE ON)
```

The owner supplies an initialized File with READ and ATOMIC_REPLACE capabilities.
A File Dual Flash Store binding additionally selects `ENABLE_ESY_DUAL_FLASH_STORE` and
`ENABLE_ESY_FILE_DUAL_FLASH_STORE`, and the consumer reserves the Flash regions and
establishes their safe execution policy. Other atomic file providers can be used
without adding Flash to this module.

Store initialization and a confirmed-bundle load use caller-owned RAM:

```c
/* Capacities are selected by the consumer's credential and file budgets. */
uint8_t working[SNAPSHOT_CAPACITY], scratch[SNAPSHOT_CAPACITY];
uint8_t loaded[BUNDLE_CAPACITY];
esy_credentials_store_t store = {0};
esy_credentials_store_static_resources_t memory = {
    working, scratch, sizeof(working)
};
esy_credentials_store_config_t store_config = {&file, &memory};
esy_credentials_store_load_resources_t destination = {loaded, sizeof(loaded)};
esy_credentials_config_t confirmed;
uint64_t confirmed_id;
esy_err_t result = esy_credentials_store_init(&store, &store_config);
if (result == ESY_OK) {
    result = esy_credentials_store_load(&store, ESY_CREDENTIALS_STORE_CONFIRMED,
                                        &destination, &confirmed, &confirmed_id);
    if (result == ESY_OK) {
        /* Borrow confirmed with esy_credentials_init(); loaded remains alive. */
    }
}
```

An absent confirmed bundle returns STATE rather than silently choosing a staged
candidate. First provisioning uses stage followed by explicit confirm. The file
handle in this fragment is already initialized by the consumer. No storage path,
Flash address or partition size is selected by Credentials.

```c
esy_credentials_t credentials = {0};
esy_credentials_lease_t lease = {0};
esy_credentials_config_t bundle = {0};
bundle.trust_anchors.data = ca_pem;
bundle.trust_anchors.size = sizeof(ca_pem); /* Includes the final NUL. */
bundle.trust_anchors.format = ESY_CREDENTIALS_FORMAT_PEM;
esy_err_t result = esy_credentials_init(&credentials, &bundle);
if (result == ESY_OK) {
    esy_credentials_config_t snapshot;
    result = esy_credentials_acquire(&credentials, &lease, &snapshot);
    if (result == ESY_OK) {
        /* Parse/use snapshot bytes, then stop every user before releasing. */
        result = esy_credentials_release(&lease);
    }
    if (result == ESY_OK) {
        result = esy_credentials_deinit(&credentials);
    }
}
```

A TLS adapter holds its lease from initialization until provider resources have
been destroyed during deinitialization. To rotate, stop/deinitialize every TLS
consumer, replace the bundle, and initialize consumers again. A failed consumer
initialization must release any acquired lease after cleaning its provider state.

Persistent rotation follows the same explicit owner model. Authorize and validate
the complete incoming bundle, stage it alongside the confirmed version, stop the
old consumer and release its leases, then try the candidate. Confirm it only after
the product's required TLS/application checks succeed, or discard it and restore
the confirmed version. The store never starts/stops TLS, invokes application
business callbacks or decides whether a remote connection is healthy. Boot-time
recovery uses the confirmed version; a staged candidate requires an explicit new
trial decision.

## Contracts

One non-ISR startup/main-loop/task owner serializes all calls and use of borrowed
bytes. Replacement is atomic under that ownership contract, not concurrent or
persistent synchronization. There is no scheduler, timeout, clock or background
operation. No task/ISR or Flash resource is created, so module-internal OTA
quiescence is inapplicable; consumers retain their TLS/storage/OTA coordination.

Handles and leases require `{0}` before first use. Do not move, copy, modify or
clear live objects. Initializing a nonzero handle returns STATE. A copied active
handle or lease is rejected. Zero deinit/release is idempotent. Failure preserves
state and outputs. Any active lease makes replacement/deinit return BUSY; success
restores zero state or publishes the new complete bundle. Failed replacement
retains the old bundle. Snapshots stop being usable when their lease is released.

The memory target copies descriptors only. Every borrowed byte remains alive and
immutable until successful replacement/deinit, including intervals with no
leases. The original configuration descriptor may go out of scope after init or
replace. Old bytes may be reclaimed after successful replacement unless also
referenced by the new bundle. Inputs/outputs, handles, leases and borrowed bytes
must satisfy the disjointness rules in the public header.

Trust anchors must be nonempty. Client certificate and private key are either
both absent (`NULL`, zero size) or both present. DER counts binary bytes. PEM
counts one final NUL and rejects empty text or embedded NULs. Encoding values
must be valid even for absent optional blobs. Parsing, certificate/key matching,
chain acceptance, hostname/expiry validation and policy belong to the consumer's
cryptographic provider; successful initialization asserts none of those checks.

### Optional persistent snapshots

The file contains the whole confirmed bundle, optional candidate bundle and
transaction metadata. Every update replaces that complete snapshot atomically.
Dual Flash Store's two physical regions provide redundant transaction commits; they
are not two independently writable credential slots. Each region must hold the
entire largest permitted confirmed-plus-candidate snapshot and its storage
envelope.

The owner serializes the store, file/provider, memory credentials and all other
users of the same Flash. Operations are synchronous and may block during erase,
program or readback; they create no task, timer, retry or timeout. Flash/OTA
arbitration, multicore execution safety, interrupt latency and watchdog service
remain consumer responsibilities. There is no module-owned task requiring an OTA
run policy, and no module timeout/counter-wrap behavior.

A failed mutation may already have committed. The store then rejects load and
further mutations until an explicit reload reconciles the authoritative snapshot
and its transaction/bundle identity. An I/O error or malformed existing File
payload is never treated as a factory-empty store. Load copies a selected bundle
into a separate caller output buffer; the owner must release every borrower before
loading into it again. Store reload/staging never repoints or rewrites an existing
Credentials handle.

Candidate IDs are issued by the store, start at one and are retained in the
snapshot after discard so they are not reused. Confirm/discard require the exact
nonzero candidate ID. Snapshot transaction IDs also increase without wrap;
exhaustion is rejected before writing. Staging reserves a transaction for its
later confirm/discard; it is rejected when the cached transaction ID is already
`UINT64_MAX-1` or `UINT64_MAX`.
On a live handle, reload rejects a disappearing snapshot or counters moving
backwards or one transaction ID naming different bytes. These consistency checks
do not protect against whole-media replay across reset.

### Snapshot format, version 1

All integers are unsigned little-endian values. The file is one 96-byte header,
concatenated blob payloads without padding, and a final four-byte CRC32:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | Magic `0x53524345` (`ECRS` in byte order). |
| 4 | 4 | Format version, currently 1. |
| 8 | 4 | Complete file length, including the CRC. |
| 12 | 4 | Reserved; must be zero. |
| 16 | 8 | Nonzero transaction ID. |
| 24 | 8 | Last issued bundle ID, retained after discard. |
| 32 | 8 | Confirmed ID, or zero when unprovisioned. |
| 40 | 8 | Candidate ID, or zero when absent. |
| 48 | 24 | Confirmed CA, certificate and private-key descriptors. |
| 72 | 24 | Candidate CA, certificate and private-key descriptors. |
| 96 | Variable | Confirmed CA/certificate/key bytes, then candidate CA/certificate/key bytes. |
| File length minus 4 | 4 | CRC32 of all preceding bytes. |

Each descriptor is an eight-byte pair: a uint32 byte count followed by a uint32
encoding (`DER=0`, `PEM=1`). PEM counts its final NUL. An absent slot has a zero ID
and three zero descriptors. Present slots require CA bytes and a paired client
certificate/key when client authentication is configured. The CRC uses reflected
polynomial `0xEDB88320`, initial value `0xFFFFFFFF` and final XOR `0xFFFFFFFF`.
There are no trailing bytes beyond the CRC. Unknown magic/version fails without
rewriting the file.

Persisted transaction and last-issued IDs are positive, with last-issued no
greater than transaction. Confirmed is zero or at most last-issued. A nonzero
candidate equals last-issued and is greater than confirmed. A snapshot with both
slots absent still retains its positive counters after discarding bootstrap data.

This encoding is the logical File payload. Direct factory programming through a
Dual Flash Store binding additionally requires that backend's physical transaction
envelope. Factory software can instead send approved bundles to firmware and
reuse stage/confirm, keeping physical commit handling in the storage backend.

## Resource Model

Callers own fixed-size opaque bundle and lease handles plus immutable credential
bytes. There is no allocation, singleton, native resource, callback, storage I/O
or erasure. Validation scans PEM bytes synchronously; other operations copy fixed
descriptors and update a bounded lease count. Operations are not ISR/fault safe.

The optional store additionally borrows a File and caller-owned work buffers.
Its file/provider and both work buffers must remain alive until successful store
deinit. Independent loaded copies need to remain alive until their own
Credentials/TLS borrowers have stopped; those copies can outlive the store.
The store owns no Flash initialization, socket, worker, heap or native crypto
resource.

Each of the two work buffers must hold the largest complete snapshot, including
both bundles, all metadata and its CRC. `get_info()` reports the lesser of file
capacity and the configured buffer capacity. A separate load buffer requires
the selected bundle's reported size and contains concatenated CA/certificate/key
bytes. Successful store deinit clears both work buffers; independent loaded
copies remain owned by the consumer. Clear those copies only after every
Credentials/TLS borrower has stopped.

## Verification

From the platform root, run the module suite with isolated artifacts:

```sh
ruby tools/quality/run_module_tests.rb esy_middleware/credentials/tests \
  /tmp/esy-credentials-tests gcov:all
ruby tools/quality/run_module_tests.rb esy_middleware/credentials/tests \
  /tmp/esy-credentials-sanitizer test:all --sanitizer
```

Public-API tests cover optional client pairs,
mixed encoding, malformed PEM descriptors, failed initialization/replacement,
multiple leases, lifetime guards, aliases, output preservation and reinitializing
after successful cleanup. From the module directory,
`python3 tests/test_cmake_consumer.py` checks C11/C++17 consumers, public dependency
propagation and disabled-feature isolation.

The same suite tests store snapshots through RAM File and the real File Record
Store adapter with reusable Flash fault injection. It covers staging, promotion,
discard, reopening, capacity/format errors, uncertain commits and active-memory
output preservation. CMake consumers separately select or disable the persistence gate
and verify C11/C++17 linking without HAL, OS or native crypto targets.

Host tests validate bundle ownership, not cryptographic correctness, provider
heap costs, certificate admission, physical storage security or hardware timing.

## Known Limits

- Software private key bytes remain readable/exportable. The built-in store
  format is not encrypted or authenticated; use an explicitly protected file
  provider when the product requires those guarantees. There is no secure
  element, non-exportable key ID or secure deletion implementation.
- Persistent transactions do not establish authorized updates, key generation,
  password decryption, certificate enrollment, revocation, anti-rollback or
  auto-renewal. Those remain explicit consumer/provider policies.
- CRC and ordinary transaction/bundle identifiers are corruption/lifecycle
  metadata, not authentication or trusted anti-rollback counters. Logical
  replacement/discard does not physically erase old private key copies.
- File-provider absence is authoritative. If a backend hides all invalid physical
  records by reporting an absent file, the store cannot distinguish that media
  condition from factory-empty storage. Dual Flash Store can ignore invalid records
  during recovery. First provisioning authorization and policies preventing
  unauthorized reprovisioning remain with the owner; this adapter never writes
  defaults after a reported read error or malformed existing File payload.
- Callers must not mutate borrowed bytes to bypass the lease/replace contract.
- Live memory-bundle replacement requires all leases to be released. Persistent
  staging/confirmation operates on separate snapshots and does not change an
  already loaded consumer copy. Activating a new identity and restarting its
  consumers remain explicit owner operations.
