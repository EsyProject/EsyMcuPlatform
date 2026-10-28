<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Changelog

## [0.1.0] - 2026-10-28

### Added

- Initial Apache-2.0 source release of the reusable C11 platform library.
- Public hardware abstraction APIs, device selection and CMake integration
  for Pico/RP2040, Pico 2/RP2350 Arm, selected STM32F4 devices and a partial
  classic ESP32 backend. Peripheral support follows the backend capability
  matrix; inclusion of a device does not imply support for every feature.
- FreeRTOS wrappers, static or caller-owned resource contracts, common
  containers, state machines and protocol codecs.
- Optional middleware for logging, diagnostics, watchdog supervision,
  bootloader and OTA composition, storage, protocols, connectivity and GUI
  integration, with explicit provider and feature dependencies.
- Module-owned host tests, reusable test support, contract checks, coverage
  gates and locked host-test provider setup.
- Integration, portability, resource ownership and hardware verification
  documentation.

### Compatibility and Integration

- This is the initial 0.x release. Public API and ABI stability is not yet
  guaranteed; consumers should pin a tested release commit and review changes
  before upgrading.
- Consumers own device and feature selection, board resources, SDK/RTOS
  providers, application startup and the final firmware link.
- Library-owned static storage does not eliminate allocations inside SDKs,
  third-party providers or consumer applications. See the documented memory
  model for those boundaries.

### Known Limits

- This is a source release. It does not include a universal firmware image or
  prebuilt archive suitable for every consumer configuration.
- Host tests and cross-builds do not establish hardware timing, peripheral
  behavior, Flash power-loss recovery or production readiness. Hardware smoke
  and HIL acceptance remain specific to the selected device and configuration.
- See the backend capability matrix and individual module documentation for
  unsupported operations, provider requirements and execution contracts.
