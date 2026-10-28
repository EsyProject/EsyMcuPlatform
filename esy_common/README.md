<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# esy_common

## Purpose

`esy_common` is the platform-independent definition, contract, and primitive helper
layer for EsyMcuPlatform. It is split into always-on CMake subtargets for
cross-module definitions, primitive utilities and the explicit device graph. It contains error codes, common
sentinels, peripheral configuration structures, opaque handle
layouts, backend vtables, board/system query contracts, and small
deterministic helper algorithms.

The module provides only platform-independent behavior. It exists to keep
shared public contracts stable across board, HAL backend, HAL, middleware, and
application code without introducing dependencies on those upper layers.

## Public Surface

Public headers are grouped by submodule:

- `defs/include/esy_common/defs/`: [shared definitions grouped by domain](defs/README.md),
  including base, peripheral, system, board, coordination, display, image,
  diagnostics, logging and protocol definitions. Every header uses `_defs.h`.
- `utils/include/esy_common/utils/`: [primitive helper APIs grouped by domain](utils/README.md),
  with `base`, `integrity` and `time` headers and matching implementation directories.
- `device/include/esy_common/device/`: explicit device graph lifecycle and
  resource-reference contracts.
- `smf/include/esy_common/smf/`: optional caller-driven flat and hierarchical
  [state machine framework](smf/README.md).
- `protocol_codec/include/esy_common/protocol/`: optional wire codecs and
  [JSON views/generation](protocol_codec/README.md).

The codec directory is named `protocol_codec` and uses the public header
namespace `esy_common/protocol/...`. Middleware protocol sessions, managers and
transport channels belong to `esy_middleware/protocol`.

Always-on targets:

- `esy_common_defs`: base common definitions, board, peripheral, system,
  handle, and XIO public type contracts.
- `esy_common_utils`: primitive helpers such as bits, endian, sequence, CRC32,
  and small sorting helpers.
- `esy_common_device`: device graph and lifecycle coordination.
- `esy_common`: aggregate target that links the always-on common targets and
  any enabled optional common targets.

`esy_common_smf` (`Esy::common_smf`) is available when the consumer selects
`ENABLE_ESY_COMMON_SMF`. It provides entry/run/exit actions, initial children,
event propagation and external transitions without an OS or hardware provider.

`esy_common_protocol_json` (`Esy::protocol_json`) is available with
`ENABLE_ESY_COMMON_PROTOCOL` and its child `ENABLE_ESY_COMMON_PROTOCOL_JSON`.
It adds allocation-free JSON views, exact 64-bit integer conversion and bounded
generation using a private coreJSON v3.3.1 software provider. The separate
`esy_common_protocol_codec` target has no coreJSON dependency.

The public surface includes:

- shared error and status types;
- common sentinel values and capability flags;
- board/system query contracts such as `esy_system_config_t` and
  `esy_bootloader_layout_t`;
- peripheral configuration structures;
- public handle layouts;
- backend vtable type declarations;
- common bit, endian, sequence, CRC32, and small sorting helpers;
- XIO common definitions under `esy_common/defs/peripheral/xio/`.

## Integration

This module is consumed through the aggregate `esy_common` CMake target, or by
linking the narrow common subtarget directly when a module only needs part of
the common surface:

```cmake
target_link_libraries(my_module PRIVATE esy_common)
# A utility-only consumer can instead link esy_common_utils.
```

