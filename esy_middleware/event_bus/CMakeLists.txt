# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

if(NOT ESY_RTOS STREQUAL "FreeRTOS")
    message(FATAL_ERROR "esy_event_bus requires ESY_RTOS=FreeRTOS")
endif()
if(NOT ENABLE_ESY_OS_QUEUE)
    message(FATAL_ERROR "esy_event_bus requires ENABLE_ESY_OS_QUEUE")
endif()
foreach(dependency IN ITEMS esy_common_defs esy_os esy_opt_default)
    if(NOT TARGET ${dependency})
        message(FATAL_ERROR "esy_event_bus requires target ${dependency}")
    endif()
endforeach()

add_library(esy_event_bus STATIC src/esy_event_bus.c)
target_compile_features(esy_event_bus PUBLIC c_std_11)
target_include_directories(esy_event_bus PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(esy_event_bus
    PUBLIC esy_common_defs esy_os
    PRIVATE esy_opt_default
)
