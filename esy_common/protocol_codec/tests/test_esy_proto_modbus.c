/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"

#include <string.h>

#include "esy_common/protocol/esy_proto_modbus.h"

void setUp(void) {}

void tearDown(void) {}

void test_crc16_matches_known_modbus_vector(void) {
  const uint8_t request[] = {0x01u, 0x03u, 0x00u, 0x00u, 0x00u, 0x0Au};

  TEST_ASSERT_EQUAL_HEX16(0xCDC5u,
                          esy_proto_modbus_crc16(request, sizeof(request)));
  TEST_ASSERT_EQUAL_HEX16(0xFFFFu, esy_proto_modbus_crc16(NULL, 1u));
}

void test_frame_init_rejects_invalid_arguments(void) {
  esy_proto_modbus_frame_t frame;
  uint8_t buf[16];

  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_proto_modbus_frame_init(NULL, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_proto_modbus_frame_init(&frame, NULL, sizeof(buf)));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_proto_modbus_frame_init(&frame, buf, 0u));
}

void test_build_and_parse_rtu_frame(void) {
  uint8_t buf[16];
  const uint8_t pdu[] = {0x03u, 0x00u, 0x00u, 0x00u, 0x0Au};
  esy_proto_modbus_frame_t frame;
  esy_proto_modbus_frame_view_t view;

  TEST_ASSERT_EQUAL(ESY_OK,
                    esy_proto_modbus_frame_init(&frame, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_build_rtu_frame(&frame, 0x01u, pdu,
                                                             sizeof(pdu)));

  TEST_ASSERT_EQUAL_UINT(1u + sizeof(pdu) + 2u, frame.len);
  TEST_ASSERT_TRUE(esy_proto_modbus_verify_rtu_frame(frame.buf, frame.len));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_parse_frame(
                                ESY_PROTO_MODBUS_FRAME_RTU, frame.buf,
                                (uint16_t)frame.len, &view));
  TEST_ASSERT_EQUAL_UINT8(0x01u, view.unit_id);
  TEST_ASSERT_EQUAL_UINT8(0x03u, view.func);
  TEST_ASSERT_EQUAL_UINT16(sizeof(pdu), view.pdu_len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(pdu, view.pdu, sizeof(pdu));
}

void test_build_and_parse_tcp_frame(void) {
  uint8_t buf[16];
  const uint8_t pdu[] = {0x06u, 0x00u, 0x01u, 0x00u, 0x03u};
  esy_proto_modbus_frame_t frame;
  esy_proto_modbus_frame_view_t view;

  TEST_ASSERT_EQUAL(ESY_OK,
                    esy_proto_modbus_frame_init(&frame, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_build_tcp_frame(
                                &frame, 0x1234u, 0x09u, pdu, sizeof(pdu)));

  TEST_ASSERT_EQUAL_UINT(ESY_PROTO_MODBUS_MBAP_LEN + sizeof(pdu), frame.len);
  TEST_ASSERT_TRUE(esy_proto_modbus_verify_tcp_frame(frame.buf, frame.len));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_parse_frame(
                                ESY_PROTO_MODBUS_FRAME_TCP, frame.buf,
                                (uint16_t)frame.len, &view));
  TEST_ASSERT_EQUAL_UINT16(0x1234u, view.transaction_id);
  TEST_ASSERT_EQUAL_UINT16(0u, view.protocol_id);
  TEST_ASSERT_EQUAL_UINT8(0x09u, view.unit_id);
  TEST_ASSERT_EQUAL_UINT8(0x06u, view.func);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(pdu, view.pdu, sizeof(pdu));
}

void test_build_frame_rejects_invalid_arguments_and_small_buffers(void) {
  uint8_t buf[4];
  const uint8_t pdu[] = {0x03u};
  esy_proto_modbus_frame_t frame;

  TEST_ASSERT_EQUAL(ESY_OK,
                    esy_proto_modbus_frame_init(&frame, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_build_rtu_frame(
                                             NULL, 1u, pdu, sizeof(pdu)));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_build_rtu_frame(
                                             &frame, 1u, NULL, sizeof(pdu)));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_proto_modbus_build_rtu_frame(&frame, 1u, pdu, 0u));
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_proto_modbus_build_tcp_frame(
                                        &frame, 1u, 1u, pdu, sizeof(pdu)));
}

