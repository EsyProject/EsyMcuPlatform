/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_PROTO_MODBUS_H
#define ESY_PROTO_MODBUS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_proto_modbus.h
 * @brief Modbus frame codec helpers for RTU/TCP (build/verify/parse).
 *
 * ## Terminology (standard-aligned, but public APIs use "frame")
 * - **frame**: the on-wire complete message, equivalent to Modbus ADU.
 *   - RTU frame (ADU) = [ADDR][PDU][CRC16]
 *   - TCP frame (ADU) = [MBAP][PDU]
 * - **PDU**: protocol data unit starting from function code:
 *   [FUNC][DATA...]
 *
 * ## Responsibilities
 * This module provides:
 * - **build**: assemble a PDU into an RTU/TCP frame (ADU).
 * - **parse/view**: extract unit-id/address, function code, PDU pointer/length.
 * - **verify**: RTU CRC checking / TCP MBAP length consistency checking.
 *
 * ## Non-responsibilities
 * - No master/slave transaction state machine (timeout/retry/scheduling is handled
 *   by higher-level transport/manager modules).
 * - No RTU frame boundary detection (e.g., 3.5 character silent interval). This
 *   module assumes the caller already owns a **complete** frame.
 * - No transport I/O (UART/TCP sockets).
 */

/* -------------------------------------------------------------------------- */
/* Frame types                                                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief Modbus frame type selector.
 */
typedef enum {
  ESY_PROTO_MODBUS_FRAME_RTU = 0, /**< RTU frame: [ADDR][PDU][CRC16] */
  ESY_PROTO_MODBUS_FRAME_TCP = 1, /**< TCP frame: [MBAP][PDU]        */
} esy_proto_modbus_frame_type_t;

/* -------------------------------------------------------------------------- */
/* Basic limits                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief Maximum PDU length (excluding RTU addr/CRC and excluding TCP MBAP).
 *
 * Default is the library limit of 252 bytes (protocol maximum: 253 bytes).
 * Overrides must be in 1..253 and identical when compiling the codec and its
 * consumers. Build, verify and parse enforce this limit independently of buffer
 * capacity. A lower manager limit may further restrict submitted requests.
 */
#ifndef ESY_PROTO_MODBUS_MAX_PDU_LEN
#define ESY_PROTO_MODBUS_MAX_PDU_LEN 252u
#endif

#if ESY_PROTO_MODBUS_MAX_PDU_LEN < 1 || ESY_PROTO_MODBUS_MAX_PDU_LEN > 253
#error "ESY_PROTO_MODBUS_MAX_PDU_LEN must be in 1..253"
#endif

/**
 * @brief MBAP header length for Modbus TCP (bytes).
 *
 * MBAP = [TID(2)][PID(2)][LEN(2)][UID(1)] => 7 bytes.
 */
#define ESY_PROTO_MODBUS_MBAP_LEN 7u

/**
 * @brief Minimum RTU frame length (bytes).
 *
 * RTU minimum is: addr(1) + func(1) + crc(2) => 4 bytes.
 */
#define ESY_PROTO_MODBUS_RTU_MIN_FRAME_LEN 4u

/**
 * @brief Minimum TCP frame length (bytes).
 *
 * TCP minimum is: mbap(7) + func(1) => 8 bytes.
 */
#define ESY_PROTO_MODBUS_TCP_MIN_FRAME_LEN 8u

/* -------------------------------------------------------------------------- */
/* Frame buffer handle                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Modbus frame buffer descriptor (build/parse in-place).
 *
 * - `buf` / `buf_size` are provided and owned by the caller.
 * - `len` is written by build APIs to indicate the resulting frame length.
 *
 * This is similar in usage to a generic "frame buffer" handle, but its semantic
 * meaning is always a **complete Modbus frame (ADU)**.
 */
typedef struct {
  uint8_t *buf;      /**< [in]  External frame buffer pointer. */
  size_t   buf_size; /**< [in]  Total capacity in bytes. */
  size_t   len;      /**< [out] Current frame length in bytes. */
} esy_proto_modbus_frame_t;

