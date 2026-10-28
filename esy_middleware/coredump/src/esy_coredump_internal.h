/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_COREDUMP_INTERNAL_H
#define ESY_COREDUMP_INTERNAL_H

#include "esy_middleware/coredump/esy_coredump.h"

/* The stable wire prefix permits version dispatch before version-specific bounds. */
#define COREDUMP_MAGIC UINT32_C(0x31444345)
#define COREDUMP_VERSION UINT32_C(1)
#define COREDUMP_MIN_VALIDATION_BYTES 8u

/* Module-private ownership check used before an adapter writes scratch bytes.
 * It distinguishes an uninitialized handle from an initialized empty record. */
esy_err_t esy_coredump_check_external_storage(esy_coredump_t *store,
                                             const void *buffer, size_t length);

#endif /* ESY_COREDUMP_INTERNAL_H */
