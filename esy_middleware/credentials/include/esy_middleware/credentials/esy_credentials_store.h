/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_CREDENTIALS_STORE_H
#define ESY_CREDENTIALS_STORE_H

#include <stdbool.h>
#include "esy_middleware/credentials/esy_credentials.h"
#include "esy_middleware/file/esy_file.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_credentials_store.h
 * @brief Whole-bundle credential persistence over an atomic replaceable file.
 *
 * One normal startup/task/main-loop owner serializes all APIs, provider calls
 * and underlying storage users. No ISR/fault support, OS, clock, deadline,
 * background worker, cryptographic validation or update authorization is added.
 * Calls are synchronous, including provider erase/program/sync. The owner
 * establishes Flash/OTA exclusion, watchdog budgets and approved update policy.
 * The store never starts/stops TLS or owns a live esy_credentials_t instance.
 *
 * A snapshot contains the confirmed bundle AND an optional candidate. The
 * provider atomically replaces that complete snapshot. Boot selects confirmed
 * explicitly; a staged candidate is never automatically confirmed. Bootstrap
 * uses stage followed by explicit confirm, also when no confirmed bundle exists.
 * CRC detects accidental corruption only. Confidentiality, authentication,
 * replay protection and physical erasure require a suitable file backend.
 */

/** @brief Private handle capacity; not a wire-format or ABI size guarantee. */
#define ESY_CREDENTIALS_STORE_STORAGE_BYTES 192u
/** @brief Smallest encoded snapshot, including header and CRC. */
#define ESY_CREDENTIALS_STORE_MIN_SNAPSHOT_BYTES 100u

/** @brief Caller-owned handle; requires {0} before first init. Never copy/move live state. */
typedef struct {
  ESY_ALIGNED_STORAGE(storage, ESY_CREDENTIALS_STORE_STORAGE_BYTES);
} esy_credentials_store_t;

/** @brief Whether cached state can be used for reads or further mutations. */
typedef enum {
  ESY_CREDENTIALS_STORE_READY = 0, /**< Last load/commit succeeded. */
  ESY_CREDENTIALS_STORE_UNCERTAIN = 1 /**< Reload is required; cached metadata is diagnostic only. */
} esy_credentials_store_state_t;

/** @brief Bundle selected for an independent RAM copy. */
typedef enum {
  ESY_CREDENTIALS_STORE_CONFIRMED = 0, /**< Last explicitly confirmed identity. */
  ESY_CREDENTIALS_STORE_CANDIDATE = 1 /**< Staged identity, requiring owner validation/confirmation. */
} esy_credentials_store_slot_t;

/** @brief Exclusive caller RAM, borrowed until successful store deinit. */
typedef struct {
  uint8_t *working_buffer; /**< Cached complete snapshot; never exposed by load. */
  uint8_t *scratch_buffer; /**< Separate serialization/read workspace. */
  size_t buffer_size; /**< Capacity of EACH buffer, minimum snapshot size..UINT32_MAX. */
} esy_credentials_store_static_resources_t;

/** @brief Descriptor copied by init; its object need not outlive the call. */
typedef struct {
  esy_file_t *file; /**< Exclusively borrowed initialized READ + ATOMIC_REPLACE file. */
  const esy_credentials_store_static_resources_t *static_resources; /**< Two disjoint buffers. */
} esy_credentials_store_config_t;

/** @brief Caller storage receiving a complete independent bundle copy. */
typedef struct {
  uint8_t *buffer; /**< Writable, disjoint memory; NULL is allowed only for zero capacity. */
  size_t buffer_size; /**< At least the selected bundle's size from get_info(). */
} esy_credentials_store_load_resources_t;

/** @brief Cached metadata; never establishes current durable state while UNCERTAIN. */
typedef struct {
  esy_credentials_store_state_t state; /**< READY or UNCERTAIN. */
  bool has_snapshot; /**< False only before the first persisted transaction. */
  uint64_t transaction_id; /**< Successful snapshot sequence; never wraps. */
  uint64_t last_issued_id; /**< Retained across discard so candidate identifiers are not reused. */
  uint64_t confirmed_id; /**< Zero when unprovisioned; otherwise a bundle identity. */
  uint64_t candidate_id; /**< Zero when no candidate is staged. */
  size_t confirmed_size; /**< Sum of confirmed blob bytes, including PEM terminators. */
  size_t candidate_size; /**< Sum of candidate blob bytes, including PEM terminators. */
  size_t used_bytes; /**< Encoded snapshot bytes, or zero when no snapshot exists. */
  size_t capacity; /**< Lesser of file capacity and each workspace capacity. */
} esy_credentials_store_info_t;

/**
 * @brief Bind a file and load its complete snapshot, or initialize empty if absent.
 * @param store Zero-initialized or successfully deinitialized handle.
 * @param config File and exclusive workspace configuration.
 * @return OK, INVALID_ARG for aliases/invalid resources, NO_MEM for capacity,
 * STATE for repeated init, BUSY for provider callback reentry,
 * NOT_SUPPORTED for missing file capabilities/unknown
 * format, CRC_FAIL for malformed data, or a file error.
 * @note Init never writes/defaults/formats the file. Failure preserves the zero
 * handle; workspace may be cleared. Unknown/malformed existing snapshots and
 * file errors never imply absence; initial absence is reported by the backend.
 * Keep the file, backend and both buffers alive until deinit.
 */
