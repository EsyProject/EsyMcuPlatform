<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Settings

## Purpose

`esy_settings` maintains a bounded string-key/binary-value snapshot in caller-owned
RAM. Explicit commits replace one complete logical [file](../file/README.md).
The same settings bytes can be stored in RAM, a dual Flash store volume, or
another file provider implementing atomic replacement. Storage selection does not
change configuration keys, value encoding, staging or the snapshot format.

The consumer owns defaults, value validation, schema migration and application.
Unknown business keys remain intact. The module creates no task, clock, heap
allocation, directory tree, hardware initialization or automatic save.

## Public Surface

Include `esy_middleware/settings/esy_settings.h` and link `esy_settings`.

| API | Behavior |
| --- | --- |
| `init()` | Validate resources/file capabilities and load the complete snapshot. |
| `get()` | Copy staged bytes; report absence independently of an empty value. |
| `set()` / `remove()` | Change only the working RAM snapshot. |
| `commit()` | Atomically replace the complete file when dirty. |
| `reload()` | Fully validate provider bytes before publishing them in RAM. |
| `get_info()` | Query staging state and memory use without provider access. |
| `deinit()` | Release borrows and intentionally discard staged edits without saving. |

Keys are nonempty, case-sensitive NUL-terminated strings. `max_key_length` excludes
the terminator and also applies to loaded keys. `/` is an ordinary key byte, not
a file path separator. Values are opaque blobs; an absent key differs from an
empty value. Scalars require explicit encoding; native structure layout is not
a portable storage format. GET with NULL/zero output can query a value's required
size. A short output remains unchanged and returns NO_MEM, its required length
and `found=true`. Absence succeeds with `found=false` and length zero.

## Integration

Select Settings and the file abstraction in the consumer feature file:

```cmake
set(ENABLE_ESY_FILE ON)
set(ENABLE_ESY_SETTINGS ON)
```

The core depends on `esy_file`, common utilities and common types. It has no HAL,
OS, Flash or dual Flash store dependency. Select any physical provider separately;
a RAM-only consumer needs none of those hardware targets. A dual Flash store file
adapter additionally requires its provider feature and Flash dependencies.

Initialize a logical file first. It must advertise READ and ATOMIC_REPLACE.
Borrow it exclusively and allocate a zero-initialized settings handle plus two
disjoint writable buffers.

### Usage Example

One non-ISR owner supplies a `{0}` handle and a prepared configuration binding
that file, both buffers, and a key-length budget of at least four bytes. Keep the
file/provider and buffers alive until deinit succeeds. This example stores one
opaque byte under `mode`; the product owns its meaning and validation.

```c
#include "esy_middleware/settings/esy_settings.h"

esy_err_t save_mode(esy_settings_t *settings,
                    const esy_settings_config_t *config, uint8_t mode) {
    esy_err_t result = esy_settings_init(settings, config);
    if (result != ESY_OK) {
        return result;
    }
    result = esy_settings_set(settings, "mode", &mode, sizeof(mode));
    if (result == ESY_OK) {
        result = esy_settings_commit(settings);
    }
    esy_err_t cleanup = esy_settings_deinit(settings);
    return cleanup != ESY_OK ? cleanup : result;
}
```

Commit errors can follow durable changes; reconcile the stored snapshot before
another mutation. Deinit itself discards staged edits without saving them.
Descriptors are copied; the file, provider state, backing bytes and buffers
remain borrowed until deinit. Do not mutate the raw file, replace its provider
or deinitialize it while settings is live. Route remote configuration through
the configuration commands/settings API so raw writes cannot bypass staging and
validation. After settings deinit, its owner may detach the file/provider.

## Contracts

One startup/main-loop/task owner serializes every call and all borrowed-provider
access. No API is ISR/fault safe. Provider operations execute synchronously and
may block; their complete cost belongs to the owner. With a Flash provider,
controller-wide serialization, XIP/interrupt/core safety, watchdog budgets,
power-loss testing and OTA quiescence remain the consumer's responsibility.
RAM providers have no implied survival across reset or power loss.

