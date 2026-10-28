<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Remote Manager

## Purpose

`esy_remote_manager` provides authorized configuration, OTA and boot-image control,
read-only statistics, structured diagnostics and coredump collection on an existing
task or main-loop owner.
Its normalized operations and typed query results are independent of message
encoding, transport, configuration storage, UART, Flash and the OTA engine.
Capability discovery reports only operations supplied by the owner.

The optional ProtoBin payload adapter handles request/response encoding. Four
separately selected provider adapters reuse [Settings](../settings/README.md),
the retained-RAM [Coredump store](../coredump/README.md), the typed
[OTA core](../ota/README.md) and shared [diagnostic sources](../diagnostics/README.md).
Applications can instead supply their
own operations. Provider initialization, product policy and resource lifecycles
remain with their existing owners.
An included RAM statistics table binds caller-owned descriptors and live samples
without selecting a clock, collection task or persistence backend.

## Public Surface

| Header | Public contract |
| --- | --- |
| `esy_remote_manager.h` | Caller-owned handle, normalized request/result, required authorization, init/deinit and synchronous dispatch. |
| `esy_remote_manager_config.h` | GET, SET, REMOVE, APPLY and SAVE callbacks with bounded keys and opaque values. |
| `esy_remote_manager_ota.h` | Image upload, slot enumeration, exact-image confirmation and rollback; normalized provider callbacks without an OTA-engine or bootloader dependency. |
| `esy_remote_manager_coredump.h` | Typed INFO, bounded READ and optional authoritative CLEAR; no capture/storage type. |
| `esy_remote_manager_stats.h` | Typed ENUMERATE/READ with exact unsigned 64-bit measurements, units, availability and measurement epochs. |
| `esy_remote_manager_stats_table.h` | Caller-owned RAM table provider; immutable metric descriptors borrow coherently updated live samples. |
| `esy_remote_manager_diagnostics.h` | Metadata ENUMERATE, health READ and explicit RUN with shared typed diagnostic results; no diagnostic-core dependency. |
| `esy_remote_manager_diagnostics_adapter.h` | Optional borrowing of one initialized shared diagnostic core and its stable source inventory. |
| `esy_remote_manager_proto_bin.h` | Stateless versioned payload conversion. |
| `esy_remote_manager_settings.h` | Explicit Settings adaptation, including its public Settings type and key scratch contract. |
| `esy_remote_manager_ota_adapter.h` | Borrowed typed OTA core, owner INFO callback and optional independent boot-image provider. |
| `esy_remote_manager_coredump_store.h` | Explicit borrowing of an initialized retained-RAM Coredump store. |

| Group | Commands | Inputs and successful results |
| --- | --- | --- |
| System `0` | DISCOVER `1` | No inputs; returns the implemented capability mask. |
| Configuration `1` | GET `1`, SET `2`, REMOVE `3`, APPLY `4`, SAVE `5` | GET/SET/REMOVE use a nonempty key; SET also takes opaque value bytes. GET returns bytes plus `found`. APPLY/SAVE take no input. |
| OTA `2` | INFO `1`, BEGIN `2`, STATUS `3`, WRITE `4`, FINISH `5`, ABORT `6`, ACTIVATE `7` | BEGIN takes a manifest; WRITE takes session ID, offset and bytes; STATUS/FINISH/ABORT/ACTIVATE take a session ID. Every successful operation except INFO returns typed session status. |
| OTA `2` | SLOT `8`, CONFIRM `9`, ROLLBACK `10` | SLOT reads one enumeration index. CONFIRM/ROLLBACK take an exact logical slot/version/SHA-256 identity. All three return one typed slot snapshot. |
| Coredump `4` | INFO `1`, READ `2`, CLEAR `3` | INFO returns size, CRC and opaque 16-byte record ID; READ takes that ID plus offset/length; CLEAR takes that ID. |
| Statistics `5` | ENUMERATE `1`, READ `2` | ENUMERATE takes a zero-based metric index; READ uses an exact metric name as key. Both return a typed descriptor and current sample. |
| Diagnostics `6` | ENUMERATE `1`, READ `2`, RUN `3` | ENUMERATE takes an index and returns metadata only. READ/RUN take an exact canonical source name as key. Only RUN starts a self-test. |

The public capability enum defines the supported operation bits. Unsupported groups,
commands and NULL operations return `ESY_ERR_NOT_SUPPORTED`. Discovery does not
bypass per-request authorization. Core query metadata stays typed; a future JSON
adapter can encode the same results without changing provider operations.

## Integration

Set the parent and only the required children in the consumer's selected feature
file. This minimal selection uses normalized operations without wire adaptation:

