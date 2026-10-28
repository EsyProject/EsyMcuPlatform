<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Bound File

## Purpose

`esy_file` exposes one prebound binary file through a common interface. A file
may live in RAM, one exclusive Flash region, the transactional dual Flash store,
or a bound filesystem path. It requires no global filesystem, directory,
allocator, path parser or OS. Settings and normal-context Coredump persistence
can share this contract while retaining their distinct domain policies.
Terminal fault capture must continue to use its bounded retained-RAM path.

## Public Surface

| Header / target | Contract |
| --- | --- |
| `esy_file.h` / `esy_file` | Caller-owned facade, capability discovery, information, exact range reads, create-if-absent, atomic whole replacement and removal. |
| `esy_file_ram.h` / `esy_file` | Caller-owned volatile buffer provider with every operation. |
| `esy_file_flash.h` / `esy_file_flash` | One-region create/read/remove provider; atomic replacement is unsupported. |
| `esy_file_dual_flash_store.h` / `esy_file_dual_flash_store` | Adapter borrowing one initialized two-region dual Flash store; atomic replacement and logical tombstones. |
| `esy_file_fs.h` / `esy_file_fs` | Adapter retaining one mounted filesystem volume and binding a target path; atomic publication requires a capable backend and reserved staging path. |

A present zero-length file differs from absence. `get_info()` reports existence,
committed length, capacity and capabilities derived from supplied callbacks.
`get_capabilities()` performs no provider query or storage I/O. Neither operation
initializes a provider. Unsupported mutations return NOT_SUPPORTED; capacity
limits and ranges are checked before mutation.

## Integration

The consumer selects the parent and each actual storage dependency explicitly:

```cmake
set(ENABLE_ESY_FILE ON)
set(ENABLE_ESY_FILE_FLASH OFF)
set(ENABLE_ESY_FILE_DUAL_FLASH_STORE OFF)
set(ENABLE_ESY_FILE_FS OFF)
```

The core includes RAM adaptation and depends only on common types. Flash
adaptation additionally requires `ENABLE_ESY_FLASH` and public HAL. Dual Flash store
adaptation requires `ENABLE_ESY_DUAL_FLASH_STORE` and its initialized provider.
Enabling either child without the parent or required provider fails configuration.
No provider is silently enabled. Public normalized/RAM headers include no HAL,
OS, board or dual Flash store header; the explicit dual Flash store adapter declares
its public dependency on that module.

Initialize all handles with `{0}`. Bind a provider, obtain its copied operations
with `get_ops()`, then call `esy_file_init()`. Pass the resulting file to a domain
consumer. Stop consumers before detaching the facade and provider. Providers do
not own the lifecycle of the global Flash controller or dual Flash store handle.
The filesystem adapter binds its volume and paths during initialization and
retains the mounted volume until successful detach. Enable `ENABLE_ESY_FS` and
`ENABLE_ESY_FILE_FS` explicitly; see [Filesystem](../fs/README.md) for providers
and resource requirements. Atomic CREATE/REPLACE are exposed only with a capable
backend and a caller-reserved staging path; FatFs exposes READ/REMOVE only.

### Usage Example

This RAM-only example needs `ENABLE_ESY_FILE` and no hardware provider. One
non-ISR owner supplies `{0}` facade/provider handles and a RAM configuration
whose content buffer is large enough for the payload. Keep handles and backing
bytes alive until both detach calls succeed. Payload storage is separate from
the provider's content buffer. RAM metadata does not survive deinit or reset.

```c
#include "esy_middleware/file/esy_file_ram.h"

esy_err_t replace_ram_file(esy_file_t *file, esy_file_ram_t *ram,
                          const esy_file_ram_config_t *config,
                          const void *payload, size_t length) {
    esy_file_ops_t ops;
    esy_err_t result = esy_file_ram_init(ram, config);
    if (result != ESY_OK) {
        return result;
    }
    result = esy_file_ram_get_ops(ram, &ops);
    if (result == ESY_OK) {
        result = esy_file_init(file, &ops);
    }
    if (result == ESY_OK) {
        result = esy_file_replace(file, payload, length);
        esy_err_t cleanup = esy_file_deinit(file);
        if (cleanup != ESY_OK) {
            return cleanup;
        }
    }
    esy_err_t cleanup = esy_file_ram_deinit(ram);
    return cleanup != ESY_OK ? cleanup : result;
}
```

The helper detaches immediately after replacement. A domain consumer instead
keeps both bindings live while it uses the file, then detaches the facade before
the provider. Preserve caller-owned handles on any cleanup error.

## Contracts

One non-ISR task/main-loop owner serializes every provider/facade call and every
other access to the same storage. Multi-call operations such as checking a
Coredump record ID before removal require this ownership across their complete
sequence. The facade's busy guard rejects callback reentry; it is not a lock.
Callbacks and synchronous Flash/filesystem work contribute to caller latency.
There is no task, timer, retry, deadline or independent timebase.

