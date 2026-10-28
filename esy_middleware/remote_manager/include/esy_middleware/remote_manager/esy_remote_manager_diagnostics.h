/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ESY_REMOTE_MANAGER_DIAGNOSTICS_H
#define ESY_REMOTE_MANAGER_DIAGNOSTICS_H

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/diagnostics/esy_diagnostics_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief ENUMERATE argument content: one zero-based source index. */
#define ESY_REMOTE_MANAGER_DIAGNOSTICS_INDEX_BYTES 4u

/** @brief Structured health queries and explicitly requested self-tests. */
typedef enum {
    ESY_REMOTE_MANAGER_DIAGNOSTICS_ENUMERATE = 1, /**< Metadata only; never queries/tests. */
    ESY_REMOTE_MANAGER_DIAGNOSTICS_READ = 2, /**< Query the named source without testing. */
    ESY_REMOTE_MANAGER_DIAGNOSTICS_RUN = 3, /**< Execute one explicit synchronous self-test. */
} esy_remote_manager_diagnostics_command_t;

/**
 * @brief Optional diagnostic-owner operations; no source lifecycle is owned.
 * One non-ISR owner serializes every call with producer updates, lifecycle and
 * any admitted self-test effects. Each dispatch authorizes then calls at most
 * one operation synchronously. Inputs/outputs are borrowed only until return;
 * context and dependencies outlive all copied operations. No callback reentry.
 * Names are canonical explicit spans bounded by ESY_DIAGNOSTICS_NAME_MAX_BYTES.
 * ENUMERATE returns metadata and a zero result without invoking a test/query.
 * READ and RUN match the complete exact name. Unknown/unavailable operations
 * return NOT_SUPPORTED. Assessments follow esy_diagnostics_snapshot_t; detecting
 * an unhealthy condition succeeds with ERROR health. Positive callback statuses
 * or malformed results become STATE. A failed RUN may already have effects;
 * reconcile owner state or READ rather than automatically retrying. The owner
 * supplies execution budgets, test admission, authentication and OTA/Flash/output
 * safety. No task, timeout, storage or implicit self-test is introduced.
 */
typedef struct {
    esy_err_t (*enumerate)(void *context, uint32_t index,
                           esy_diagnostics_snapshot_t *snapshot); /**< Copy one descriptor. */
    esy_err_t (*read)(void *context, const char *name, size_t name_length,
                      esy_diagnostics_snapshot_t *snapshot); /**< Read current health. */
    esy_err_t (*run)(void *context, const char *name, size_t name_length,
                     esy_diagnostics_snapshot_t *snapshot); /**< Execute one explicit test. */
    void *context; /**< Borrowed diagnostic owner; NULL is permitted. */
} esy_remote_manager_diagnostics_ops_t;

#ifdef __cplusplus
}
#endif
#endif /* ESY_REMOTE_MANAGER_DIAGNOSTICS_H */
