/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_core_lockout.h
 * @brief OS adapter for permanent peer-core pause workers.
 */
#ifndef ESY_CORE_LOCKOUT_H
#define ESY_CORE_LOCKOUT_H

#include "esy_middleware/core_lockout/esy_core_lockout_core.h"

/** @brief Maximum number of schedulable cores supported by the OS adapter. */
#define ESY_CORE_LOCKOUT_MAX_CORES 2u

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize permanent, pinned workers for peer-core coordination.
 * Single-schedulable-core configurations create no workers. Every physical peer
 * outside OS scheduling must remain inactive. Multicore configurations create
 * one worker per scheduled core, reused by repeated init and kept until reset.
 * There is no deinit. Worker priority defaults to 31; supply a compatible OS
 * priority range or override ESY_CORE_LOCKOUT_TASK_PRIORITY for this target.
 * The port must support RAM execution of worker and IRQ/event primitives.
 *
 * Initialization never accesses Flash or registers a safe-execute hook. Flash
 * consumers must explicitly bind esy_core_lockout_execute before mutations;
 * binding before init ensures execution is rejected while workers are unready.
 * On partial creation failure, existing workers remain allocated and execute
 * returns STATE. Retry the same init path to create missing workers; do not
 * switch mode or clear live storage. Successful init freezes guard configuration.
 * A guard release fault permanently rejects init and execution until reset.
 * See esy_core_lockout_execute for timeout, callback and concurrency semantics.
 * @retval ESY_OK Ready, with workers where needed.
 * @retval ESY_ERR_STATE ISR/reentry, mode mismatch or latched guard fault.
 * @retval ESY_ERR_NOT_SUPPORTED Schedulable core count exceeds the adapter limit.
 * @retval <0 Worker preparation/creation error; stop startup or retry this mode.
 * @note Externally serialize init, guard configuration and execution.
 */
esy_err_t esy_core_lockout_init(void);

#ifdef __cplusplus
}
#endif
#endif /* ESY_CORE_LOCKOUT_H */