```cmake
set(ENABLE_ESY_REMOTE_MANAGER ON)
set(ENABLE_ESY_REMOTE_MANAGER_PROTO_BIN OFF)
set(ENABLE_ESY_REMOTE_MANAGER_SETTINGS OFF)
set(ENABLE_ESY_REMOTE_MANAGER_COREDUMP OFF)
set(ENABLE_ESY_REMOTE_MANAGER_OTA OFF)
set(ENABLE_ESY_REMOTE_MANAGER_DIAGNOSTICS OFF)
```

| Gate / target | Additional dependencies and ownership |
| --- | --- |
| Parent / `esy_remote_manager` | Common types only. Supplies all normalized operation groups and the RAM statistics table; no metrics or other providers are selected automatically. |
| `ENABLE_ESY_REMOTE_MANAGER_PROTO_BIN` / `esy_remote_manager_proto_bin` | Parent only; no frame codec, channel/session, HAL, OS or Flash target. |
| `ENABLE_ESY_REMOTE_MANAGER_OTA` / `esy_remote_manager_ota` | Requires `ENABLE_ESY_OTA` and target `esy_ota_core`; no Flash/HAL/OS/wire dependency. Borrows one initialized typed OTA core and an owner INFO callback. Flash installation separately selects `ENABLE_ESY_OTA_FLASH` and its provider dependencies. |
| `ENABLE_ESY_REMOTE_MANAGER_SETTINGS` / `esy_remote_manager_settings` | Requires `ENABLE_ESY_SETTINGS` and its file abstraction. Borrows one initialized Settings instance and caller-owned key scratch; GET/SET/REMOVE and SAVE map to existing Settings APIs. APPLY is unsupported. |
| `ENABLE_ESY_REMOTE_MANAGER_COREDUMP` / `esy_remote_manager_coredump_store` | Requires `ENABLE_ESY_COREDUMP`; retained-RAM core only. No Flash or Logger export backend is selected. |
| `ENABLE_ESY_REMOTE_MANAGER_DIAGNOSTICS` / `esy_remote_manager_diagnostics` | Requires `ENABLE_ESY_DIAGNOSTICS` and `esy_diagnostics`. Borrows its initialized source inventory; no HAL, OS, task or wire dependency. |

Each child requires the parent. Provider feature inputs must be selected
explicitly; enabling an adapter does not silently enable its provider. Specific
provider headers expose their declared provider types; normalized headers remain
free of them.

Initialize providers and adapters on their existing serialized owner, obtain
copied operations with the adapter's `get_ops()` function, then put those ops in
manager configuration. Stop requests and detach the manager before adapter or
provider deinit. The Settings adapter's SAVE atomically replaces the Settings file through its
selected provider. RAM, transactional Flash and other complete-file backends
share the same Settings API; no Flash dependency is imposed by this adapter.
The manager imposes neither Flash persistence nor universal commit semantics.
Settings binds one READ/ATOMIC_REPLACE file and preserves its staged validation
boundary. The manager exposes CONFIG operations, without a raw-file route that
could overwrite the same snapshot. Coredump persistence can separately bind
`esy_coredump_file` to a RAM, raw Flash or transactional file provider. When it
is assembled, the owner's clear bridge calls `esy_coredump_file_clear()` with
the exact record ID, preserving the persistent-first/RAM-second checks. The
RAM-store management adapter itself still owns no file or persistence lifecycle.

The following complete example uses one RAM-only value. Its authorization hook
accepts an already trusted owner context; replace it with the product's actual
session and permission checks. Request fields do not authenticate the caller.

