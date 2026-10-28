# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

foreach(dependency esy_common_defs esy_network esy_opt_default esy_http_parser_provider)
    if(NOT TARGET ${dependency})
        message(FATAL_ERROR "esy_http_client requires target ${dependency}; provide pre-generated llhttp v9.4.3 through esy_http_parser_provider or cmake/esy_llhttp_provider.cmake.")
    endif()
endforeach()

add_library(esy_http_client STATIC src/esy_http_client.c)
add_library(Esy::http_client ALIAS esy_http_client)
target_compile_features(esy_http_client PUBLIC c_std_11)
target_include_directories(esy_http_client PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(esy_http_client PUBLIC esy_common_defs esy_network
    PRIVATE esy_http_parser_provider esy_opt_default)
