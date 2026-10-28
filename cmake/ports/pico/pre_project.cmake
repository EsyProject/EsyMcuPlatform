# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-FileCopyrightText: 2026 EsyCloud
#
# SPDX-License-Identifier: Apache-2.0

string(TOLOWER "${ESY_TARGET_PLATFORM}" esy_pico_board)
set(PICO_BOARD "${esy_pico_board}" CACHE STRING "Consumer-selected Pico board" FORCE)
if(ESY_TARGET_PLATFORM MATCHES "^PICO2")
    set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Pico architecture" FORCE)
    set(esy_pico_default_compiler pico_arm_cortex_m33_gcc)
    set(ESY_PLATFORM_COMPILE_OPTIONS
        -mcpu=cortex-m33 -mthumb -march=armv8-m.main+fp+dsp -mfloat-abi=softfp -mcmse)
else()
    set(PICO_PLATFORM rp2040 CACHE STRING "Pico architecture" FORCE)
    set(esy_pico_default_compiler pico_arm_cortex_m0plus_gcc)
    set(ESY_PLATFORM_COMPILE_OPTIONS -mcpu=cortex-m0plus -mthumb)
endif()
set(ESY_PLATFORM_LINK_OPTIONS ${ESY_PLATFORM_COMPILE_OPTIONS})

if(NOT PICO_TOOLCHAIN_PATH)
    if(DEFINED ENV{PICO_TOOLCHAIN_PATH} AND NOT "$ENV{PICO_TOOLCHAIN_PATH}" STREQUAL "")
        set(PICO_TOOLCHAIN_PATH "$ENV{PICO_TOOLCHAIN_PATH}")
    elseif(ESY_TOOLCHAIN_ROOT)
        set(PICO_TOOLCHAIN_PATH "${ESY_TOOLCHAIN_ROOT}")
    elseif(DEFINED ENV{ESY_TOOLCHAIN_ROOT})
        set(PICO_TOOLCHAIN_PATH "$ENV{ESY_TOOLCHAIN_ROOT}")
    endif()
endif()

if(TARGET pico_stdlib)
    return()
endif()
if(NOT PICO_SDK_PATH)
    if(DEFINED ENV{PICO_SDK_PATH} AND NOT "$ENV{PICO_SDK_PATH}" STREQUAL "")
        set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
    elseif(ESY_PLATFORM_SDK_ROOT)
        set(PICO_SDK_PATH "${ESY_PLATFORM_SDK_ROOT}")
    elseif(DEFINED ENV{ESY_PLATFORM_SDK_ROOT})
        set(PICO_SDK_PATH "$ENV{ESY_PLATFORM_SDK_ROOT}")
    endif()
endif()
if(NOT PICO_SDK_PATH OR NOT EXISTS "${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
    message(FATAL_ERROR
        "Pico integration requires a local SDK. Set PICO_SDK_PATH or ESY_PLATFORM_SDK_ROOT to its root.")
endif()
get_filename_component(PICO_SDK_PATH "${PICO_SDK_PATH}" REALPATH BASE_DIR "${CMAKE_SOURCE_DIR}")

# The SDK selects a toolchain whenever PICO_COMPILER is defined. Preserve an
# explicit caller toolchain even when that compiler selector is also supplied.
if(CMAKE_TOOLCHAIN_FILE)
    set(esy_pico_saved_compiler "${PICO_COMPILER}")
    unset(PICO_COMPILER)
    unset(PICO_COMPILER CACHE)
else()
    if(NOT PICO_COMPILER)
        set(PICO_COMPILER "${esy_pico_default_compiler}")
    endif()
endif()
include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
if(esy_pico_saved_compiler)
    set(PICO_COMPILER "${esy_pico_saved_compiler}")
endif()
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
