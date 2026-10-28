/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HB_INTERNAL_H
#define ESY_HB_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((always_inline)) inline void esy_cpu_wfi(void) {
  __asm volatile(
      "dsb\n"
      "wfi\n"
      "isb"
      :
      :
      : "memory");
}

__attribute__((always_inline)) inline void esy_disable_irq(void) {
  __asm volatile("cpsid i" : : : "memory");
}

__attribute__((always_inline)) inline void esy_enable_irq(void) {
  __asm volatile("cpsie i" : : : "memory");
}

__attribute__((always_inline)) inline bool esy_is_in_isr(void) {
  uint32_t ipsr;
  __asm volatile("MRS %0, ipsr" : "=r"(ipsr));
  return (ipsr != 0u);
}

#ifdef __cplusplus
}
#endif

#endif /* ESY_HB_INTERNAL_H */