CREATE publishes only into an absent file and returns BUSY if one exists.
ATOMIC_REPLACE publishes the complete new content while retaining the previous
committed content until publication; media surviving an interruption exposes
an old or new complete file, not an accepted partial replacement. RAM supports
complete replacement only within its externally serialized volatile lifetime.
The filesystem backend must provide the necessary durable transaction
(such as a complete temporary write, synchronization and guaranteed atomic
publication); a plain in-place write does not meet this capability.

Mutation errors may follow durable effects. Requery authoritative state before
retrying. Reads validate existence even at zero length and return exactly the
requested span; output after an I/O failure is not authoritative. Information
and capability outputs remain unchanged on errors. Accessible spans are caller
preconditions; outputs and inputs must be disjoint from provider/facade handles
and borrowed storage. RAM provider data inputs/outputs must not alias its content
buffer. Flash inputs must not alias its reserved region or program scratch.

REMOVE means committed logical absence, not secure physical destruction.
RAM removal changes metadata without wiping content. The single-region Flash
provider erases and verifies its complete region. Dual Flash store removal commits
a tombstone; old bytes remain and its documented corruption/fallback limits
apply. Domain code must not silently substitute one physical-removal guarantee
for another. No provider selects partitions or permits overlapping storage.

### Single-region Flash format

One caller-reserved region owns complete physical sectors. The file is its
payload starting at the region base, followed by 32-byte version-one FMD1
metadata and a separate four-byte EFL1 publication marker at the region end.
Metadata and marker each own an independently rounded-up HAL program span.
Metadata contains magic, version, payload length, payload CRC32 and metadata
CRC32, encoded little endian. No native C structure is serialized and no
Coredump format is interpreted. Empty content is valid. Program padding remains
erased. Capacity excludes both reserved spans.

Creation scans existing state, rejects an existing file, erases the region,
programs payload units, verifies payload integrity, writes/verifies all metadata,
then programs the independent publication marker last. Only complete commits are
accepted after reopening. Missing or torn publication markers mean absence;
once the marker is valid, damaged metadata is an error and never treated as
absence. Recognizable committed unsupported versions, committed payload
corruption and read errors fail closed before mutation;
there is no implicit formatting or unconditional recovery erase. A failed erase
can leave corrupt media requiring explicit owner reconciliation. CRC protects
integrity, not authenticity.

Only a program-unit buffer is required: scans and verification stream through
it without a second full-file RAM copy. Initialization checks geometry and
whole-sector ownership but performs no media read/erase/program, so the domain
owner can apply its product layout checks before the first storage query.

### Transactional dual Flash store binding

`esy_file_dual_flash_store` delegates atomic replacement to the dual Flash store's
record transaction and uses `esy_dual_flash_store_read_range()` for bounded reads
after complete-record validation. The provider owns the persistent format and
generation semantics. The two regions are transaction copies of **one file**, not OTA
firmware image slots. Both firmware image slots can bind the same configuration
file when their formats/policies permit it. Sharing one interface does not force
the one-region Coredump backend to allocate two regions.

Independent raw Flash providers must use disjoint physical regions. Sharing a
filesystem volume requires that filesystem's allocation/routing owner; pointing
two raw-file bindings at the same region does not create multiple files.

## Resource Model

All facades, providers, descriptors, content and scratch storage are caller
owned. No dynamic allocation, hidden singleton, automatic saving, IRQ or OS
object is introduced. RAM descriptors are copied; content remains borrowed.
RAM metadata is volatile and its initial existence/length must be supplied on
reinitialization. Provider detach does not persist or erase bytes. Opaque Flash
handles detect copied ownership and retain bindings until successful deinit.
Dual Flash store adaptation borrows its handle and workspace exclusively.

## Verification

From this directory's `tests` subdirectory, run `ceedling test:all`,
`ceedling --mixin=sanitizer test:all` and `ceedling gcov:all`. From the platform
root run `python3 -B esy_middleware/file/tests/test_cmake_consumer.py` and
`tools/checks/check_no_dynamic_alloc.sh esy_middleware/file`.

Tests cover facade reentry/lifecycle and provider errors, capability queries
without I/O, absent versus empty files, bounded reads and overlap rejection,
RAM complete replacement, real dual Flash store adaptation and failed transactions.
Flash tests inject no/partial/complete programming effects at every payload and
metadata/marker program step across 1/4/16/64/256-byte program units, reopen the
provider, and accept only absence or the exact complete file. They exercise
committed metadata CRC corruption, unknown format protection, whole-sector
validation and physical removal. C11/C++17 consumers run real provider targets,
verify parent/child/provider failures, and require no OS/backend/board target.
In target integration/HIL tests, exercise Flash power-loss behavior, controller
arbitration, execution stalls and watchdog timing.

## Known Limits

- One bound file per facade; there is no directory, namespace, open-file registry,
  streaming write session or remote file-management protocol.
- The single-region Flash provider cannot atomically replace an existing file;
  Settings must use a capable RAM, dual Flash store or filesystem provider.
- CRC32 is not authentication. Logical deletion does not promise secure erasure
  or protection from all subsequent physical media corruption.
- Providers perform synchronous work and require owner serialization. They are
  not ISR/fault safe and do not supply a filesystem, partition allocator or
  generic cross-task synchronization layer.