```c
#include <string.h>
#include "esy_middleware/remote_manager/esy_remote_manager.h"

typedef struct {
    uint8_t mode;
    bool trusted;
} example_config_t;

static esy_err_t authorize(void *context,
                           const esy_remote_manager_request_t *request) {
    example_config_t *config = context;
    (void)request;
    return config->trusted ? ESY_OK : ESY_ERR_STATE;
}

static bool is_mode(const char *key, size_t length) {
    return length == 4u && memcmp(key, "mode", 4u) == 0;
}

static esy_err_t get_mode(void *context, const char *key, size_t key_length,
                          uint8_t *output, size_t capacity, size_t *length,
                          bool *found) {
    example_config_t *config = context;
    *found = is_mode(key, key_length);
    *length = *found ? 1u : 0u;
    if (!*found) return ESY_OK;
    if (capacity < *length) return ESY_ERR_NO_MEM;
    output[0] = config->mode;
    return ESY_OK;
}

static esy_err_t set_mode(void *context, const char *key, size_t key_length,
                          const uint8_t *value, size_t length) {
    example_config_t *config = context;
    if (!is_mode(key, key_length) || length != 1u || value[0] > 2u) {
        return ESY_ERR_INVALID_ARG;
    }
    config->mode = value[0];
    return ESY_OK;
}

int main(void) {
    example_config_t state = {.mode = 0u, .trusted = true};
    esy_remote_manager_t manager = {0};
    esy_remote_manager_config_t config = {
        .max_key_length = 16u,
        .max_request_value_length = 1u, .max_response_value_length = 1u,
        .authorize = authorize,
        .authorize_context = &state,
        .config_ops = {.get = get_mode, .set = set_mode, .context = &state},
    };
    uint8_t value = 2u;
    esy_remote_manager_response_t response;
    esy_remote_manager_request_t request = {
        .request_id = 1u,
        .group = ESY_REMOTE_MANAGER_GROUP_CONFIG,
        .command = ESY_REMOTE_MANAGER_CONFIG_SET,
        .key = "mode", .key_length = 4u,
        .value = &value, .value_length = 1u,
    };
    if (esy_remote_manager_init(&manager, &config) != ESY_OK) return 1;
    if (esy_remote_manager_dispatch(&manager, &request, NULL, 0u,
                                    &response) != ESY_OK) return 2;
    request.command = ESY_REMOTE_MANAGER_CONFIG_GET;
    request.value = NULL;
    request.value_length = 0u;
    value = 0u;
    if (esy_remote_manager_dispatch(&manager, &request, &value, 1u,
                                    &response) != ESY_OK) return 3;
    if (!response.found || response.value_length != 1u || value != 2u) return 4;
    return esy_remote_manager_deinit(&manager) == ESY_OK ? 0 : 5;
}
```

Transport owners pass only complete validated, nonduplicate DATA payloads to
`esy_remote_manager_proto_bin_process()`, with disjoint input/output storage.
Reserve output and downstream delivery capacity before admitting a mutation.
Submit the response through the existing session; this module does not send,
poll, retry or acknowledge it. A transport ACK proves reception; the encoded
operation status reports command outcome.

## Contracts

### Execution, configuration and provider results

- Initialize manager and provider-adapter handles with `{0}`. Repeated init
  returns STATE and preserves live bindings. Successful deinit detaches borrowed
  state without applying, saving or clearing provider data. Stop every copied
  ops borrower before deinitializing its adapter or storage.
- One task or main-loop owner serializes dispatch and provider access. APIs are
  not ISR/fault-safe. Each dispatch performs at most one authorization and one
  operation callback synchronously. Callbacks contribute their complete cost;
  there is no nonblocking or hard-real-time guarantee. Reentry to manager
  lifecycle/dispatch returns BUSY; this guard is not a concurrency lock.
- The required authorization hook admits each structurally valid request,
  including discovery. Products supply authenticated context and permissions
  separately from untrusted request data. No authentication or encryption is
  implemented here. Unsupported operations remain unsupported after admission.
- Configuration keys are nonempty explicit spans without embedded NUL. Values
  are opaque; missing and empty remain distinct. The provider owns defaults,
  schema, validation and the logical view returned by GET. SET/REMOVE acceptance
  does not imply application or persistence. APPLY and SAVE are separate optional
  operations; neither implies a multi-key transaction or a rollback guarantee.
- GET returns actual bytes on OK and required output
  capacity on NO_MEM. Providers respect `max_response_value_length`. Output is
  authoritative only on OK; failed operations carry no output data. Invalid
  provider lengths or inconsistent query metadata return STATE.
- Coredump INFO/READ never clear a record. The store adapter requires an already
  initialized store; public STATE cannot distinguish an empty store from an
  invalid handle, so its initialization is an owner precondition. CLEAR is
  installed only when the owner explicitly supplies authoritative discard,
  including persistent sources in the required order. No RAM-only clear is
  silently substituted for persistent deletion.
  READ/CLEAR require the nonzero 16-byte ID returned by INFO, including for a
  zero-length read. Providers compare the whole ID with the authoritative
  record before copying output or changing storage. A stale ID returns STATE
  without accessing another occurrence. The RAM adapter checks the current ID
  before invoking owner discard; the owner also checks persistent authority
  before erasing it. A failed persistent comparison leaves both records intact.
  Size, CRC, fault bytes and request ID do not identify an occurrence. The ID
  is stored with the Coredump and survives restore/export/firmware changes;
  another capture receives a new ID even when its fault data is identical.
  The normal-context Coredump owner provisions its allocator. Random 128-bit
  IDs offer probabilistic uniqueness; deterministic uniqueness across power
  loss requires an owner-managed durable source. No hardware source, persistence
  partition, clock or randomness callback is selected by the manager.
- Failed mutations or failed response delivery may leave effects in place.
  Neither manager nor adapter replays commands. Request IDs correlate results;
  they do not provide durable deduplication or exactly-once execution. Reconcile
  uncertain state before retrying. Cross-task work needs an explicit narrow
  request/completion contract and must not directly execute private owner code.

