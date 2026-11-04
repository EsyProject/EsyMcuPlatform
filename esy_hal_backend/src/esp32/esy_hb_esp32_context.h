/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HB_ESP32_CONTEXT_H
#define ESY_HB_ESP32_CONTEXT_H

#include <stdint.h>
#include "xtensa/config/core-isa.h"
#include "xtensa/corebits.h"
#include "../esy_hb_context_internal.h"

/* CPU-local IRQ tokens preserve the complete prior PS value. This is not an
 * inter-core lock and does not suspend a scheduler or acquire an OS object. */
static inline uint32_t esy_hb_esp32_irq_save(void) {
    uint32_t status;
    __asm volatile("rsil %0, %1" : "=a"(status) : "i"(XCHAL_EXCM_LEVEL) : "memory");
    return status;
}

static inline void esy_hb_esp32_irq_restore(uint32_t status) {
    __asm volatile("wsr %0, ps\n\trsync" :: "a"(status) : "memory");
}

static inline uint32_t esy_hb_esp32_status(void) {
    uint32_t status;
    __asm volatile("rsr %0, ps" : "=a"(status));
    return status;
}

static inline bool esy_hb_esp32_is_in_isr(void) {
    return esy_hb_context_is_in_isr(
        (esy_hb_esp32_status() & PS_INTLEVEL_MASK) != 0u);
}

static inline bool esy_hb_esp32_can_block(void) {
    return (esy_hb_esp32_status() & PS_INTLEVEL_MASK) == 0u &&
        esy_hb_context_can_block();
}

#endif /* ESY_HB_ESP32_CONTEXT_H */
