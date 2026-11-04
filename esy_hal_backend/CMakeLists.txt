# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/esy_target_device.cmake")
esy_configure_target_device()
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/esy_port.cmake")

set(MODULE_NAME esy_hal_backend)

add_library(${MODULE_NAME} STATIC)

target_include_directories(${MODULE_NAME}
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/${ESY_BUILD_PORT}/CMakeLists.txt")
    message(FATAL_ERROR
        "No HAL backend for ESY_BUILD_PORT=${ESY_BUILD_PORT}; ESY_TARGET_PLATFORM=${ESY_TARGET_PLATFORM} is unsupported")
endif()
add_subdirectory("src/${ESY_BUILD_PORT}")

if(ENABLE_ESY_SYSTEM OR (ESY_BUILD_PORT STREQUAL "esp32" AND
   (ENABLE_ESY_ADC OR ENABLE_ESY_CAN OR ENABLE_ESY_LOW_POWER OR
    ENABLE_ESY_TIMER_CAPTURE)))
    target_sources(${MODULE_NAME} PRIVATE src/esy_hb_context.c)
endif()

get_target_property(esy_sources esy_hal_backend SOURCES)
if(NOT esy_sources)
    target_sources(esy_hal_backend PRIVATE src/esy_hb_empty.c)
endif()
