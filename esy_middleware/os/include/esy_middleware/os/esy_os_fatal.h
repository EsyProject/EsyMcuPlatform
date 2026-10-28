/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_OS_FATAL_H
#define ESY_OS_FATAL_H

#include "esy_common/defs/diagnostics/esy_fault_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Immutable fatal callback, compatible with esy_hal_fault_raise.
 * The handler must never return, block, allocate, log, invoke OS APIs or read
 * the opaque detail addresses. Stack overflow can be reported with kernel locks
 * held. Handler/callees/data must remain valid in RAM for the complete boot.
 */
typedef void (*esy_os_fatal_handler_t)(esy_fault_reason_t reason,
                                      uint32_t detail0, uint32_t detail1);

/**
 * @brief Publish one immutable fatal handler from the serialized startup owner.
 * Call after esy_os_init(). Cortex ports require the scheduler not to have
 * started. ESP32's native app entry already runs under the SDK scheduler, so
 * its app startup owner binds before creating application-owned tasks instead.
 * Runtime registration after application startup is unsupported on all ports.
 * @return ESY_OK, INVALID_ARG for NULL, or STATE if uninitialized/already bound
 * or a Cortex scheduler has started. The owner serializes all binding attempts;
 * fatal readers on either core may run concurrently with the single publication.
 * The function pointer is copied; no allocation or worker is created. There is
 * no runtime unbind: the callback remains owned until reset. No handler preserves
 * the default disable-interrupts-and-halt behavior, including a returned handler.
 * A concurrent fatal event observes either the absent or fully published handler.
 */
esy_err_t esy_os_set_fatal_handler(esy_os_fatal_handler_t handler);

/**
 * @brief Report a failed assertion without dereferencing the file address.
 * @param file Opaque source-file pointer, passed as detail0.
 * @param line Source line, passed as detail1.
 * @warning Never returns. Details identify the assert report; this does not
 * recover an interrupted task's CPU frame. Callable from task/ISR/kernel fatal
 * contexts; dispatch reads the callback without a lock or OS API. An absent or
 * returned callback uses the native port's disable-interrupts-and-halt fallback.
 */
#if defined(__GNUC__)
__attribute__((noreturn)) ESY_FAULT_RAM_FUNC
#endif
void esy_os_assert_failed(const char *file, uint32_t line);

#ifdef __cplusplus
}
#endif
#endif /* ESY_OS_FATAL_H */
