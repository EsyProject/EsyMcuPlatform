/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"

#include "esy_common/protocol/esy_proto_tlv.h"

void setUp(void) {}

void tearDown(void) {}

void test_append_u32_encodes_item_and_advances_length(void) {
  uint8_t payload[16] = {0};
  uint16_t payload_len = 0u;

  TEST_ASSERT_TRUE(esy_tlv_append_u32(payload, sizeof(payload), &payload_len,
                                      0x1234u, 0xA1B2C3D4u));

  TEST_ASSERT_EQUAL_UINT16(8u, payload_len);
  TEST_ASSERT_EQUAL_UINT8(0x34u, payload[0]);
  TEST_ASSERT_EQUAL_UINT8(0x12u, payload[1]);
  TEST_ASSERT_EQUAL_UINT8(0x04u, payload[2]);
  TEST_ASSERT_EQUAL_UINT8(0x00u, payload[3]);
  TEST_ASSERT_EQUAL_UINT8(0xD4u, payload[4]);
  TEST_ASSERT_EQUAL_UINT8(0xC3u, payload[5]);
  TEST_ASSERT_EQUAL_UINT8(0xB2u, payload[6]);
  TEST_ASSERT_EQUAL_UINT8(0xA1u, payload[7]);
}

void test_append_u32_rejects_invalid_state_or_insufficient_capacity(void) {
  uint8_t payload[8] = {0};
  uint16_t payload_len = 1u;

  TEST_ASSERT_FALSE(
      esy_tlv_append_u32(NULL, sizeof(payload), &payload_len, 1u, 2u));
  TEST_ASSERT_FALSE(esy_tlv_append_u32(payload, sizeof(payload), NULL, 1u, 2u));
  TEST_ASSERT_FALSE(
      esy_tlv_append_u32(payload, sizeof(payload), &payload_len, 1u, 2u));

  payload_len = 9u;
  TEST_ASSERT_FALSE(
      esy_tlv_append_u32(payload, sizeof(payload), &payload_len, 1u, 2u));
}

void test_next_reads_items_sequentially(void) {
  uint8_t payload[16] = {0};
  uint16_t payload_len = 0u;
  uint16_t offset = 0u;
  esy_tlv_item_t item;

  TEST_ASSERT_TRUE(esy_tlv_append_u32(payload, sizeof(payload), &payload_len,
                                      1u, 0x11223344u));
  TEST_ASSERT_TRUE(esy_tlv_append_u32(payload, sizeof(payload), &payload_len,
                                      2u, 0x55667788u));

  TEST_ASSERT_TRUE(esy_tlv_next(payload, payload_len, &offset, &item));
  TEST_ASSERT_EQUAL_UINT16(1u, item.label);
  TEST_ASSERT_EQUAL_UINT16(4u, item.len);
  TEST_ASSERT_EQUAL_UINT8(0x44u, item.value[0]);
  TEST_ASSERT_EQUAL_UINT16(8u, offset);

  TEST_ASSERT_TRUE(esy_tlv_next(payload, payload_len, &offset, &item));
  TEST_ASSERT_EQUAL_UINT16(2u, item.label);
  TEST_ASSERT_EQUAL_UINT16(16u, offset);

  TEST_ASSERT_FALSE(esy_tlv_next(payload, payload_len, &offset, &item));
}

void test_next_rejects_invalid_or_malformed_payload(void) {
  uint8_t short_header[] = {0x01u, 0x00u, 0x04u};
  uint8_t overflow_value[] = {0x01u, 0x00u, 0x04u, 0x00u, 0xAAu};
  uint16_t offset = 0u;
  esy_tlv_item_t item;

  TEST_ASSERT_FALSE(esy_tlv_next(NULL, sizeof(short_header), &offset, &item));
  TEST_ASSERT_FALSE(
      esy_tlv_next(short_header, sizeof(short_header), NULL, &item));
  TEST_ASSERT_FALSE(
      esy_tlv_next(short_header, sizeof(short_header), &offset, NULL));

  TEST_ASSERT_FALSE(
      esy_tlv_next(short_header, sizeof(short_header), &offset, &item));
  offset = 0u;
  TEST_ASSERT_FALSE(
      esy_tlv_next(overflow_value, sizeof(overflow_value), &offset, &item));
}