/**
 * @brief A parsed, zero-copy "view" into a Modbus frame.
 *
 * @note All pointers in this view reference the original `frame` buffer.
 *       Their lifetime is bounded by the caller-managed frame buffer.
 */
typedef struct {
  esy_proto_modbus_frame_type_t type;

  /* Common fields */
  uint8_t  unit_id; /**< RTU: slave address; TCP: Unit Identifier (MBAP UID). */
  uint8_t  func;    /**< Raw function code as seen on wire (may include 0x80). */

  /**
   * @brief Pointer to PDU start (points to FUNC byte).
   *
   * PDU layout: [FUNC][DATA...]
   */
  const uint8_t *pdu;

  /**
   * @brief PDU length in bytes (FUNC + DATA...).
   */
  uint16_t pdu_len;

  /* TCP-only fields */
  uint16_t transaction_id; /**< MBAP Transaction ID (TID). */
  uint16_t protocol_id;    /**< MBAP Protocol ID (PID). Should be 0 for Modbus. */
} esy_proto_modbus_frame_view_t;

/* -------------------------------------------------------------------------- */
/* CRC16 (Modbus/RTU)                                                         */
/* -------------------------------------------------------------------------- */

/**
 * @brief Compute Modbus RTU CRC16 (poly 0xA001, init 0xFFFF).
 *
 * @param[in] data    Input bytes (typically from ADDR up to end of PDU).
 * @param[in] length  Number of bytes.
 *
 * @return CRC16 value in Modbus RTU convention.
 */
uint16_t esy_proto_modbus_crc16(const uint8_t *data, uint16_t length);

/* -------------------------------------------------------------------------- */
/* Frame buffer init                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialize a frame buffer descriptor.
 *
 * Descriptor storage and buffer bytes may contain arbitrary initial values.
 * Success replaces the binding and sets length to zero without clearing bytes.
 * Repeated init discards the previous descriptor state and frees no storage.
 * Serialize with builders/readers and stop any asynchronous buffer user before
 * rebinding or reusing storage. The caller owns the buffer and any parsed views.
 * Invalid arguments leave the descriptor and supplied buffer unchanged. After a
 * failed first init, retry successfully before building frames; a rejected
 * reinit leaves the previous initialized descriptor usable.
 *
 * @param[out] frame    Frame descriptor to initialize.
 * @param[in]  buf      External buffer pointer.
 * @param[in]  buf_size Buffer capacity in bytes.
 *
 * @retval ESY_OK              Success.
 * @retval ESY_ERR_INVALID_ARG Null pointers or zero size.
 */
esy_err_t esy_proto_modbus_frame_init(esy_proto_modbus_frame_t *frame,
                                      uint8_t *buf,
                                      size_t buf_size);

/* -------------------------------------------------------------------------- */
/* Build frames from PDU                                                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief Build an RTU frame (ADU): [ADDR][PDU][CRC16].
 *
 * @note `pdu` must start from FUNC (i.e., PDU = [FUNC][DATA...]).
 * @note CRC is computed over [ADDR][PDU] and appended as CRC16 (2 bytes).
 * @note PDU length must be in 1..ESY_PROTO_MODBUS_MAX_PDU_LEN. Failure leaves
 * the descriptor and destination buffer unchanged.
 *
 * @param[in,out] frame      Destination frame buffer descriptor.
 * @param[in]     slave_addr RTU slave address (ADDR field).
 * @param[in]     pdu        PDU bytes starting from FUNC.
 * @param[in]     pdu_len    PDU length in bytes.
 *
 * @retval ESY_OK              Built successfully and `frame->len` is updated.
 * @retval ESY_ERR_INVALID_ARG Null pointers or invalid lengths.
 * @retval ESY_ERR_NO_MEM      `frame->buf_size` is insufficient.
 * @retval ESY_ERR_FAIL        Other build failure.
 */
esy_err_t esy_proto_modbus_build_rtu_frame(esy_proto_modbus_frame_t *frame,
                                          uint8_t slave_addr,
                                          const uint8_t *pdu,
                                          uint16_t pdu_len);

