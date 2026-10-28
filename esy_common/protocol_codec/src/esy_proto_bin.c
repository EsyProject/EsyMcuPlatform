/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/protocol/esy_proto_bin.h"

#include <string.h>

#include "esy_common/utils/integrity/esy_crc32.h"

esy_err_t esy_proto_bin_frame_init(esy_proto_bin_frame_t *frame, uint8_t *buf,
                                   size_t buf_size) {
  if (!frame || !buf || buf_size == 0u) {
    return ESY_ERR_INVALID_ARG;
  }

  frame->buf = buf;
  frame->buf_size = buf_size;
  frame->len = 0;
  return ESY_OK;
}

esy_err_t esy_proto_bin_frame_put_header(esy_proto_bin_frame_t *frame,
                                         uint8_t cmd, uint8_t seq) {
  if (!frame || !frame->buf) {
    return ESY_ERR_INVALID_ARG;
  }
  if (frame->buf_size < 5u) {
    return ESY_ERR_NO_MEM;
  }

  frame->len = 0;
  frame->buf[frame->len++] = ESY_PROTO_BIN_HEAD;
  frame->buf[frame->len++] = cmd;
  frame->buf[frame->len++] = seq;
  frame->buf[frame->len++] = 0u;
  frame->buf[frame->len++] = 0u;
  return ESY_OK;
}

esy_err_t esy_proto_bin_frame_put_data(esy_proto_bin_frame_t *frame,
                                       const void *data, size_t data_len) {
  if (!frame || !frame->buf || (data_len > 0u && !data)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (frame->len < 5u) {
    return ESY_ERR_INVALID_ARG;
  }
  if (frame->len > frame->buf_size || frame->buf_size - frame->len < 4u ||
      data_len > frame->buf_size - frame->len - 4u) {
    return ESY_ERR_NO_MEM;
  }
  if (data_len == 0u) {
    return ESY_OK;
  }

  memcpy(&frame->buf[frame->len], data, data_len);
  frame->len += data_len;
  return ESY_OK;
}

esy_err_t esy_proto_bin_frame_finish(esy_proto_bin_frame_t *frame) {
  if (!frame || !frame->buf || frame->len < 5u) {
    return ESY_ERR_INVALID_ARG;
  }
  if (frame->len > frame->buf_size || frame->buf_size - frame->len < 4u) {
    return ESY_ERR_NO_MEM;
  }
  if ((frame->len - 5u) > UINT16_MAX) {
    return ESY_ERR_INVALID_ARG;
  }

  uint16_t data_len = (uint16_t)(frame->len - 5u);
  frame->buf[3] = (data_len >> 8) & 0xFF;
  frame->buf[4] = data_len & 0xFF;
  uint32_t crc = ~esy_crc32(0xFFFFFFFF, frame->buf, frame->len);
  frame->buf[frame->len++] = (crc >> 24) & 0xFF;
  frame->buf[frame->len++] = (crc >> 16) & 0xFF;
  frame->buf[frame->len++] = (crc >> 8) & 0xFF;
  frame->buf[frame->len++] = crc & 0xFF;
  return ESY_OK;
}

size_t esy_proto_bin_frame_length(const esy_proto_bin_frame_t *frame) {
  if (!frame) {
    return 0u;
  }
  return frame->len;
}

esy_err_t esy_proto_bin_parser_init(esy_proto_bin_parser_t *parser) {
  if (!parser) {
    return ESY_ERR_INVALID_ARG;
  }

  parser->state = ESY_PROTO_BIN_STATE_HEAD;
  parser->expected_len = 0;
  return ESY_OK;
}

void esy_proto_bin_parser_reset(esy_proto_bin_parser_t *parser,
                                esy_proto_bin_frame_t *frame) {
  if (!parser || !frame) {
    return;
  }

  parser->state = ESY_PROTO_BIN_STATE_HEAD;
  parser->expected_len = 0;
  frame->len = 0;
}

esy_proto_bin_feed_result_t esy_proto_bin_parser_feed(
    esy_proto_bin_parser_t *parser, esy_proto_bin_frame_t *frame,
    uint8_t byte) {
  if (!parser || !frame || !frame->buf || frame->buf_size == 0u) {
    return ESY_PROTO_BIN_FEED_CRC_ERROR;
  }
  if (frame->len >= frame->buf_size) {
    return ESY_PROTO_BIN_FEED_CRC_ERROR;
  }

  frame->buf[frame->len++] = byte;

  switch (parser->state) {
    case ESY_PROTO_BIN_STATE_HEAD:
      if (byte == ESY_PROTO_BIN_HEAD) {
        parser->state = ESY_PROTO_BIN_STATE_FIXED;
      } else {
        frame->len = 0;
      }
      break;

    case ESY_PROTO_BIN_STATE_FIXED:
      if (frame->len >= 5) {
        parser->expected_len = (frame->buf[3] << 8) | frame->buf[4];
        if (5 + parser->expected_len + 4 > frame->buf_size) {
          return ESY_PROTO_BIN_FEED_CRC_ERROR;
        }
        parser->state = parser->expected_len > 0 ? ESY_PROTO_BIN_STATE_DATA
                                                 : ESY_PROTO_BIN_STATE_CRC;

        if (frame->buf[1] < ESY_PROTO_BIN_CMD_PING ||
            frame->buf[1] >= ESY_PROTO_BIN_CMD_MAX) {
          esy_proto_bin_parser_reset(parser, frame);
        }
      }
      break;

    case ESY_PROTO_BIN_STATE_DATA:
      if (frame->len >= 5 + parser->expected_len) {
        parser->state = ESY_PROTO_BIN_STATE_CRC;
      }
      break;

    case ESY_PROTO_BIN_STATE_CRC:
      if (frame->len >= 5 + parser->expected_len + 4) {
        uint32_t crc_recv = ((uint32_t)frame->buf[frame->len - 4] << 24) |
                            ((uint32_t)frame->buf[frame->len - 3] << 16) |
                            ((uint32_t)frame->buf[frame->len - 2] << 8) |
                            frame->buf[frame->len - 1];
        uint32_t crc_calc = ~esy_crc32(0xFFFFFFFF, frame->buf, frame->len - 4);

        parser->state = ESY_PROTO_BIN_STATE_HEAD;

        if (crc_recv == crc_calc) {
          return ESY_PROTO_BIN_FEED_OK;
        } else {
          return ESY_PROTO_BIN_FEED_CRC_ERROR;
        }
      }
      break;
  }

  return ESY_PROTO_BIN_FEED_INCOMPLETE;
}

const uint8_t *esy_proto_bin_frame_get_data(
    const esy_proto_bin_frame_t *frame) {
  if (!frame || !frame->buf || frame->len < 5u) {
    return NULL;
  }
  return &frame->buf[5];
}

size_t esy_proto_bin_frame_get_data_len(const esy_proto_bin_frame_t *frame) {
  if (!frame || !frame->buf || frame->len < 5u) {
    return 0u;
  }
  return (frame->buf[3] << 8) | frame->buf[4];
}

uint8_t esy_proto_bin_frame_get_cmd(const esy_proto_bin_frame_t *frame) {
  if (!frame || !frame->buf || frame->len < 2u) {
    return 0u;
  }
  return frame->buf[1];
}

uint8_t esy_proto_bin_frame_get_seq(const esy_proto_bin_frame_t *frame) {
  if (!frame || !frame->buf || frame->len < 3u) {
    return 0u;
  }
  return frame->buf[2];
}
