<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Dual Flash Store

## Purpose

`esy_dual_flash_store` preserves one latest binary record in two caller-reserved
Flash regions. Updates commit in the alternate region while retaining the
previous committed state. The module provides synchronous persistence for
infrequent records such as configuration snapshots or diagnostic evidence.

"Dual" refers to two independently erasable regions used by one store, not two
Flash chips or OTA firmware image slots. Both firmware image slots can use the
same reserved data regions.

It uses the public Flash HAL and has no Logger, bootloader, OTA or OS dependency.
Consumers own the payload format and export policy. A future filesystem can be
an independent module over the same public HAL, using its own exclusive volume;
no filesystem or interchangeable storage-provider framework is included here.
The [bound-file adapter](../file/README.md) can expose this record through
`esy_file` while retaining this module's transaction and tombstone semantics.

## Public Surface

Include `esy_middleware/dual_flash_store/esy_dual_flash_store.h` and link
`esy_dual_flash_store`.

| API | Behavior |
| --- | --- |
| `esy_dual_flash_store_init()` | Validate geometry and reserved ranges, borrow workspace and scan existing state without modifying Flash. |
| `esy_dual_flash_store_get_info()` | Rescan and report record presence, length, capacity and generation. |
| `esy_dual_flash_store_write()` | Replace the latest binary record through a new transaction. Zero-length records are valid. |
| `esy_dual_flash_store_read()` | Validate state and read the complete latest payload. |
| `esy_dual_flash_store_read_range()` | Validate the complete latest payload, then copy one exact bounded range without a full-record output buffer. |
| `esy_dual_flash_store_clear()` | Commit a tombstone to make the store logically empty. |
| `esy_dual_flash_store_deinit()` | Release borrowed RAM without erasing Flash or changing its global lifecycle. |

## Integration

The consuming project's feature file supplies both gates:

```cmake
set(ENABLE_ESY_FLASH ON)
set(ENABLE_ESY_DUAL_FLASH_STORE ON)
```

The library assembles `esy_dual_flash_store` and links it into `esy_middleware`.
Direct consumers can link the concrete target. Disabling the module omits its
implementation; enabling it without Flash support fails configuration.

The consumer reserves two non-overlapping Flash ranges in its physical layout.
Each range must cover complete physical sectors and meet HAL program alignment.
Ranges may have different sizes; capacity uses the smaller usable range. They
must be disjoint from executable images, boot metadata, filesystem volumes and
every other writer's reserved storage. This module does not edit link scripts,
partition tables or application layouts.

The startup/task owner initializes Flash and establishes its safe execution
policy before initializing the store. In a baremetal application, the serialized
main context can own the same API.

### Usage Example

Supply a `{0}` store in the owner's persistent storage and a configuration with
two reserved whole-sector regions and a suitably aligned program workspace.
Keep that workspace alive until deinit succeeds. The payload must be disjoint
from the handle and workspace; the synchronous write retains no payload pointer.

```c
#include "esy_middleware/dual_flash_store/esy_dual_flash_store.h"

esy_err_t save_record(esy_dual_flash_store_t *store,
                      const esy_dual_flash_store_config_t *config,
                      const void *payload, size_t length) {
    esy_err_t result = esy_dual_flash_store_init(store, config);
    if (result != ESY_OK) {
        return result;
    }
    result = esy_dual_flash_store_write(store, payload, length);
    esy_err_t cleanup = esy_dual_flash_store_deinit(store);
    return cleanup != ESY_OK ? cleanup : result;
}
```

Check the returned status. A write error may follow a committed effect; inspect
stored state before retrying. Deinit releases RAM without erasing the record.
The workspace must provide at least the HAL `program_size` bytes in writable RAM
suitable for program operations; descriptor objects are copied during init.

## Contracts

### Persistent transactions

The versioned binary format contains a 32-byte header, payload, and a separate
32-byte commit record. Header and commit spans are each rounded up to the HAL
program unit; payload programming is padded to that unit. The commit span is
never touched by header/payload writes and is programmed last. For program unit
`P`, let `S = align_up(32, P)`. Payload capacity is
`min(region[0].size_bytes, region[1].size_bytes) - 2 * S` after validating the
aligned region sizes.

The header starts at the region address, the payload at `region.address + S`,
and the commit at `region.address + region.size_bytes - S`. Padding bytes are
`0xFF`. All integer fields are serialized little-endian without relying on C
structure layout. Version 1 uses the following offsets, in bytes:

| Offset | Width | Header | Commit |
| --- | --- | --- | --- |
| 0 | 4 | Magic `0x52595345` | Magic `0x54494D43` |
| 4 | 4 | Format version `1` | Format version `1` |
| 8 | 8 | Generation | Same generation |
| 16 | 4 | Payload length | Header CRC |
| 20 | 4 | Kind: data `1`, tombstone `2` | Payload CRC |
| 24 | 4 | Payload CRC | Payload length |
| 28 | 4 | CRC of header bytes `[0, 28)` | CRC of commit bytes `[0, 28)` |

