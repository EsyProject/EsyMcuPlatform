/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_COMMON_UTILS_ESY_ENDIAN_H
#define ESY_COMMON_UTILS_ESY_ENDIAN_H

/**
 * @file esy_endian.h
 * @brief Common little-endian integer encoding and decoding helpers.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read a 16-bit unsigned integer in little-endian order.
 *
 * @param[in] buf Input buffer with at least 2 bytes.
 *
 * @return Decoded value.
 */
static inline uint16_t esy_read_u16_le(const uint8_t *buf) {
  if (buf == NULL) {
    return 0u;
  }

  return (uint16_t)(((uint16_t)buf[0]) | ((uint16_t)buf[1] << 8));
}

/**
 * @brief Read a 32-bit unsigned integer in little-endian order.
 *
 * @param[in] buf Input buffer with at least 4 bytes.
 *
 * @return Decoded value.
 */
static inline uint32_t esy_read_u32_le(const uint8_t *buf) {
  if (buf == NULL) {
    return 0u;
  }

  return ((uint32_t)buf[0]) | ((uint32_t)buf[1] << 8) |
         ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

/**
 * @brief Write a 16-bit unsigned integer in little-endian order.
 *
 * @param[out] buf Output buffer with at least 2 bytes.
 * @param[in] value Value to encode.
 */
static inline void esy_write_u16_le(uint8_t *buf, uint16_t value) {
  if (buf == NULL) {
    return;
  }

  buf[0] = (uint8_t)(value & 0xFFu);
  buf[1] = (uint8_t)((value >> 8) & 0xFFu);
}

/**
 * @brief Write a 32-bit unsigned integer in little-endian order.
 *
 * @param[out] buf Output buffer with at least 4 bytes.
 * @param[in] value Value to encode.
 */
static inline void esy_write_u32_le(uint8_t *buf, uint32_t value) {
  if (buf == NULL) {
    return;
  }

  buf[0] = (uint8_t)(value & 0xFFu);
  buf[1] = (uint8_t)((value >> 8) & 0xFFu);
  buf[2] = (uint8_t)((value >> 16) & 0xFFu);
  buf[3] = (uint8_t)((value >> 24) & 0xFFu);
}

#ifdef __cplusplus
}
#endif

#endif /* ESY_COMMON_UTILS_ESY_ENDIAN_H */
