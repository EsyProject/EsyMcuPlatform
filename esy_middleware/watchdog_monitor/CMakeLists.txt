# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

# Shared watchdog supervision; execution mode is selected by the consumer.
if(NOT ESY_RTOS STREQUAL "None" AND NOT ESY_RTOS STREQUAL "FreeRTOS")
    message(FATAL_ERROR "esy_watchdog_monitor requires ESY_RTOS=None or ESY_RTOS=FreeRTOS")
endif()
foreach(feature IN ITEMS ENABLE_ESY_WATCHDOG ENABLE_ESY_SYSTEM)
    if(NOT ${feature})
        message(FATAL_ERROR "esy_watchdog_monitor requires ${feature}")
    endif()
endforeach()

add_library(esy_watchdog_monitor_core STATIC)
target_include_directories(esy_watchdog_monitor_core PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_compile_features(esy_watchdog_monitor_core PUBLIC c_std_11)
target_link_libraries(esy_watchdog_monitor_core PUBLIC esy_common
    PRIVATE esy_hal esy_opt_default)

if(ESY_RTOS STREQUAL "FreeRTOS")
    if(NOT TARGET esy_os)
        message(FATAL_ERROR "esy_watchdog_monitor requires the esy_os target for its OS adapter")
    endif()
    add_library(esy_watchdog_monitor STATIC)
    target_link_libraries(esy_watchdog_monitor PUBLIC esy_watchdog_monitor_core esy_os
        PRIVATE esy_hal esy_opt_default)
endif()
add_subdirectory(src)
