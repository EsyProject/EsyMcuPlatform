# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Verify pure management targets and explicitly selected provider adapters."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
MODULE = ROOT / "esy_middleware/remote_manager"
PROJECT = r"""
cmake_minimum_required(VERSION 3.20)
project(remote_manager_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra -Werror -pedantic)
include(esy_feature_import.cmake)
add_library(esy_opt_default INTERFACE)
target_compile_definitions(esy_opt_default INTERFACE PRIVATE_OPTIONS_SENTINEL)
add_subdirectory("${ROOT}/esy_common" common)
if(ENABLE_ESY_FLASH)
    add_library(esy_hal STATIC "${ROOT}/esy_test/fake/hal/fake_esy_hal_flash.c")
    target_include_directories(esy_hal PUBLIC "${ROOT}/esy_hal/include"
        PRIVATE "${ROOT}/esy_test/fake/hal")
    target_link_libraries(esy_hal PUBLIC esy_common_defs)
endif()
add_subdirectory("${ROOT}/esy_middleware" middleware)
foreach(forbidden IN ITEMS esy_os esy_hal_backend esy_board esy_proto_bin_session
        esy_common_protocol_codec
        esy_coredump_logger esy_ota_flash)
    if(TARGET ${forbidden})
        message(FATAL_ERROR "Remote manager unexpectedly requires ${forbidden}")
    endif()
endforeach()
if(NOT ENABLE_ESY_OTA AND TARGET esy_ota_core)
    message(FATAL_ERROR "Pure management unexpectedly requires esy_ota_core")
endif()
if(NOT ENABLE_ESY_FLASH)
    foreach(forbidden IN ITEMS esy_hal esy_dual_flash_store esy_file_dual_flash_store esy_file_flash)
        if(TARGET ${forbidden})
            message(FATAL_ERROR "Pure management unexpectedly requires ${forbidden}")
        endif()
    endforeach()
endif()
file(WRITE "${CMAKE_BINARY_DIR}/targets.txt" "")
if(ENABLE_ESY_REMOTE_MANAGER)
    if(NOT TARGET esy_remote_manager)
        message(FATAL_ERROR "Enabled manager target is missing")
    endif()
    file(APPEND "${CMAKE_BINARY_DIR}/targets.txt" "esy_remote_manager\n")
    get_target_property(core_sources esy_remote_manager SOURCES)
    file(WRITE "${CMAKE_BINARY_DIR}/core_sources.txt" "${core_sources}")
    add_library(public_headers OBJECT public.c public.cpp config.c config.cpp
        ota_contract.c ota_contract.cpp coredump.c coredump.cpp
        stats.c stats.cpp stats_table.c stats_table.cpp record_id.c record_id.cpp
        diagnostics_contract.c diagnostics_contract.cpp)
    target_link_libraries(public_headers PRIVATE esy_remote_manager)
    add_executable(readme_example example.c)
    target_link_libraries(readme_example PRIVATE esy_remote_manager)
    add_executable(json_dto_consumer json_dto.c)
    target_link_libraries(json_dto_consumer PRIVATE esy_remote_manager)
    add_executable(cpp_consumer consumer.cpp)
    target_link_libraries(cpp_consumer PRIVATE esy_remote_manager)
    foreach(language IN ITEMS c cpp)
        add_executable(stats_consumer_${language} stats_consumer.${language})
        target_link_libraries(stats_consumer_${language} PRIVATE esy_remote_manager)
    endforeach()
elseif(TARGET esy_remote_manager)
    message(FATAL_ERROR "Disabled manager target was assembled")
endif()
set(adapter_flags PROTO_BIN SETTINGS COREDUMP OTA DIAGNOSTICS)
set(adapter_targets proto_bin settings coredump_store ota diagnostics)
foreach(index RANGE 0 4)
    list(GET adapter_flags ${index} flag)
    list(GET adapter_targets ${index} suffix)
    set(adapter_target "esy_remote_manager_${suffix}")
    if(ENABLE_ESY_REMOTE_MANAGER_${flag})
        if(NOT TARGET ${adapter_target})
            message(FATAL_ERROR "Enabled adapter target is missing: ${adapter_target}")
        endif()
        file(APPEND "${CMAKE_BINARY_DIR}/targets.txt" "${adapter_target}\n")
        add_library(${suffix}_headers OBJECT ${suffix}.c ${suffix}.cpp)
        target_link_libraries(${suffix}_headers PRIVATE ${adapter_target})
        foreach(language IN ITEMS c cpp)
            add_executable(${suffix}_consumer_${language} ${suffix}_consumer.${language})
            target_link_libraries(${suffix}_consumer_${language} PRIVATE ${adapter_target})
            if(flag STREQUAL "SETTINGS")
                target_link_libraries(${suffix}_consumer_${language} PRIVATE esy_file)
            elseif(flag STREQUAL "COREDUMP")
                target_link_libraries(${suffix}_consumer_${language} PRIVATE esy_coredump_file esy_file)
            endif()
        endforeach()
    elseif(TARGET ${adapter_target})
        message(FATAL_ERROR "Disabled adapter target was assembled: ${adapter_target}")
    endif()
endforeach()
"""
DIRECT_PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(remote_manager_missing_target LANGUAGES C)
set(ENABLE_ESY_REMOTE_MANAGER ON)
foreach(dependency IN ITEMS esy_common_defs esy_opt_default)
    if(NOT dependency STREQUAL OMIT_TARGET)
        add_library(${dependency} INTERFACE)
    endif()
endforeach()
add_subdirectory("${ROOT}/esy_middleware/remote_manager" remote_manager)
'''
PUBLIC_SUFFIX = r'''
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private optimization definitions leaked through the public target"
#endif
#if defined(ESY_SETTINGS_H) || defined(ESY_DUAL_FLASH_STORE_H) || \
    defined(ESY_HAL_FLASH_H) || defined(ESY_OS_TASK_H) || defined(ESY_PROTO_BIN_H)