### Image transactions and session safety

BEGIN admits a manifest and synchronously prepares an upload. The normalized
manifest contains version, image_size, image_sha256[32] and signature[64]; its
meaning does not depend on ProtoBin, JSON or a particular storage provider.
WRITE accepts a bounded contiguous block. FINISH flushes and verifies the image
and enters READY; it does not publish boot selection or reboot. ACTIVATE is a
separate operation. The owner decides whether/when a confirmed ACTIVATE response
requires reboot. ABORT performs checked cleanup; failed cleanup retains resources
for retry and does not imply rollback of persistent effects.

Every successful BEGIN/WRITE/FINISH/STATUS/ABORT/ACTIVATE returns a copied
`esy_image_status_t`: phase, session_id, next_offset, total_bytes, result and
result_valid. Phases are IDLE=0, RECEIVING=1, READY=2, ACTIVATED=3, ABORTED=4,
FAILED=5. The session ID is nonzero after admission. STATUS ID zero asks for the
current snapshot; a nonzero ID must match. Other session commands require nonzero
IDs. next_offset reports accepted bytes, which may still be cached until FINISH.
ACTIVATED, ABORTED and FAILED have terminal results.

The optional OTA adapter delegates to the real typed OTA core. Exact repeated
BEGIN and the immediately preceding identical WRITE use that core's repeat
contract; there is no request-ID replay cache. After uncertain delivery, query
STATUS and reconcile the session/offset before retrying. An error can follow
changed storage; error responses contain no status bytes. ID freshness across
connections, disconnect abort/cleanup, update-version policy, authentication,
signature validation, Flash coordination, task behavior, watchdog and recovery
remain with the consumer. An opaque signature is not proof of authentication.

The OTA adapter owns neither core init/deinit nor its provider lifetime. Keep the
initialized core and owner INFO callback alive until manager and adapter detach.
All operations run on the existing serialized owner. INFO returns limits that
match the core and framing budgets. In particular, maximum WRITE data is at most
`max_request_value_length - 8`, and must fit the channel after its 18-byte request
header. A full BEGIN needs 104 argument bytes plus the 18-byte header. The core's
last-chunk storage and optional progress timeout belong to the OTA owner.

### Slot enumeration, confirmation and rollback

The optional `ota_ops.images` callbacks implement boot-image administration
independently of an upload session. They have their own context, so a caller can
provide only these callbacks without assembling an OTA engine, Flash adapter or
bootloader target. The existing OTA adapter also copies optional `config.images`
callbacks into its returned operations. No provider is silently selected and no
native boot operation is inferred from a function pointer being present.

The optional `images` member precedes the upload `context`, which is the final
member of `esy_remote_manager_ota_ops_t`. Use C11 designated fields or zero
initialization followed by named member assignments. Upload-only providers keep
`images` zero, leaving image administration disabled.

SLOT reads one zero-based `slot_index` and returns `slot_count`, `index`, an image
identity (`slot`, `version`, `sha256`), `capacity`, `image_size` and `flags`.
Logical slot identifiers need not equal indices, and the manager imposes no
two-slot limit. Iterate from index zero through `slot_count - 1`. A provider with
no slots returns an entirely zero snapshot for index zero; any other out-of-range
index returns INVALID_ARG. This is a live enumeration, without a retained cursor
or inventory lock. Serialize topology changes, or restart if count/order changes.
An empty slot still reports its identifier and positive capacity, with zero
image version, digest, size and flags.

| Flag | Value | Meaning |
| --- | --- | --- |
| PRESENT | `1` | The slot contains the identified image. |
| RUNNING | `2` | The image is currently executing. |
| PENDING | `4` | The boot owner durably selected this image for next boot. |
| CONFIRMED | `8` | The boot owner accepted this image. |
| BOOTABLE | `16` | The provider validated integrity, authentication and boot eligibility. |

Except for PRESENT itself, these properties require PRESENT. Readable metadata
alone does not establish BOOTABLE. The manager checks sizes, known flags,
enumeration positions and successful mutation identities; it cannot validate
image bytes or native boot state itself.

CONFIRM takes the exact running image identity. The provider must compare all
three identity fields before writing, reject stale identities, apply the owner's
health/permission policy, and complete both metadata and native confirmation.
Success returns the identified RUNNING and CONFIRMED image. Retrying that same
confirmation is idempotent, including recovery of an incomplete native confirm.
No upload session ID, implicit health check or manager-owned boot confirmation
is involved.

