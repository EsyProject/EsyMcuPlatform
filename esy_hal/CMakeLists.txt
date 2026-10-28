# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

add_library(esy_hal STATIC)

target_include_directories(esy_hal
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(esy_hal
    PUBLIC
        esy_common_defs
        esy_common_device
    PRIVATE
        esy_hal_backend
        esy_board
        esy_opt_default
        esy_common
)

add_subdirectory(src)

get_target_property(esy_sources esy_hal SOURCES)
if(NOT esy_sources)
    target_sources(esy_hal PRIVATE src/esy_hal_empty.c)
endif()
