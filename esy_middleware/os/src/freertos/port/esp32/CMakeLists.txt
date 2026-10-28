# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

if(TARGET esy_os_context)
    return()
endif()

if(NOT ESY_TARGET_PLATFORM STREQUAL "ESP32" OR NOT ESY_RTOS STREQUAL "FreeRTOS")
    message(FATAL_ERROR "esy_os_context requires ESP32 with ESY_RTOS=FreeRTOS")
endif()
if(NOT FREE_RTOS_KERNEL_LIB OR NOT TARGET ${FREE_RTOS_KERNEL_LIB})
    message(FATAL_ERROR "esy_os_context requires the native FreeRTOS provider target")
endif()
add_library(esy_os_context STATIC esy_os_context.c)
target_include_directories(esy_os_context PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../../../../include")
target_link_libraries(esy_os_context PRIVATE ${FREE_RTOS_KERNEL_LIB} esy_opt_default)
