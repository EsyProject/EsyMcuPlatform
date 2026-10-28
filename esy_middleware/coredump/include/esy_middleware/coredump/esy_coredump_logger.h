/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_COREDUMP_LOGGER_H
#define ESY_COREDUMP_LOGGER_H

/** @file esy_coredump_logger.h
 * @brief Caller-paced best-effort export through the Logger frontend. */

#include "esy_middleware/coredump/esy_coredump.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum record bytes offered in one Logger call. */
#define ESY_COREDUMP_LOGGER_CHUNK_BYTES 16u

/**
 * @brief Offer one validated record chunk to Logger without clearing the source.
 * @param[in,out] store Initialized coredump handle; serialized with all record operations.
 * @param[in] offset First record byte to offer, strictly less than record_size.
 * @return ESY_OK means one line was offered; it does not confirm acceptance,
 * physical transmission or host collection. Other results are the record
 * validation error or ESY_ERR_INVALID_ARG for an invalid offset.
 * @note Normal owner task/loop context only. A call validates the complete
 * bounded record, copies at most 16 bytes, and formats one CD1 line. Logger may block,
 * filter, truncate or drop it. The owner paces successive offsets against
 * backend capacity and can repeat an offset or the complete record later.
 * Get record_size with esy_coredump_get_info(), start at zero and advance by
 * min(ESY_COREDUMP_LOGGER_CHUNK_BYTES, record_size - offset). No clock, task,
 * delivery acknowledgment or automatic clearing is supplied by this adapter.
 * The line carries the stored opaque identity as exactly 32 hexadecimal digits,
 * followed by record size, offset and bytes. CRC is integrity metadata inside
 * the record and is never used as the occurrence identifier.
 */
esy_err_t esy_coredump_logger_export_chunk(esy_coredump_t *store, size_t offset);

#ifdef __cplusplus
}
#endif
#endif /* ESY_COREDUMP_LOGGER_H */
