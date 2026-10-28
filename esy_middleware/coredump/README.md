<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors
SPDX-License-Identifier: Apache-2.0
-->

# Coredump

## Purpose

Capture a bounded fault record in caller-reserved retained RAM, inspect it after
reset, and export it from normal execution context. Optional file persistence
keeps the first committed record until its owner explicitly clears it. Capture
does not log, allocate memory, call an OS, or program Flash.

## Public Surface

The core target is `esy_coredump_core`. Optional `esy_coredump_logger` and
`esy_coredump_file` targets provide Logger export and synchronous persistence
through a prebound [file](../file/README.md). The persistence adapter does not
select storage, expose physical addresses or depend on a Flash controller, OS,
filesystem or `dual_flash_store`.

- `esy_coredump.h`: caller-owned handle/configuration, bounded fatal capture,
  raw validation, persistent occurrence identities, conditional range reads,
  authoritative restore and conditional clear.
- `esy_coredump_logger.h`: `esy_coredump_logger_export_chunk()` offers one
  16-byte-or-shorter chunk at a caller-supplied offset.
- `esy_coredump_file.h`: normal-context create-if-absent persistence,
  authoritative restore and identity-checked clear through `esy_file_t`.

## Integration

The consumer selects `ENABLE_ESY_COREDUMP` and optionally
`ENABLE_ESY_COREDUMP_LOGGER` and `ENABLE_ESY_COREDUMP_FILE` in its feature import.
Logger export requires the Logger target. File persistence requires
`ENABLE_ESY_FILE` and an initialized file with READ and CREATE capabilities.
The core and generic file adapter have no OS or HAL dependency.

The consumer owns capture installation, startup ordering, retained-memory
placement, file/provider lifecycle and storage coordination. Bootloader and all
application images must reserve exactly the same retained-memory range. A
`NOLOAD` section alone does not establish retention: reset behavior, boot ROM,
bootloader startup, image handoff, stack placement and DMA ownership also matter.

Initialize the selected file backend and facade, then bind the initialized RAM
store and facade with `esy_coredump_file_init()`. Supply exclusive record
workspace of at least `ESY_COREDUMP_MAX_RECORD_BYTES`; its configuration is
`{store, file, static_resources}`. Init checks capabilities without reading the
file, preserving consumer-owned startup and storage-admission order. Detach the
coredump adapter before the facade, backend or RAM store. A RAM file can be used
without Flash, Logger or an OS; an external filesystem provider can supply the
same public file contract without changing this module.

A consumer selecting `esy_file_flash` separately enables its Flash feature and
supplies one exclusive region, disjoint from bootloader, image/metadata,
configuration and filesystem regions. That backend borrows its own program
scratch. Its scratch and provider state must be disjoint from the coredump
record workspace, retained RAM and all handles. No additional coredump-specific
Flash partition, footer or erase algorithm is allocated by this adapter.

Configuration requires a normal-context `new_record_id` callback and its borrowed
context. Init and a matching clear obtain a fresh nonzero 128-bit identity for
the next capture lifetime. The source must avoid reuse across resets and image
changes; an owner-managed durable allocator can guarantee uniqueness, while a
128-bit random source provides probabilistic uniqueness. The module chooses no
RNG, clock, counter partition or persistence mechanism. The callback may perform
its explicitly provisioned normal-context work, must not reenter the store, and
is never called from fault capture. Its full cost belongs to startup/clear.

### Usage Example

After normal startup has initialized an owner-held `esy_coredump_t` with its
retained RAM and identity source, read the saved header through the public core:

```c
#include "esy_middleware/coredump/esy_coredump.h"

esy_err_t read_fault_header(esy_coredump_t *store,
                            void *header, size_t capacity) {
    if (header == NULL || capacity < ESY_COREDUMP_HEADER_BYTES) {
        return ESY_ERR_INVALID_ARG;
    }
    esy_coredump_info_t info;
    esy_err_t result = esy_coredump_get_info(store, &info);
    if (result != ESY_OK) {
        return result;
    }
    return esy_coredump_read(store, &info.record_id, 0u,
                             header, ESY_COREDUMP_HEADER_BYTES);
}
```

Call from the serialized normal-context owner, with an output buffer disjoint
from the store and retained RAM. An empty store returns `STATE`; do not consume
the header on any error. Reading preserves the record for collection and does
not clear it or invoke terminal fault capture.

