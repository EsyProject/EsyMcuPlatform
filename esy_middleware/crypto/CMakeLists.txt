# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

foreach(dependency esy_common_defs esy_opt_default esy_mbedtls_provider)
    if(NOT TARGET ${dependency})
        message(FATAL_ERROR "esy_crypto requires target ${dependency}; provide Mbed TLS 3.6.x through esy_mbedtls_provider.")
    endif()
endforeach()

add_library(esy_crypto STATIC src/esy_crypto_mbedtls.c)
add_library(Esy::crypto ALIAS esy_crypto)
target_compile_features(esy_crypto PUBLIC c_std_11)
target_include_directories(esy_crypto PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(esy_crypto PUBLIC esy_common_defs esy_mbedtls_provider
    PRIVATE esy_opt_default)