void test_verify_and_parse_reject_malformed_frames(void) {
  uint8_t tcp_bad_pid[] = {0x00u, 0x01u, 0x00u, 0x01u,
                           0x00u, 0x02u, 0x01u, 0x03u};
  uint8_t tcp_bad_len[] = {0x00u, 0x01u, 0x00u, 0x00u,
                           0x00u, 0x03u, 0x01u, 0x03u};
  uint8_t rtu_bad_crc[] = {0x01u, 0x03u, 0x00u, 0x00u};
  esy_proto_modbus_frame_view_t view;

  TEST_ASSERT_FALSE(esy_proto_modbus_verify_rtu_frame(NULL, 0u));
  TEST_ASSERT_FALSE(
      esy_proto_modbus_verify_rtu_frame(rtu_bad_crc, sizeof(rtu_bad_crc)));
  TEST_ASSERT_FALSE(
      esy_proto_modbus_verify_tcp_frame(tcp_bad_pid, sizeof(tcp_bad_pid)));
  TEST_ASSERT_FALSE(
      esy_proto_modbus_verify_tcp_frame(tcp_bad_len, sizeof(tcp_bad_len)));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_proto_modbus_parse_frame(ESY_PROTO_MODBUS_FRAME_RTU,
                                                 NULL, 0u, &view));
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_proto_modbus_parse_frame(
                                      ESY_PROTO_MODBUS_FRAME_TCP, tcp_bad_pid,
                                      sizeof(tcp_bad_pid), &view));
  TEST_ASSERT_EQUAL(
      ESY_ERR_INVALID_ARG,
      esy_proto_modbus_parse_frame((esy_proto_modbus_frame_type_t)99,
                                   tcp_bad_pid, sizeof(tcp_bad_pid), &view));
}

void test_build_and_parse_accept_configured_pdu_limit(void) {
  uint8_t pdu[ESY_PROTO_MODBUS_MAX_PDU_LEN];
  uint8_t buf[ESY_PROTO_MODBUS_MAX_PDU_LEN + ESY_PROTO_MODBUS_MBAP_LEN];
  esy_proto_modbus_frame_t frame;
  esy_proto_modbus_frame_view_t view;
  memset(pdu, 0x03, sizeof(pdu));

  TEST_ASSERT_EQUAL(ESY_OK,
                    esy_proto_modbus_frame_init(&frame, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_build_rtu_frame(
                                &frame, 1u, pdu, sizeof(pdu)));
  TEST_ASSERT_TRUE(esy_proto_modbus_verify_rtu_frame(buf, frame.len));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_parse_frame(
                                ESY_PROTO_MODBUS_FRAME_RTU, buf,
                                (uint16_t)frame.len, &view));
  TEST_ASSERT_EQUAL_UINT16(sizeof(pdu), view.pdu_len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(pdu, view.pdu, sizeof(pdu));

  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_build_tcp_frame(
                                &frame, 7u, 1u, pdu, sizeof(pdu)));
  TEST_ASSERT_TRUE(esy_proto_modbus_verify_tcp_frame(buf, frame.len));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_parse_frame(
                                ESY_PROTO_MODBUS_FRAME_TCP, buf,
                                (uint16_t)frame.len, &view));
  TEST_ASSERT_EQUAL_UINT16(sizeof(pdu), view.pdu_len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(pdu, view.pdu, sizeof(pdu));
}

