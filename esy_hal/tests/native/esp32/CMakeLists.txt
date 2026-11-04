# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)
get_filename_component(ESY_NATIVE_PLATFORM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
if(NOT ESY_NATIVE_PROFILE MATCHES "^(core|peripherals|managed|radio|power)$")
    message(FATAL_ERROR "Set ESY_NATIVE_PROFILE to core, peripherals, managed, radio or power")
endif()
set(ESY_TARGET_DEVICE ESP32)
set(ESY_BOARD_PATH "${CMAKE_CURRENT_LIST_DIR}/board")
set(ESY_FEATURE_IMPORT_FILE "${CMAKE_CURRENT_LIST_DIR}/features/${ESY_NATIVE_PROFILE}.cmake")
set(ESY_SPECS_DIR "")
set(ESY_RTOS_CONFIG_DIR "")
set(ESY_VERSION_TEMPLATE "")
set(COMPONENTS main)
include("${ESY_NATIVE_PLATFORM_ROOT}/cmake/esy_early_import.cmake")
idf_build_set_property(ESY_NATIVE_PROFILE "${ESY_NATIVE_PROFILE}")
project(esy_esp32_native VERSION 0.1.0 LANGUAGES C CXX ASM)
