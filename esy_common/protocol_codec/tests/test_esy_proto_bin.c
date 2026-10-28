/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The sanitizer job must fail if its compiler flags are silently ignored. */
#if defined(ESY_PROTOCOL_SANITIZER) && !defined(__SANITIZE_ADDRESS__)
#error "Protocol sanitizer tests require GCC address-sanitizer instrumentation"
#endif

#include "unity.h"

#include <string.h>

#include "esy_common/protocol/esy_proto_bin.h"

void setUp(void) {}

void tearDown(void) {}

static void build_valid_frame(uint8_t *buf, size_t buf_size,
                              esy_proto_bin_frame_t *frame) {
  const uint8_t payload[] = {0x11u, 0x22u, 0x33u};

  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_init(frame, buf, buf_size));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_put_header(
                                frame, ESY_PROTO_BIN_CMD_DATA, 0x42u));
  TEST_ASSERT_EQUAL(
      ESY_OK, esy_proto_bin_frame_put_data(frame, payload, sizeof(payload)));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_finish(frame));
}

void test_frame_builder_rejects_invalid_arguments(void) {
  esy_proto_bin_frame_t frame;
  uint8_t buf[16];

  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_proto_bin_frame_init(NULL, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_proto_bin_frame_init(&frame, NULL, sizeof(buf)));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_proto_bin_frame_init(&frame, buf, 0u));

  memset(&frame, 0, sizeof(frame));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_proto_bin_frame_put_header(&frame, 1u, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_proto_bin_frame_put_data(&frame, buf, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_bin_frame_finish(&frame));
}

void test_frame_builder_writes_header_payload_length_and_crc(void) {
  uint8_t buf[32];
  esy_proto_bin_frame_t frame;

  build_valid_frame(buf, sizeof(buf), &frame);

  TEST_ASSERT_EQUAL_UINT8(ESY_PROTO_BIN_HEAD, buf[0]);
  TEST_ASSERT_EQUAL_UINT8(ESY_PROTO_BIN_CMD_DATA, buf[1]);
  TEST_ASSERT_EQUAL_UINT8(0x42u, buf[2]);
  TEST_ASSERT_EQUAL_UINT8(0u, buf[3]);
  TEST_ASSERT_EQUAL_UINT8(3u, buf[4]);
  TEST_ASSERT_EQUAL_UINT8(0x11u, buf[5]);
  TEST_ASSERT_EQUAL_UINT8(0x22u, buf[6]);
  TEST_ASSERT_EQUAL_UINT8(0x33u, buf[7]);
  TEST_ASSERT_EQUAL_UINT(ESY_PROTO_BIN_OVERHEAD_BYTES + 3u,
                         esy_proto_bin_frame_length(&frame));
}

void test_frame_builder_checks_capacity_and_call_order(void) {
  uint8_t small_buf[8];
  uint8_t buf[16];
  esy_proto_bin_frame_t frame;
  const uint8_t payload[8] = {0};

  TEST_ASSERT_EQUAL(
      ESY_OK, esy_proto_bin_frame_init(&frame, small_buf, sizeof(small_buf)));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_put_header(
                                &frame, ESY_PROTO_BIN_CMD_DATA, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_proto_bin_frame_put_data(
                                        &frame, payload, sizeof(payload)));

  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_init(&frame, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_proto_bin_frame_put_data(&frame, NULL, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_proto_bin_frame_put_data(&frame, payload, 1u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_put_header(
                                &frame, ESY_PROTO_BIN_CMD_DATA, 1u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_put_data(&frame, NULL, 0u));
}

void test_parser_accepts_a_valid_generated_frame(void) {
  uint8_t tx_buf[32];
  uint8_t rx_buf[32];
  esy_proto_bin_frame_t tx_frame;
  esy_proto_bin_frame_t rx_frame;
  esy_proto_bin_parser_t parser;
  esy_proto_bin_feed_result_t result = ESY_PROTO_BIN_FEED_INCOMPLETE;

  build_valid_frame(tx_buf, sizeof(tx_buf), &tx_frame);
  TEST_ASSERT_EQUAL(
      ESY_OK, esy_proto_bin_frame_init(&rx_frame, rx_buf, sizeof(rx_buf)));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_parser_init(&parser));

  for (size_t i = 0u; i < esy_proto_bin_frame_length(&tx_frame); ++i) {
    result = esy_proto_bin_parser_feed(&parser, &rx_frame, tx_buf[i]);
  }

  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_FEED_OK, result);
  TEST_ASSERT_EQUAL_UINT8(ESY_PROTO_BIN_CMD_DATA,
                          esy_proto_bin_frame_get_cmd(&rx_frame));
  TEST_ASSERT_EQUAL_UINT8(0x42u, esy_proto_bin_frame_get_seq(&rx_frame));
  TEST_ASSERT_EQUAL_UINT(3u, esy_proto_bin_frame_get_data_len(&rx_frame));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(&tx_buf[5],
                                esy_proto_bin_frame_get_data(&rx_frame), 3u);
}

void test_parser_rejects_crc_mismatch_and_invalid_inputs(void) {
  uint8_t tx_buf[32];
  uint8_t rx_buf[32];
  esy_proto_bin_frame_t tx_frame;
  esy_proto_bin_frame_t rx_frame;
  esy_proto_bin_parser_t parser;
  esy_proto_bin_feed_result_t result = ESY_PROTO_BIN_FEED_INCOMPLETE;

  build_valid_frame(tx_buf, sizeof(tx_buf), &tx_frame);
  tx_buf[esy_proto_bin_frame_length(&tx_frame) - 1u] ^= 0x01u;

  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_FEED_CRC_ERROR,
                    esy_proto_bin_parser_feed(NULL, &rx_frame, 0u));
  TEST_ASSERT_EQUAL(
      ESY_OK, esy_proto_bin_frame_init(&rx_frame, rx_buf, sizeof(rx_buf)));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_parser_init(&parser));

  for (size_t i = 0u; i < esy_proto_bin_frame_length(&tx_frame); ++i) {
    result = esy_proto_bin_parser_feed(&parser, &rx_frame, tx_buf[i]);
  }

  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_FEED_CRC_ERROR, result);
}

