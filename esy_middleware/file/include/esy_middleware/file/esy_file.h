/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_FILE_H
#define ESY_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Operations supported by one bound file; derived from provider callbacks. */
typedef enum {
    ESY_FILE_CAP_READ = 1u << 0, /**< Information and exact range reads. */
    ESY_FILE_CAP_CREATE = 1u << 1, /**< Publish a complete file only when absent. */
    ESY_FILE_CAP_ATOMIC_REPLACE = 1u << 2, /**< Commit a whole replacement or initial file. */
    ESY_FILE_CAP_REMOVE = 1u << 3, /**< Commit logical absence of the file. */
} esy_file_capability_t;

/**
 * @brief Current committed file state; errors never imply an absent file.
 * An existing zero-length file is distinct from absence. Length is at most
 * capacity and is zero when absent. Capacity is the provider's maximum complete
 * content size, not free space or a physical address. Capability bits are filled
 * by the file facade; providers need only supply exists, length and capacity.
 */
typedef struct {
    bool exists; /**< Whether one complete committed file is present. */
    uint32_t length; /**< Committed content bytes, zero when absent. */
    uint32_t capacity; /**< Maximum content size accepted by this binding. */
    uint32_t capabilities; /**< Derived esy_file_capability_t bits. */
} esy_file_info_t;

/**
 * @brief Backend operations for one prebound file, copied by file init.
 *
 * The backend chooses its region or path during its own initialization. No
 * global filesystem, directory, path parser or allocator is required. One
 * non-ISR task/main-loop owner serializes every operation, provider lifetime
 * and other access to the same file/storage. Multi-call domain transactions
 * (for example inspect identity then remove) require that same serialization
 * across their whole sequence. This interface supplies no cross-task lock.
 * A provider must not reenter its file facade. All pointers are borrowed only
 * until return; context and backing resources outlive all copied operations.
 * Provider state and file/input/output storage must be mutually disjoint unless
 * the provider explicitly documents otherwise; accessible spans are caller
 * preconditions. Callbacks return OK or a negative error, never a byte count.
 *
 * info and read are required. Read returns exactly length bytes from an existing
 * file, validates the complete range, and returns STATE if absent. NULL buffer
 * is valid only for zero length; even an empty read validates file existence.
 * I/O errors can partially change read output, which is authoritative only on OK.
 *
 * create publishes the entire input only if the file is absent; an existing
 * file returns BUSY without replacing it. replace publishes the entire input
 * whether present or absent, preserving the previous committed file until the
 * complete replacement commits. A reset/interruption exposes the old or new
 * complete file, not an accepted partial replacement, for media that survives
 * that event. RAM providers have only their documented volatile lifetime.
 * Missing mutation callbacks mean NOT_SUPPORTED; callers check capabilities.
 *
 * remove commits absence, also succeeding when already absent. Logical removal
 * is not secure physical erasure. Durability, retention and physical recovery
 * limits belong to the selected backend. Every mutation error can follow a
 * committed effect; inspect authoritative state before retries. No automatic
 * retry, rollback, sync-on-detach or hidden resource lifecycle is performed.
 * Provider latency (including Flash/filesystem sync) belongs to the owner;
 * no new timebase, deadline, task, heap, ISR or fault-safe path is introduced.
 */
typedef struct {
    esy_err_t (*info)(void *context, esy_file_info_t *info); /**< Read committed metadata. */
    esy_err_t (*read)(void *context, uint32_t offset, void *buffer,
                      size_t length); /**< Copy one exact range. */
    esy_err_t (*create)(void *context, const void *data, size_t length); /**< Create if absent. */
    esy_err_t (*replace)(void *context, const void *data, size_t length); /**< Atomic whole-file upsert. */
    esy_err_t (*remove)(void *context); /**< Commit logical absence. */
    void *context; /**< Borrowed provider owner, NULL when its contract permits. */
} esy_file_ops_t;

/**
 * @brief Caller-owned facade requiring {0} before first init.
 * Fields are private by contract; do not copy, move or reset a live file. The
 * facade owns no provider lifecycle. Detach every borrower before deinit and
 * detach the facade before releasing its provider or backing resources.
 */
typedef struct {
    esy_file_ops_t ops; /**< Private copied binding. */
    bool initialized; /**< Private lifecycle flag. */
    bool busy; /**< Private synchronous reentry guard, not a lock. */
} esy_file_t;

/**
 * @brief Bind copied operations without invoking the provider or changing data.
 * @return OK; INVALID_ARG for NULL/missing info/read; STATE for repeated init;
 * BUSY for callback reentry. Failure preserves the handle. No resources acquired.
 */
esy_err_t esy_file_init(esy_file_t *file, const esy_file_ops_t *ops);
/** @brief Detach without saving/removing data; zero/repeated deinit succeeds.
 * @return INVALID_ARG for NULL; BUSY during a callback; otherwise OK. */
esy_err_t esy_file_deinit(esy_file_t *file);
/** @brief Query implemented operations without reading or modifying the backend.
 * @return OK, INVALID_ARG, STATE or BUSY; output is unchanged on failure. */
esy_err_t esy_file_get_capabilities(esy_file_t *file, uint32_t *capabilities);
/** @brief Copy validated metadata/capabilities; output is unchanged on error.
 * @return OK, INVALID_ARG, STATE, BUSY or a negative provider error. */
esy_err_t esy_file_get_info(esy_file_t *file, esy_file_info_t *info);
/** @brief Read one exact range; invalid arguments are rejected before callbacks.
 * @return OK; INVALID_ARG for pointer/range overflow; STATE when absent/detached;
 * BUSY on reentry or a negative provider error. Error output is not authoritative. */
esy_err_t esy_file_read(esy_file_t *file, uint32_t offset, void *buffer, size_t length);
/** @brief Publish complete content only if absent; inputs borrowed until return.
 * @return OK; BUSY if present/reentered; INVALID_ARG for invalid spans/length;
 * NO_MEM for insufficient capacity; NOT_SUPPORTED if unavailable; other errors. */
esy_err_t esy_file_create(esy_file_t *file, const void *data, size_t length);
/** @brief Atomically replace/upsert complete content using a capable backend.
 * @return OK; INVALID_ARG, STATE, BUSY, NO_MEM, NOT_SUPPORTED or provider error.
 * A failed commit may already be durable; reconcile before another mutation. */
esy_err_t esy_file_replace(esy_file_t *file, const void *data, size_t length);
/** @brief Commit absence without assuming physical erasure or automatic retries.
 * @return OK also if absent; INVALID_ARG, STATE, BUSY, NOT_SUPPORTED or provider error. */
esy_err_t esy_file_remove(esy_file_t *file);

#ifdef __cplusplus
}
#endif
#endif /* ESY_FILE_H */
