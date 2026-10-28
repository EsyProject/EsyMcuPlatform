/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_MIDDLEWARE_PROTOCOL_ESY_PROTO_TLV_H
#define ESY_MIDDLEWARE_PROTOCOL_ESY_PROTO_TLV_H

/**
 * @file esy_proto_tlv.h
 * @brief TLV encoding/decoding helpers (label:u16 + len:u16 + value:len).
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief TLV item view.
 */
typedef struct {
  uint16_t label;       /**< TLV label. */
  uint16_t len;         /**< Value length in bytes. */
  const uint8_t *value; /**< Value pointer inside the source payload. */
} esy_tlv_item_t;

/**
 * @brief Append one u32 TLV item to the payload tail.
 *
 * Values are encoded in little-endian order.
 *
 * @param[in,out] payload TLV payload buffer.
 * @param[in] payload_capacity Payload capacity in bytes.
 * @param[in,out] payload_len Current used byte count; updated on success.
 * @param[in] label TLV label.
 * @param[in] value u32 value.
 *
 * @retval true Append succeeded.
 * @retval false Invalid arguments or insufficient space.
 */
bool esy_tlv_append_u32(uint8_t *payload, uint16_t payload_capacity,
                        uint16_t *payload_len, uint16_t label, uint32_t value);

/**
 * @brief Read the next TLV item sequentially.
 *
 * The returned item points into @p payload; it does not copy the value bytes.
 *
 * @param[in] payload TLV payload base address.
 * @param[in] payload_len Payload length in bytes.
 * @param[in,out] offset Current offset; advanced to the next item on success.
 * @param[out] out_item Output item.
 *
 * @retval true Item read successfully.
 * @retval false End of payload, invalid arguments, or malformed TLV.
 */
bool esy_tlv_next(const uint8_t *payload, uint16_t payload_len,
                  uint16_t *offset, esy_tlv_item_t *out_item);

#ifdef __cplusplus
}
#endif

#endif /* ESY_MIDDLEWARE_PROTOCOL_ESY_PROTO_TLV_H */