ROLLBACK takes the exact nonrunning fallback identity. The provider verifies
eligibility under current boot and trust policy, then durably selects it without
resetting. Success returns the identified BOOTABLE and PENDING image. Repeating
the same committed selection while the target remains nonrunning is idempotent;
after reboot, rollback to the running image is rejected. The selected image must remain the
accepted target after later confirmation and reboots; merely writing TRY_BOOT
and forwarding to upload ACTIVATE can violate this requirement. Conflicting
uploads, protected trials and pending boot changes must be rejected before
destructive work. Downgrade permission remains an owner decision. The manager
neither erases images nor implicitly aborts uploads or confirms the source.

All boot-state writers and uploads share external serialization. Their provider
must also enforce state safety across requests and outside management dispatch;
the manager's synchronous busy guard supplies neither an upload lock nor a
persistent transaction. A failed mutation may have durable partial effects.
Reconcile authoritative provider/native state before retrying or issuing another
mutation, even if a slot query appears unchanged. The caller reserves response
delivery and decides when to reboot after a successful rollback response.

This module supplies the complete normalized and ProtoBin management path, not
a built-in bootloader rollback implementation. The current bootloader has a
running-image confirmation API but lacks a portable durable rollback operation;
its existing upload activation API is not a substitute. Install image callbacks
only when the selected boot provider can meet the above contract. Unsupported
callbacks remain NULL and their discovery bits stay clear.

### Read-only RAM statistics

Statistics queries discover one measurement at a time with ENUMERATE, or read a
known metric using READ and its exact name. Names contain 1..48 ASCII letters,
digits, `_`, `.`, `-` or `/`; matching is case sensitive. Each result contains
the catalogue count, enumeration index, copied name, counter/gauge kind, unit,
availability, unsigned 64-bit value and unsigned 64-bit epoch. An empty catalogue
returns a zero snapshot only for ENUMERATE index zero. READ of an unpublished
name returns NOT_SUPPORTED; a temporarily unavailable measurement succeeds with
`available = false` and value zero. An available zero is a distinct valid sample.

The included `esy_remote_manager_stats_table` provider borrows an immutable
array of `esy_remote_manager_stats_entry_t` descriptors and their RAM sample
pointers. Initialize its zeroed caller-owned handle, obtain operations with
`esy_remote_manager_stats_table_get_ops()`, and assign them to `config.stats_ops`.
The existing owner updates samples; each query copies the current RAM value.
The table does not increment, reset or clear counters, retain history, allocate
storage, start a task or timer, or touch Flash. It is part of the core target
and needs no child feature switch. Applications can instead provide normalized
statistics callbacks directly.

No metric names or measurements are installed automatically. A caller can
publish the following catalogue, using its existing producers:

| Example name | Kind | Unit and producer meaning |
| --- | --- | --- |
| `system.uptime_ms` | Counter | Milliseconds from an owner-maintained monotonic uptime source. |
| `system.reboot_count` | Counter | Count within the explicitly documented RAM/retention lifetime; unavailable when no such source exists. |
| `comm.rx_bytes`, `comm.tx_bytes` | Counter | Bytes accepted by the specified channel layer. |
| `comm.timeouts` | Counter | Timeout events observed by the communication owner. |
| `ota.success_count`, `ota.failure_count` | Counter | Results recorded at the owner's documented OTA completion boundary. |
| `ota.last_duration_ms` | Gauge | Last completed operation duration in milliseconds; unavailable before a result exists. |
| `resource.stack_min_words` | Gauge | Minimum free stack words observed by the stack owner. |
| `resource.queue_peak` | Gauge | Peak queue occupancy in objects. |
| `resource.buffer_overflows` | Counter | Overflow events reported by the buffer owner. |

Kinds are COUNTER `1` and GAUGE `2`. Units are NONE `0`, COUNT `1`, BYTES `2`,
MILLISECONDS `3`, WORDS `4` and whole PERCENT `5`; available percentages must be
0..100. The RAM table returns STATE for an invalid available percentage when
queried. A watermark is an owner-maintained gauge, not a value computed by the
manager. This unsigned integer model does not represent signed or fractional
measurements. A future JSON adapter must preserve integer precision beyond
2^53 instead of converting values through a floating-point number.

A nonzero epoch identifies one measurement lifetime. Change it when a counter
resets, wraps or its producer restarts; clients compare counter differences
only within the same nonzero epoch. Epoch zero means continuity is unspecified.
An identifier held only in ordinary RAM is not guaranteed unique across reboots.
Saturate a counter at UINT64_MAX or start a new epoch instead of silently
wrapping. RAM-only values do not imply persistent reboot or lifetime totals.
The module creates no persistent partition and performs no persistence writes.

