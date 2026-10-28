/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/protocol/esy_proto_tlv.h"

#include <stddef.h>

#include "esy_common/utils/base/esy_endian.h"

bool esy_tlv_append_u32(uint8_t *payload, uint16_t payload_capacity,
                        uint16_t *payload_len, uint16_t label, uint32_t value) {
  uint16_t pos;

  if ((payload == NULL) || (payload_len == NULL)) {
    return false;
  }

  pos = *payload_len;
  if (pos > payload_capacity) {
    return false;
  }
  if ((uint16_t)(payload_capacity - pos) < 8u) {
    return false;
  }

  esy_write_u16_le(&payload[pos], label);
  esy_write_u16_le(&payload[pos + 2u], 4u);
  esy_write_u32_le(&payload[pos + 4u], value);
  *payload_len = (uint16_t)(pos + 8u);
  return true;
}

bool esy_tlv_next(const uint8_t *payload, uint16_t payload_len,
                  uint16_t *offset, esy_tlv_item_t *out_item) {
  uint16_t pos;
  uint16_t len;

  if ((payload == NULL) || (offset == NULL) || (out_item == NULL)) {
    return false;
  }

  pos = *offset;
  if (pos >= payload_len) {
    return false;
  }
  if ((uint16_t)(payload_len - pos) < 4u) {
    return false;
  }

  out_item->label = esy_read_u16_le(&payload[pos]);
  len = esy_read_u16_le(&payload[pos + 2u]);
  pos = (uint16_t)(pos + 4u);
  if (len > (uint16_t)(payload_len - pos)) {
    return false;
  }

  out_item->len = len;
  out_item->value = &payload[pos];
  *offset = (uint16_t)(pos + len);
  return true;
}
