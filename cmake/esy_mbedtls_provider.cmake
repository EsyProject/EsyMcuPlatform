# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

include_guard(GLOBAL)

# Optional explicit assembly helper. No download, entropy source or product
# feature is selected here. Consumers may instead define the provider target.
function(esy_configure_mbedtls_provider)
    if(TARGET esy_mbedtls_provider)
        return()
    endif()
    if(CMAKE_VERSION VERSION_LESS "3.21")
        message(FATAL_ERROR "The optional Mbed TLS assembly helper requires CMake 3.21; define esy_mbedtls_provider directly with older CMake")
    endif()
    if(NOT ESY_MBEDTLS_PATH)
        message(FATAL_ERROR "Set ESY_MBEDTLS_PATH to a Mbed TLS 3.6.x release, or supply esy_mbedtls_provider")
    endif()
    get_filename_component(provider_path "${ESY_MBEDTLS_PATH}" ABSOLUTE)
    if(NOT EXISTS "${provider_path}/include/mbedtls/version.h" OR
       NOT EXISTS "${provider_path}/CMakeLists.txt")
        message(FATAL_ERROR "ESY_MBEDTLS_PATH must contain a complete Mbed TLS 3.6.x release")
    endif()
    set(provider_version_header "${provider_path}/include/mbedtls/version.h")
    if(EXISTS "${provider_path}/include/mbedtls/build_info.h")
        set(provider_version_header "${provider_path}/include/mbedtls/build_info.h")
    endif()
    file(STRINGS "${provider_version_header}"
        provider_version REGEX "^#define[ \t]+MBEDTLS_VERSION_STRING[ \t]+")
    if(NOT provider_version MATCHES "\"3\\.6\\.[0-9]+\"")
        message(FATAL_ERROR "ESY_MBEDTLS_PATH must provide Mbed TLS 3.6.x; found ${provider_version}")
    endif()
    if(TARGET mbedtls OR TARGET mbedx509 OR TARGET mbedcrypto)
        message(FATAL_ERROR "Existing Mbed TLS targets require an explicit esy_mbedtls_provider target")
    endif()
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0126 NEW)
    set(ENABLE_PROGRAMS OFF)
    set(ENABLE_TESTING OFF)
    set(USE_SHARED_MBEDTLS_LIBRARY OFF)
    set(USE_STATIC_MBEDTLS_LIBRARY ON)
    add_subdirectory("${provider_path}" "${CMAKE_CURRENT_BINARY_DIR}/esy_mbedtls_native" EXCLUDE_FROM_ALL)
    add_library(esy_mbedtls_provider INTERFACE)
    target_link_libraries(esy_mbedtls_provider INTERFACE mbedtls mbedx509 mbedcrypto)
endfunction()