void test_over_limit_build_preserves_descriptor_and_destination(void) {
  uint8_t pdu[254] = {0x03u};
  uint8_t buf[sizeof(pdu) + ESY_PROTO_MODBUS_MBAP_LEN];
  uint8_t before[sizeof(buf)];
  const uint16_t lengths[] = {ESY_PROTO_MODBUS_MAX_PDU_LEN + 1u, 254u,
                               UINT16_MAX};
  esy_proto_modbus_frame_t frame;
  memset(buf, 0xA5, sizeof(buf));
  memcpy(before, buf, sizeof(buf));
  TEST_ASSERT_EQUAL(ESY_OK,
                    esy_proto_modbus_frame_init(&frame, buf, sizeof(buf)));
  frame.len = 11u;

  for (size_t i = 0u; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_build_rtu_frame(
                                               &frame, 1u, pdu, lengths[i]));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_build_tcp_frame(
                                               &frame, 7u, 1u, pdu, lengths[i]));
    TEST_ASSERT_EQUAL_PTR(buf, frame.buf);
    TEST_ASSERT_EQUAL_UINT(sizeof(buf), frame.buf_size);
    TEST_ASSERT_EQUAL_UINT(11u, frame.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(before, buf, sizeof(buf));
  }
}

void test_verify_and_parse_reject_valid_envelopes_above_pdu_limit(void) {
  uint8_t buf[254u + ESY_PROTO_MODBUS_MBAP_LEN];
  const uint16_t lengths[] = {ESY_PROTO_MODBUS_MAX_PDU_LEN + 1u, 254u};
  esy_proto_modbus_frame_view_t view;
  esy_proto_modbus_frame_view_t before;
  memset(&view, 0xA5, sizeof(view));
  memcpy(&before, &view, sizeof(view));

  for (size_t i = 0u; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
    const uint16_t pdu_len = lengths[i];
    const uint16_t rtu_len = pdu_len + 3u;
    memset(buf, 0x03, sizeof(buf));
    buf[0] = 1u;
    const uint16_t crc = esy_proto_modbus_crc16(buf, pdu_len + 1u);
    buf[rtu_len - 2u] = (uint8_t)crc;
    buf[rtu_len - 1u] = (uint8_t)(crc >> 8);
    TEST_ASSERT_FALSE(esy_proto_modbus_verify_rtu_frame(buf, rtu_len));
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_proto_modbus_parse_frame(
                                       ESY_PROTO_MODBUS_FRAME_RTU, buf,
                                       rtu_len, &view));
    TEST_ASSERT_EQUAL_MEMORY(&before, &view, sizeof(view));

    const uint16_t tcp_len = pdu_len + ESY_PROTO_MODBUS_MBAP_LEN;
    buf[2] = 0u;
    buf[3] = 0u;
    buf[4] = (uint8_t)((pdu_len + 1u) >> 8);
    buf[5] = (uint8_t)(pdu_len + 1u);
    TEST_ASSERT_FALSE(esy_proto_modbus_verify_tcp_frame(buf, tcp_len));
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_proto_modbus_parse_frame(
                                       ESY_PROTO_MODBUS_FRAME_TCP, buf,
                                       tcp_len, &view));
    TEST_ASSERT_EQUAL_MEMORY(&before, &view, sizeof(view));
  }
}

void test_parse_failure_preserves_existing_view(void) {
  const uint8_t short_frame[] = {1u, 3u};
  esy_proto_modbus_frame_view_t view;
  esy_proto_modbus_frame_view_t before;
  memset(&view, 0xA5, sizeof(view));
  memcpy(&before, &view, sizeof(view));

  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_proto_modbus_parse_frame(
                                     ESY_PROTO_MODBUS_FRAME_RTU, short_frame,
                                     sizeof(short_frame), &view));
  TEST_ASSERT_EQUAL_MEMORY(&before, &view, sizeof(view));
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_proto_modbus_parse_frame(
                                     ESY_PROTO_MODBUS_FRAME_TCP, short_frame,
                                     sizeof(short_frame), &view));
  TEST_ASSERT_EQUAL_MEMORY(&before, &view, sizeof(view));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_parse_frame(
                                           (esy_proto_modbus_frame_type_t)99,
                                           short_frame, sizeof(short_frame),
                                           &view));
  TEST_ASSERT_EQUAL_MEMORY(&before, &view, sizeof(view));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_parse_frame(
                                           ESY_PROTO_MODBUS_FRAME_RTU, NULL,
                                           0u, &view));
  TEST_ASSERT_EQUAL_MEMORY(&before, &view, sizeof(view));
}
