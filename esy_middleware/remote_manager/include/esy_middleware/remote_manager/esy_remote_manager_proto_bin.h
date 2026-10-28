/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_REMOTE_MANAGER_PROTO_BIN_H
#define ESY_REMOTE_MANAGER_PROTO_BIN_H

#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/protocol/esy_proto_domain_defs.h"
#include "esy_middleware/remote_manager/esy_remote_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @file esy_remote_manager_proto_bin.h
 * @brief Stateless remote-management payload adaptation for ProtoBin DATA.
 */

/** @brief Fixed request payload bytes before the key and value spans. */
#define ESY_REMOTE_MANAGER_PROTO_BIN_REQUEST_HEADER_BYTES 18u
/** @brief Fixed response payload bytes before successful data or typed metadata. */
#define ESY_REMOTE_MANAGER_PROTO_BIN_RESPONSE_HEADER_BYTES 25u
/** @brief Serialized successful OTA INFO value size. */
#define ESY_REMOTE_MANAGER_PROTO_BIN_OTA_INFO_BYTES 16u
/** @brief Serialized successful OTA STATUS value size. */
#define ESY_REMOTE_MANAGER_PROTO_BIN_OTA_STATUS_BYTES 24u
/** @brief Serialized successful SLOT, CONFIRM or ROLLBACK snapshot size. */
#define ESY_REMOTE_MANAGER_PROTO_BIN_IMAGE_SLOT_BYTES 60u
/** @brief Serialized successful COREDUMP INFO value size. */
#define ESY_REMOTE_MANAGER_PROTO_BIN_COREDUMP_INFO_BYTES 28u
/** @brief COREDUMP READ request value: record ID, offset:u32 and length:u32. */
#define ESY_REMOTE_MANAGER_PROTO_BIN_COREDUMP_READ_BYTES ESY_REMOTE_MANAGER_COREDUMP_READ_BYTES
/** @brief Fixed successful statistics snapshot, including a padded name span. */
#define ESY_REMOTE_MANAGER_PROTO_BIN_STATS_BYTES 88u
/** @brief Fixed successful diagnostic descriptor/result with padded text spans. */
#define ESY_REMOTE_MANAGER_PROTO_BIN_DIAGNOSTICS_BYTES 172u

/** @brief Stable payload version and message-kind identifiers. */
enum {
    ESY_REMOTE_MANAGER_PROTO_BIN_VERSION = 1, /**< Complete image transaction payload version. */
    ESY_REMOTE_MANAGER_PROTO_BIN_REQUEST = 1, /**< Request message kind. */
    ESY_REMOTE_MANAGER_PROTO_BIN_RESPONSE = 2, /**< Response message kind. */
};