/**
 * @brief Build a TCP frame (ADU): [MBAP][PDU].
 *
 * MBAP = [TID(2)][PID(2)][LEN(2)][UID(1)]
 * - PID is set to 0 for Modbus.
 * - LEN is the number of bytes following the LEN field:
 *     LEN = UID(1) + PDU_len
 *
 * @note `pdu` must start from FUNC (i.e., PDU = [FUNC][DATA...]).
 * @note PDU length must be in 1..ESY_PROTO_MODBUS_MAX_PDU_LEN. Failure leaves
 * the descriptor and destination buffer unchanged.
 *
 * @param[in,out] frame          Destination frame buffer descriptor.
 * @param[in]     transaction_id MBAP Transaction ID (TID).
 * @param[in]     unit_id        MBAP Unit Identifier (UID).
 * @param[in]     pdu            PDU bytes starting from FUNC.
 * @param[in]     pdu_len        PDU length in bytes.
 *
 * @retval ESY_OK              Built successfully and `frame->len` is updated.
 * @retval ESY_ERR_INVALID_ARG Null pointers or invalid lengths.
 * @retval ESY_ERR_NO_MEM      `frame->buf_size` is insufficient.
 * @retval ESY_ERR_FAIL        Other build failure.
 */
esy_err_t esy_proto_modbus_build_tcp_frame(esy_proto_modbus_frame_t *frame,
                                          uint16_t transaction_id,
                                          uint8_t unit_id,
                                          const uint8_t *pdu,
                                          uint16_t pdu_len);

/* -------------------------------------------------------------------------- */
/* Verify / Parse                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief Verify an RTU frame: basic length + CRC16 check.
 *
 * @param[in] frame      RTU frame bytes: [ADDR][PDU][CRC16].
 * @param[in] frame_len  Frame length in bytes.
 *
 * @return true if CRC and basic constraints are valid and the PDU length is in
 * 1..ESY_PROTO_MODBUS_MAX_PDU_LEN; false otherwise.
 */
bool esy_proto_modbus_verify_rtu_frame(const uint8_t *frame,
                                       uint16_t frame_len);

/**
 * @brief Verify a TCP frame: basic length + MBAP consistency check.
 *
 * Checks typically include:
 * - Minimum length (MBAP + FUNC).
 * - PDU length is at most ESY_PROTO_MODBUS_MAX_PDU_LEN.
 * - Protocol ID (PID) must be 0 for Modbus.
 * - MBAP LEN must match actual bytes following LEN.
 *
 * @param[in] frame      TCP frame bytes: [MBAP][PDU].
 * @param[in] frame_len  Frame length in bytes.
 *
 * @return true if MBAP constraints are consistent; false otherwise.
 */
bool esy_proto_modbus_verify_tcp_frame(const uint8_t *frame,
                                       uint16_t frame_len);

/**
 * @brief Parse a complete Modbus frame into a zero-copy view.
 *
 * This function verifies the input frame (RTU CRC or TCP MBAP consistency)
 * and then fills `out_view` with pointers referencing the input buffer. PDU
 * length must be in 1..ESY_PROTO_MODBUS_MAX_PDU_LEN. Failure leaves `out_view`
 * unchanged; an over-limit frame returns ESY_ERR_FAIL.
 *
 * @warning This function does NOT reassemble stream data. The caller must ensure
 *          the input is a complete frame (ADU).
 *
 * @param[in]  type       Frame type (RTU/TCP).
 * @param[in]  frame      Frame bytes.
 * @param[in]  frame_len  Frame length in bytes.
 * @param[out] out_view   Output parsed view.
 *
 * @retval ESY_OK              Parsed and verified successfully.
 * @retval ESY_ERR_INVALID_ARG Null pointers or unsupported frame type.
 * @retval ESY_ERR_FAIL        Verification failed, invalid frame length, or
 *                            PDU length exceeds the configured limit.
 */
esy_err_t esy_proto_modbus_parse_frame(esy_proto_modbus_frame_type_t type,
                                       const uint8_t *frame,
                                       uint16_t frame_len,
                                       esy_proto_modbus_frame_view_t *out_view);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_PROTO_MODBUS_H */
