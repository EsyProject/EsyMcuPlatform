/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/remote_manager/esy_remote_manager_proto_bin.h"

#include <stdbool.h>
#include <string.h>

#include "esy_common/defs/protocol/esy_proto_domain_defs.h"

#define WIRE_DOMAIN_OFFSET 0u
#define WIRE_KIND_OFFSET 1u
#define WIRE_VERSION_OFFSET 2u
#define WIRE_RESERVED_OFFSET 3u
#define WIRE_GROUP_OFFSET 4u
#define WIRE_COMMAND_OFFSET 6u
#define WIRE_REQUEST_ID_OFFSET 8u
#define WIRE_KEY_LENGTH_OFFSET 12u
#define WIRE_VALUE_LENGTH_OFFSET 14u
#define WIRE_COMMON_BYTES 12u
#define WIRE_STATUS_OFFSET 12u
#define WIRE_CAPABILITIES_OFFSET 16u
#define WIRE_FOUND_OFFSET 20u
#define WIRE_RESPONSE_LENGTH_OFFSET 21u

static uint16_t read_u16(const uint8_t *bytes) {
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u));
}

static uint32_t read_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
           ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static void write_u32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
    bytes[2] = (uint8_t)(value >> 16u);
    bytes[3] = (uint8_t)(value >> 24u);
}

static void write_u64(uint8_t *bytes, uint64_t value) {
    for (size_t i = 0u; i < sizeof(value); ++i)
        bytes[i] = (uint8_t)(value >> (i * 8u));
}

static size_t metadata_size(const esy_remote_manager_request_t *request) {
    if (request->group == ESY_REMOTE_MANAGER_GROUP_OTA) {
        if (request->command == ESY_REMOTE_MANAGER_OTA_INFO)
            return ESY_REMOTE_MANAGER_PROTO_BIN_OTA_INFO_BYTES;
        if (request->command >= ESY_REMOTE_MANAGER_OTA_BEGIN &&
            request->command <= ESY_REMOTE_MANAGER_OTA_ACTIVATE)
            return ESY_REMOTE_MANAGER_PROTO_BIN_OTA_STATUS_BYTES;
        if (request->command == ESY_REMOTE_MANAGER_OTA_SLOT ||
            request->command == ESY_REMOTE_MANAGER_OTA_CONFIRM ||
            request->command == ESY_REMOTE_MANAGER_OTA_ROLLBACK)
            return ESY_REMOTE_MANAGER_PROTO_BIN_IMAGE_SLOT_BYTES;
    }
    if (request->group == ESY_REMOTE_MANAGER_GROUP_COREDUMP &&
        request->command == ESY_REMOTE_MANAGER_COREDUMP_INFO)
        return ESY_REMOTE_MANAGER_PROTO_BIN_COREDUMP_INFO_BYTES;
    if (request->group == ESY_REMOTE_MANAGER_GROUP_STATS &&
        (request->command == ESY_REMOTE_MANAGER_STATS_ENUMERATE ||
         request->command == ESY_REMOTE_MANAGER_STATS_READ))
        return ESY_REMOTE_MANAGER_PROTO_BIN_STATS_BYTES;
    if (request->group == ESY_REMOTE_MANAGER_GROUP_DIAGNOSTICS &&
        (request->command == ESY_REMOTE_MANAGER_DIAGNOSTICS_ENUMERATE ||
         request->command == ESY_REMOTE_MANAGER_DIAGNOSTICS_READ ||
         request->command == ESY_REMOTE_MANAGER_DIAGNOSTICS_RUN))
        return ESY_REMOTE_MANAGER_PROTO_BIN_DIAGNOSTICS_BYTES;
    return 0u;
}

