/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_STORAGE_INTERNAL_H
#define ESY_HAL_STORAGE_INTERNAL_H

/* Private runtime objects overlay caller-owned byte arrays. Size and alignment
 * alone do not make these accesses valid under type-based alias analysis.
 * GCC and Clang provide this explicit aliasing contract without changing layout
 * or the atomic operations used by the UART DMA lifecycle. Apply it to every
 * runtime type accessed through an independent pointer, including nested types.
 * Other subobjects must be copied to typed locals before their address escapes.
 * A new compiler port must supply an equivalent contract rather than silently
 * omitting the attribute. */
#if defined(__has_attribute)
#if !__has_attribute(__may_alias__)
#error "HAL private storage requires compiler support for may_alias"
#endif
#elif !defined(__GNUC__)
#error "HAL private storage requires compiler support for may_alias"
#endif

#define ESY_HAL_MAY_ALIAS __attribute__((__may_alias__))

#endif /* ESY_HAL_STORAGE_INTERNAL_H */