## Contracts

### First fault and explicit collection

An existing valid record is retained. Neither a later fault, normal restart,
file persistence nor Logger export authorizes replacing it. Clearing the
record is an explicit owner operation after collection. A successful export
call means a line was offered to Logger; it does not confirm transport delivery
or collection by the host. Keep the record available for another export.

The saved build ID, image base and firmware version describe the image that
faulted. They must not be replaced with the identity of an image that exports
the record after an OTA rollback. Keep the exact ELF for each A/B build.

The record also contains an opaque `record_id` identifying one fault occurrence.
Two otherwise identical captures use different identities. Init, raw restore,
file restore and export preserve the identity stored in an existing record.
It is distinct from the CRC and supplies no authentication. Capture copies only
the identity prepared in ordinary RAM; it performs no ID generation, clock/RNG
call or 64-bit integer work. The terminal-fault contract forbids resuming normal
execution after capture, even when capture fails.

Use `get_info()` to obtain the current identity, then supply it to every `read()`
and `clear()`. A stale identity returns STATE before output or invalidation;
zero/missing identities are invalid arguments. Clear validates the complete
record and obtains its successor identity before invalidating the commit word.
Source failure, zero output or immediate identity reuse preserves the record and
handle. Clearing an empty store returns STATE. No unqualified read/clear route
or format fallback is provided. Unknown/corrupt committed records remain
protected by these conditional operations.

### Normal-context export

Logger emits hexadecimal chunks with this format:

```text
CD1 <record_id_32hex> <total_bytes_decimal> <offset_decimal> <hex_bytes>
```

Each line contains at most 16 record bytes. Prefixes and ANSI color decoration
from Logger are permitted. Repeated exports may fill gaps from earlier attempts.
The decoder accepts reordered and identical duplicate chunks, and rejects
missing bytes, conflicting duplicates, inconsistent lengths and checksum errors.
Multiple record IDs require explicit selection with `--record` and the complete
32 hexadecimal digits. The decoder checks that this identity matches the
reassembled binary record, then independently validates its CRC. JSON output
reports both `record_id` and `record_crc`.

Decode a captured serial log:

```sh
python3 tools/coredump/esy_coredump_decode.py serial.log --output fault.bin
```

Validate an exact ELF and symbolize the saved PC and LR:

```sh
python3 tools/coredump/esy_coredump_decode.py serial.log \
  --output fault.bin --elf retained-slot-a.elf
```

The tool requires a matching 20-byte GNU build ID before invoking
`arm-none-eabi-addr2line`. This reports saved PC/LR locations, not a recovered
backtrace. Xtensa records symbolize only PC: A0 is retained as a raw register
because windowed ABI return-address encoding requires additional interpretation.
Select the matching Xtensa tools with `--readelf` and `--addr2line` for ESP32.
Raw binary input can be selected with `--format binary`. JSON output
defaults to `fault.json`; neither output may overwrite the input.

### Record format

Version one is little endian: a 384-byte header followed by the captured stack.
The aligned `ECD1` magic word at byte zero is committed last. Bytes 4, 8 and 12
hold the version, complete byte count and finalized reflected CRC32. CRC uses
polynomial `0xEDB88320` and covers bytes 4..11 followed by bytes 16..end, excluding
the commit word and the CRC field. The stack length at byte 44 must equal the
complete byte count minus 384. Image base, firmware version and the 20-byte GNU
build ID begin at bytes 32, 36 and 48. Architecture-specific register slots
occupy 32 words beginning at byte 96. Architecture 1 uses ARM R0..R15 and xPSR
in slots 0..16; architecture 2 uses Xtensa A0..A15, PC, PS, EXCCAUSE and EXCVADDR
in slots 0..19. Bytes 268..283 contain the 16 opaque occurrence-identity bytes in
their original order; these are not serialized as an integer. They must not all be zero and are
covered by the record CRC. Remaining register slots and reserved header bytes
are zero; the trailing reserved range begins at 284. The format
version is one and the header is 384 bytes.

### File persistence safety

All file operations run synchronously on the existing normal-context owner,
never from a fault or ISR. The owner serializes an entire domain operation with
all other users of the bound file, RAM record and provider. A facade reentry
guard does not make inspect-then-remove atomic against independent writers.
Storage admission, provider blocking, Flash safe execution, watchdog and OTA
coordination remain with the consumer and selected backend.

