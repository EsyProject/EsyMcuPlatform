/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_COMMON_UTILS_ESY_CRC32_H
#define ESY_COMMON_UTILS_ESY_CRC32_H

/**
 * @file esy_crc32.h
 * @brief Common CRC32 checksum and CRC32-framed-buffer helpers.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_common/utils/base/esy_endian.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Compute a CRC32 checksum using the reflected polynomial 0xEDB88320.
 *
 * This function supports incremental CRC calculation by passing the previous
 * CRC value as input.
 *
 * @pre If @p length is greater than zero, @p data must not be NULL. This low
 *      level primitive intentionally uses a contract-style precondition to keep
 *      the hot CRC loop free of repeated defensive checks. Callers that accept
 *      external input must validate buffers before calling this function.
 *
 * @param[in] crc Initial CRC value, typically 0xFFFFFFFFU.
 * @param[in] data Pointer to the input buffer.
 * @param[in] length Length of the input buffer in bytes.
 *
 * @return Computed CRC32 value.
 */
uint32_t esy_crc32(uint32_t crc, const void *data, size_t length);

/**
 * @brief Compute the finalized CRC32 value for a data segment.
 *
 * The result is `~esy_crc32(0xFFFFFFFFU, buf, len)`.
 *
 * @param[in] buf Data buffer.
 * @param[in] len Data length without the CRC field.
 *
 * @return Finalized CRC32 value. Returns 0 if @p buf is NULL.
 */
static inline uint32_t esy_crc32_finalize(const uint8_t *buf, size_t len) {
  if (buf == NULL) {
    return 0u;
  }

  return ~esy_crc32(0xFFFFFFFFu, buf, len);
}

/**
 * @brief Write a finalized CRC32 value to the buffer tail in little-endian
 * order.
 *
 * @param[in,out] buf Frame buffer.
 * @param[in] len_without_crc Frame length without the CRC field.
 */
static inline void esy_crc32_fill_tail_le(uint8_t *buf,
                                          size_t len_without_crc) {
  if (buf == NULL) {
    return;
  }

  esy_write_u32_le(&buf[len_without_crc],
                   esy_crc32_finalize(buf, len_without_crc));
}

/**
 * @brief Validate a little-endian CRC32 value stored at the frame tail.
 *
 * @param[in] buf Frame buffer. The last 4 bytes contain the CRC field.
 * @param[in] total_len Total frame length including the CRC field.
 *
 * @retval true CRC validation passed.
 * @retval false CRC validation failed or arguments are invalid.
 */
static inline bool esy_crc32_validate_tail_le(const uint8_t *buf,
                                              size_t total_len) {
  if ((buf == NULL) || (total_len < 4u)) {
    return false;
  }

  return esy_read_u32_le(&buf[total_len - 4u]) ==
         esy_crc32_finalize(buf, total_len - 4u);
}

#ifdef __cplusplus
}
#endif

#endif /* ESY_COMMON_UTILS_ESY_CRC32_H */