One task/main-loop owner serializes all queries with complete sample updates.
Asynchronous producers must first hand off coherent samples to that owner;
64-bit values are not assumed atomic on 32-bit MCUs. The table supplies no lock
or ISR entry point. Enumeration is live rather than a cross-metric snapshot;
keep descriptor order stable until every borrower detaches. Initialization
validates descriptors and duplicate names in O(count squared), enumeration is
O(1), and exact-name READ searches in O(count). These costs belong to the caller.
No polling cadence or new timebase is introduced. Uptime extension, clock-stop
semantics and observation cadence belong to its producer; simply widening a
wrapping 32-bit millisecond tick does not produce valid 64-bit uptime.

### Structured diagnostics

Diagnostics group 6 discovers stable named sources independently of the wire
format and provider implementation. ENUMERATE copies one descriptor without
querying health or starting a test. READ obtains current health without a
self-test. RUN explicitly executes one synchronous self-test. Each request is
authorized before its single provider operation; there are no implicit retries,
automatic tests, asynchronous jobs, history, clocks or storage writes.

Names contain 1..48 ASCII letters/digits, `_`, `.`, `-` or `/`, and READ/RUN
match the complete name. Snapshots carry count/index, source capabilities,
health, source-specific reason code and optional explanatory text. Detail is
0..96 printable ASCII bytes. Health values are UNKNOWN=0, OK=1, WARNING=2 and
ERROR=3; code zero means no additional reason. A successfully completed test
that detects a fault returns operation OK with ERROR health. A negative
operation status means no authoritative assessment is published, although RUN
may already have effects. Reconcile with READ or owner state before a retry.
UNKNOWN is allowed for READ and metadata-only ENUMERATE, but not completed RUN.

Every source advertises READ and may additionally advertise RUN. Unknown source
capability bits, invalid strings/results and positive provider error codes become
STATE. ENUMERATE's result is entirely zero; an empty inventory is an entirely
zero snapshot available only at index zero. No source's test is inferred from
its name, a statistics value or a Coredump record.

The optional diagnostic adapter borrows one initialized `esy_diagnostics_t`.
Source registration, lifetime and test policy remain with the owner. Init probes
metadata only, deinit never changes the core, and active operations reject
adapter lifecycle/get-ops/reentry with BUSY. Keep the core, immutable entries,
names and contexts alive until all copied operations stop. The adapter exposes
the RUN route globally; per-source capability masks decide which names can run.

The owner supplies authentication, hardware-test admission and OTA/cross-task
policy. Sources must use owner-approved snapshots or narrow requests, with
explicit execution budgets and output/Flash coordination.

### ProtoBin payload version 1

The outer frame remains ProtoBin DATA. All multibyte fields are little-endian;
no native C structure is serialized. Domain `0x02` is
`ESY_PROTO_DOMAIN_REMOTE_MANAGER`. Payload version and request ID are independent
of the outer frame sequence.

| Request offset | Bytes | Field |
| --- | --- | --- |
| 0 | 1 | Domain `0x02` |
| 1 | 1 | Request kind `1` |
| 2 | 1 | Version `1` |
| 3 | 1 | Reserved zero |
| 4 | 2 | Management group |
| 6 | 2 | Group-local command |
| 8 | 4 | Request ID |
| 12 | 2 | Key length |
| 14 | 4 | Value length |
| 18 | variable | Key bytes followed by value bytes |

Payload size must match exactly. Truncation, trailing bytes, another domain or
version, invalid kind and nonzero reserved bytes are rejected without invoking
an operation. COREDUMP READ uses no key and exactly 24 value bytes:
`record_id[16], offset:u32, length:u32`. CLEAR takes exactly `record_id[16]`
without a key. IDs are copied as opaque bytes with no integer byte-order
conversion. INFO has no arguments. The adapter converts these into normalized
request fields. No OTA command uses a key. Statistics ENUMERATE takes no key and exactly four value bytes for
`index:u32`; READ takes the metric name as key and no value. Both convert into
normalized fields before dispatch.
Diagnostics ENUMERATE likewise takes exactly four value bytes for its index,
without a key. Diagnostic READ/RUN take the source name as key and no value.
All unused normalized fields, including diagnostic_index outside ENUMERATE,
must be zero.

| OTA command | Exact request value layout |
| --- | --- |
| INFO `1` | Empty. |
| BEGIN `2` | 104 bytes: version:u32, image_size:u32, SHA-256[32], signature[64]. |
| STATUS `3` | 4 bytes: session_id:u32; zero selects current snapshot. |
| WRITE `4` | session_id:u32, offset:u32, then one or more image bytes. |
| FINISH `5`, ABORT `6`, ACTIVATE `7` | 4 bytes: nonzero session_id:u32. |
| SLOT `8` | 4 bytes: slot_index:u32. |
| CONFIRM `9`, ROLLBACK `10` | 40 bytes: slot:u32, version:u32, SHA-256[32]. |

