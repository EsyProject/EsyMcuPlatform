<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Filesystem

## Purpose

`esy_fs` provides synchronous, caller-owned volume, streaming file and directory
interfaces. Independent littlefs and FatFs volumes can be mounted together. It
has no global path namespace, working directory, automatic task, allocator,
automatic formatting or dependency on an operating system.

Use littlefs for erase-before-program Flash and FatFs for sector-addressed media
such as SD cards that must exchange files with a computer. The filesystem does
not select product partitions, reserve pins or choose an OTA policy.

## Public Surface

| Header / target | Contract |
| --- | --- |
| `esy_fs.h` / `esy_fs` | Backend-neutral volume, file, directory and lifetime interfaces. |
| `esy_fs_littlefs.h` / `esy_fs_littlefs` | Explicit littlefs software adapter, typed pools and static caches. |
| `esy_fs_flash.h` / `esy_fs_flash` | Public Flash HAL to littlefs media adapter with whole-sector validation. |
| `esy_fs_fatfs.h` / `esy_fs_fatfs` | Explicit FatFs software adapter and typed native resources. |
| `esy_fs_block.h` / `esy_fs_fatfs` | Synchronous sector-device contract for SD or another actual block provider. |
| `../file/include/esy_middleware/file/esy_file_fs.h` / `esy_file_fs` | One reserved filesystem path exposed through the existing bound-file contract. |

The common header hides native provider types. The explicitly named adapter
headers intentionally expose their provider's native types so callers can own
correctly typed storage without heap allocation or byte-array aliasing. Their
public targets propagate the corresponding provider headers and configuration.

## Integration

Declare final features in the consumer's selected feature entry:

```cmake
set(ENABLE_ESY_FS ON)
set(ENABLE_ESY_FS_LITTLEFS ON)
set(ENABLE_ESY_FS_FATFS ON)

# Optional built-in public HAL Flash adapter.
set(ENABLE_ESY_FS_FLASH ON)
set(ENABLE_ESY_FLASH ON)

# Optional binding for Settings, Credentials or normal-context Coredump.
set(ENABLE_ESY_FILE ON)
set(ENABLE_ESY_FILE_FS ON)
```

Set `ESY_LITTLEFS_PATH` to consumer-provisioned official littlefs 2.11.2 sources
and `ESY_FATFS_PATH` to the official FatFs R0.16 `source` directory. Enabled
adapters require these explicit source inputs; configuration never downloads
dependencies.

