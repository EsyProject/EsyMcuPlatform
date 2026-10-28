/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_proto_bin.h
 * @brief Lightweight binary framing protocol (ProtoBin) codec.
 *
 * This module provides:
 * - Frame building (encode): write header, append payload, finalize with LEN +
 * CRC.
 * - Incremental parsing (decode): a byte-fed state machine that detects
 * complete frames.
 *
 * Frame layout:
 *   HEAD | CMD | SEQ | LEN(2) | DATA | CRC32(4)
 *
 * Design notes:
 * - Transport-agnostic: UART/SPI/USB/TCP streams can reuse the same codec.
 * - No OS dependency, no dynamic allocation.
 * - The caller owns the buffers; this module only reads/writes within provided
 * buffers.
 */

#ifndef ESY_PROTO_BIN_H
#define ESY_PROTO_BIN_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fixed head byte of a ProtoBin frame.
 *
 * The parser searches for this byte to start a new frame.
 */
#define ESY_PROTO_BIN_HEAD           0xAA

/**
 * @brief Fixed overhead bytes excluding payload (DATA).
 *
 * Minimal overhead is:
 * - HEAD(1) + CMD(1) + SEQ(1) + LEN(2) + CRC32(4) = 9
 */
#define ESY_PROTO_BIN_OVERHEAD_BYTES 9u

/**
 * @brief Example command codes.
 *
 * @note The parser accepts only this fixed command set. Extensions require a
 * shared codec change.
 */
typedef enum {
  ESY_PROTO_BIN_CMD_PING = 0x01, /**< Ping request. */
  ESY_PROTO_BIN_CMD_PONG = 0x02, /**< Ping response. */
  ESY_PROTO_BIN_CMD_DATA = 0x03, /**< Generic data frame. */
  ESY_PROTO_BIN_CMD_ACK = 0x04,  /**< ACK control frame. */
  ESY_PROTO_BIN_CMD_NACK = 0x05, /**< NACK control frame. */
  ESY_PROTO_BIN_CMD_MAX = ESY_PROTO_BIN_CMD_NACK + 1
} esy_proto_bin_cmd_t;

/**
 * @brief Frame context for both building and receiving.
 *
 * The caller provides a raw buffer. The codec writes/reads within [buf,
 * buf+buf_size).
 *
 * - For building: @ref len is the current write offset.
 * - For parsing:  @ref len is the current parsed length (bytes accumulated into
 * buf).
 *
 * Ownership/lifetime:
 * - The buffer is owned by the caller. Any pointer returned by getter APIs
 *   points into this buffer and remains valid until the buffer is
 * reused/overwritten.
 */
typedef struct {
  uint8_t *buf;    /**< [in]  Caller-provided frame buffer base address. */
  size_t buf_size; /**< [in]  Total buffer size in bytes. Must be >= overhead +
                      payload. */
  size_t len;      /**< [out] Current written/parsed length in bytes. */
} esy_proto_bin_frame_t;

/**
 * @brief Parser state machine states.
 */
typedef enum {
  ESY_PROTO_BIN_STATE_HEAD,  /**< Waiting for HEAD. */
  ESY_PROTO_BIN_STATE_FIXED, /**< Receiving fixed fields: CMD/SEQ/LEN. */
  ESY_PROTO_BIN_STATE_DATA,  /**< Receiving payload (DATA). */
  ESY_PROTO_BIN_STATE_CRC    /**< Receiving CRC field. */
} esy_proto_bin_parse_state_t;

/**
 * @brief Parser feed result.
 */
typedef enum {
  ESY_PROTO_BIN_FEED_INCOMPLETE =
      0,                 /**< Frame not complete yet; keep feeding bytes. */
  ESY_PROTO_BIN_FEED_OK, /**< A complete frame is received and CRC is valid. */
  ESY_PROTO_BIN_FEED_CRC_ERROR /**< A complete frame is received but CRC check
                                  failed. */
} esy_proto_bin_feed_result_t;