#error "A provider or transport implementation leaked into a public header"
#endif
'''
CPP_SOURCE = r'''
#include "esy_middleware/remote_manager/esy_remote_manager.h"
int main() {
    esy_remote_manager_t manager{};
    if (esy_remote_manager_init(nullptr, nullptr) != ESY_ERR_INVALID_ARG) return 1;
    return esy_remote_manager_deinit(&manager) == ESY_OK ? 0 : 2;
}
'''
WIRE_SOURCE = r'''
#include "esy_middleware/remote_manager/esy_remote_manager_proto_bin.h"
#include "esy_common/defs/protocol/esy_proto_domain_defs.h"
static esy_err_t allow(void *context,
                       const esy_remote_manager_request_t *request) {
    (void)context;
    (void)request;
    return ESY_OK;
}
int main(void) {
    esy_remote_manager_t manager = {0};
    esy_remote_manager_config_t config = {
        .max_key_length = 4u, .max_request_value_length = 1u, .max_response_value_length = 1u, .authorize = allow};
    uint8_t request[18] = {ESY_PROTO_DOMAIN_REMOTE_MANAGER, 1u, 1u, 0u,
                          0u, 0u, 1u, 0u, 0x44u, 0x33u, 0x22u, 0x11u};
    uint8_t output[25] = {0};
    size_t length = 0u;
    if (esy_remote_manager_init(&manager, &config) != ESY_OK) return 1;
    if (esy_remote_manager_proto_bin_process(&manager, request, sizeof(request),
            output, sizeof(output), &length) != ESY_OK) return 2;
    if (length != sizeof(output) || output[0] != ESY_PROTO_DOMAIN_REMOTE_MANAGER ||
        output[1] != 2u || output[2] != 1u || output[3] != 0u ||
        output[6] != 1u || output[8] != 0x44u || output[11] != 0x11u ||
        output[12] != 0u || output[13] != 0u || output[14] != 0u ||
        output[15] != 0u || output[16] != ESY_REMOTE_MANAGER_CAP_DISCOVER ||
        output[20] != 0u || output[21] != 0u) return 3;
    return esy_remote_manager_deinit(&manager) == ESY_OK ? 0 : 4;
}
'''

PROVIDER_PUBLIC_SUFFIX = r'''
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private implementation options leaked through the provider target"
#endif
'''
SETTINGS_SOURCE = r'''
#include "esy_middleware/remote_manager/esy_remote_manager_settings.h"
#include "esy_middleware/file/esy_file_ram.h"
int main(void) {
    esy_remote_manager_settings_t adapter = INITIALIZER;
    esy_remote_manager_config_ops_t ops = INITIALIZER;
    esy_file_ram_t ram = INITIALIZER;
    esy_file_t file = INITIALIZER;
    esy_settings_t settings = INITIALIZER;
    uint8_t data[128], work[128], scratch[128];
    char key_buffer[16];
    esy_file_ram_static_resources_t backing = INITIALIZER;
    backing.data_buffer = data; backing.capacity = sizeof(data);
    esy_file_ram_config_t ram_config = INITIALIZER; ram_config.static_resources = &backing;
    esy_file_ops_t file_ops = INITIALIZER;
    if (esy_file_ram_init(&ram, &ram_config) != ESY_OK || esy_file_ram_get_ops(&ram, &file_ops) != ESY_OK) return 1;
    if (esy_file_init(&file, &file_ops) != ESY_OK) return 2;
    esy_settings_static_resources_t resources = INITIALIZER;
    resources.working_buffer = work; resources.scratch_buffer = scratch; resources.buffer_size = sizeof(work);
    esy_settings_config_t settings_config = INITIALIZER;
    settings_config.file = &file; settings_config.max_key_length = 15u; settings_config.static_resources = &resources;
    if (esy_settings_init(&settings, &settings_config) != ESY_OK) return 3;
    esy_remote_manager_settings_static_resources_t keys = INITIALIZER;
    keys.key_buffer = key_buffer; keys.key_buffer_size = sizeof(key_buffer);
    esy_remote_manager_settings_config_t config = INITIALIZER;
    config.settings = &settings; config.static_resources = &keys;
    if (esy_remote_manager_settings_init(&adapter, &config) != ESY_OK ||
        esy_remote_manager_settings_get_ops(&adapter, &ops) != ESY_OK) return 4;
    const uint8_t value = 42u;
    if (ops.set(ops.context, "address", 7u, &value, 1u) != ESY_OK) return 5;
    esy_file_info_t stored = INITIALIZER;
    if (esy_file_get_info(&file, &stored) != ESY_OK || stored.exists) return 6;
    if (ops.save(ops.context) != ESY_OK || esy_settings_reload(&settings) != ESY_OK) return 7;
    uint8_t output = 0u; size_t length = 0u; bool found = false;
    if (ops.get(ops.context, "address", 7u, &output, 1u, &length, &found) != ESY_OK || !found || output != 42u) return 8;
    if (esy_remote_manager_settings_deinit(&adapter) != ESY_OK || esy_settings_deinit(&settings) != ESY_OK) return 9;
    if (esy_file_deinit(&file) != ESY_OK) return 10;
    return esy_file_ram_deinit(&ram) == ESY_OK ? 0 : 11;
}
'''
COREDUMP_SOURCE = r'''
#include "esy_middleware/remote_manager/esy_remote_manager_coredump_store.h"
#include "esy_middleware/coredump/esy_coredump_file.h"
#include "esy_middleware/file/esy_file_ram.h"
#include "esy_common/defs/diagnostics/esy_coredump_defs.h"
#include <string.h>
static esy_err_t next_id(void *context, esy_coredump_record_id_t *id) {
    unsigned *sequence = (unsigned *)context;
    memset(id, 0, sizeof(*id)); id->bytes[0] = (uint8_t)++*sequence;
    return ESY_OK;
}
static esy_err_t discard(void *context, const esy_coredump_record_id_t *id) {
    return esy_coredump_file_clear((esy_coredump_file_t *)context, id);
}
int main(void) {
    esy_remote_manager_coredump_store_t adapter = INITIALIZER;
    esy_remote_manager_coredump_ops_t ops = INITIALIZER;
    esy_coredump_t store = INITIALIZER;
    esy_coredump_file_t persistence = INITIALIZER;
    esy_file_t file = INITIALIZER;
    esy_file_ram_t ram_file = INITIALIZER;
    uint8_t file_data[ESY_COREDUMP_MAX_RECORD_BYTES], workspace[ESY_COREDUMP_MAX_RECORD_BYTES];
    esy_file_ram_static_resources_t file_resources = INITIALIZER;
    file_resources.data_buffer = file_data; file_resources.capacity = sizeof(file_data);
    esy_file_ram_config_t file_config = INITIALIZER; file_config.static_resources = &file_resources;
    esy_file_ops_t file_ops = INITIALIZER;
    if (esy_file_ram_init(&ram_file, &file_config) != ESY_OK || esy_file_ram_get_ops(&ram_file, &file_ops) != ESY_OK) return 16;
    if (esy_file_init(&file, &file_ops) != ESY_OK) return 17;
    union { max_align_t alignment; uint8_t bytes[ESY_COREDUMP_HEADER_BYTES]; } ram = INITIALIZER;
    esy_coredump_static_resources_t resources = INITIALIZER;
    resources.retained_buffer = ram.bytes; resources.retained_size = sizeof(ram.bytes);
    esy_coredump_config_t config = INITIALIZER;
    unsigned sequence = 0u;
    config.static_resources = &resources; config.new_record_id = next_id;
    config.record_id_context = &sequence;
    esy_remote_manager_coredump_store_config_t binding = INITIALIZER;
    binding.store = &store; binding.clear = discard; binding.clear_context = &persistence;
    esy_fault_snapshot_t snapshot = INITIALIZER;
    snapshot.architecture = ESY_FAULT_ARCH_ARM_M;
    snapshot.reason = ESY_FAULT_REASON_SOFTWARE;
    esy_remote_manager_coredump_info_t first = INITIALIZER;
    esy_remote_manager_coredump_info_t second = INITIALIZER;
    uint8_t byte = 0xa5u;
    if (esy_remote_manager_coredump_store_init(NULL, NULL) != ESY_ERR_INVALID_ARG) return 1;
    if (esy_remote_manager_coredump_store_get_ops(&adapter, &ops) != ESY_ERR_STATE) return 2;
    if (esy_coredump_init(&store, &config) != ESY_OK) return 3;
    esy_coredump_file_static_resources_t record_resources = INITIALIZER;
    record_resources.record_buffer = workspace; record_resources.record_capacity = sizeof(workspace);
    esy_coredump_file_config_t persistence_config = INITIALIZER;
    persistence_config.store = &store; persistence_config.file = &file;
    persistence_config.static_resources = &record_resources;
    if (esy_coredump_file_init(&persistence, &persistence_config) != ESY_OK) return 18;
    if (esy_remote_manager_coredump_store_init(&adapter, &binding) != ESY_OK) return 4;
    if (esy_remote_manager_coredump_store_get_ops(&adapter, &ops) != ESY_OK) return 5;
    if (esy_coredump_capture_fault(&snapshot, &store) != ESY_OK || esy_coredump_file_persist(&persistence) != ESY_OK) return 6;
    if (ops.info(ops.context, &first) != ESY_OK || !first.available) return 7;
    if (ops.read(ops.context, &first.record_id, 0u, &byte, 1u) != ESY_OK) return 8;
    if (ops.clear(ops.context, &first.record_id) != ESY_OK) return 9;
    if (esy_coredump_capture_fault(&snapshot, &store) != ESY_OK || esy_coredump_file_persist(&persistence) != ESY_OK) return 10;
    if (ops.info(ops.context, &second) != ESY_OK || !second.available ||
        memcmp(first.record_id.bytes, second.record_id.bytes, ESY_COREDUMP_RECORD_ID_BYTES) == 0) return 11;
    byte = 0xa5u;
    if (ops.read(ops.context, &first.record_id, 0u, &byte, 1u) != ESY_ERR_STATE || byte != 0xa5u) return 12;
    if (ops.clear(ops.context, &first.record_id) != ESY_ERR_STATE) return 13;
    if (esy_remote_manager_coredump_store_deinit(&adapter) != ESY_OK) return 14;
    if (esy_coredump_file_deinit(&persistence) != ESY_OK || esy_coredump_deinit(&store) != ESY_OK) return 15;
    if (esy_file_deinit(&file) != ESY_OK) return 19;
    return esy_file_ram_deinit(&ram_file) == ESY_OK ? 0 : 20;
}
'''
OTA_BINDING_SOURCE = r'''
#include "esy_middleware/remote_manager/esy_remote_manager.h"
static esy_err_t allow_binding(void *context,
                               const esy_remote_manager_request_t *request) {
    (void)context; (void)request; return ESY_OK;
}
static esy_err_t upload_info(void *context, esy_remote_manager_ota_info_t *info) {
    if (context == NULL) return ESY_ERR_INVALID_ARG;
    ++*(unsigned *)context;
    info->current_version = 7u;
    info->running_slot = 2;
    info->max_image_bytes = 128u;
    info->max_chunk_bytes = 8u;
    return ESY_OK;
}
static esy_err_t image_slot(void *context, uint32_t index,
                            esy_remote_manager_image_slot_t *slot) {
    if (context == NULL || index != 0u) return ESY_ERR_INVALID_ARG;
    ++*(unsigned *)context;
    esy_remote_manager_image_slot_t value = INITIALIZER;
    value.slot_count = 1u;
    value.image.slot = 42u;
    value.image.version = 7u;
    value.image.sha256[0] = 0xa5u;
    value.capacity = 128u;
    value.image_size = 8u;
    value.flags = ESY_REMOTE_MANAGER_IMAGE_PRESENT | ESY_REMOTE_MANAGER_IMAGE_BOOTABLE;
    *slot = value;
    return ESY_OK;
}
int main(void) {
    unsigned upload_calls = 0u;
    unsigned image_calls = 0u;
    const esy_remote_manager_ota_ops_t operations = {
        upload_info, NULL, NULL, NULL, NULL, NULL, NULL,
        {image_slot, NULL, NULL, &image_calls}, &upload_calls};
    esy_remote_manager_t manager = INITIALIZER;
    esy_remote_manager_config_t config = INITIALIZER;
    esy_remote_manager_request_t request = INITIALIZER;
    esy_remote_manager_response_t response = INITIALIZER;
    if (operations.context != &upload_calls || operations.images.slot != image_slot ||
        operations.images.context != &image_calls || operations.images.confirm != NULL ||
        operations.images.rollback != NULL) return 1;
    config.max_key_length = 1u;
    config.max_request_value_length = ESY_REMOTE_MANAGER_OTA_SLOT_INDEX_BYTES;
    config.authorize = allow_binding;
    config.ota_ops = operations;
    request.group = ESY_REMOTE_MANAGER_GROUP_OTA;
    request.command = ESY_REMOTE_MANAGER_OTA_INFO;
    if (esy_remote_manager_init(&manager, &config) != ESY_OK) return 2;
    if (esy_remote_manager_dispatch(&manager, &request, NULL, 0u, &response) != ESY_OK ||
        upload_calls != 1u || image_calls != 0u ||
        response.ota_info.current_version != 7u || response.ota_info.running_slot != 2 ||
        response.ota_info.max_image_bytes != 128u || response.ota_info.max_chunk_bytes != 8u)
        return 3;
    request.command = ESY_REMOTE_MANAGER_OTA_SLOT;
    if (esy_remote_manager_dispatch(&manager, &request, NULL, 0u, &response) != ESY_OK ||
        upload_calls != 1u || image_calls != 1u || response.image_slot.image.slot != 42u)
        return 4;
    request.group = ESY_REMOTE_MANAGER_GROUP_SYSTEM;
    request.command = ESY_REMOTE_MANAGER_SYSTEM_DISCOVER;
    if (esy_remote_manager_dispatch(&manager, &request, NULL, 0u, &response) != ESY_OK ||
        response.capabilities != (ESY_REMOTE_MANAGER_CAP_DISCOVER |
            ESY_REMOTE_MANAGER_CAP_OTA_INFO | ESY_REMOTE_MANAGER_CAP_OTA_SLOT)) return 5;
    if (esy_remote_manager_deinit(&manager) != ESY_OK) return 6;

    /* An upload-only provider uses named members and leaves images disabled. */
    esy_remote_manager_ota_ops_t upload_only = INITIALIZER;
    upload_only.info = upload_info;
    upload_only.context = &upload_calls;
    config.ota_ops = upload_only;
    if (esy_remote_manager_init(&manager, &config) != ESY_OK) return 7;
    if (esy_remote_manager_dispatch(&manager, &request, NULL, 0u, &response) != ESY_OK ||
        response.capabilities != (ESY_REMOTE_MANAGER_CAP_DISCOVER |
                                  ESY_REMOTE_MANAGER_CAP_OTA_INFO)) return 8;
    request.group = ESY_REMOTE_MANAGER_GROUP_OTA;
    request.command = ESY_REMOTE_MANAGER_OTA_INFO;
    if (esy_remote_manager_dispatch(&manager, &request, NULL, 0u, &response) != ESY_OK ||
        upload_calls != 2u || image_calls != 1u) return 9;
    request.command = ESY_REMOTE_MANAGER_OTA_SLOT;
    if (esy_remote_manager_dispatch(&manager, &request, NULL, 0u, &response) !=
        ESY_ERR_NOT_SUPPORTED || upload_calls != 2u || image_calls != 1u) return 10;
    return esy_remote_manager_deinit(&manager) == ESY_OK ? 0 : 11;
}
'''

JSON_DTO_SOURCE = r'''
#include "esy_middleware/remote_manager/esy_remote_manager.h"
#include <stdio.h>
static esy_err_t allow_json(void *context, const esy_remote_manager_request_t *request) {
    (void)context; (void)request; return ESY_OK;
}
static esy_err_t begin_json(void *context, const esy_image_manifest_t *manifest,
                           esy_image_status_t *status) {
    (void)context;
    if (manifest->version != 2u || manifest->image_size != 3u) return ESY_ERR_INVALID_ARG;
    *status = (esy_image_status_t){.phase = ESY_IMAGE_RECEIVING, .session_id = 7u,
                                  .total_bytes = manifest->image_size};
    return ESY_OK;
}
static esy_err_t slot_json(void *context, uint32_t index,
                          esy_remote_manager_image_slot_t *slot) {
    ++*(unsigned *)context;
    if (index != 0u) return ESY_ERR_INVALID_ARG;
    *slot = (esy_remote_manager_image_slot_t){.slot_count = 1u, .index = index,
        .image = {.slot = 42u, .version = 2u, .sha256 = {0xA5u}},
        .capacity = 128u, .image_size = 3u,
        .flags = ESY_REMOTE_MANAGER_IMAGE_PRESENT | ESY_REMOTE_MANAGER_IMAGE_RUNNING |
            ESY_REMOTE_MANAGER_IMAGE_BOOTABLE};
    return ESY_OK;
}
static esy_err_t confirm_json(void *context,
                             const esy_remote_manager_image_id_t *image,
                             esy_remote_manager_image_slot_t *slot) {
    if (image->slot != 42u || image->version != 2u || image->sha256[0] != 0xA5u)
        return ESY_ERR_STATE;
    esy_err_t result = slot_json(context, 0u, slot);
    slot->flags |= ESY_REMOTE_MANAGER_IMAGE_CONFIRMED;
    return result;
}
static esy_err_t rollback_json(void *context,
                              const esy_remote_manager_image_id_t *image,
                              esy_remote_manager_image_slot_t *slot) {
    esy_err_t result = confirm_json(context, image, slot);
    slot->flags = ESY_REMOTE_MANAGER_IMAGE_PRESENT | ESY_REMOTE_MANAGER_IMAGE_PENDING |
        ESY_REMOTE_MANAGER_IMAGE_BOOTABLE;
    return result;
}
int main(void) {
    /* This fixed fixture decodes two JSON fields; it is not a JSON parser API. */
    const char *json = "{\"version\":2,\"image_size\":3}";
    unsigned version = 0u, bytes = 0u;
    if (sscanf(json, "{\"version\":%u,\"image_size\":%u}", &version, &bytes) != 2) return 1;
    esy_remote_manager_t manager = {0};
    unsigned image_calls = 0u;
    esy_remote_manager_config_t config = {.max_key_length = 1u,
        .max_request_value_length = 104u, .max_response_value_length = 0u,
        .authorize = allow_json, .ota_ops = {.begin = begin_json,
            .images = {.slot = slot_json, .confirm = confirm_json,
                .rollback = rollback_json, .context = &image_calls}}};
    esy_remote_manager_request_t request = {.group = ESY_REMOTE_MANAGER_GROUP_OTA,
        .command = ESY_REMOTE_MANAGER_OTA_BEGIN,
        .manifest = {.version = version, .image_size = bytes}};
    esy_remote_manager_response_t response;
    if (esy_remote_manager_init(&manager, &config) != ESY_OK) return 2;
    if (esy_remote_manager_dispatch(&manager, &request, NULL, 0u, &response) != ESY_OK ||
        response.ota_status.session_id != 7u) return 3;
    request = (esy_remote_manager_request_t){.group = ESY_REMOTE_MANAGER_GROUP_OTA,
        .command = ESY_REMOTE_MANAGER_OTA_SLOT};
    if (esy_remote_manager_dispatch(&manager, &request, NULL, 0u, &response) != ESY_OK ||
        response.image_slot.image.slot != 42u || response.image_slot.slot_count != 1u) return 4;
    request = (esy_remote_manager_request_t){.group = ESY_REMOTE_MANAGER_GROUP_OTA,
        .command = ESY_REMOTE_MANAGER_OTA_CONFIRM, .image = response.image_slot.image};
    if (esy_remote_manager_dispatch(&manager, &request, NULL, 0u, &response) != ESY_OK ||
        (response.image_slot.flags & ESY_REMOTE_MANAGER_IMAGE_CONFIRMED) == 0u) return 5;
    request.command = ESY_REMOTE_MANAGER_OTA_ROLLBACK;
    if (esy_remote_manager_dispatch(&manager, &request, NULL, 0u, &response) != ESY_OK ||
        (response.image_slot.flags & ESY_REMOTE_MANAGER_IMAGE_PENDING) == 0u ||
        image_calls != 3u) return 6;
    return esy_remote_manager_deinit(&manager) == ESY_OK ? 0 : 7;
}
'''

OTA_SOURCE = r'''
#include "esy_middleware/remote_manager/esy_remote_manager_ota_adapter.h"
static esy_err_t claim(void *ctx, const esy_image_manifest_t *manifest) {
    (void)ctx; (void)manifest; return ESY_OK;
}
static esy_err_t write_bytes(void *ctx, uint32_t offset, const uint8_t *data, size_t length) {
    (void)ctx; (void)offset; (void)data; (void)length; return ESY_OK;
}
static esy_err_t done(void *ctx) { (void)ctx; return ESY_OK; }
static esy_err_t new_id(void *ctx, uint32_t *id) { (void)ctx; *id = 1u; return ESY_OK; }
static esy_err_t info(void *ctx, esy_remote_manager_ota_info_t *image) {
    (void)ctx;
    image->current_version = 1u; image->running_slot = -1;
    image->max_image_bytes = 16u; image->max_chunk_bytes = 8u;
    return ESY_OK;
}
static esy_err_t slot_info(void *context, uint32_t index,
                           esy_remote_manager_image_slot_t *slot) {
    ++*(unsigned *)context;
    if (index != 0u) return ESY_ERR_INVALID_ARG;
    slot->slot_count = 1u; slot->index = index;
    slot->image.slot = 42u; slot->image.version = 3u; slot->image.sha256[0] = 0xA5u;
    slot->capacity = 128u; slot->image_size = 8u;
    slot->flags = ESY_REMOTE_MANAGER_IMAGE_PRESENT | ESY_REMOTE_MANAGER_IMAGE_BOOTABLE;
    return ESY_OK;
}
static esy_err_t image_control(void *context,
                               const esy_remote_manager_image_id_t *image,
                               esy_remote_manager_image_slot_t *slot) {
    if (image->slot != 42u || image->version != 3u || image->sha256[0] != 0xA5u)
        return ESY_ERR_STATE;
    return slot_info(context, 0u, slot);
}
int main(void) {
    esy_ota_t ota = INITIALIZER;
    esy_ota_config_t core = INITIALIZER;
    uint8_t bytes[8];
    esy_ota_static_resources_t resources = INITIALIZER;
    resources.last_chunk_buffer = bytes; resources.last_chunk_capacity = sizeof(bytes);
    core.provider.begin = claim; core.provider.write = write_bytes;
    core.provider.finish = done; core.provider.activate = done; core.provider.abort = done;
    core.max_image_bytes = 16u; core.max_chunk_bytes = 8u;
    core.static_resources = &resources; core.new_session_id = new_id;
    esy_remote_manager_ota_adapter_t adapter = INITIALIZER;
    esy_remote_manager_ota_adapter_config_t config = INITIALIZER;
    esy_remote_manager_ota_ops_t ops = INITIALIZER;
    esy_image_manifest_t manifest = INITIALIZER;
    esy_image_status_t status = INITIALIZER;
    esy_remote_manager_image_slot_t slot = INITIALIZER;
    unsigned image_calls = 0u;
    manifest.version = 2u; manifest.image_size = 1u;
    const uint8_t image = 7u;
    config.ota = &ota; config.info = info;
    config.images.slot = slot_info;
    config.images.confirm = image_control; config.images.rollback = image_control;
    config.images.context = &image_calls;
    if (esy_ota_init(&ota, &core) != ESY_OK) return 1;
    if (esy_remote_manager_ota_adapter_init(&adapter, &config) != ESY_OK) return 2;
    if (esy_remote_manager_ota_adapter_get_ops(&adapter, &ops) != ESY_OK) return 3;
    if (ops.images.context != &image_calls || ops.context != &adapter ||
        ops.images.slot != slot_info || ops.images.confirm != image_control ||
        ops.images.rollback != image_control) return 10;
    if (ops.images.slot(ops.images.context, 0u, &slot) != ESY_OK ||
        slot.image.slot != 42u) return 11;
    if (ops.images.confirm(ops.images.context, &slot.image, &slot) != ESY_OK ||
        ops.images.rollback(ops.images.context, &slot.image, &slot) != ESY_OK ||
        image_calls != 3u) return 12;
    if (ops.begin(ops.context, &manifest, &status) != ESY_OK) return 4;
    if (ops.write(ops.context, status.session_id, 0u, &image, 1u, &status) != ESY_OK) return 5;
    if (ops.finish(ops.context, status.session_id, &status) != ESY_OK ||
        status.phase != ESY_IMAGE_READY) return 6;
    if (ops.activate(ops.context, status.session_id, &status) != ESY_OK ||
        status.phase != ESY_IMAGE_ACTIVATED) return 7;
    if (esy_remote_manager_ota_adapter_deinit(&adapter) != ESY_OK) return 8;
    return esy_ota_deinit(&ota) == ESY_OK ? 0 : 9;
}
'''

DIAGNOSTICS_SOURCE = r'''
#include "esy_middleware/remote_manager/esy_remote_manager_diagnostics_adapter.h"
static esy_err_t read_health(void *context, esy_diagnostics_result_t *result) {
    unsigned *calls = (unsigned *)context; ++calls[0];
    esy_diagnostics_result_t value = INITIALIZER;
    value.health = ESY_DIAGNOSTICS_HEALTH_OK; *result = value; return ESY_OK;
}
static esy_err_t run_test(void *context, esy_diagnostics_result_t *result) {
    unsigned *calls = (unsigned *)context; ++calls[1];
    esy_diagnostics_result_t value = INITIALIZER;
    value.health = ESY_DIAGNOSTICS_HEALTH_ERROR; value.code = 7u;
    *result = value; return ESY_OK;
}
int main(void) {
    unsigned calls[2] = {0u, 0u};
    esy_diagnostics_t core = INITIALIZER;
    esy_diagnostics_entry_t entries[2] = INITIALIZER;
    entries[0].name = "bus"; entries[0].read = read_health;
    entries[0].run = run_test; entries[0].context = calls;
    entries[1].name = "clock"; entries[1].read = read_health; entries[1].context = calls;
    esy_diagnostics_static_resources_t resources = INITIALIZER;
    resources.entries = entries; resources.count = 2u;
    esy_diagnostics_config_t core_config = INITIALIZER; core_config.static_resources = &resources;
    if (esy_diagnostics_init(&core, &core_config) != ESY_OK) return 1;
    esy_remote_manager_diagnostics_adapter_t adapter = INITIALIZER;
    esy_remote_manager_diagnostics_adapter_config_t config = INITIALIZER; config.diagnostics = &core;
    if (esy_remote_manager_diagnostics_adapter_init(&adapter, &config) != ESY_OK) return 2;
    esy_remote_manager_diagnostics_ops_t ops = INITIALIZER;
    if (esy_remote_manager_diagnostics_adapter_get_ops(&adapter, &ops) != ESY_OK) return 3;
    esy_diagnostics_snapshot_t snapshot = INITIALIZER;
    if (ops.enumerate(ops.context, 0u, &snapshot) != ESY_OK || calls[0] != 0u || calls[1] != 0u) return 4;
    if (ops.read(ops.context, "bus", 3u, &snapshot) != ESY_OK || calls[0] != 1u || calls[1] != 0u) return 5;
    if (ops.run(ops.context, "clock", 5u, &snapshot) != ESY_ERR_NOT_SUPPORTED || calls[1] != 0u) return 6;
    if (ops.run(ops.context, "bus", 3u, &snapshot) != ESY_OK || calls[1] != 1u ||
        snapshot.result.health != ESY_DIAGNOSTICS_HEALTH_ERROR || snapshot.result.code != 7u) return 7;
    if (esy_remote_manager_diagnostics_adapter_deinit(&adapter) != ESY_OK) return 8;
    return esy_diagnostics_deinit(&core) == ESY_OK ? 0 : 9;
}
'''

ADAPTERS = {
    "PROTO_BIN": ("proto_bin", WIRE_SOURCE),
    "SETTINGS": ("settings", SETTINGS_SOURCE),
    "COREDUMP": ("coredump_store", COREDUMP_SOURCE),
    "OTA": ("ota", OTA_SOURCE),
    "DIAGNOSTICS": ("diagnostics", DIAGNOSTICS_SOURCE),
}

STATS_SOURCE = r'''
#include "esy_middleware/remote_manager/esy_remote_manager.h"
#include "esy_middleware/remote_manager/esy_remote_manager_stats_table.h"
#include <string.h>
static esy_err_t allow_stats(void *context, const esy_remote_manager_request_t *request) {
    (void)context; (void)request; return ESY_OK;
}
int main(void) {
    enum { metric_count = 11 };
    const char *const names[metric_count] = {
        "system.uptime_ms", "system.reboot_count", "comm.rx_bytes", "comm.tx_bytes",
        "comm.timeouts", "ota.success_count", "ota.failure_count", "ota.last_duration_ms",
        "resource.stack_min_words", "resource.queue_peak", "resource.buffer_overflows"
    };
    const esy_remote_manager_stats_unit_t units[metric_count] = {
        ESY_REMOTE_MANAGER_STATS_UNIT_MILLISECONDS, ESY_REMOTE_MANAGER_STATS_UNIT_COUNT,
        ESY_REMOTE_MANAGER_STATS_UNIT_BYTES, ESY_REMOTE_MANAGER_STATS_UNIT_BYTES,
        ESY_REMOTE_MANAGER_STATS_UNIT_COUNT, ESY_REMOTE_MANAGER_STATS_UNIT_COUNT,
        ESY_REMOTE_MANAGER_STATS_UNIT_COUNT, ESY_REMOTE_MANAGER_STATS_UNIT_MILLISECONDS,
        ESY_REMOTE_MANAGER_STATS_UNIT_WORDS, ESY_REMOTE_MANAGER_STATS_UNIT_COUNT,
        ESY_REMOTE_MANAGER_STATS_UNIT_COUNT
    };
    esy_remote_manager_stats_sample_t samples[metric_count] = INITIALIZER;
    esy_remote_manager_stats_entry_t entries[metric_count] = INITIALIZER;
    for (size_t i = 0u; i < metric_count; ++i) {
        samples[i].value = (uint64_t)i;
        samples[i].epoch = 7u; samples[i].available = true;
        entries[i].name = names[i]; entries[i].name_length = strlen(names[i]);
        entries[i].kind = i >= 7u && i <= 9u
            ? ESY_REMOTE_MANAGER_STATS_GAUGE : ESY_REMOTE_MANAGER_STATS_COUNTER;
        entries[i].unit = units[i]; entries[i].sample = &samples[i];
    }
    const uint64_t precise_value = UINT64_C(9007199254740993);
    samples[2].value = precise_value;
    samples[1].available = false;
    esy_remote_manager_stats_table_t table = INITIALIZER;
    esy_remote_manager_stats_table_config_t table_config = INITIALIZER;
    table_config.entries = entries; table_config.count = metric_count;
    esy_remote_manager_t manager = INITIALIZER;
    esy_remote_manager_config_t config = INITIALIZER;
    config.max_key_length = ESY_REMOTE_MANAGER_STATS_NAME_MAX_BYTES;
    config.max_request_value_length = ESY_REMOTE_MANAGER_STATS_INDEX_BYTES;
    config.max_response_value_length = 0u; config.authorize = allow_stats;
    if (esy_remote_manager_stats_table_init(&table, &table_config) != ESY_OK) return 1;
    if (esy_remote_manager_stats_table_get_ops(&table, &config.stats_ops) != ESY_OK) return 2;
    if (esy_remote_manager_init(&manager, &config) != ESY_OK) return 3;
    esy_remote_manager_request_t request = INITIALIZER;
    esy_remote_manager_response_t response = INITIALIZER;
    request.group = ESY_REMOTE_MANAGER_GROUP_STATS;
    request.command = ESY_REMOTE_MANAGER_STATS_ENUMERATE;
    for (uint32_t i = 0u; i < metric_count; ++i) {
        request.stat_index = i;
        if (esy_remote_manager_dispatch(&manager, &request, NULL, 0u, &response) != ESY_OK ||
            response.stats.count != metric_count || response.stats.index != i ||
            response.stats.name_length != strlen(names[i]) ||
            memcmp(response.stats.name, names[i], strlen(names[i])) != 0) return 4;
    }
    request.stat_index = 0u; request.command = ESY_REMOTE_MANAGER_STATS_READ;
    request.key = names[2]; request.key_length = strlen(names[2]);
    if (esy_remote_manager_dispatch(&manager, &request, NULL, 0u, &response) != ESY_OK ||
        response.stats.sample.value != precise_value || !response.stats.sample.available) return 5;
    samples[2].value = UINT64_MAX; samples[2].epoch = 8u;
    if (esy_remote_manager_dispatch(&manager, &request, NULL, 0u, &response) != ESY_OK ||
        response.stats.sample.value != UINT64_MAX || response.stats.sample.epoch != 8u) return 6;
    request.key = names[1]; request.key_length = strlen(names[1]);
    if (esy_remote_manager_dispatch(&manager, &request, NULL, 0u, &response) != ESY_OK ||
        response.stats.sample.available || response.stats.sample.value != 0u) return 7;
    samples[1].available = true; samples[1].value = 0u;
    if (esy_remote_manager_dispatch(&manager, &request, NULL, 0u, &response) != ESY_OK ||
        !response.stats.sample.available || response.stats.sample.value != 0u) return 8;
    if (esy_remote_manager_deinit(&manager) != ESY_OK ||
        esy_remote_manager_stats_table_deinit(&table) != ESY_OK) return 9;
    return samples[2].value == UINT64_MAX && samples[2].epoch == 8u ? 0 : 10;
}
'''


class RemoteManagerConsumerTests(unittest.TestCase):
    def command(self, *arguments, error=None):
        result = subprocess.run(arguments, capture_output=True, text=True)
        output = result.stdout + result.stderr
        if error is not None:
            self.assertNotEqual(0, result.returncode, output)
            self.assertIn(error, output)
        else:
            self.assertEqual(0, result.returncode, output)
        return output

    def prepare(self, path):
        (path / "CMakeLists.txt").write_text(PROJECT)
        for suffix in ("c", "cpp"):
            (path / f"record_id.{suffix}").write_text(
                '#include "esy_common/defs/diagnostics/esy_coredump_defs.h"\n' + PUBLIC_SUFFIX)
        for name, header in (("public", "esy_remote_manager.h"),
                             ("config", "esy_remote_manager_config.h"),
                             ("ota_contract", "esy_remote_manager_ota.h"),
                             ("coredump", "esy_remote_manager_coredump.h"),
                             ("stats", "esy_remote_manager_stats.h"),
                             ("stats_table", "esy_remote_manager_stats_table.h"),
                             ("diagnostics_contract", "esy_remote_manager_diagnostics.h")):
            source = '#include "esy_middleware/remote_manager/' + header + '"\n'
            for suffix in ("c", "cpp"):
                (path / f"{name}.{suffix}").write_text(source + PUBLIC_SUFFIX)
        for suffix in ("c", "cpp"):
            initial = "{}" if suffix == "cpp" else "{0}"
            (path / f"stats_consumer.{suffix}").write_text(
                STATS_SOURCE.replace("INITIALIZER", initial))
        for flag, (name, consumer) in ADAPTERS.items():
            header = {'OTA': 'ota_adapter', 'DIAGNOSTICS': 'diagnostics_adapter'}.get(flag, name)
            source = '#include "esy_middleware/remote_manager/esy_remote_manager_' + header + '.h"\n'
            checks = PUBLIC_SUFFIX if flag == "PROTO_BIN" else PROVIDER_PUBLIC_SUFFIX
            for suffix in ("c", "cpp"):
                (path / f"{name}.{suffix}").write_text(source + checks)
                initial = "{}" if suffix == "cpp" else "{0}"
                adapted = consumer.replace("INITIALIZER", initial)
                if flag == "PROTO_BIN" and suffix == "cpp":
                    adapted = adapted.replace("esy_remote_manager_t manager = {0};",
                                               "esy_remote_manager_t manager{};")
                    adapted = adapted.replace(
                        "esy_remote_manager_config_t config = {\n"
                        "        .max_key_length = 4u, .max_request_value_length = 1u, .max_response_value_length = 1u, .authorize = allow};",
                        "esy_remote_manager_config_t config{};\n"
                        "    config.max_key_length = 4u; config.max_request_value_length = 1u; config.max_response_value_length = 1u; config.authorize = allow;")
                (path / f"{name}_consumer.{suffix}").write_text(adapted)
        example = re.findall(r"```c\n(.*?)\n```", (MODULE / "README.md").read_text(), re.S)
        self.assertEqual(1, len(example), "The runnable README example must be unique")
        (path / "example.c").write_text(example[0] + "\n")
        (path / "consumer.cpp").write_text(CPP_SOURCE)
        (path / "json_dto.c").write_text(JSON_DTO_SOURCE)

    def configure(self, path, enabled=True, adapters=(), error=None, cache=(),
                  settings_provider=None, coredump_provider=None,
                  file_provider=None, ota_provider=None, diagnostics_provider=None):
        selected = set(adapters)
        settings = "SETTINGS" in selected if settings_provider is None else settings_provider
        coredump = "COREDUMP" in selected if coredump_provider is None else coredump_provider
        files = settings or coredump if file_provider is None else file_provider
        flags = {
            "ENABLE_ESY_REMOTE_MANAGER": enabled,
            **{f"ENABLE_ESY_REMOTE_MANAGER_{flag}": flag in selected for flag in ADAPTERS},
            "ENABLE_ESY_SETTINGS": settings,
            "ENABLE_ESY_FILE": files,
            "ENABLE_ESY_FILE_FLASH": False,
            "ENABLE_ESY_FILE_DUAL_FLASH_STORE": False,
            "ENABLE_ESY_DUAL_FLASH_STORE": False,
            "ENABLE_ESY_FLASH": False,
            "ENABLE_ESY_OTA": "OTA" in selected if ota_provider is None else ota_provider,
            "ENABLE_ESY_OTA_FLASH": False,
            "ENABLE_ESY_DIAGNOSTICS": "DIAGNOSTICS" in selected if diagnostics_provider is None else diagnostics_provider,
            "ENABLE_ESY_COREDUMP": coredump,
            "ENABLE_ESY_COREDUMP_FILE": coredump,
            "ENABLE_ESY_COREDUMP_LOGGER": False,
        }
        (path / "esy_feature_import.cmake").write_text(
            "".join(f'set({name} {"ON" if value else "OFF"})\n'
                    for name, value in flags.items()) + 'set(ESY_RTOS None)\n')
        self.command("cmake", "-S", str(path), "-B", str(path / "out"),
                     f"-DROOT={ROOT}", *cache, error=error)
        if error is None:
            expected = ["esy_remote_manager"] if enabled else []
            expected += ["esy_remote_manager_" + suffix
                         for flag, (suffix, _) in ADAPTERS.items() if flag in selected]
            self.assertEqual(expected, (path / "out/targets.txt").read_text().splitlines())

    def build_and_run(self, path, adapters=(), enabled=True):
        self.command("cmake", "--build", str(path / "out"), "--parallel", "2")
        if not enabled:
            return
        for executable in ("readme_example", "cpp_consumer", "json_dto_consumer",
                           "stats_consumer_c", "stats_consumer_cpp"):
            self.command(str(path / "out" / executable))
        sources = (path / "out/core_sources.txt").read_text()
        for provider in ("proto_bin", "settings", "coredump_store", "ota_adapter", "diagnostics_adapter"):
            self.assertNotIn(provider, sources)
        for flag in adapters:
            name = ADAPTERS[flag][0]
            for suffix in ("c", "cpp"):
                self.command(str(path / "out" / f"{name}_consumer_{suffix}"))
        commands = (path / "out/compile_commands.json").read_text()
        self.assertNotIn("esy_middleware/os", commands)
        self.assertNotIn("esy_hal_backend", commands)
        self.assertNotIn("esy_hal/", commands)
        for flag, (suffix, _) in ADAPTERS.items():
            filename = {"OTA": "ota_adapter", "DIAGNOSTICS": "diagnostics_adapter"}.get(flag, suffix)
            if flag not in adapters:
                self.assertNotIn("adapters/esy_remote_manager_" + filename + ".c", commands)

    def test_core_public_headers_and_readme_example_without_providers(self):
        with tempfile.TemporaryDirectory(prefix="remote manager consumer ") as directory:
            path = Path(directory)
            self.prepare(path)
            self.configure(path)
            self.build_and_run(path)

    def test_current_ota_initializers_bind_context_and_optional_images_in_c_and_cpp(self):
        with tempfile.TemporaryDirectory(prefix="remote manager current ota ") as directory:
            path = Path(directory)
            self.prepare(path)
            project = path / "CMakeLists.txt"
            project.write_text(project.read_text() + r'''
