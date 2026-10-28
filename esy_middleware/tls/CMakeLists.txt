# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

foreach(dependency esy_common_defs esy_common_utils esy_network esy_credentials
                   esy_crypto esy_mbedtls_provider esy_opt_default)
    if(NOT TARGET ${dependency})
        message(FATAL_ERROR "esy_tls requires target ${dependency}")
    endif()
endforeach()

add_library(esy_tls STATIC src/esy_tls.c)
add_library(Esy::tls ALIAS esy_tls)
target_compile_features(esy_tls PUBLIC c_std_11)
target_include_directories(esy_tls PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(esy_tls PUBLIC esy_network esy_credentials esy_crypto
    esy_mbedtls_provider PRIVATE esy_common_utils esy_opt_default)