For library host tests, `./setup.sh` prepares seven pinned official providers;
the Python/Ruby test helpers discover their cache automatically. Firmware CMake
uses the consumer's explicit providers, independently of this test cache and
lock. See [software provider setup](../../tools/README.md#software-provider-setup).
Keep their upstream licenses with the source checkouts. Disabled adapters do not
validate paths, compile sources or propagate third-party dependencies.
Native provider sources privately inherit the consumer's `esy_opt_default`
compile options so their target architecture and ABI match the filesystem adapters.

The littlefs provider is built with `LFS_NO_MALLOC`; read/program/lookahead caches
and every open file cache are supplied by the caller. Its ABI does not enable
`LFS_READONLY`, `LFS_THREADSAFE` or `LFS_MULTIVERSION`. The FatFs provider uses the
module's `config/fatfs/ffconf.h`, UTF-8 long names with stack workspace, FAT/FAT32,
four volumes and native file sharing checks. exFAT and heap-backed long names are
not enabled. Every native consumer must use the same provider target/configuration.

Initialize handles with `{0}`. Initialize the media, bind an adapter and its
static resources, obtain copied operations, initialize `esy_fs_t`, then explicitly
mount. First-use provisioning must explicitly format an unmounted volume before
mounting it; a corrupt volume must not be silently reformatted. On shutdown stop
consumers, detach bound-file adapters, close files/directories, unmount, deinit the
facade, deinit the backend, then release media resources. Errors retain resources
unless the documented close result explicitly consumes the object.

For built-in Flash, call `esy_fs_flash_init()` with a consumer-reserved region,
then `esy_fs_flash_get_config()` and set the native littlefs cache, lookahead and
wear-level parameters. Supply that descriptor and a
`esy_fs_littlefs_static_resources_t` to `esy_fs_littlefs_init()`. The media adapter
borrows the already initialized Flash controller and never initializes or shuts
down the controller itself. External NOR can provide littlefs media callbacks
through an actual public HAL-based driver.

For SD, supply `esy_fs_block_ops_t` callbacks backed by the consumer's initialized
SD driver, together with its actual geometry and static FatFs resources. This
module implements the FatFs disk routing, not an SD electrical/protocol driver.
An SDMMC hardware port belongs behind public HAL; an SPI SD implementation uses
public SPI/GPIO APIs. No raw vendor/board API is called by the filesystem. The
current implementation does not assume a particular SD bus, card-detect pin or
board wiring.

### Usage Example

One non-ISR owner supplies a `{0}` volume/file pair and operations obtained from
an initialized littlefs or FatFs backend with its media and static pools already
configured. Keep all handles and backend storage alive through successful close,
unmount and deinit. Mount failure is returned without formatting the media. This
example creates a new `/note.txt`; an existing file is left untouched.

```c
#include "esy_middleware/fs/esy_fs.h"

esy_err_t create_note(esy_fs_t *volume, esy_fs_file_t *file,
                     const esy_fs_ops_t *ops, const void *data, size_t length) {
    size_t written = 0u;
    esy_err_t result = esy_fs_init(volume, ops);
    if (result != ESY_OK) return result;
    result = esy_fs_mount(volume);
    if (result != ESY_OK) goto detach;
    result = esy_fs_open(volume, file, "/note.txt",
        ESY_FS_OPEN_WRITE | ESY_FS_OPEN_CREATE | ESY_FS_OPEN_EXCLUSIVE);
    if (result != ESY_OK) goto unmount;
    result = esy_fs_write(file, data, length, &written);
    if (result == ESY_OK && written != length) result = ESY_ERR_IO;
    if (result == ESY_OK) result = esy_fs_sync(file);
    esy_err_t cleanup = esy_fs_close(file);
    if (cleanup != ESY_OK) return cleanup;
unmount:
    cleanup = esy_fs_unmount(volume);
    if (cleanup != ESY_OK) return cleanup;
detach:
    cleanup = esy_fs_deinit(volume);
    return cleanup != ESY_OK ? cleanup : result;
}
```

A write error can leave a partial new file; this streaming example provides no
atomic replacement guarantee. A close or unmount error returns with caller-owned
storage intact for inspection and backend-specific recovery.

## Contracts

Calls are synchronous and can perform complete media operations, allocation
scans, metadata compaction and synchronization. Their latency includes provider
and hardware latency; they are neither ISR/fault safe nor hard-real-time. There
is no independent timeout, clock or polling cadence. Baremetal and task callers
use the same core with no OS headers or target required.

One non-ISR owner serializes a volume, its native backend and all direct accesses
to the same media. All FatFs instances and disk registration/lifecycle additionally
share one serialized owner because the selected native configuration has global
state. The facade busy flag only rejects callback reentry. Separate volumes can
coexist; this does not authorize concurrent access to one controller. A backend
context is bound to exactly one live facade, which must be detached before
backend deinitialization.

Paths start with `/` and are relative to the supplied volume. Empty components,
trailing separators except `/`, `.`, `..`, backslashes, colons and ASCII controls
are rejected before I/O. The common path limit is 255 bytes. Native encoding,
basename, case and filename restrictions still apply. FatFs uses case-insensitive
FAT names; littlefs uses case-sensitive names. Rename is volume-local and ordinary
overwrite behavior is backend-specific. No symbolic links are provided.

Read and write report transferred bytes separately from status. Short reads and
zero-byte EOF succeed; short writes must be inspected. Metadata/count outputs are
unchanged on errors, but data buffers and durable media may already have changed.
Observe sync and close errors. A close failure can either retain an object for
retry or consume it; the file/directory handle reflects the backend's released
result. A consumed handle must not be retried as a live object.

The littlefs backend supplies optional whole-file atomic replacement: write the
complete caller-reserved staging path, sync and close it, then atomically rename
it over the target. Stale staging files may be overwritten. The reserved staging
path must not name another consumer's data, must differ from the target and must
have sufficient free space. Media erase/program/sync correctness is a prerequisite
for the native power-loss guarantees. A failed operation may already have
published the new target; requery before retries.

FatFs does not advertise atomic whole-file replacement. `f_sync`, ordinary
`rename` and a successful write do not establish the same power-loss guarantee.
Following an I/O/media-removal failure during close, the adapter can consume the
handle and fault the volume. Close all remaining objects and unmount before
remount/recovery; do not continue normal operations against that faulted volume.
Unsynchronized data can be lost. Native sharing checks reject conflicting opens
and mutations of open files within their configured object limit.

`esy_file_fs` borrows a mounted volume with a lease, preventing unmount while
Settings or another domain consumer uses its binding. It advertises CREATE and
ATOMIC_REPLACE only when a capable backend and a reserved staging path are both
provided; otherwise it exposes READ and REMOVE. An absent file differs from an
existing empty file. The configured bound-file capacity is a logical size limit,
not a storage reservation or current free space. Removal is not secure erasure.

## Resource Model

All facades, typed native contexts, file/directory pools, caches and formatting
workspace are caller owned. Resource descriptors are copied; backing storage,
media contexts, callback code and bound paths remain borrowed. They must remain
valid, correctly aligned and disjoint for the documented lifetime. Pool exhaustion
returns NO_MEM and media exhaustion is reported separately as NO_SPACE or a short
write according to the native operation. Never copy or reset a live handle.

Flash volumes must cover complete physical sectors and stay disjoint from OTA
images, boot metadata, raw bound files and transactional dual Flash stores.
Logical littlefs blocks must cover whole physical erase sectors; mixed physical
sector sizes require a valid complete mapping, not the first sector's size.
Address separation does not resolve controller contention, XIP stalls, other-core
access, ISR/DMA activity or watchdog timing. Those remain consumer integration
responsibilities. This module owns no task; any task using it
must define its own OTA quiescence and recovery behavior.

For USB mass-storage export, synchronize and close users, detach bindings and
unmount before giving the host write ownership. Revoke host ownership before
remounting. The MCU and a computer must not independently mount writable FAT on
the same medium at the same time.

## Verification

Run `./setup.sh` once from the platform repository root to prepare the cached
software providers. Run the core suite with `ceedling test:all` from this
module's `tests` directory. From the module directory, run
`python3 -B tests/run_backend_tests.py` for the actual upstream backends.
The runner discovers the cache and stages FatFs
beside the selected configuration and invokes Ceedling's backend mixin. Add
`--sanitize` for AddressSanitizer and UndefinedBehaviorSanitizer or `gcov:all`
for coverage. Run
`python3 -B tests/test_cmake_consumer.py` from this module for backend
build cases. The consumer suite also checks provider compile-option inheritance
and isolation. Core/off/error cases run without provider sources. Run allocation
and hardware-boundary checks from the platform root. Provider sources are test
inputs, not committed or fetched implicitly.

Optionally set the `ESY_LITTLEFS_PATH` and `ESY_FATFS_PATH` environment variables
to test other compatible upstream checkouts.

Tests exercise lifecycle/reentry, copied handles, close retries and consumed
objects, output snapshots, canonical paths, EOF/short I/O, resource exhaustion,
real littlefs/FatFs media behavior, bound-file adaptation, Flash geometry and
injected I/O/power-loss failures. C11/C++17 consumers verify optional dependency
closure without an OS. Embedded consumer builds and target-toolchain compilation
complement host tests; host simulation does not establish electrical behavior.

Target/HIL procedures exercise actual Flash power cuts and sync semantics,
Flash-safe execution and OTA arbitration, worst-case erase/compaction latency,
watchdog timing, SD removal/reinsertion and write protection, power loss during
FAT updates, and exclusive handoff to a computer. Test each selected medium and
provider version under the product's memory and timing budget.

## Known Limits

- No global VFS, POSIX descriptor table, permissions, timestamps in the common
  interface, cross-volume rename or multi-file transaction.
- No built-in SDMMC/SPI SD card driver, external NOR protocol driver or product
  partition allocator. Block callbacks must implement actual media access.
- Flash HAL currently selects one controller; the filesystem does not add a
  multiple-chip HAL abstraction or bypass that boundary.
- FatFs offers FAT/FAT32 compatibility, not power-loss-safe atomic replacement.
  SD controllers can cache internally; the block provider must implement sync.
- Filesystem access is unsuitable for terminal fault capture. Preserve the
  existing bounded retained-RAM fault path and persist after normal startup.