/**
 * @brief Parser context.
 *
 * This is the minimal state required for incremental parsing.
 * - @ref state tracks which section is being parsed.
 * - @ref expected_len is the payload length extracted from LEN field.
 */
typedef struct {
  esy_proto_bin_parse_state_t state; /**< [in,out] Current parser state. */
  size_t expected_len; /**< [in,out] Expected payload length from LEN field. */
} esy_proto_bin_parser_t;

/* -------------------------------------------------------------------------- */
/* Frame building (encoding) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Bind a frame context to a caller-provided buffer and reset length.
 *
 * Frame storage and buffer bytes may contain arbitrary initial values. Success
 * replaces the descriptor and sets length to zero without clearing buffer bytes.
 * Repeated init is allowed only after feed/build operations and all readers or
 * asynchronous users of the old frame/buffer have stopped; it frees no storage.
 * Invalid arguments leave the descriptor and supplied buffer unchanged. After a
 * failed first init, retry successfully before frame operations or parser feed;
 * a rejected reinit preserves the previous initialized descriptor.
 *
 * @param[out] frame     Frame context to initialize.
 * @param[in]  buf       Caller-provided buffer.
 * @param[in]  buf_size  Buffer size in bytes.
 *
 * @retval ESY_OK              Success.
 * @retval ESY_ERR_INVALID_ARG Invalid parameters (NULL pointers or zero size).
 */
esy_err_t esy_proto_bin_frame_init(esy_proto_bin_frame_t *frame, uint8_t *buf,
                                   size_t buf_size);

/**
 * @brief Write the frame header (HEAD/CMD/SEQ) and reserve LEN for later fill.
 *
 * After this call, payload can be appended by @ref
 * esy_proto_bin_frame_put_data. LEN and CRC are written by @ref
 * esy_proto_bin_frame_finish.
 *
 * @param[in,out] frame  Frame context.
 * @param[in]     cmd    Command byte.
 * @param[in]     seq    Sequence byte.
 *
 * @retval ESY_OK              Success.
 * @retval ESY_ERR_INVALID_ARG Invalid parameters.
 * @retval ESY_ERR_NO_MEM      Not enough space in the buffer.
 */
esy_err_t esy_proto_bin_frame_put_header(esy_proto_bin_frame_t *frame,
                                         uint8_t cmd, uint8_t seq);

/**
 * @brief Append payload bytes into the frame.
 *
 * Can be called multiple times; LEN is finalized in @ref
 * esy_proto_bin_frame_finish.
 *
 * @param[in,out] frame     Frame context.
 * @param[in]     data      Payload bytes to append.
 * @param[in]     data_len  Payload length in bytes.
 *
 * @retval ESY_OK              Success.
 * @retval ESY_ERR_INVALID_ARG Invalid parameters.
 * @retval ESY_ERR_NO_MEM      Not enough space in the buffer.
 */
esy_err_t esy_proto_bin_frame_put_data(esy_proto_bin_frame_t *frame,
                                       const void *data, size_t data_len);

/**
 * @brief Finalize the frame: fill LEN and append CRC32.
 *
 * @param[in,out] frame  Frame context.
 *
 * @retval ESY_OK              Success.
 * @retval ESY_ERR_INVALID_ARG Invalid parameters or invalid internal state.
 * @retval ESY_ERR_NO_MEM      Not enough space to append CRC.
 */
esy_err_t esy_proto_bin_frame_finish(esy_proto_bin_frame_t *frame);

/**
 * @brief Get total frame length in bytes (including CRC).
 *
 * @param[in] frame Frame context.
 * @return Current frame length in bytes.
 */
size_t esy_proto_bin_frame_length(const esy_proto_bin_frame_t *frame);

/**
 * @brief Get pointer to payload (DATA) start.
 *
 * @param[in] frame Frame context.
 * @return Pointer to DATA start inside @ref esy_proto_bin_frame_t::buf, or NULL
 * if not available.
 */