| Response offset | Bytes | Field |
| --- | --- | --- |
| 0 | 1 | Domain `0x02` |
| 1 | 1 | Response kind `2` |
| 2 | 1 | Version `1` |
| 3 | 1 | Reserved zero |
| 4 | 2 | Echoed group |
| 6 | 2 | Echoed command |
| 8 | 4 | Echoed request ID |
| 12 | 4 | Signed operation status, two's complement |
| 16 | 4 | Capability mask; successful DISCOVER only |
| 20 | 1 | CONFIG GET found flag, zero or one |
| 21 | 4 | Actual appended bytes on OK, required capacity on NO_MEM |
| 25 | variable | Successful output or serialized query metadata |

| Successful output | Appended byte layout |
| --- | --- |
| CONFIG GET / COREDUMP READ | Provider output bytes. |
| OTA INFO | 16 bytes: current_version:u32, running_slot:i32, max_image_bytes:u32, max_chunk_bytes:u32. |
| OTA BEGIN / STATUS / WRITE / FINISH / ABORT / ACTIVATE | 24 bytes: phase:u32, session_id:u32, next_offset:u32, total_bytes:u32, result:i32, result_valid:u32. |
| OTA SLOT / CONFIRM / ROLLBACK | 60 bytes: slot_count:u32, index:u32, slot:u32, version:u32, capacity:u32, image_size:u32, flags:u32, SHA-256[32]. |
| COREDUMP INFO | 28 bytes: available:u32, record_size:u32, record_crc:u32, record_id[16]. |
| Statistics ENUMERATE / READ | 88 bytes: count:u32, index:u32, kind:u32, unit:u32, available:u32, name_length:u32, epoch:u64, value:u64, name[48]. |
| Diagnostics ENUMERATE / READ / RUN | 172 bytes: count:u32, index:u32, capabilities:u32, health:u32, code:u32, name_length:u32, detail_length:u32, name[48], detail[96]. |

Statistics names occupy exactly 48 wire bytes, padded with zeros after the
reported length, without a serialized C terminator. Epoch and value use exact
little-endian 64-bit integers. A zero-catalogue result is entirely zero.
Diagnostic scalars occupy offsets 0, 4, 8, 12, 16, 20 and 24 of its value.
Name starts at offset 28 and detail at 76; both fixed spans are zero padded
after their declared lengths, without serialized C terminators. ENUMERATE
includes no assessment; READ/RUN return the shared diagnostic finding.

Boolean metadata fields are encoded as 32-bit zero or one; CONFIG found remains
one byte. OTA phase values match the public enum. The owner defines OTA result
codes, which are authoritative only when result_valid is one. Response status
remains the manager operation's success/error code.

The payload function returns OK when a complete response is encoded, including
business errors. Envelope/argument errors produce no response and zero output
length for valid disjoint output-length storage. Invalid overlapping arguments
leave that output unchanged. At least 25 response bytes must be available before
any operation executes. A known typed operation additionally reserves its full
success value before authorization/provider execution: INFO needs 41 output
bytes, upload mutations/STATUS 49, SLOT/CONFIRM/ROLLBACK 85, Coredump INFO 53, and
statistics ENUMERATE/READ 113, and diagnostics ENUMERATE/READ/RUN 197.
Insufficient capacity
encodes a 25-byte NO_MEM response and required value length without executing the
operation. The binary adapter checks its metadata against `max_response_value_length` at
that same boundary. In normalized core dispatch this budget caps only opaque
output value buffers; typed snapshots need no output buffer and have no encoding
size requirement. Another codec budgets its own encoding independently. Request
argument budgets use `max_request_value_length`: manifest content is 104 bytes,
WRITE is eight scalar argument bytes plus data, session arguments and SLOT index
are four bytes, and CONFIRM/ROLLBACK identities are 40 bytes. Statistics
ENUMERATE uses four argument bytes; READ uses only its bounded name key.
Diagnostic commands likewise charge four ENUMERATE bytes or only the READ/RUN
name key; typed results require no opaque output buffer in normalized dispatch.
The binary adapter's fixed 172-byte diagnostic response must fit the channel
and configured response-value budget before authorization or a self-test runs.
These are decoded semantic content limits, not a JSON document-size
limit, and exclude the envelope. Data providers receive only the
remaining bounded output capacity. NO_MEM encodes the required length without appending data. No error
response includes partial provider output, and lengths are never truncated to
fit 32 bits. Consult the adapter header for exact aliasing/range preconditions.

COREDUMP READ counts 24 argument bytes and CLEAR counts 16; zero IDs are rejected
before authorization. Unavailable INFO returns zero size, CRC and ID. READ and
CLEAR have no implicit current-record selection, and errors append no bytes.

