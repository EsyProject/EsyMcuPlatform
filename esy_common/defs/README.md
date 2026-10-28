<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Shared Definitions

## Purpose

`defs` owns definitions shared across platform layers and modules without
depending on HAL, hardware backends, board implementations, middleware or an OS.
It contains types, constants, callback signatures, operation tables, board port
declarations and compile-time capability requirements. Module-private definitions
remain with their owning module.

## Public Surface

All public headers follow `esy_<topic>_defs.h` under
`include/esy_common/defs/<domain>/`:

| Domain | Definitions |
| --- | --- |
| `base` | Common errors, constants, storage alignment and time values. |
| `peripheral` | ADC, CAN, DMA, Flash, GPIO, I2C, PWM, RTC, SPI, timers, UART, USB and Wi-Fi. |
| `peripheral/xio` | Programmable I/O and its SPI/UART configurations. |
| `system` | System configuration, multicore handles, power and GPIO power management. |
| `board` | Board resource queries and controller pad configuration interfaces. |
| `coordination` | Injected activity guards and atomic capability requirements. |
| `display` | Shared pixel format, orientation and touch sample values. |
| `image` | Boot layout, image manifest and image transaction status. |
| `diagnostics` | Diagnostic values, Coredump identity and fault capture definitions. |
| `logging` | Log severity levels. |
| `protocol` | Payload routing domain identifiers. |

Domains organize headers; they do not create separate build targets or select
runtime implementations.

## Integration

Link `esy_common_defs` or its alias `Esy::common_defs`. The aggregate
`esy_common` / `Esy::common` also propagates this target. Include the complete
public path, for example:

```c
#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_spi_defs.h"
#include "esy_common/defs/coordination/esy_activity_guard_defs.h"
```

Qualified includes use the exported include root. Source and test configuration
paths refer to `esy_common/defs/include`; public headers use the domain paths
listed above.

## Contracts

C type and function names, public enum/error values, structure layouts,
callback semantics and resource ownership are defined by each public header and
the [common contracts](../README.md#contracts).

## Resource Model

The target is header-only and owns no runtime resources. Embedded storage and
callback contexts retain their existing caller/provider ownership. Inline guard
helpers invoke the injected provider; this target supplies no hardware or OS
implementation.

## Verification

Run `tools/checks/check_esy_common_contracts.sh` from the platform repository root
for C11/C++17 header self-containment and public-value checks. The platform quality
gate validates module consumers and test assembly. An existing embedded consumer
build validates integration with the selected target toolchain.

## Known Limits

Header availability does not enable a peripheral, select a backend or establish
ISR safety. Runtime capabilities and concurrency requirements follow the relevant
HAL or middleware API.
