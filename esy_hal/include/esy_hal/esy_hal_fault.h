/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_FAULT_H
#define ESY_HAL_FAULT_H

#include "esy_common/defs/diagnostics/esy_fault_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install one immutable boot-lifetime fatal binding during port startup.
 * @param config Required callback/context and bounded stack policy, copied.
 * @return ESY_OK, INVALID_ARG, STATE for repeated init, BUSY for existing vector
 * ownership, or NOT_SUPPORTED for an unsupported execution/vector environment.
 * @pre Cortex: privileged startup on core 0 before scheduler/peer startup.
 * ESP32: native app startup, before application-owned tasks start; the native
 * scheduler/SDK cores may already run. One serialized registration owner.
 * Cortex ports require no MPU; RP2350 requires secure-only
 * execution with no security transitions. STM32 initially supports verified
 * STM32F405/407/415/417 main-SRAM layouts. Check the ESP32 integration contract
 * for native panic configuration and execution requirements.
 * The callback, its callees/data and context remain in SRAM until reset. Existing
 * vector owners are preserved. No deinit is provided: this owns fatal vectors
 * until reset. Cortex ports own one fixed emergency stack per core; RP2350 uses
 * a native atomic claim, RP2040 reserves one SDK hardware spinlock at startup
 * and only tries it once during fatal capture, and STM32 has one core. ESP32
 * retains its native panic stack and policy. No heap or task is created.
 */
esy_err_t esy_hal_fault_init(const esy_fault_config_t *config);

/**
 * @brief Raise a fatal software event; never returns, including before init.
 * @param reason Event classification; invalid values become SOFTWARE.
 * @param detail0 Opaque 32-bit detail, never dereferenced by this API.
 * @param detail1 Opaque 32-bit detail, never dereferenced by this API.
 * @warning Fatal contexts only. Available in SRAM on supported ports; captures
 * through the installed handler, or follows its native fatal policy if uninitialized.
 * Other simultaneous fatal cores park; the first claimant resets after capture.
 * A corrupted/failed capture path still requires the hardware watchdog backup.
 */
#if defined(__GNUC__)
__attribute__((noreturn)) ESY_FAULT_RAM_FUNC
#endif
void esy_hal_fault_raise(esy_fault_reason_t reason, uint32_t detail0,
                         uint32_t detail1);

#ifdef __cplusplus
}
#endif
#endif /* ESY_HAL_FAULT_H */