## Resource Model

The manager, provider state, adapters, requests and output buffers are caller
owned. Init copies descriptors, while callbacks, contexts and backing storage
remain borrowed until all users detach. No request or output pointer survives
dispatch. There is no core pool, hidden singleton, task, timer, IRQ, DMA or
watchdog participant.

Settings adaptation borrows one store and a bounded key conversion buffer.
Command adaptation borrows a synchronous handler, a line buffer including its
NUL and an aligned argv array declared/bound with its aggregate storage macros.
Coredump adaptation borrows an initialized store and optional owner clear
callback. Adapters neither initialize nor destroy their providers. Flash
serialization, failure reconciliation and any provider allocations/blocking
remain governed by the selected provider's contract.
The RAM statistics table borrows its immutable descriptor array, names and
owner-updated samples until manager detach followed by table deinit; deinit
does not clear or persist measurements. No counter storage is owned internally.

Per-request work is bounded by key/value/output limits and callback cost. The
ProtoBin adapter writes directly into caller output, adding 25 response bytes.
Metadata requires its documented extra capacity; the actual channel payload
limit also applies. There are no management deadlines, automatic retries or
polling requirements. OTA upload progress and timeouts belong to the typed OTA owner; this module
creates no independent timebase.

## Verification

From the platform root, run the module tests, real-target consumer checks and
allocation scan:

```sh
cd esy_middleware/remote_manager/tests
ceedling test:all
```

Separately, from the platform root:

```sh
python3 -B esy_middleware/remote_manager/tests/test_cmake_consumer.py
tools/checks/check_no_dynamic_alloc.sh esy_middleware/remote_manager
```

Focused tests cover all operation groups, authorization, capability discovery,
provider errors, lifecycle/reentry, bounded binary data, typed metadata and golden
wire fixtures. Statistics tests cover 64-bit values beyond 2^53 and through
UINT64_MAX, availability versus measured zero, epochs, name/descriptor validation,
empty catalogues, live RAM updates, exact-name reads and fixed wire padding.
Provider suites exercise actual Settings persistence, Coredump
range reads/explicit conditional clear, stale A-to-B record requests, exact-ID
wire fixtures and a real typed OTA core over a memory provider. Mutating wire tests verify response preflight
before side effects, exact manifest/chunk/identity layouts and version rejection.
Image tests cover noncontiguous logical slot IDs, empty inventories/slots, exact
identity checks, malformed provider snapshots, unsupported callbacks and provider
errors. They validate the management contract rather than a physical bootloader
rollback. Consumer
checks compile C11/C++17 public headers, exercise current aggregate initializers
and independent upload/image contexts in both languages, and link the real core, payload and
provider targets; pure Settings/Coredump consumers use the RAM file provider
without a Flash HAL. Coredump collection and explicit clearing exercise the real
file persistence adapter. They verify feature
on/off, illegal children, missing providers, cache precedence and reconfiguration,
and execute the unique C example above. A fixed JSON DTO fixture supplies the
same normalized BEGIN manifest and image-administration requests without a JSON
library or binary codec target. Pure core/payload consumers have
no HAL, OS, Flash, session or codec target. C11/C++17 consumers also execute the
real RAM statistics table for system, communication, OTA and resource metrics
with zero opaque output budget and no provider tasks or persistence target.

The quality gate discovers the manager Ceedling project and runs its tests
under sanitizers. Target integration procedures exercise native timing, Flash
behavior, OTA interruption/recovery, actual transport delivery and task
coordination beyond the scope of host fake/consumer evidence.

## Known Limits

- Supported groups are configuration, complete image upload/control, slot
  enumeration, image confirmation/rollback and Coredump
  collection, read-only RAM statistics and structured diagnostics. Generic files and a production
  JSON codec are not implemented or advertised.
- Diagnostics require explicitly registered sources and owner-approved tests;
  no automatic system instrumentation, asynchronous test job or diagnostic
  history is supplied. Source callbacks may block and RUN errors may follow effects.
- Statistics expose explicitly supplied samples; automatic instrumentation,
  durable totals, counter reset commands, history and signed/fractional values
  are not supplied.
- Image administration requires an explicitly supplied boot provider; no native
  bootloader rollback implementation or automatic reset is supplied here.
- All provider operations are synchronous. There is no generic queue, scheduler,
  durable request replay, transaction engine, schema registry or universal store.
- Authentication, signature admission, transport framing, duplicate suppression,
  session boundaries, response confirmation and reset remain with the owner.
- Provider blocking/allocation and target Flash safety depend on the selected
  provider, memory budget and hardware; the manager cannot supply those guarantees.
