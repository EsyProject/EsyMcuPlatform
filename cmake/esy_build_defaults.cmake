# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-FileCopyrightText: 2026 EsyCloud
#
# SPDX-License-Identifier: Apache-2.0

include_guard(DIRECTORY)

# These conventions apply only through the opt-in consumer integration entries.
# Explicit values, including empty option lists, take precedence over defaults.
if(NOT DEFINED CMAKE_C_STANDARD)
    set(CMAKE_C_STANDARD 11)
endif()
if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
if(NOT DEFINED CMAKE_C_STANDARD_REQUIRED)
    set(CMAKE_C_STANDARD_REQUIRED ON)
endif()
if(NOT DEFINED CMAKE_C_EXTENSIONS)
    set(CMAKE_C_EXTENSIONS ON)
endif()
if(NOT DEFINED CMAKE_EXPORT_COMPILE_COMMANDS)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

get_property(esy_generator_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT esy_generator_is_multi_config AND NOT CMAKE_CONFIGURATION_TYPES
        AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
unset(esy_generator_is_multi_config)

if(NOT DEFINED ESY_FEATURE_IMPORT_FILE
        AND EXISTS "${ESY_PROJECT_ROOT}/esy_feature_import.cmake")
    set(ESY_FEATURE_IMPORT_FILE "${ESY_PROJECT_ROOT}/esy_feature_import.cmake")
endif()
if(NOT DEFINED ESY_BOARD_PATH)
    set(ESY_BOARD_PATH "${ESY_PROJECT_ROOT}/esy_board")
endif()
if(NOT DEFINED ESY_SPECS_DIR)
    set(ESY_SPECS_DIR "${ESY_PROJECT_ROOT}/specs/${ESY_SPECS_PLATFORM_PATH}")
endif()
if(NOT DEFINED ESY_RTOS_CONFIG_DIR)
    if(ESY_SPECS_DIR)
        set(ESY_RTOS_CONFIG_DIR "${ESY_SPECS_DIR}/include/specs")
    else()
        set(ESY_RTOS_CONFIG_DIR "")
    endif()
endif()
if(NOT DEFINED ESY_VERSION_TEMPLATE)
    set(ESY_VERSION_TEMPLATE "${CMAKE_CURRENT_LIST_DIR}/version.h.in")
endif()
if(NOT DEFINED GENERATED_INCLUDE_DIR)
    set(GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated")
endif()
if(NOT DEFINED FREERTOS_KERNEL_PATH)
    if(DEFINED ENV{FREERTOS_KERNEL_PATH} AND NOT "$ENV{FREERTOS_KERNEL_PATH}" STREQUAL "")
        set(FREERTOS_KERNEL_PATH "$ENV{FREERTOS_KERNEL_PATH}")
    else()
        set(FREERTOS_KERNEL_PATH "${ESY_PROJECT_ROOT}/external/FreeRTOS-Kernel")
    endif()
endif()
if(NOT DEFINED LVGL_PATH)
    if(DEFINED ENV{LVGL_PATH} AND NOT "$ENV{LVGL_PATH}" STREQUAL "")
        set(LVGL_PATH "$ENV{LVGL_PATH}")
    else()
        set(LVGL_PATH "${ESY_PROJECT_ROOT}/external/lvgl")
    endif()
endif()
if(NOT DEFINED LV_BUILD_CONF_PATH)
    if(ESY_SPECS_DIR)
        set(LV_BUILD_CONF_PATH "${ESY_SPECS_DIR}/include/specs/lv_conf.h")
    else()
        set(LV_BUILD_CONF_PATH "")
    endif()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/esy_port.cmake")
esy_call_port_hook(configure_build_defaults)

# File names follow the template convention; the board supplies their contents.
if(NOT DEFINED ESY_LINKER_SCRIPT_DEFAULT)
    set(ESY_LINKER_SCRIPT_DEFAULT
        "${ESY_BOARD_PATH}/${ESY_BOARD_PLATFORM_PATH}/support/esy_memmap/default_memmap.ld")
endif()
if(NOT DEFINED ESY_LINKER_SCRIPT_A)
    set(ESY_LINKER_SCRIPT_A
        "${ESY_BOARD_PATH}/${ESY_BOARD_PLATFORM_PATH}/support/esy_memmap/memmap_a.ld")
endif()
if(NOT DEFINED ESY_LINKER_SCRIPT_B)
    set(ESY_LINKER_SCRIPT_B
        "${ESY_BOARD_PATH}/${ESY_BOARD_PLATFORM_PATH}/support/esy_memmap/memmap_b.ld")
endif()

# Evaluate compiler predicates after project() has identified the toolchain.
set(esy_compile_gnu_or_clang
    "$<OR:$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>>")
set(esy_compile_gnu
    "$<OR:$<COMPILE_LANG_AND_ID:C,GNU>,$<COMPILE_LANG_AND_ID:CXX,GNU>>")
if(NOT DEFINED ESY_DEFAULT_COMPILE_OPTIONS)
    set(ESY_DEFAULT_COMPILE_OPTIONS
        "$<${esy_compile_gnu_or_clang}:-Wall>"
        "$<${esy_compile_gnu_or_clang}:-Wextra>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:Debug>>:-Og>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:Debug>>:-g3>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:Debug>>:-fno-omit-frame-pointer>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:Debug>>:-fno-optimize-sibling-calls>"
        "$<$<AND:${esy_compile_gnu},$<CONFIG:Debug>>:-fvar-tracking-assignments>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:Release>>:-O2>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:Release>>:-ffunction-sections>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:Release>>:-fdata-sections>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:RelWithDebInfo>>:-Os>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:RelWithDebInfo>>:-g3>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:RelWithDebInfo>>:-ffunction-sections>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:RelWithDebInfo>>:-fdata-sections>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:MinSizeRel>>:-Os>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:MinSizeRel>>:-ffunction-sections>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:MinSizeRel>>:-fdata-sections>")
endif()
if(NOT DEFINED ESY_SPEED_COMPILE_OPTIONS)
    set(ESY_SPEED_COMPILE_OPTIONS
        "$<${esy_compile_gnu_or_clang}:-Wall>"
        "$<${esy_compile_gnu_or_clang}:-Wextra>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:Debug>>:-Og>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:Debug>>:-g3>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:Debug>>:-fno-omit-frame-pointer>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:Debug>>:-fno-optimize-sibling-calls>"
        "$<$<AND:${esy_compile_gnu},$<CONFIG:Debug>>:-fvar-tracking-assignments>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:Release>>:-O3>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:Release>>:-ffunction-sections>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:Release>>:-fdata-sections>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:RelWithDebInfo>>:-Os>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:RelWithDebInfo>>:-g3>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:RelWithDebInfo>>:-ffunction-sections>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:RelWithDebInfo>>:-fdata-sections>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:MinSizeRel>>:-O3>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:MinSizeRel>>:-ffunction-sections>"
        "$<$<AND:${esy_compile_gnu_or_clang},$<CONFIG:MinSizeRel>>:-fdata-sections>")
endif()
if(NOT DEFINED ESY_DEFAULT_LINK_OPTIONS)
    set(ESY_DEFAULT_LINK_OPTIONS
        "$<$<OR:$<LINK_LANG_AND_ID:C,GNU,Clang>,$<LINK_LANG_AND_ID:CXX,GNU,Clang>>:-Wl,--gc-sections>")
endif()
unset(esy_compile_gnu_or_clang)
unset(esy_compile_gnu)
