/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_DIAGNOSTICS_DEFS_H
#define ESY_DIAGNOSTICS_DEFS_H

#include <stddef.h>
#include <stdint.h>

/** @brief Maximum canonical diagnostic name bytes, excluding its terminator. */
#define ESY_DIAGNOSTICS_NAME_MAX_BYTES 48u
/** @brief Maximum optional explanatory text bytes, excluding its terminator. */
#define ESY_DIAGNOSTICS_DETAIL_MAX_BYTES 96u

/** @brief A diagnostic finding, independent of command execution success. */
typedef enum {
    ESY_DIAGNOSTICS_HEALTH_UNKNOWN = 0, /**< No authoritative health assessment. */
    ESY_DIAGNOSTICS_HEALTH_OK = 1, /**< The queried or tested condition is healthy. */
    ESY_DIAGNOSTICS_HEALTH_WARNING = 2, /**< Degraded operation requiring attention. */
    ESY_DIAGNOSTICS_HEALTH_ERROR = 3, /**< An unhealthy condition was detected. */
} esy_diagnostics_health_t;

/** @brief Operations available for one named diagnostic source. */
typedef enum {
    ESY_DIAGNOSTICS_CAP_READ = 1u << 0, /**< Query current health without starting a test. */
    ESY_DIAGNOSTICS_CAP_RUN = 1u << 1, /**< Explicitly execute a synchronous self-test. */
} esy_diagnostics_capability_t;

/**
 * @brief One copied health assessment, without a storage or wire dependency.
 *
 * code is source-defined and interpreted together with the canonical source
 * name; zero means no additional reason code. detail is optional explanatory
 * printable ASCII (0x20..0x7e), not a machine-readable replacement for health/code.
 * detail_length is 0..96, and detail[detail_length] is NUL. Trailing storage is
 * ignored. No timestamp, counter, retained history or persistence is implied.
 * A successful operation can report WARNING/ERROR; the operation status itself
 * describes whether a trustworthy assessment was obtained. A completed RUN
 * returns OK, WARNING or ERROR health; UNKNOWN is valid only for READ.
 */
typedef struct {
    esy_diagnostics_health_t health; /**< Current finding or completed test finding. */
    uint32_t code; /**< Source-specific reason; independent of esy_err_t. */
    char detail[ESY_DIAGNOSTICS_DETAIL_MAX_BYTES + 1u]; /**< Copied explanatory text. */
    size_t detail_length; /**< Text length without the terminator. */
} esy_diagnostics_result_t;

/**
 * @brief A diagnostic descriptor and optional assessment copied as one result.
 *
 * A nonempty inventory has index < count, READ capability, no unknown capability
 * bits, and a name of 1..48 ASCII letters/digits/'_' / '.' / '-' / '/' bytes.
 * name[name_length] is NUL; trailing storage is ignored. ENUMERATE returns only
 * metadata and a zero result (UNKNOWN, no code/detail), without querying a source.
 * READ/RUN include their assessment. An empty inventory can be enumerated at
 * index zero and returns an entirely zero snapshot; other empty indices fail.
 * Enumeration identities/order stay stable for the source binding lifetime.
 * There is no cross-source coherent snapshot or implicit self-test.
 */
typedef struct {
    uint32_t count; /**< Number of registered diagnostic sources. */
    uint32_t index; /**< Zero-based stable inventory position. */
    uint32_t capabilities; /**< esy_diagnostics_capability_t bits. */
    char name[ESY_DIAGNOSTICS_NAME_MAX_BYTES + 1u]; /**< Copied canonical name. */
    size_t name_length; /**< Name bytes without the terminator. */
    esy_diagnostics_result_t result; /**< Assessment, or zero metadata-only result. */
} esy_diagnostics_snapshot_t;

#endif /* ESY_DIAGNOSTICS_DEFS_H */
