# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

add_library(esy_board STATIC system.c)
target_include_directories(esy_board PUBLIC include)
target_link_libraries(esy_board PUBLIC esy_common_defs)
if(NOT ESY_NATIVE_PROFILE STREQUAL "core")
    target_sources(esy_board PRIVATE board.c)
    target_link_libraries(esy_board PRIVATE idf::esp_driver_gpio idf::esp_driver_uart)
endif()
target_compile_options(esy_board PRIVATE -Wall -Wextra -Werror)
