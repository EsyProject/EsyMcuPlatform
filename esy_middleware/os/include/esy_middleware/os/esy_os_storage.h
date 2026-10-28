/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_os_storage.h
 * @brief Common caller-owned OS object lifecycle contract.
 *
 * Static create accepts uninitialized, naturally aligned storage; no initial
 * zeroing is required. Storage and output pointer objects must not overlap.
 * Create must only target unused storage. Recreating a live object, including
 * after an incomplete cleanup, is a caller error: the port cannot inspect
 * uninitialized bytes to distinguish it from a first creation.
 *
 * A failed create sets a valid output pointer to NULL and retains no resources;
 * the storage can be retried. A successful create borrows all backing buffers
 * and callback arguments until cleanup completes. Keep them alive and unmoved.
 *
 * Lifecycle operations require a single externally serialized owner. Before
 * delete/reset, stop other users, including ISR producers and blocked tasks.
 * Mutexes must be unlocked before deletion. Successful synchronous deletion
 * invalidates all handle aliases; repeated delete of the same still-resident
 * wrapper is a no-op. Do not call other operations on deleted handles.
 *
 * Timer cleanup is explicitly pollable: ESY_ERR_BUSY retains ownership and
 * requires retry; only ESY_OK permits reclaiming its storage/context. Task self
 * deletion can defer native TCB reclamation to idle: retain self-deleted task
 * storage for system lifetime, unless the port separately proves reclamation.
 * Do not recreate it merely because the public handle was cleared. Deleting a
 * different task requires that it is parked and not running on another core.
 *
 * All static buffers must satisfy the published capacity and alignment. These
 * are ABI requirements in both C11 and C++17; rebuild consumers after changes.
 */
#ifndef ESY_OS_STORAGE_H
#define ESY_OS_STORAGE_H
#endif
