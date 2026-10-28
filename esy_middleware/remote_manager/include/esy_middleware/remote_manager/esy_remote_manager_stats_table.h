/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_REMOTE_MANAGER_STATS_TABLE_H
#define ESY_REMOTE_MANAGER_STATS_TABLE_H

#include "esy_middleware/remote_manager/esy_remote_manager_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Immutable descriptor borrowing one owner-updated RAM sample. */
typedef struct {
    const char *name; /**< Borrowed canonical ASCII span; terminator not required. */
    size_t name_length; /**< Positive name bytes, at most NAME_MAX_BYTES. */
    esy_remote_manager_stats_kind_t kind; /**< Counter or gauge. */
    esy_remote_manager_stats_unit_t unit; /**< Exact sample unit. */
    const esy_remote_manager_stats_sample_t *sample; /**< Borrowed, coherently updated RAM value. */
} esy_remote_manager_stats_entry_t;

/** @brief Copied binding of a caller-owned, immutable descriptor array. */
typedef struct {
    const esy_remote_manager_stats_entry_t *entries; /**< NULL permitted only for an empty table. */
    size_t count; /**< Entries, at most UINT32_MAX; caller budgets validation/search cost. */
} esy_remote_manager_stats_table_config_t;

/**
 * @brief Caller-owned table adapter requiring {0} before first init.
 * Fields are private by contract. No heap, task, clock, lock or Flash operation
 * is created. Do not copy/move/reset a live handle. All entries, names and sample
 * storage remain valid until successful detach of every copied ops borrower.
 */
typedef struct {
    esy_remote_manager_stats_table_config_t config; /**< Private copied binding. */
    bool initialized; /**< Private lifecycle flag. */
} esy_remote_manager_stats_table_t;

/**
 * @brief Bind a RAM table, checking names, units, kinds and duplicate identities.
 * Validation is O(count squared); no arbitrary table capacity is imposed. Names
 * and descriptors remain immutable until detach. A single non-ISR owner
 * serializes every operation with complete sample updates; asynchronous sources
 * first hand off snapshots to this owner rather than racing raw sample fields.
 * The table copies one live sample at query time, masking unavailable values to
 * zero. Samples need not be available at init. Invalid live percentages return
 * STATE when read. Enumeration costs O(1), exact-name search costs O(count).
 * No provider lifecycle or sample modification occurs.
 * @return OK; INVALID_ARG for NULL, invalid descriptors or duplicate names;
 * STATE for repeated init. Failure preserves the original handle and borrows
 * nothing new. Init accepts zero or successfully deinitialized storage only.
 */
esy_err_t esy_remote_manager_stats_table_init(
    esy_remote_manager_stats_table_t *table,
    const esy_remote_manager_stats_table_config_t *config);

/**
 * @brief Copy ENUMERATE/READ operations borrowing this live table.
 * Stop requests and detach the manager before table deinit or releasing borrowed
 * descriptors/samples. Returned callbacks reject detached table state. No
 * manager or sample is retained beyond the table's explicit borrowed bindings.
 * @return OK; INVALID_ARG for NULL; STATE before init/after deinit.
 */
esy_err_t esy_remote_manager_stats_table_get_ops(
    esy_remote_manager_stats_table_t *table, esy_remote_manager_stats_ops_t *ops);

/**
 * @brief Detach borrowed RAM without clearing, saving or resetting measurements.
 * One serialized owner calls after all copied operations have detached.
 * @return INVALID_ARG for NULL; otherwise OK, including repeated deinit.
 * Success restores zero storage and permits reinitialization.
 */
esy_err_t esy_remote_manager_stats_table_deinit(esy_remote_manager_stats_table_t *table);

#ifdef __cplusplus
}
#endif

#endif /* ESY_REMOTE_MANAGER_STATS_TABLE_H */