foreach(language IN ITEMS c cpp)
    add_executable(ota_binding_${language} ota_binding.${language})
    target_link_libraries(ota_binding_${language} PRIVATE esy_remote_manager)
endforeach()
''')
            for suffix in ("c", "cpp"):
                initial = "{}" if suffix == "cpp" else "{0}"
                (path / f"ota_binding.{suffix}").write_text(
                    OTA_BINDING_SOURCE.replace("INITIALIZER", initial))
            self.configure(path)
            self.build_and_run(path)
            for suffix in ("c", "cpp"):
                self.command(str(path / "out" / f"ota_binding_{suffix}"))

    def test_payload_adapter_runs_without_session_hal_os_or_storage(self):
        with tempfile.TemporaryDirectory(prefix="remote manager payload ") as directory:
            path = Path(directory)
            self.prepare(path)
            self.configure(path, adapters=("PROTO_BIN",))
            self.build_and_run(path, adapters=("PROTO_BIN",))

    def test_ota_adapter_runs_real_typed_core_without_hal_os_flash_or_wire(self):
        with tempfile.TemporaryDirectory(prefix="remote manager ota ") as directory:
            path = Path(directory)
            self.prepare(path)
            self.configure(path, adapters=("OTA",))
            self.build_and_run(path, adapters=("OTA",))

    def test_ram_coredump_adapter_links_without_hal_os_or_flash(self):
        with tempfile.TemporaryDirectory(prefix="remote manager dump ") as directory:
            path = Path(directory)
            self.prepare(path)
            self.configure(path, adapters=("COREDUMP",))
            self.build_and_run(path, adapters=("COREDUMP",))

    def test_settings_adapter_links_ram_file_without_hal_flash_or_dual_flash_store(self):
        with tempfile.TemporaryDirectory(prefix="remote manager settings ") as directory:
            path = Path(directory)
            self.prepare(path)
            self.configure(path, adapters=("SETTINGS",))
            self.build_and_run(path, adapters=("SETTINGS",))

    def test_diagnostics_adapter_runs_shared_core_without_hal_or_os(self):
        with tempfile.TemporaryDirectory(prefix="remote manager diagnostics ") as directory:
            path = Path(directory)
            self.prepare(path)
            self.configure(path, adapters=("DIAGNOSTICS",))
            self.build_and_run(path, adapters=("DIAGNOSTICS",))

    def test_all_adapters_can_be_selected_together(self):
        with tempfile.TemporaryDirectory(prefix="remote manager all adapters ") as directory:
            path = Path(directory)
            self.prepare(path)
            selected = tuple(ADAPTERS)
            self.configure(path, adapters=selected)
            self.build_and_run(path, adapters=selected)

    def test_disabled_module_adds_no_targets(self):
        with tempfile.TemporaryDirectory(prefix="remote manager disabled ") as directory:
            path = Path(directory)
            self.prepare(path)
            self.configure(path, enabled=False)
            self.build_and_run(path, enabled=False)

    def test_every_adapter_requires_parent_feature(self):
        for flag in ADAPTERS:
            with self.subTest(adapter=flag), tempfile.TemporaryDirectory(prefix="remote manager invalid ") as directory:
                path = Path(directory)
                self.prepare(path)
                self.configure(path, enabled=False, adapters=(flag,),
                    error=f"ENABLE_ESY_REMOTE_MANAGER_{flag} requires ENABLE_ESY_REMOTE_MANAGER")

    def test_provider_features_are_required(self):
        cases = (
            (("DIAGNOSTICS",), {"diagnostics_provider": False},
             "ENABLE_ESY_REMOTE_MANAGER_DIAGNOSTICS requires ENABLE_ESY_DIAGNOSTICS"),
            (("OTA",), {"ota_provider": False},
             "ENABLE_ESY_REMOTE_MANAGER_OTA requires ENABLE_ESY_OTA"),
            (("SETTINGS",), {"settings_provider": False},
             "ENABLE_ESY_REMOTE_MANAGER_SETTINGS requires ENABLE_ESY_SETTINGS"),
            (("COREDUMP",), {"coredump_provider": False},
             "ENABLE_ESY_REMOTE_MANAGER_COREDUMP requires ENABLE_ESY_COREDUMP"),
            (("SETTINGS",), {"file_provider": False},
             "esy_settings requires ENABLE_ESY_FILE"),
        )
        for adapters, inputs, error in cases:
            with self.subTest(inputs=inputs), tempfile.TemporaryDirectory(prefix="remote manager provider ") as directory:
                path = Path(directory)
                self.prepare(path)
                self.configure(path, adapters=adapters, error=error, **inputs)

    def test_reconfigure_removes_provider_adapters_then_core_and_reenables(self):
        with tempfile.TemporaryDirectory(prefix="remote manager reconfigure ") as directory:
            path = Path(directory)
            self.prepare(path)
            selected = tuple(ADAPTERS)
            self.configure(path, adapters=selected)
            self.build_and_run(path, adapters=selected)
            self.configure(path, adapters=("DIAGNOSTICS",))
            self.build_and_run(path, adapters=("DIAGNOSTICS",))
            self.configure(path)
            self.build_and_run(path)
            self.configure(path, enabled=False)
            self.build_and_run(path, enabled=False)
            self.configure(path, adapters=("PROTO_BIN",))
            self.build_and_run(path, adapters=("PROTO_BIN",))

    def test_feature_file_assignments_override_opposite_cache_values(self):
        with tempfile.TemporaryDirectory(prefix="remote manager feature ") as directory:
            path = Path(directory)
            self.prepare(path)
            flags = ("ENABLE_ESY_REMOTE_MANAGER",) + tuple(
                f"ENABLE_ESY_REMOTE_MANAGER_{flag}" for flag in ADAPTERS)
            self.configure(path, adapters=("PROTO_BIN", "DIAGNOSTICS"),
                           cache=tuple(f"-D{flag}=OFF" for flag in flags))
            self.build_and_run(path, adapters=("PROTO_BIN", "DIAGNOSTICS"))
            self.configure(path, enabled=False,
                           cache=tuple(f"-D{flag}=ON" for flag in flags))

    def test_required_targets_have_explicit_diagnostics(self):
        for dependency in ("esy_common_defs", "esy_opt_default"):
            with self.subTest(dependency=dependency):
                with tempfile.TemporaryDirectory(prefix="remote manager missing ") as directory:
                    path = Path(directory)
                    (path / "CMakeLists.txt").write_text(DIRECT_PROJECT)
                    self.command("cmake", "-S", str(path), "-B", str(path / "out"),
                                 f"-DROOT={ROOT}", f"-DOMIT_TARGET={dependency}",
                                 error=f"esy_remote_manager requires target {dependency}")


if __name__ == "__main__":
    unittest.main()