static void encode_metadata(const esy_remote_manager_request_t *request,
                             const esy_remote_manager_response_t *response,
                             uint8_t *output) {
    if (request->group == ESY_REMOTE_MANAGER_GROUP_OTA &&
        request->command == ESY_REMOTE_MANAGER_OTA_INFO) {
        write_u32(output, response->ota_info.current_version);
        write_u32(output + 4u, (uint32_t)response->ota_info.running_slot);
        write_u32(output + 8u, response->ota_info.max_image_bytes);
        write_u32(output + 12u, response->ota_info.max_chunk_bytes);
    } else if (request->group == ESY_REMOTE_MANAGER_GROUP_OTA &&
               (request->command == ESY_REMOTE_MANAGER_OTA_SLOT ||
                request->command == ESY_REMOTE_MANAGER_OTA_CONFIRM ||
                request->command == ESY_REMOTE_MANAGER_OTA_ROLLBACK)) {
        write_u32(output, response->image_slot.slot_count);
        write_u32(output + 4u, response->image_slot.index);
        write_u32(output + 8u, response->image_slot.image.slot);
        write_u32(output + 12u, response->image_slot.image.version);
        write_u32(output + 16u, response->image_slot.capacity);
        write_u32(output + 20u, response->image_slot.image_size);
        write_u32(output + 24u, response->image_slot.flags);
        memcpy(output + 28u, response->image_slot.image.sha256,
               ESY_IMAGE_SHA256_BYTES);
    } else if (request->group == ESY_REMOTE_MANAGER_GROUP_OTA) {
        write_u32(output, (uint32_t)response->ota_status.phase);
        write_u32(output + 4u, response->ota_status.session_id);
        write_u32(output + 8u, response->ota_status.next_offset);
        write_u32(output + 12u, response->ota_status.total_bytes);
        write_u32(output + 16u, (uint32_t)response->ota_status.result);
        write_u32(output + 20u, response->ota_status.result_valid ? 1u : 0u);
    } else if (request->group == ESY_REMOTE_MANAGER_GROUP_STATS) {
        write_u32(output, response->stats.count);
        write_u32(output + 4u, response->stats.index);
        write_u32(output + 8u, (uint32_t)response->stats.kind);
        write_u32(output + 12u, (uint32_t)response->stats.unit);
        write_u32(output + 16u, response->stats.sample.available ? 1u : 0u);
        write_u32(output + 20u, (uint32_t)response->stats.name_length);
        write_u64(output + 24u, response->stats.sample.epoch);
        write_u64(output + 32u, response->stats.sample.value);
        memset(output + 40u, 0, ESY_REMOTE_MANAGER_STATS_NAME_MAX_BYTES);
        memcpy(output + 40u, response->stats.name, response->stats.name_length);
    } else if (request->group == ESY_REMOTE_MANAGER_GROUP_DIAGNOSTICS) {
        write_u32(output, response->diagnostic.count);
        write_u32(output + 4u, response->diagnostic.index);
        write_u32(output + 8u, response->diagnostic.capabilities);
        write_u32(output + 12u, (uint32_t)response->diagnostic.result.health);
        write_u32(output + 16u, response->diagnostic.result.code);
        write_u32(output + 20u, (uint32_t)response->diagnostic.name_length);
        write_u32(output + 24u, (uint32_t)response->diagnostic.result.detail_length);
        memset(output + 28u, 0, ESY_DIAGNOSTICS_NAME_MAX_BYTES + ESY_DIAGNOSTICS_DETAIL_MAX_BYTES);
        memcpy(output + 28u, response->diagnostic.name, response->diagnostic.name_length);
        memcpy(output + 76u, response->diagnostic.result.detail, response->diagnostic.result.detail_length);
    } else {
        write_u32(output, response->coredump_info.available ? 1u : 0u);
        write_u32(output + 4u, response->coredump_info.record_size);
        write_u32(output + 8u, response->coredump_info.record_crc);
        memcpy(output + 12u, response->coredump_info.record_id.bytes,
               ESY_COREDUMP_RECORD_ID_BYTES);
    }
}

static bool range_valid(const void *data, size_t size) {
    return size <= UINTPTR_MAX - (uintptr_t)data;
}

static bool ranges_overlap(const void *left, size_t left_size,
                           const void *right, size_t right_size) {
    if (left_size == 0u || right_size == 0u) return false;
    const uintptr_t left_begin = (uintptr_t)left;
    const uintptr_t right_begin = (uintptr_t)right;
    return left_begin < right_begin + right_size &&
           right_begin < left_begin + left_size;
}