/**
 * @brief Dispatch one verified, nonduplicate ProtoBin DATA payload.
 *
 * The payload is independent of the ProtoBin frame header and CRC. The owner
 * validates framing and suppresses duplicates before calling this function.
 * All multibyte fields use little endian. The request layout is:
 * domain:u8, kind:u8, version:u8, reserved:u8, group:u16, command:u16,
 * request_id:u32, key_length:u16, value_length:u32, key bytes, value bytes.
 * Domain must be ESY_PROTO_DOMAIN_REMOTE_MANAGER (0x02), kind REQUEST, version
 * VERSION and reserved zero. No trailing bytes are allowed. The response shares
 * the first 12 bytes with kind RESPONSE, followed by status:i32,
 * capabilities:u32, found:u8 and value_length:u32. Successful CONFIG GET and COREDUMP READ append value bytes.
 * OTA INFO appends current_version:u32, running_slot:i32, max_image_bytes:u32,
 * max_chunk_bytes:u32. OTA transaction commands append phase:u32, session_id:u32,
 * next_offset:u32, total_bytes:u32, result:i32, result_valid:u32.
 * BEGIN takes exactly 104 bytes: version:u32, image_size:u32, SHA-256[32],
 * signature[64]. WRITE takes session_id:u32, offset:u32, then nonempty data.
 * STATUS, FINISH, ABORT and ACTIVATE take exactly session_id:u32; zero is only
 * valid for STATUS (current snapshot). INFO has no value.
 * SLOT takes exactly index:u32. CONFIRM and ROLLBACK take exactly 40 bytes:
 * slot:u32, version:u32 and SHA-256[32]. Each appends a 60-byte slot snapshot:
 * slot_count:u32, index:u32, slot:u32, version:u32, capacity:u32, image_size:u32,
 * flags:u32 and SHA-256[32]. Slot identifiers are opaque provider values;
 * enumeration indices need not equal identifiers. These operations reserve
 * 85 response bytes before authorization and do not use upload session IDs.
 * COREDUMP INFO uses available:u32, record_size:u32, record_crc:u32, record_id[16].
 * COREDUMP READ takes exactly 24 request value bytes: record_id[16], offset:u32,
 * length:u32. CLEAR takes exactly record_id[16]. Both require a nonzero ID
 * copied from INFO. IDs are opaque bytes, independent of integer endianness;
 * these become normalized request fields, not configuration value bytes.
 * STATS ENUMERATE takes exactly index:u32 as its value, without a key. STATS READ
 * takes a canonical statistic name as its key, without a value. Both return
 * exactly 88 bytes: count:u32, index:u32, kind:u32, unit:u32, available:u32,
 * name_length:u32, epoch:u64, value:u64, name[48]. Name bytes after name_length
 * are zero. Full unsigned 64-bit epoch/value precision is preserved. These
 * commands reserve 113 response bytes before authorization/provider execution.
 * DIAGNOSTICS ENUMERATE takes exactly index:u32 without a key. READ/RUN take
 * the canonical source name as key without a value. Each returns 172 bytes:
 * count:u32, index:u32, capabilities:u32, health:u32, code:u32, name_length:u32,
 * detail_length:u32, name[48], detail[96]. Text after its declared length is
 * zero padded. Full 197-byte response capacity is reserved before authorization
 * or any provider/self-test. RUN errors append no assessment or implicit retry.
 * NO_MEM reports required value length without appending data. Typed response capacity and configured response budgets are checked before
 * authorization or any provider call. Errors append no value bytes.
 *
 * One serialized non-ISR owner calls the initialized manager synchronously.
 * Input and output spans, manager storage and output_length must be mutually
 * disjoint, and must not overlap provider-owned state. Detectable overlap and
 * address-range overflow are rejected before dispatch. Otherwise-valid objects
 * and accessible spans are caller preconditions. The adapter retains no memory
 * and creates no transport, sender, session, task, timer or storage resources.
 *
 * At least RESPONSE_HEADER_BYTES writable output bytes are required before any
 * authorization or operation runs. A known typed command additionally reserves
 * its entire success response before dispatch; insufficient space encodes NO_MEM
 * with the required value length and invokes no callback. The owner reserves
 * downstream response capacity separately and owns response submission,
 * retries and reconciliation after uncertain delivery. This function never
 * replays an operation or rolls back provider side effects.
 *
 * @param manager Initialized caller-owned manager.
 * @param payload Complete request payload; must be non-NULL.
 * @param length Payload byte count.
 * @param output Response storage; NULL is valid only when capacity is zero.
 * @param capacity Accessible output bytes; GET receives the remaining capacity,
 * capped at UINT32_MAX because its wire length is 32 bits.
 * @param output_length Required response size output. Reset to zero after
 * pointer/range validation. Invalid overlapping arguments leave it unchanged.
 * @return OK when a complete response was encoded, including business errors.
 * INVALID_ARG for malformed payloads/arguments, NOT_SUPPORTED for another
 * domain or payload version, NO_MEM for less than RESPONSE_HEADER_BYTES output.
 * These failures do not dispatch and return zero output_length for valid,
 * disjoint arguments. A core GET length above UINT32_MAX encodes STATE and zero
 * length, never a truncated length. Core/provider errors otherwise appear in
 * response status. No wire response is produced for malformed requests.
 */
esy_err_t esy_remote_manager_proto_bin_process(
    esy_remote_manager_t *manager, const uint8_t *payload, size_t length,
    uint8_t *output, size_t capacity, size_t *output_length);

#ifdef __cplusplus
}
#endif

#endif /* ESY_REMOTE_MANAGER_PROTO_BIN_H */
