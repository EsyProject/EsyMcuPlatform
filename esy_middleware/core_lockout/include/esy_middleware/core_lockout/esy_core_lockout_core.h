/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_core_lockout_core.h
 * @brief Synchronous core coordination and local interrupt protection without OS.
 */
#ifndef ESY_CORE_LOCKOUT_CORE_H
#define ESY_CORE_LOCKOUT_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include "esy_common/defs/coordination/esy_activity_guard_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Synchronous operation executed inside the IRQ-masked safe window.
 * @param[in] arg Borrowed argument, optionally NULL, valid until execution returns.
 * The callback must not block, reenter core lockout, or depend on peer execution
 * or interrupts. The provider owns code/data placement when XIP is unavailable;
 * all required instructions and data must remain accessible throughout the call.
 * Operation-specific results are communicated through arg, not this void return.
 */
typedef void (*esy_core_lockout_operation_t)(void *arg);

/**
 * @brief Initialize single-active-core protection without OS resources.
 * Every peer must remain inactive until reset. Only the initializing core may
 * execute operations in this mode; physical core count cannot verify inactivity.
 * Active scheduled peers require esy_core_lockout_init() from the OS adapter.
 * Initialization never accesses Flash or installs a HAL hook.
 * Repeated initialization on the owner core is allowed. Mode/owner is fixed
 * until reset; switching mode after selection, including partial OS preparation,
 * returns STATE. Successful init freezes the optional guard. No deinit exists.
 * @retval ESY_OK Ready to execute operations without allocating workers.
 * @retval ESY_ERR_STATE ISR/reentry, invalid core, mode/owner mismatch or guard fault.
 * @note Externally serialize initialization, guard configuration and execution.
 */
esy_err_t esy_core_lockout_core_init(void);

/**
 * @brief Execute one synchronous operation while local IRQs and peers are paused.
 * @param[in] operation Callback; NULL is invalid.
 * @param[in] arg Borrowed callback argument; may be NULL.
 * Acquire the optional guard, coordinate peers when using the OS adapter, mask
 * local IRQs, invoke operation once, restore IRQs, release peers, then attempt
 * guard release once. Guard callbacks run outside the local IRQ-masked window.
 * Calls must be externally serialized, non-reentrant and outside ISR context.
 * Once the scheduler runs, call from task context with IRQs enabled. Before
 * scheduler start, every peer must be inactive. The caller owns DMA/XIP-user
 * quiescence, operation scheduling and watchdog/IRQ latency budgets. This module
 * adds no mutex and guarantees no hard elapsed-time bound or deferred completion.
 * A peer lock timeout prevents execution and waits once more for cancellation.
 * An undrained cancellation blocks new operations with BUSY; retry nudges workers.
 * A peer release timeout may occur after execution: do not blindly replay it.
 * Guard acquire failure prevents execution and receives no release attempt.
 * Guard release failure overrides other errors and latches STATE until reset;
 * retain borrowed resources and use the fatal/reset path without cleanup retries.
 * This function can be explicitly registered as a HAL Flash safe-execute
 * hook by a consumer; initialization does not register it automatically.
 * @retval ESY_OK Operation and coordination completed; inspect arg for its result.
 * @retval ESY_ERR_INVALID_ARG operation is NULL.
 * @retval ESY_ERR_STATE Not ready, ISR/reentry, wrong owner or latched guard fault.
 * @retval ESY_ERR_BUSY A canceled worker request remains outstanding.
 * @retval ESY_ERR_TIMEOUT Peer lock or release acknowledgement timed out.
 * @retval <0 Guard acquisition or peer notification/coordination error.
 */
esy_err_t esy_core_lockout_execute(esy_core_lockout_operation_t operation, void *arg);

/**
 * @brief Configure the optional guard around single- or multicore operations.
 * @param[in] guard Borrowed descriptor; NULL disables it before successful init.
 * The descriptor is retained, not copied. Keep its fields unchanged and its
 * storage, callbacks and ctx valid until successful replacement/disable before
 * init or system reset. Invalid replacement and init failure preserve the borrow.
 * Successful init freezes configuration until reset, including after a fault.
 * Callbacks run synchronously in the caller context outside local IRQ masking;
 * they must support startup if execution occurs before scheduler start, and must
 * not reenter execution, initialization or guard configuration. An acquire error
 * must leave no ownership behind. Each successful acquire gets one release
 * attempt even when lockout fails. Release failure is permanently latched;
 * there is no safe generic retry, unregister or recovery API.
 * @retval ESY_OK Configuration accepted.
 * @retval ESY_ERR_INVALID_ARG Non-NULL guard has missing ctx or callbacks.
 * @retval ESY_ERR_STATE ISR/reentry, frozen configuration or latched guard fault.
 * @note Externally serialize with init/execution; provider synchronizes its ctx.
 */
esy_err_t esy_core_lockout_set_activity_guard(const esy_activity_guard_t *guard);

#ifdef __cplusplus
}
#endif
#endif /* ESY_CORE_LOCKOUT_CORE_H */