Persist creates the complete validated RAM record only when the file is absent.
An existing byte-identical record succeeds without another create; a different
valid record returns BUSY and remains untouched. Invalid, unknown or unreadable
existing content propagates its error and is never treated as absence. No replace,
corruption-reclaim or automatic deletion path is used. File capacity is checked
by its facade/backend when creating. A failed create can already have committed;
retrying the same record first inspects the authoritative file and reconciles it.
The adapter uses bounded local comparison chunks rather than another full-record
buffer. RAM remains pending after successful persistence.

Restore validates a complete file in the record workspace before changing RAM.
Its stored occurrence ID, build ID and fault identity are preserved, including
when an older uncollected file replaces a newer retained fault. Absent files
return STATE. Unknown record versions are recognized through the stable eight-byte
ECD1/version prefix before version-one record-size bounds; no alternate format
is interpreted. A malformed record, read failure or unsupported version leaves
retained RAM unchanged.

Clear requires REMOVE capability and a copied expected record ID matching RAM
and any existing valid file. Different identities, unknown/corrupt content or
file I/O/state errors fail closed before removal. A successfully queried absent
file is accepted only while RAM still matches, supporting retry after a partially
completed clear. Removal commits absence before core clear obtains the next
capture identity and invalidates RAM. Removal errors preserve RAM even when the
provider already committed absence. A later identity-source failure also leaves
RAM intact with an error while the file may already be absent; retain the
original ID and reconcile/retry. No unconditional discard fallback exists.

The file backend defines durability, physical commit/recovery and whether logical
removal physically erases data. A volatile RAM file survives only its declared
RAM lifetime. The generic adapter adds no Flash writes, sector ownership rules,
filesystem sync or physical power-loss guarantee of its own.

## Resource Model

The consumer supplies retained bytes and one normal-context record workspace.
No module task, timer, queue, filesystem, allocator or hidden Flash partition is created.
Capture work is bounded by the configured snapshot size. Logger export is
caller-driven, so its owner controls pacing relative to backend capacity and
other application traffic.
The identity callback/context remain borrowed until core deinit. A prepared
16-byte identity and callback binding occupy opaque handle storage. Retained
storage uses the record layout described above.

## Verification

Host tests exercise independent binary fixtures, logger loss/reordering,
duplicate conflicts, CRC and length validation, exact build-ID matching, and
error reporting. Module tests cover first-record retention, lifecycle, actual RAM
file persistence, exact-file retries, malformed/unknown contents, read/remove
failures and operation errors after committed effects. An integration fixture
uses the real file Flash backend and reusable Flash fake to check persistence,
restore, clear and isolation of the one reserved region. CMake C11/C++17 consumers
execute generic persistence through RAM without HAL, Logger or OS dependencies.
Backend-specific commit and interrupted-Flash tests belong to the file module.
Identity regressions cover identical captures with distinct IDs, restore and
cross-image stability, stale reads/clears without effects, allocation failure or
reuse, zero/CRC-corrupt IDs, different authoritative file IDs, failed-clear
retry, and fault capture without identity-source calls. Logger/decoder fixtures
require exactly 32 identifier digits and never group records by checksum.

Target integration procedures exercise a real fault and reset through the bootloader,
capture in one slot followed by export in the other, actual power interruption
during persistence, and continued OTA access only within its own partitions.
Host tests do not prove SRAM retention or physical Flash power-failure behavior.

## Known Limits

Power loss before durable file persistence may lose a RAM-only record. A hard
watchdog reset, inaccessible stack, corrupted execution state, or a second fault during
capture may yield no complete dump. Capturing one core does not imply that the
other core's registers or all task stacks are available. Stack bytes can contain
application secrets; retention and collection policy belong to the consumer.
Logger is best effort and can filter or drop chunks. File operations do not run
inside the fault handler, and the module supplies neither a filesystem nor an
automated upload/acknowledgment protocol.

ARM fault entry uses an emergency stack to reduce dependence on the faulting
task's stack. ESP32 integration enters from the native panic path and remains
subject to that provider's panic-stack and snapshot limitations. Host validation
cannot establish that either path survives arbitrary stack or memory corruption.
The identity source owns uniqueness across power loss. Restoring the same
record preserves its identity but cannot turn a reused provider token into a
globally unique occurrence ID.