esy_err_t esy_credentials_store_init(esy_credentials_store_t *store,
                                    const esy_credentials_store_config_t *config);

/**
 * @brief Clear both workspace buffers and detach without saving or removing data.
 * @param store Live or zero-state handle, with no operation in progress.
 * @return OK including repeated zero deinit; INVALID_ARG, STATE for copied/invalid
 * state, or BUSY during provider callback reentry. Success restores {0}.
 * @note Copies previously loaded into consumer RAM remain consumer-owned and
 * unchanged. Clear them only after all credential/TLS borrowers have stopped.
 */
esy_err_t esy_credentials_store_deinit(esy_credentials_store_t *store);

/**
 * @brief Query cached state without file access; valid also while UNCERTAIN.
 * @return OK, INVALID_ARG for NULL/aliases, STATE or BUSY. Output unchanged on error.
 */
esy_err_t esy_credentials_store_get_info(const esy_credentials_store_t *store,
                                        esy_credentials_store_info_t *info);

/**
 * @brief Copy one complete bundle to independent caller memory.
 * @param store READY store; no file/provider call is performed.
 * @param slot CONFIRMED or CANDIDATE, which must exist.
 * @param resources Destination for the three concatenated blob payloads.
 * @param config Output descriptors pointing into resources->buffer.
 * @param bundle_id Output identity for the selected bundle.
 * @return OK, INVALID_ARG for pointers/aliases/slot, NO_MEM for capacity, STATE
 * for missing bundle/UNCERTAIN/invalid handle, or BUSY on callback reentry.
 * @note All outputs remain unchanged on error. Outputs/resources must be mutually
 * disjoint and must not overlap store/file/workspace. The destination MUST NOT
 * currently be borrowed by any live Credentials/TLS consumer; that ownership
 * precondition cannot be inferred from raw pointers. No caller output is retained.
 * Later store stage/reload/confirm/discard/deinit cannot alter a loaded copy.
 */
esy_err_t esy_credentials_store_load(const esy_credentials_store_t *store,
                                    esy_credentials_store_slot_t slot,
                                    const esy_credentials_store_load_resources_t *resources,
                                    esy_credentials_config_t *config,
                                    uint64_t *bundle_id);

/**
 * @brief Atomically persist confirmed plus a copied candidate, retaining confirmed.
 * @param store READY store with no existing candidate.
 * @param candidate Complete approved bundle, using the base descriptor contract.
 * @param candidate_id Output nonzero identity, written only on success.
 * @return OK; BUSY for an existing candidate/reentry; INVALID_ARG for invalid
 * descriptors/aliases; NO_MEM for capacity; STATE for UNCERTAIN/exhausted counters;
 * or a file error. Preflight errors do not write or change the snapshot.
 * @note The owner authorizes updates and validates certificates/key matching
 * before staging. Data is copied before return. An attempted file mutation error
 * may already have committed: enter UNCERTAIN and reload before load/another
 * mutation. Recovered metadata reveals any committed candidate identity.
 * Staging reserves one further transaction for confirm/discard and rejects a
 * transaction counter already at UINT64_MAX-1 or UINT64_MAX.
 */
esy_err_t esy_credentials_store_stage(esy_credentials_store_t *store,
                                     const esy_credentials_config_t *candidate,
                                     uint64_t *candidate_id);

/**
 * @brief Atomically promote exactly the identified candidate and remove its staging entry.
 * @param store READY store. The owner explicitly decides trial success.
 * @param candidate_id Required matching nonzero candidate identity.
 * @return OK, INVALID_ARG for zero ID, STATE for mismatch/absence/UNCERTAIN/counter
 * exhaustion, BUSY for reentry, or file error (enters UNCERTAIN).
 * @note Rewrites a complete snapshot; does not rotate live RAM or stop consumers.
 */
esy_err_t esy_credentials_store_confirm(esy_credentials_store_t *store,
                                       uint64_t candidate_id);

/**
 * @brief Atomically discard exactly the identified candidate, preserving confirmed and ID history.
 * @return Same statuses and uncertain-commit semantics as confirm.
 */
esy_err_t esy_credentials_store_discard(esy_credentials_store_t *store,
                                       uint64_t candidate_id);

/**
 * @brief Reconcile authoritative file state after an uncertain write or explicit reload.
 * @param store Initialized handle, including UNCERTAIN.
 * @return OK or a file/format/capacity error; BUSY for reentry. Success publishes
 * READY. Failure retains cached bytes but marks UNCERTAIN, blocking load/mutation.
 * @note A known persisted snapshot cannot silently disappear or move its counters
 * backwards while this handle lives. Across reset, whole-media replay remains a
 * backend security concern; transaction IDs and CRC are not anti-rollback anchors.
 */
esy_err_t esy_credentials_store_reload(esy_credentials_store_t *store);

#ifdef __cplusplus
}
#endif
#endif /* ESY_CREDENTIALS_STORE_H */
