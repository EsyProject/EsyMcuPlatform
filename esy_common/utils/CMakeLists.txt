# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

set(MODULE_NAME esy_common_utils)

add_library(${MODULE_NAME} STATIC
    src/integrity/esy_crc32.c
    src/integrity/esy_sha256.c
    src/base/esy_sort.c
    src/time/esy_datetime.c
)
add_library(Esy::common_utils ALIAS ${MODULE_NAME})

set_target_properties(${MODULE_NAME}
    PROPERTIES
        EXPORT_NAME common_utils
)

target_compile_features(${MODULE_NAME} PUBLIC c_std_11)

target_include_directories(${MODULE_NAME}
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(${MODULE_NAME}
    PUBLIC
        esy_common_defs
)

if(TARGET esy_opt_default)
    target_link_libraries(${MODULE_NAME}
        PRIVATE
            esy_opt_default
    )
endif()

if(ENABLE_ESY_COMMON_UTILS_CRC32_TABLE)
    target_compile_definitions(${MODULE_NAME} PRIVATE ENABLE_ESY_COMMON_UTILS_CRC32_TABLE)
endif()
