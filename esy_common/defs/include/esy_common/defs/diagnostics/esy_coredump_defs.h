/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_COREDUMP_DEFS_H
#define ESY_COREDUMP_DEFS_H

#include <stdint.h>

/** @brief Opaque record identity bytes, independent of a CRC or message encoding. */
#define ESY_COREDUMP_RECORD_ID_BYTES 16u

/**
 * @brief Nonzero identity of one captured fault occurrence, stored with the record.
 *
 * Treat the bytes as opaque, not an integer or an authentication credential.
 * The normal-context owner allocates a fresh identity for every capture lifetime
 * and never reuses one for a later occurrence. Restoring or exporting the same
 * record preserves its bytes across resets and image changes. A random 128-bit
 * source offers probabilistic uniqueness; deterministic cross-power-loss
 * uniqueness requires an owner-managed durable allocator. All-zero is invalid
 * for a record or a conditional operation and represents only absent metadata.
 */
typedef struct {
    uint8_t bytes[ESY_COREDUMP_RECORD_ID_BYTES]; /**< Persisted opaque identity. */
} esy_coredump_record_id_t;

#endif /* ESY_COREDUMP_DEFS_H */
