# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

add_subdirectory(defs)
add_subdirectory(utils)
add_subdirectory(device)

if(ENABLE_ESY_COMMON_PROTOCOL_JSON AND NOT ENABLE_ESY_COMMON_PROTOCOL)
    message(FATAL_ERROR "ENABLE_ESY_COMMON_PROTOCOL_JSON requires ENABLE_ESY_COMMON_PROTOCOL.")
endif()

if(ENABLE_ESY_COMMON_PROTOCOL)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/protocol_codec/CMakeLists.txt")
        add_subdirectory(protocol_codec)
    else()
        message(FATAL_ERROR "ENABLE_ESY_COMMON_PROTOCOL is enabled, but esy_common/protocol_codec is not available.")
    endif()
endif()

if(ENABLE_ESY_COMMON_DS)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/ds/CMakeLists.txt")
        add_subdirectory(ds)
    else()
        message(FATAL_ERROR "ENABLE_ESY_COMMON_DS is enabled, but esy_common/ds is not available.")
    endif()
endif()

if(ENABLE_ESY_COMMON_SMF)
    add_subdirectory(smf)
endif()

add_library(esy_common INTERFACE)
add_library(Esy::common ALIAS esy_common)

set_target_properties(esy_common
    PROPERTIES
        EXPORT_NAME common
)

target_link_libraries(esy_common
    INTERFACE
        esy_common_defs
        esy_common_utils
        esy_common_device
)

if(TARGET esy_common_ds)
    target_link_libraries(esy_common
        INTERFACE
            esy_common_ds
    )
endif()

if(TARGET esy_common_protocol_codec)
    target_link_libraries(esy_common
        INTERFACE
            esy_common_protocol_codec
    )
endif()

if(TARGET esy_common_protocol_json)
    target_link_libraries(esy_common
        INTERFACE
            esy_common_protocol_json
    )
endif()

if(TARGET esy_common_smf)
    target_link_libraries(esy_common
        INTERFACE
            esy_common_smf
    )
endif()
