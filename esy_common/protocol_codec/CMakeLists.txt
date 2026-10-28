# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

add_library(esy_common_protocol_codec STATIC
    src/esy_proto_bin.c
    src/esy_proto_modbus.c
    src/esy_proto_tlv.c
)

target_include_directories(esy_common_protocol_codec
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(esy_common_protocol_codec
    PUBLIC
        esy_common_defs
    PRIVATE
        esy_common_utils
)

if(TARGET esy_opt_default)
    target_link_libraries(esy_common_protocol_codec PRIVATE esy_opt_default)
endif()

if(ENABLE_ESY_COMMON_PROTOCOL_JSON)
    if(NOT ENABLE_ESY_COMMON_PROTOCOL)
        message(FATAL_ERROR "ENABLE_ESY_COMMON_PROTOCOL_JSON requires ENABLE_ESY_COMMON_PROTOCOL.")
    endif()
    foreach(esy_json_dependency IN ITEMS esy_common_defs esy_json_parser_provider)
        if(NOT TARGET ${esy_json_dependency})
            message(FATAL_ERROR
                "esy_common_protocol_json requires target ${esy_json_dependency}; provide coreJSON v3.3.1 through esy_json_parser_provider or cmake/esy_corejson_provider.cmake.")
        endif()
    endforeach()

    add_library(esy_common_protocol_json STATIC
        src/esy_proto_json_reader.c
        src/esy_proto_json_writer.c
    )
    add_library(Esy::protocol_json ALIAS esy_common_protocol_json)
    target_compile_features(esy_common_protocol_json PUBLIC c_std_11)
    target_include_directories(esy_common_protocol_json PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include)
    target_link_libraries(esy_common_protocol_json
        PUBLIC esy_common_defs
        PRIVATE esy_json_parser_provider)
    if(TARGET esy_opt_default)
        target_link_libraries(esy_common_protocol_json PRIVATE esy_opt_default)
    endif()
endif()
