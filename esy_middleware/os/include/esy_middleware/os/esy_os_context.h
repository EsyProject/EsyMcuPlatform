/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_OS_CONTEXT_H
#define ESY_OS_CONTEXT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_os_context.h
 * @brief ESP-IDF execution-context boundary used by the system HAL backend.
 *
 * Link esy_os_context, a native-port target without a HAL dependency. These
 * primitives allocate no resources and are usable before scheduler startup.
 * IRQ masks affect only the calling core and do not serialize shared objects
 * across cores. Save/restore pairs must be nested on the same core; they mask
 * the native port's interrupt level, not unmaskable/high-priority exceptions.
 */

/** @brief Whether the calling ESP-IDF context is an ISR. */
bool esy_os_context_is_in_isr(void);
/** @brief Number of cores configured for the native scheduler. */
uint8_t esy_os_context_core_count(void);
/** @brief Mask local interrupts at the native port's critical level. */
void esy_os_context_irq_disable(void);
/** @brief Enable local interrupts; requires ownership of the current mask. */
void esy_os_context_irq_enable(void);
/** @brief Mask local interrupts and return the previous nesting token. */
uint32_t esy_os_context_irq_save(void);
/** @brief Restore a token returned by the matching local save operation. */
void esy_os_context_irq_restore(uint32_t saved_status);

#ifdef __cplusplus
}
#endif
#endif