| State | Permitted behavior |
| --- | --- |
| CLEAN | Queries and edits; commit is a no-op without provider access. |
| DIRTY | Queries and edits; commit replaces the complete working snapshot. |
| UNCERTAIN | Queries, reload and deinit; set/remove/commit return STATE. |

Commit enters UNCERTAIN before calling `esy_file_replace()`. Success means the
selected atomic provider accepted the new complete snapshot; state becomes CLEAN.
An error may follow a committed replacement, so staged bytes and last-confirmed
`has_snapshot` are retained but do not establish current stored contents. Reload
may recover either complete old or complete new bytes. A batch of staged calls
is not automatically undone when a later call fails; check every result.

Reload uses scratch and publishes only after complete validation. Errors preserve
working bytes and observable state. A missing file creates an empty CLEAN working
snapshot with `has_snapshot=false`, without creating a file. An existing empty or
malformed file is an error. Removing all keys and committing creates a valid
empty snapshot with `has_snapshot=true`. Provider errors never trigger automatic
formatting or default writes. A selected dual Flash store provider may recover an
older valid revision according to its own interruption contract.

Deinit never saves, including UNCERTAIN state; it restores the zero handle.
Repeated zero deinit succeeds. Failed first init preserves the handle for retry,
although backing buffers may change. Repeated init cannot replace a live handle.
Do not copy, move or reset live handles. Buffers/handles and provider backing
storage must not overlap. Hidden provider workspace disjointness is a caller
precondition that the generic API cannot inspect.

The Settings encoding uses version 1 and is independent of storage. All
integers are little-endian; no pointer, native enum, padding or key terminator
is written. Integrity/durability are supplied by the chosen file provider.

| Offset | Bytes | Header field |
| --- | --- | --- |
| 0 | 4 | Magic `0x53595345` |
| 4 | 4 | Format version `1` |
| 8 | 4 | Entry count |
| 12 | 4 | Total bytes, including this 16-byte header |

Each entry is `key_length:u32, value_length:u32, key bytes, value bytes` without
alignment padding. Empty/duplicate/NUL-containing keys, truncation, inconsistent
counts/lengths and trailing bytes are rejected. Unknown format versions return
NOT_SUPPORTED. Product schema versions belong in product-defined values.

## Resource Model

Two caller buffers of `buffer_size` bytes each hold working and scratch snapshots.
Effective capacity is the lesser of that size and file capacity, bounded by
UINT32_MAX. The header costs 16 bytes; each entry costs `8 + key + value` bytes.
The file/provider owns any additional workspace; neither is initialized or
destroyed by Settings. There is no hidden committed copy, timer, pool or worker.

Key lookup is linear and duplicate validation is quadratic in the configured
small key set. No module timeout or software timebase is introduced.

## Verification

From `tests/`, run:

```sh
ceedling test:all
ceedling gcov:all
ruby ../../../tools/quality/run_module_tests.rb . build/sanitizer test:all --sanitizer
python3 -B test_cmake_consumer.py
```

Tests exercise staged values, absence/empty blobs, exact capacity, aliases,
lifecycle, unsupported/malformed snapshots, backend-independent RAM operation,
RAM-to-Flash byte transfer and complete old/new recovery through real dual Flash store
file adaptation with reusable Flash fault injection. Consumers compile C11/C++17
public headers, run a pure RAM build without HAL/Flash/dual Flash store/OS, and verify
feature isolation/dependency diagnostics. Host evidence does not prove hardware
power-loss behavior, Flash execution safety or watchdog timing.

## Known Limits

- The whole snapshot and scratch must fit in RAM; there is no append log,
  streaming schema, automatic migration, typed-value registry or wear scheduling.
- Atomic file replacement does not imply persistent media, authentication,
  encryption, anti-rollback or secure deletion. Choose providers accordingly.
- Raw file/provider changes while borrowed violate ownership and can invalidate
  staged assumptions. All configuration mutation uses the Settings API.
- A failed save requires successful reload or deinit/reinit before further saves.
