/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_DIAGNOSTICS_H
#define ESY_DIAGNOSTICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/diagnostics/esy_diagnostics_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_diagnostics.h
 * @brief Caller-owned named health queries and explicitly requested self-tests.
 *
 * One non-ISR task/main-loop owner serializes all APIs, callbacks, source updates
 * and lifecycle changes. A busy guard rejects synchronous callback reentry and
 * is not a lock. Cross-task sources use their owner's coherent snapshot/request
 * interface; this module never executes another task's private business flow.
 * Callbacks execute synchronously on the caller, must return within the owner's
 * declared execution budget and cannot reenter this instance. RUN completes one
 * self-test before returning; there is no asynchronous job, scheduler, timebase,
 * timeout, task, storage, hardware initialization or allocation. The consumer
 * supplies source implementations and authorization/admission for self-tests,
 * including OTA/Flash/output safety and any necessary quiescence.
 */

/**
 * @brief One immutable diagnostic registration, borrowed until deinit.
 *
 * name is a NUL-terminated canonical name of 1..48 bytes. read is required and
 * must not start a self-test or mutate controlled outputs. run is optional and
 * returns a completed assessment. Both callbacks return OK or a negative error;
 * positive values and malformed assessments become STATE. The callback result
 * pointer is borrowed only until return and must not be retained. A completed test
 * detecting a fault returns OK with ERROR health, not an operation error. A
 * failed RUN may already have effects: reconcile through READ/owner state and
 * never retry automatically. context may be NULL when its source permits it.
 */
typedef struct {
    const char *name; /**< Immutable canonical name and stable source identity. */
    esy_err_t (*read)(void *context, esy_diagnostics_result_t *result); /**< Query callback. */
    esy_err_t (*run)(void *context, esy_diagnostics_result_t *result); /**< Optional self-test. */
    void *context; /**< Borrowed source state, kept alive with the registration. */
} esy_diagnostics_entry_t;

/** @brief Immutable caller-owned inventory; descriptors/names are borrowed. */
typedef struct {
    const esy_diagnostics_entry_t *entries; /**< Array; NULL permitted only for count zero. */
    size_t count; /**< Entry count, at most UINT32_MAX; duplicate names are invalid. */
} esy_diagnostics_static_resources_t;

/** @brief Initialization descriptor, copied without querying any source. */
typedef struct {
    const esy_diagnostics_static_resources_t *static_resources; /**< Required inventory descriptor. */
} esy_diagnostics_config_t;

/**
 * @brief Caller-owned handle, requiring {0} before first initialization.
 * Fields are private by contract. Do not copy/move/reset a live handle. Stop
 * every frontend before deinit; borrowed entries, names and callback contexts
 * must remain valid and immutable until then. Outputs must not overlap this
 * handle, inventory, registered names, lookup name or source-owned state.
 * Lookup names may refer directly to immutable registered names.
 */
typedef struct {
    esy_diagnostics_static_resources_t resources; /**< Private copied inventory binding. */
    bool initialized; /**< Private lifecycle state. */
    bool busy; /**< Private callback reentry guard. */
} esy_diagnostics_t;

/**
 * @brief Validate and borrow one inventory without invoking callbacks.
 * @return OK, INVALID_ARG for configuration/name/duplicate errors, STATE for
 * repeated initialization or BUSY during a callback. Failure preserves handle.
 */
esy_err_t esy_diagnostics_init(esy_diagnostics_t *diagnostics,
                              const esy_diagnostics_config_t *config);
/** @brief Detach without querying/testing; zero/repeated deinit succeeds.
 * @return INVALID_ARG for NULL, BUSY during a callback, otherwise OK; resets to zero. */
esy_err_t esy_diagnostics_deinit(esy_diagnostics_t *diagnostics);
/**
 * @brief Copy one descriptor without source callbacks; empty index zero succeeds.
 * @return OK, INVALID_ARG for pointers/index, STATE if detached or BUSY on reentry.
 * Output remains unchanged on error. Never starts a test or queries health.
 */
esy_err_t esy_diagnostics_enumerate(esy_diagnostics_t *diagnostics, uint32_t index,
                                   esy_diagnostics_snapshot_t *snapshot);
/**
 * @brief Query one exact canonical name with one source callback.
 * @return OK, INVALID_ARG for invalid pointers/name, STATE for detached/malformed
 * provider result, BUSY on reentry, NOT_SUPPORTED for an unknown name, or a
 * negative provider error. Output remains unchanged on error.
 */
esy_err_t esy_diagnostics_read(esy_diagnostics_t *diagnostics,
                              const char *name, size_t name_length,
                              esy_diagnostics_snapshot_t *snapshot);
/**
 * @brief Explicitly execute one synchronous self-test with one source callback.
 * @return As READ; NOT_SUPPORTED also when RUN is unavailable. UNKNOWN health
 * from RUN is a malformed completed assessment (STATE). Output is unchanged on
 * error, though the provider may already have effects. No automatic retry.
 */
esy_err_t esy_diagnostics_run(esy_diagnostics_t *diagnostics,
                             const char *name, size_t name_length,
                             esy_diagnostics_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif

#endif /* ESY_DIAGNOSTICS_H */