These targets must already be assembled by the consuming project's
`add_subdirectory(external/EsyMcuPlatform)` call, with the
[required integration inputs](../README.md#quick-start). A narrow common-only
project may instead add `esy_common` directly; set its optional feature
variables before doing so and use a C11-capable compiler. The common subtree
itself does not require a board, hardware SDK or OS provider. Optional DS and
protocol codecs also work in this mode. Common targets link `esy_opt_default`
only when that target already exists; a direct common-only consumer owns its
compiler and optimization flags.

The optional state machine framework works in the same common-only mode.
Enable it in the selected consumer feature file with
`set(ENABLE_ESY_COMMON_SMF ON)` and link `Esy::common_smf`, or consume it through
`Esy::common`. An unset or false feature excludes its source and public include
path from the aggregate. The framework does not replace existing module state
machines or device power-management contracts.

Downstream modules should include the narrow header that owns the required type
or helper.

`esy_lcd_defs.h` contains shared pixel-format and orientation values;
`esy_touch_defs.h` contains the touch sample value. Complete LCD/touch runtime
handles, their storage capacity macros and touch SPI callbacks belong to the
respective middleware facade headers, `esy_lcd.h` and `esy_touch.h`. Common does
not include or depend on those runtime headers.

`esy_bootloader_layout_t` is a neutral value type in `esy_boot_defs.h`. Public
HAL and middleware headers use it without including board query declarations.
`esy_flash_defs.h` separately defines `esy_flash_geometry_t` (accessible base,
capacity and required programming granularity) and `esy_flash_sector_t` (one
complete erase sector). Flash geometry belongs to the hardware port and may be
nonuniform; product image/metadata regions remain a separate boot layout.

## Feature Policy

`esy_common_defs`, `esy_common_utils` and `esy_common_device` are always built. They form the
dependency root for HAL, HAL backend, middleware, tests, and downstream
applications, so they must not be disabled by feature gates.

Optional heavier common modules may be added under their own subdirectories and
selected by consuming-project feature variables such as `ENABLE_ESY_COMMON_DS`.
EsyMcuPlatform only consumes those variables; it does not define feature defaults.
`ENABLE_ESY_COMMON_SMF` independently selects the state machine framework and
does not require DS, protocol, OS, HAL or board support.

## Contracts

Public headers in this module are consumed by board, HAL backend, HAL,
middleware, and application code. Treat these items as public contracts:

- numeric values of shared error codes and externally visible enums;
- public macro values such as unused-pin sentinels and capability flags;
- board query structures that define cross-module ownership, such as
  `esy_system_config_t` and `esy_bootloader_layout_t`;
- handle field order where backend implementations initialize or inspect
  `ops`, `ctx`, and in-place backend storage.

Do not lock these items in `esy_common` self checks unless a separate design
review promotes them to public contracts:

- internal implementation details;
- opaque backend storage sizes such as `ESY_SPI_PRIV_BYTES`;
- backend vtable field order.

Breaking public contract changes require deliberate review, an appropriate
major-version increment and documented consumer impacts.

The XIO SPI and UART configurations expose caller-selected `instance` and
`sm_index` fields directly.

## Resource Model

`esy_common` performs no dynamic allocation and has no OS, HAL backend, HAL,
board, middleware, or application dependency. Helper APIs operate on
caller-owned buffers and deterministic CPU-only state. Types that embed backend
storage still use caller-owned object storage; backend implementations decide
how to use their opaque bytes.

DS and protocol init/reset functions document their first-storage and retry
requirements in their public headers. Reinitialization is a bookkeeping reset,
not asynchronous cancellation or resource cleanup: stop all endpoints and retire
borrowed blocks, spans, nodes and frame-buffer users first. A rejected first init
does not create a usable object; follow the specific API's retry contract rather
than treating reset or deinit as a universal recovery operation.

SHA-256 uses caller-owned streaming state or a one-shot stack context. Initial
storage may contain arbitrary bytes because `esy_sha256_init` writes every field
before use; calling init again discards partial input. Update consumes its input
synchronously. Final writes the digest and clears state, after which another init
is required. A context has one serialized owner and must not overlap streaming
input/output storage. See `esy_sha256.h` for NULL, length and ISR budget contracts.

## Verification

Run the header contract checks from the EsyMcuPlatform repository root:

```sh
tools/checks/check_esy_common_contracts.sh
```

The check verifies public header self-containment and C++ compatibility, shared
display/input value contracts, and the absence of LCD/touch runtime declarations.
The LCD and touch module consumer tests own their runtime public-header checks.

The utility suite checks SHA-256 against independent known answers at the
55/56/63/64/65-byte padding boundaries, additional multi-block boundaries and
1024 bytes, using both one-shot and irregular streaming updates. It also checks
arbitrary initial bytes, restart, invalid-input preservation and final clearing.

The [state machine suite](smf/README.md#verification) verifies hierarchical
transition order, lifecycle recovery, callback reentry rejection, and independent
C11/C++17 consumers with the feature enabled, disabled and unset.

## Known Limits

- Header compatibility checks are intentionally narrow and do not lock all
  public macro values.
- Buffer sizes and vtable order are not treated as stable contracts by default.
- CRC32 table acceleration is controlled by
  `ENABLE_ESY_COMMON_UTILS_CRC32_TABLE`.
