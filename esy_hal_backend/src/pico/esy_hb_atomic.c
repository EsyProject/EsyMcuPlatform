/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* SDK atomic operations serialize task/ISR and dual-core callers. The SDK
 * masks local interrupts before locking and restores the previous IRQ state.
 * Only short primitive operations may hold this lock; NMI/fault and core
 * reset/halt while a lock is held are outside the provider contract. */
#include <stdatomic.h>
#include "pico.h"
#include "hardware/sync.h"

_Static_assert(PICO_RP2040, "the IRQ-spinlock provider is audited for RP2040");
_Static_assert(sizeof(unsigned int) == 4u, "SDK 32-bit atomic helper ABI required");
_Static_assert(PICO_SPINLOCK_ID_ATOMIC >= 0 &&
               PICO_SPINLOCK_ID_ATOMIC < NUM_SPIN_LOCKS,
               "the SDK atomic lock must be a valid hardware lock");
_Static_assert(PICO_SPINLOCK_ID_ATOMIC != PICO_SPINLOCK_ID_OS1 &&
               PICO_SPINLOCK_ID_ATOMIC != PICO_SPINLOCK_ID_OS2,
               "the SDK atomic lock must not overlap RTOS-reserved locks");

#if PICO_USE_SW_SPIN_LOCKS
#error "RP2040 atomics require SDK hardware spinlocks"
#endif
_Static_assert(PICO_SPINLOCK_ID_ATOMIC != PICO_SPINLOCK_ID_IRQ &&
               PICO_SPINLOCK_ID_ATOMIC != PICO_SPINLOCK_ID_TIMER &&
               PICO_SPINLOCK_ID_ATOMIC != PICO_SPINLOCK_ID_HARDWARE_CLAIM &&
               PICO_SPINLOCK_ID_ATOMIC != PICO_SPINLOCK_ID_RAND,
               "the SDK atomic lock must not overlap other SDK lock owners");
_Static_assert(PICO_SPINLOCK_ID_ATOMIC < PICO_SPINLOCK_ID_STRIPED_FIRST ||
               PICO_SPINLOCK_ID_ATOMIC > PICO_SPINLOCK_ID_STRIPED_LAST,
               "the SDK atomic lock must not overlap shared striped locks");
_Static_assert(PICO_SPINLOCK_ID_ATOMIC < PICO_SPINLOCK_ID_CLAIM_FREE_FIRST ||
               PICO_SPINLOCK_ID_ATOMIC > PICO_SPINLOCK_ID_CLAIM_FREE_LAST,
               "the SDK atomic lock must not overlap dynamically claimed locks");

#if PICO_C_COMPILER_IS_GNU
/* GCC emits this ABI symbol for atomic_flag callers using the standard C
 * header. Pico SDK exports the equivalent operation under a different name.
 * This private runtime entry is consumed by compiler-generated calls; it is
 * not a firmware API. Delegate the entire operation to the SDK IRQ-safe helper. */
bool esy_hb_atomic_test_and_set(volatile void *address, int memory_order)
    __asm__("__atomic_test_and_set");

bool esy_hb_atomic_test_and_set(volatile void *address, int memory_order) {
  return __atomic_test_and_set_c(address, memory_order);
}
#endif
