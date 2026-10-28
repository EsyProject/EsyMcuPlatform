/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HB_RESOURCE_ASSEMBLY_INTERNAL_H
#define ESY_HB_RESOURCE_ASSEMBLY_INTERNAL_H

#include <stdbool.h>

/* Task-only reservation shared by Pico DMA/XIO claims and native Wi-Fi calls
 * that may lazily allocate SDK resources. Contenders retry without waiting.
 * This does not disable IRQs or hold a hardware spinlock over SDK calls. */
bool esy_hb_resource_assembly_try_acquire(void);
void esy_hb_resource_assembly_release(void);

#endif /* ESY_HB_RESOURCE_ASSEMBLY_INTERNAL_H */
