<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Common Utilities

## Purpose

`utils` provides reusable CPU-only calculations, encoding helpers and value
conversions. It has no hardware, OS or third-party provider dependency. Business
orchestration and capabilities that own runtime resources belong to their owning
modules rather than this directory.

## Public Surface

Public headers live below `include/esy_common/utils/`:

| Domain | Headers | Responsibility |
| --- | --- | --- |
| `base` | `esy_bits.h`, `esy_endian.h`, `esy_seq.h`, `esy_sort.h` | Bit/integer helpers, little-endian access, sequence comparisons and small-array sorting. |
| `integrity` | `esy_crc32.h`, `esy_sha256.h` | CRC32 calculations and software SHA-256 hashing. |
| `time` | `esy_datetime.h` | UTC calendar validation and Unix timestamp conversion. |

Implementation files follow the same domains: `src/base/esy_sort.c`,
`src/integrity/esy_crc32.c`, `src/integrity/esy_sha256.c` and
`src/time/esy_datetime.c`. Header-only helpers remain in their public headers.

## Integration

Link `esy_common_utils` or `Esy::common_utils`; the aggregate `esy_common` /
`Esy::common` also propagates it. There is one utility target for all domains,
with an `esy_common_defs` dependency and the CRC32 table feature input
`ENABLE_ESY_COMMON_UTILS_CRC32_TABLE`.
Use qualified includes through the exported include root:

```c
#include "esy_common/utils/base/esy_endian.h"
#include "esy_common/utils/integrity/esy_sha256.h"
#include "esy_common/utils/time/esy_datetime.h"
```

The exported include root is `esy_common/utils/include`. Direct source-file
references use the corresponding `src/<domain>/` locations listed above.

Ceedling configurations retain the include root for qualified includes and use
`include/esy_common/utils/**` to discover domain directories for generated runner
includes. Source discovery must include the implementation subdirectories;
explicit support-source paths point to their domain locations.

## Contracts

Each public header defines its buffer, range, ownership and concurrency
requirements. Sequence comparisons have explicit range constraints; SHA-256
defines initialization, restart and finalization behavior for caller-owned state.

## Resource Model

Helpers operate synchronously on caller-owned values, arrays, buffers or hash
contexts. They create no task, timer, hardware handle or provider resource and
perform no dynamic allocation. Input-dependent CPU and stack costs remain part
of each algorithm's use constraints; organizing a helper under `utils` does not
establish a general ISR latency guarantee.

## Verification

From `tests/`, run `ceedling test:all` for utility and UTC conversion regression
tests. The coverage configuration includes the implementation subdirectories.
From the platform repository root, run
`tools/checks/check_esy_common_contracts.sh` for C11/C++17 public-header checks.
Affected codec, HAL RTC, bootloader, OTA, time and TLS tests validate their use
of these APIs; an embedded consumer build verifies source and target assembly.

## Known Limits

The domains are organizational groups with one shared target, not independently
selectable modules.