esy_err_t esy_remote_manager_proto_bin_process(
    esy_remote_manager_t *manager, const uint8_t *payload, size_t length,
    uint8_t *output, size_t capacity, size_t *output_length) {
    if (output_length == NULL) return ESY_ERR_INVALID_ARG;
    if (!range_valid(manager, sizeof(*manager)) ||
        !range_valid(payload, length) || !range_valid(output, capacity) ||
        !range_valid(output_length, sizeof(*output_length)))
        return ESY_ERR_INVALID_ARG;
    if (ranges_overlap(payload, length, output, capacity) ||
        ranges_overlap(manager, sizeof(*manager), payload, length) ||
        ranges_overlap(manager, sizeof(*manager), output, capacity) ||
        ranges_overlap(output_length, sizeof(*output_length), payload, length) ||
        ranges_overlap(output_length, sizeof(*output_length), output, capacity) ||
        ranges_overlap(output_length, sizeof(*output_length), manager,
                       sizeof(*manager)))
        return ESY_ERR_INVALID_ARG;

    *output_length = 0u;
    if (manager == NULL || payload == NULL || (output == NULL && capacity != 0u))
        return ESY_ERR_INVALID_ARG;
    if (length < ESY_REMOTE_MANAGER_PROTO_BIN_REQUEST_HEADER_BYTES)
        return ESY_ERR_INVALID_ARG;
    if (payload[WIRE_DOMAIN_OFFSET] != ESY_PROTO_DOMAIN_REMOTE_MANAGER ||
        payload[WIRE_VERSION_OFFSET] != ESY_REMOTE_MANAGER_PROTO_BIN_VERSION)
        return ESY_ERR_NOT_SUPPORTED;
    if (payload[WIRE_KIND_OFFSET] != ESY_REMOTE_MANAGER_PROTO_BIN_REQUEST ||
        payload[WIRE_RESERVED_OFFSET] != 0u)
        return ESY_ERR_INVALID_ARG;

    const size_t key_length = read_u16(payload + WIRE_KEY_LENGTH_OFFSET);
    const uint32_t value_length = read_u32(payload + WIRE_VALUE_LENGTH_OFFSET);
    size_t remaining = length - ESY_REMOTE_MANAGER_PROTO_BIN_REQUEST_HEADER_BYTES;
    if (key_length > remaining) return ESY_ERR_INVALID_ARG;
    remaining -= key_length;
    if ((uintmax_t)remaining != (uintmax_t)value_length)
        return ESY_ERR_INVALID_ARG;
    if (capacity < ESY_REMOTE_MANAGER_PROTO_BIN_RESPONSE_HEADER_BYTES)
        return ESY_ERR_NO_MEM;

    const uint8_t *key = payload + ESY_REMOTE_MANAGER_PROTO_BIN_REQUEST_HEADER_BYTES;
    esy_remote_manager_request_t request = {
        .request_id = read_u32(payload + WIRE_REQUEST_ID_OFFSET),
        .group = read_u16(payload + WIRE_GROUP_OFFSET),
        .command = read_u16(payload + WIRE_COMMAND_OFFSET),
        .key = key_length == 0u ? NULL : (const char *)key,
        .key_length = key_length,
        .value = value_length == 0u ? NULL : key + key_length,
        .value_length = (size_t)value_length,
    };
    if (request.group == ESY_REMOTE_MANAGER_GROUP_OTA) {
        switch (request.command) {
        case ESY_REMOTE_MANAGER_OTA_BEGIN:
            if (value_length != ESY_REMOTE_MANAGER_OTA_MANIFEST_BYTES)
                return ESY_ERR_INVALID_ARG;
            request.manifest.version = read_u32(request.value);
            request.manifest.image_size = read_u32(request.value + 4u);
            memcpy(request.manifest.image_sha256, request.value + 8u,
                   ESY_IMAGE_SHA256_BYTES);
            memcpy(request.manifest.signature,
                   request.value + 8u + ESY_IMAGE_SHA256_BYTES,
                   ESY_IMAGE_SIGNATURE_BYTES);
            request.value = NULL;
            request.value_length = 0u;
            break;
        case ESY_REMOTE_MANAGER_OTA_WRITE:
            if (value_length <= ESY_REMOTE_MANAGER_OTA_WRITE_PREFIX_BYTES)
                return ESY_ERR_INVALID_ARG;
            request.session_id = read_u32(request.value);
            request.offset = read_u32(request.value + 4u);
            request.value += ESY_REMOTE_MANAGER_OTA_WRITE_PREFIX_BYTES;
            request.value_length -= ESY_REMOTE_MANAGER_OTA_WRITE_PREFIX_BYTES;
            break;
        case ESY_REMOTE_MANAGER_OTA_STATUS:
        case ESY_REMOTE_MANAGER_OTA_FINISH:
        case ESY_REMOTE_MANAGER_OTA_ABORT:
        case ESY_REMOTE_MANAGER_OTA_ACTIVATE:
            if (value_length != ESY_REMOTE_MANAGER_OTA_SESSION_BYTES)
                return ESY_ERR_INVALID_ARG;
            request.session_id = read_u32(request.value);
            request.value = NULL;
            request.value_length = 0u;
            break;
        case ESY_REMOTE_MANAGER_OTA_SLOT:
            if (value_length != ESY_REMOTE_MANAGER_OTA_SLOT_INDEX_BYTES)
                return ESY_ERR_INVALID_ARG;
            request.slot_index = read_u32(request.value);
            request.value = NULL;
            request.value_length = 0u;
            break;
        case ESY_REMOTE_MANAGER_OTA_CONFIRM:
        case ESY_REMOTE_MANAGER_OTA_ROLLBACK:
            if (value_length != ESY_REMOTE_MANAGER_OTA_IMAGE_ID_BYTES)
                return ESY_ERR_INVALID_ARG;
            request.image.slot = read_u32(request.value);
            request.image.version = read_u32(request.value + 4u);
            memcpy(request.image.sha256, request.value + 8u,
                   ESY_IMAGE_SHA256_BYTES);
            request.value = NULL;
            request.value_length = 0u;
            break;
        default:
            break;
        }
    }
    if (request.group == ESY_REMOTE_MANAGER_GROUP_COREDUMP &&
        request.command == ESY_REMOTE_MANAGER_COREDUMP_READ) {
        if (value_length != ESY_REMOTE_MANAGER_PROTO_BIN_COREDUMP_READ_BYTES)
            return ESY_ERR_INVALID_ARG;
        memcpy(request.record_id.bytes, request.value, ESY_COREDUMP_RECORD_ID_BYTES);
        request.offset = read_u32(request.value + ESY_COREDUMP_RECORD_ID_BYTES);
        request.length = read_u32(request.value + ESY_COREDUMP_RECORD_ID_BYTES + sizeof(uint32_t));
        request.value = NULL;
        request.value_length = 0u;
    }
    if (request.group == ESY_REMOTE_MANAGER_GROUP_COREDUMP &&
        request.command == ESY_REMOTE_MANAGER_COREDUMP_CLEAR) {
        if (value_length != ESY_COREDUMP_RECORD_ID_BYTES) return ESY_ERR_INVALID_ARG;
        memcpy(request.record_id.bytes, request.value, ESY_COREDUMP_RECORD_ID_BYTES);
        request.value = NULL;
        request.value_length = 0u;
    }
    if (request.group == ESY_REMOTE_MANAGER_GROUP_STATS &&
        request.command == ESY_REMOTE_MANAGER_STATS_ENUMERATE) {
        if (value_length != ESY_REMOTE_MANAGER_STATS_INDEX_BYTES)
            return ESY_ERR_INVALID_ARG;
        request.stat_index = read_u32(request.value);
        request.value = NULL;
        request.value_length = 0u;
    }
    if (request.group == ESY_REMOTE_MANAGER_GROUP_DIAGNOSTICS &&
        request.command == ESY_REMOTE_MANAGER_DIAGNOSTICS_ENUMERATE) {
        if (value_length != ESY_REMOTE_MANAGER_DIAGNOSTICS_INDEX_BYTES)
            return ESY_ERR_INVALID_ARG;
        request.diagnostic_index = read_u32(request.value);
        request.value = NULL;
        request.value_length = 0u;
    }
    esy_remote_manager_response_t response = {0};
    size_t value_capacity = capacity - ESY_REMOTE_MANAGER_PROTO_BIN_RESPONSE_HEADER_BYTES;
#if SIZE_MAX > UINT32_MAX
    if ((uintmax_t)value_capacity > UINT32_MAX) value_capacity = UINT32_MAX;
#endif
    const size_t metadata_bytes = metadata_size(&request);
    if (metadata_bytes > value_capacity ||
        (manager->initialized && metadata_bytes > manager->config.max_response_value_length)) {
        response.status = ESY_ERR_NO_MEM;
        response.value_length = metadata_bytes;
    } else {
        (void)esy_remote_manager_dispatch(
            manager, &request,
            output + ESY_REMOTE_MANAGER_PROTO_BIN_RESPONSE_HEADER_BYTES,
            value_capacity, &response);
        if (response.status == ESY_OK && metadata_bytes != 0u) {
            encode_metadata(&request, &response,
                output + ESY_REMOTE_MANAGER_PROTO_BIN_RESPONSE_HEADER_BYTES);
            response.value_length = metadata_bytes;
        }
    }

#if SIZE_MAX > UINT32_MAX
    if ((uintmax_t)response.value_length > UINT32_MAX) {
        response = (esy_remote_manager_response_t){.status = ESY_ERR_STATE};
    }
#endif
    memcpy(output, payload, WIRE_COMMON_BYTES);
    output[WIRE_KIND_OFFSET] = ESY_REMOTE_MANAGER_PROTO_BIN_RESPONSE;
    write_u32(output + WIRE_STATUS_OFFSET, (uint32_t)response.status);
    write_u32(output + WIRE_CAPABILITIES_OFFSET, response.capabilities);
    output[WIRE_FOUND_OFFSET] = response.found ? 1u : 0u;
    write_u32(output + WIRE_RESPONSE_LENGTH_OFFSET, (uint32_t)response.value_length);
    *output_length = ESY_REMOTE_MANAGER_PROTO_BIN_RESPONSE_HEADER_BYTES;
    if (response.status == ESY_OK) *output_length += response.value_length;
    return ESY_OK;
}