CRC is reflected CRC-32 with polynomial `0xEDB88320`, initial value `0xFFFFFFFF`
and final bitwise complement. Payload CRC covers exactly the unpadded payload;
the empty payload CRC is zero.

An update scans both regions, erases the alternate region, writes the new
header/payload, verifies readback, writes the final commit and verifies the
committed record. CRC checks protect record metadata and payload against
accidental corruption. A record with an incomplete commit or corrupt contents
does not supersede a valid previous record. HAL read failures and recognizable
unsupported format versions fail closed instead of being treated as empty
space.

Committed transactions use a monotonically increasing `uint64_t` generation.
It never wraps: updates requiring a new generation fail at `UINT64_MAX`. Two
valid regions must have consecutive generations; ambiguous state is rejected.
Clear commits a newer tombstone through the same transaction path. The tombstone
retains its generation across restart and prevents an interrupted replacement
from reviving the previous record. Clearing an already empty store requires no
new transaction.

A successful zero-length write is a data record: info reports `has_record=true`
and `length=0`, and read succeeds with zero bytes. A tombstone reports
`has_record=false`; read returns `ESY_ERR_STATE`. Both can have a nonzero
generation, whereas a store with no valid transaction reports generation zero.

Any failed mutation may already have committed, including failures during final
verification. Every operation rescans Flash, so inspect the current state before
retrying. A successful mutation means readback verification completed; physical
durability depends on the selected HAL/device persistence contract.

### Execution and lifecycle

Calls are synchronous and blocking, with one externally serialized owner.
Operations are forbidden in ISR and fault context. The implementation performs
no dynamic context detection, task creation, background work, retries or timeout
handling. Each scan checks complete committed payloads; operation cost grows
with record size and includes HAL erase/program/read duration. The consumer
budgets interrupt latency, multicore coordination and watchdog service around
these operations.

Serialize all handles and all users of the same physical Flash, including OTA,
boot metadata and a future filesystem. The store does not acquire a global
Flash lock, initialize/deinitialize Flash, install safe-execution hooks, stop
tasks or coordinate cores. The consumer must establish any required OTA quiesce
policy. There are no module tasks, ISR callbacks, DMA transfers or module clocks
to stop, and no module timeout/counter-wrap timing contract.

Do not copy/move a live handle or overlap its workspace with the handle, payload
or output objects. A write payload must not alias either reserved Flash range.
Failed first init leaves the zero handle reusable; repeated init of a live
handle fails. Deinit does not modify persistent data and restores the zero state;
repeated deinit of that state succeeds.

Info and read-length outputs remain unchanged on error. A too-small read buffer
remains unchanged; a HAL error during the final payload read can partially modify
the destination. Callers must check the result before consuming those bytes.

## Resource Model

All resources are caller-owned: one zero-initialized handle, a workspace of at
least one HAL program unit, and two exclusive Flash regions. The module allocates
no heap memory and creates no OS resources. It borrows the handle/workspace for
the initialized lifetime, copies configuration descriptors and does not own the
Flash driver. Multiple stores are allowed with disjoint regions and externally
serialized access.

## Verification

From the platform repository root:

```sh
python3 -B esy_middleware/dual_flash_store/tests/test_cmake_consumer.py
cd esy_middleware/dual_flash_store/tests
ceedling test:all
ceedling gcov:all
```

Module tests use the reusable Flash fake for geometry, argument, lifecycle,
transaction and fault-injection checks. Range-read tests cover partial spans,
end-of-record empty reads, invalid/overflowing ranges, borrowed-workspace overlap,
absence, tombstones and scan I/O errors without allocating a full-record output.
The CMake consumer builds independent
C11/C++17 public-header probes, links and executes the real core with the shared
Flash fake without an OS, executes the public range API from C and links it from
C++, and verifies enabled/disabled feature assembly and
the required Flash gate. The repository gate also runs the module with
sanitizers. Host checks do not establish hardware power-loss behavior, flash
execution safety, interrupt/core latency or watchdog budgets; validate these
with the consumer's real layout, HAL port and hardware.

## Known Limits

- One latest record per store; no record catalog, keys, files, streaming writer,
  compression, wear levelling or history retention policy.
- Every replacement erases the alternate region. The consumer determines update
  frequency from its device's endurance limits.
- CRC provides corruption detection, without authentication, encryption or
  anti-rollback guarantees.
- Logical clear is not secure deletion. Old payload bytes remain in Flash;
  later physical corruption of a newer revision can expose an older valid
  revision through fallback.
- RAM fault capture, reset retention, coredump decoding and Logger export belong
  to separate modules/consumer integration. This API writes only in normal
  execution context.
