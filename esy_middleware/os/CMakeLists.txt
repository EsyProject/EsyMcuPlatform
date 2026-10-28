# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/esy_target_device.cmake")
esy_configure_target_device()

set(MODULE_NAME esy_os)

add_library(${MODULE_NAME} STATIC)

target_include_directories(${MODULE_NAME}
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

if(ESY_RTOS STREQUAL "FreeRTOS")
    add_subdirectory(src/freertos)
else()
    message(FATAL_ERROR "ESY_RTOS not set or unsupported")
endif()