const uint8_t *esy_proto_bin_frame_get_data(const esy_proto_bin_frame_t *frame);

/**
 * @brief Get payload (DATA) length in bytes.
 *
 * @param[in] frame Frame context.
 * @return Payload length in bytes.
 */
size_t esy_proto_bin_frame_get_data_len(const esy_proto_bin_frame_t *frame);

/**
 * @brief Get command byte (CMD).
 *
 * @param[in] frame Frame context.
 * @return CMD byte.
 */
uint8_t esy_proto_bin_frame_get_cmd(const esy_proto_bin_frame_t *frame);

/**
 * @brief Get sequence byte (SEQ).
 *
 * @param[in] frame Frame context.
 * @return SEQ byte.
 */
uint8_t esy_proto_bin_frame_get_seq(const esy_proto_bin_frame_t *frame);

/* -------------------------------------------------------------------------- */
/* Incremental parser (decoding) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialize/reset the parser to the initial state (waiting for HEAD).
 *
 * Parser storage may contain arbitrary initial bytes. Success writes every
 * parser field; repeated init discards partial parser state. This function does
 * not reset an associated frame's length or binding. Stop all feed operations
 * first, and also initialize the frame or use parser_reset before feeding a
 * previously used frame again. One owner serializes parser/frame operations.
 * NULL returns INVALID_ARG without writes; a parser requires successful init
 * (or parser_reset with an initialized frame) before feed.
 *
 * @param[out] parser Parser context.
 *
 * @retval ESY_OK              Success.
 * @retval ESY_ERR_INVALID_ARG Invalid parameters.
 */
esy_err_t esy_proto_bin_parser_init(esy_proto_bin_parser_t *parser);

/**
 * @brief Feed one byte into the parser state machine.
 *
 * When the return value is:
 * - @ref ESY_PROTO_BIN_FEED_OK:
 *   a complete frame has been assembled into @p frame->buf, and CRC is valid.
 * - @ref ESY_PROTO_BIN_FEED_CRC_ERROR:
 *   a complete frame has been assembled, but CRC validation failed.
 * - @ref ESY_PROTO_BIN_FEED_INCOMPLETE:
 *   frame is not complete yet; keep feeding.
 *
 * Buffer requirements:
 * - @p frame must be initialized with a buffer large enough to hold the full
 * frame.
 * - If the incoming payload length exceeds buffer capacity, the implementation
 * should drop/ignore the frame (implementation-defined) and the caller may
 * reset the parser.
 *
 * @param[in,out] parser  Parser context.
 * @param[in,out] frame   Frame context that provides storage for parsed bytes.
 * @param[in]     byte    Incoming byte.
 *
 * @return @ref esy_proto_bin_feed_result_t The parse result after consuming
 * this byte.
 */
esy_proto_bin_feed_result_t esy_proto_bin_parser_feed(
    esy_proto_bin_parser_t *parser, esy_proto_bin_frame_t *frame, uint8_t byte);

/**
 * @brief Reset parser state and drop the in-flight frame content.
 *
 * Use this when you want to abandon the current partial frame (e.g., on link
 * error or application-level resynchronization).
 * Also call after each OK or CRC_ERROR feed result, after readers/borrowers of
 * the previous frame have finished, before feeding the next frame. Stop feed,
 * build and buffer users first; one owner serializes these operations. Parser
 * storage needs no prior values, but frame must already be successfully bound
 * by frame_init. Reset writes parser fields and frame length only: it preserves
 * the buffer binding and bytes and performs no external-resource cleanup.
 * Repeated reset is harmless; if either argument is NULL, neither is changed.
 *
 * @param[in,out] parser Parser context.
 * @param[in,out] frame  Frame context (its length will be reset).
 */
void esy_proto_bin_parser_reset(esy_proto_bin_parser_t *parser,
                                esy_proto_bin_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* ESY_PROTO_BIN_H */
