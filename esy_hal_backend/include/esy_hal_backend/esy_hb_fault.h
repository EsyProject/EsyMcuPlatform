/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HB_FAULT_H
#define ESY_HB_FAULT_H
#include "esy_common/defs/diagnostics/esy_fault_defs.h"

/** @brief Install the RP2350 secure-only fatal port; see the public HAL contract. */
esy_err_t esy_hb_fault_init(const esy_fault_config_t *config);
/** @brief Enter the fatal port without using a caller stack; never returns. */
#if defined(__GNUC__)
__attribute__((noreturn))
#endif
void esy_hb_fault_raise(esy_fault_reason_t reason, uint32_t detail0,
                        uint32_t detail1);
#endif /* ESY_HB_FAULT_H */
