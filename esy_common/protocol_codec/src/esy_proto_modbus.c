/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/protocol/esy_proto_modbus.h"

#include <string.h>

/* ------------------------------ small helpers ------------------------------
 */

static inline uint16_t u16_be_read(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline void u16_be_write(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v & 0xFFu);
}

static inline void u16_le_write(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)(v >> 8);
}

static inline uint16_t u16_le_read(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[1] << 8) | (uint16_t)p[0]);
}

/* ------------------------------- CRC16 RTU -------------------------------- */

uint16_t esy_proto_modbus_crc16(const uint8_t *data, uint16_t length) {
  uint16_t crc = 0xFFFFu;
  if (!data || length == 0) return crc;

  for (uint16_t i = 0; i < length; ++i) {
    crc ^= (uint16_t)data[i];
    for (uint8_t b = 0; b < 8; ++b) {
      if (crc & 0x0001u) {
        crc = (uint16_t)((crc >> 1) ^ 0xA001u);
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

/* ---------------------------------- init ---------------------------------- */

esy_err_t esy_proto_modbus_frame_init(esy_proto_modbus_frame_t *frame,
                                      uint8_t *buf, size_t buf_size) {
  if (!frame || !buf || buf_size == 0) return ESY_ERR_INVALID_ARG;
  frame->buf = buf;
  frame->buf_size = buf_size;
  frame->len = 0;
  return ESY_OK;
}

/* ---------------------------------- build --------------------------------- */

esy_err_t esy_proto_modbus_build_rtu_frame(esy_proto_modbus_frame_t *frame,
                                           uint8_t slave_addr,
                                           const uint8_t *pdu,
                                           uint16_t pdu_len) {
  if (!frame || !frame->buf || !pdu) return ESY_ERR_INVALID_ARG;
  if (pdu_len > ESY_PROTO_MODBUS_MAX_PDU_LEN) return ESY_ERR_INVALID_ARG;

  /* RTU frame = addr(1) + pdu + crc(2) */
  const uint32_t need = 1u + (uint32_t)pdu_len + 2u;
  if (need > frame->buf_size) return ESY_ERR_NO_MEM;
  if (pdu_len == 0) return ESY_ERR_INVALID_ARG;

  uint8_t *w = frame->buf;
  w[0] = slave_addr;
  memcpy(&w[1], pdu, pdu_len);

  const uint16_t crc = esy_proto_modbus_crc16(w, (uint16_t)(1u + pdu_len));
  u16_le_write(&w[1u + pdu_len], crc);

  frame->len = need;
  return ESY_OK;
}

esy_err_t esy_proto_modbus_build_tcp_frame(esy_proto_modbus_frame_t *frame,
                                           uint16_t transaction_id,
                                           uint8_t unit_id, const uint8_t *pdu,
                                           uint16_t pdu_len) {
  if (!frame || !frame->buf || !pdu) return ESY_ERR_INVALID_ARG;
  if (pdu_len == 0 || pdu_len > ESY_PROTO_MODBUS_MAX_PDU_LEN)
    return ESY_ERR_INVALID_ARG;

  /* TCP frame = MBAP(7) + PDU */
  const uint32_t need = (uint32_t)ESY_PROTO_MODBUS_MBAP_LEN + (uint32_t)pdu_len;
  if (need > frame->buf_size) return ESY_ERR_NO_MEM;

  uint8_t *w = frame->buf;

  /* MBAP */
  u16_be_write(&w[0], transaction_id); /* TID */
  u16_be_write(&w[2], 0u);             /* PID must be 0 for Modbus TCP */
  u16_be_write(&w[4], (uint16_t)(1u + pdu_len)); /* LEN = UID(1) + PDU */
  w[6] = unit_id;                                /* UID */

  memcpy(&w[7], pdu, pdu_len);

  frame->len = need;
  return ESY_OK;
}

/* -------------------------------- verify ---------------------------------- */

bool esy_proto_modbus_verify_rtu_frame(const uint8_t *frame,
                                       uint16_t frame_len) {
  if (!frame) return false;
  if (frame_len < ESY_PROTO_MODBUS_RTU_MIN_FRAME_LEN) return false;
  if (frame_len - 3u > ESY_PROTO_MODBUS_MAX_PDU_LEN) return false;

  /* CRC is last 2 bytes (LE) */
  const uint16_t expect = u16_le_read(&frame[frame_len - 2u]);
  const uint16_t calc =
      esy_proto_modbus_crc16(frame, (uint16_t)(frame_len - 2u));
  return expect == calc;
}

bool esy_proto_modbus_verify_tcp_frame(const uint8_t *frame,
                                       uint16_t frame_len) {
  if (!frame) return false;
  if (frame_len < ESY_PROTO_MODBUS_TCP_MIN_FRAME_LEN) return false;
  if (frame_len - ESY_PROTO_MODBUS_MBAP_LEN > ESY_PROTO_MODBUS_MAX_PDU_LEN)
    return false;

  const uint16_t pid = u16_be_read(&frame[2]);
  if (pid != 0u) return false;

  const uint16_t len_field = u16_be_read(&frame[4]);
  /* LEN counts bytes after LEN field => UID(1) + PDU */
  const uint32_t bytes_after_len = (uint32_t)frame_len - 6u;
  return (uint32_t)len_field == bytes_after_len;
}

/* ---------------------------------- parse --------------------------------- */

esy_err_t esy_proto_modbus_parse_frame(
    esy_proto_modbus_frame_type_t type, const uint8_t *frame,
    uint16_t frame_len, esy_proto_modbus_frame_view_t *out_view) {
  if (!frame || !out_view) return ESY_ERR_INVALID_ARG;

  esy_proto_modbus_frame_view_t view = {0};
  view.type = type;

  if (type == ESY_PROTO_MODBUS_FRAME_RTU) {
    if (!esy_proto_modbus_verify_rtu_frame(frame, frame_len))
      return ESY_ERR_FAIL;

    /* RTU: [ADDR][PDU][CRC] */
    const uint16_t pdu_len = (uint16_t)(frame_len - 1u - 2u);
    if (pdu_len < 1u) return ESY_ERR_FAIL;

    view.unit_id = frame[0];
    view.pdu = &frame[1];
    view.pdu_len = pdu_len;
    view.func = view.pdu[0];
    *out_view = view;

    return ESY_OK;
  }

  if (type == ESY_PROTO_MODBUS_FRAME_TCP) {
    if (!esy_proto_modbus_verify_tcp_frame(frame, frame_len))
      return ESY_ERR_FAIL;

    /* TCP: [MBAP(7)][PDU] */
    const uint16_t tid = u16_be_read(&frame[0]);
    const uint16_t pid = u16_be_read(&frame[2]);
    const uint16_t len_field = u16_be_read(&frame[4]);
    (void)len_field;

    const uint16_t pdu_len = (uint16_t)(frame_len - ESY_PROTO_MODBUS_MBAP_LEN);
    if (pdu_len < 1u) return ESY_ERR_FAIL;

    view.transaction_id = tid;
    view.protocol_id = pid;
    view.unit_id = frame[6];
    view.pdu = &frame[7];
    view.pdu_len = pdu_len;
    view.func = view.pdu[0];
    *out_view = view;

    return ESY_OK;
  }

  return ESY_ERR_INVALID_ARG;
}