void test_parser_resets_on_invalid_command_and_invalid_getters_are_safe(void) {
  uint8_t rx_buf[16];
  const uint8_t invalid_fixed[] = {ESY_PROTO_BIN_HEAD, ESY_PROTO_BIN_CMD_MAX,
                                   0x01u, 0x00u, 0x00u};
  esy_proto_bin_frame_t frame;
  esy_proto_bin_parser_t parser;

  TEST_ASSERT_EQUAL(ESY_OK,
                    esy_proto_bin_frame_init(&frame, rx_buf, sizeof(rx_buf)));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_parser_init(&parser));

  for (size_t i = 0u; i < sizeof(invalid_fixed); ++i) {
    TEST_ASSERT_EQUAL(
        ESY_PROTO_BIN_FEED_INCOMPLETE,
        esy_proto_bin_parser_feed(&parser, &frame, invalid_fixed[i]));
  }

  TEST_ASSERT_EQUAL_UINT(0u, esy_proto_bin_frame_length(&frame));
  TEST_ASSERT_NULL(esy_proto_bin_frame_get_data(NULL));
  TEST_ASSERT_EQUAL_UINT(0u, esy_proto_bin_frame_get_data_len(NULL));
  TEST_ASSERT_EQUAL_UINT8(0u, esy_proto_bin_frame_get_cmd(NULL));
  TEST_ASSERT_EQUAL_UINT8(0u, esy_proto_bin_frame_get_seq(NULL));

  esy_proto_bin_parser_reset(NULL, &frame);
  TEST_ASSERT_EQUAL_UINT(0u, esy_proto_bin_frame_length(&frame));
}

void test_parser_rejects_zero_command_even_with_a_valid_crc(void) {
  uint8_t tx_buf[32];
  uint8_t rx_buf[32];
  esy_proto_bin_frame_t tx;
  esy_proto_bin_frame_t rx;
  esy_proto_bin_parser_t parser;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_init(&tx, tx_buf, sizeof(tx_buf)));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_put_header(&tx, 0u, 1u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_finish(&tx));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_init(&rx, rx_buf, sizeof(rx_buf)));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_parser_init(&parser));
  for (size_t i = 0u; i < tx.len; ++i) {
    TEST_ASSERT_NOT_EQUAL(ESY_PROTO_BIN_FEED_OK,
                         esy_proto_bin_parser_feed(&parser, &rx, tx_buf[i]));
  }
}

void test_bin_frame_length_overflow_rejected_before_access(void) {
  uint8_t bytes[16] = {0};
  esy_proto_bin_frame_t f;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_init(&f, bytes, sizeof(bytes)));
  f.len = SIZE_MAX - 4;
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM,
                    esy_proto_bin_frame_put_data(&f, bytes, 16));
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_proto_bin_frame_finish(&f));
}
