# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

# Core lockout engine and optional coordinated OS workers.
if(NOT ESY_RTOS STREQUAL "None" AND NOT ESY_RTOS STREQUAL "FreeRTOS")
    message(FATAL_ERROR "Core lockout requires ESY_RTOS=None or ESY_RTOS=FreeRTOS")
endif()
foreach(feature IN ITEMS ENABLE_ESY_SYSTEM)
    if(NOT ${feature})
        message(FATAL_ERROR "esy_core_lockout_core requires ${feature}")
    endif()
endforeach()
foreach(dependency IN ITEMS esy_common_defs esy_common esy_hal esy_opt_default)
    if(NOT TARGET ${dependency})
        message(FATAL_ERROR "esy_core_lockout_core requires the ${dependency} target")
    endif()
endforeach()
add_library(esy_core_lockout_core STATIC src/esy_core_lockout_core.c)
target_include_directories(esy_core_lockout_core PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_compile_features(esy_core_lockout_core PUBLIC c_std_11)
target_link_libraries(esy_core_lockout_core PUBLIC esy_common_defs
    PRIVATE esy_common esy_hal esy_opt_default)
if(ESY_RTOS STREQUAL "FreeRTOS")
    if(NOT TARGET esy_os)
        message(FATAL_ERROR "esy_core_lockout requires the esy_os target")
    endif()
    add_library(esy_core_lockout STATIC src/esy_core_lockout.c)
    target_include_directories(esy_core_lockout PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
    target_compile_features(esy_core_lockout PUBLIC c_std_11)
    target_link_libraries(esy_core_lockout PUBLIC esy_core_lockout_core esy_os
        PRIVATE esy_common esy_hal esy_opt_default)
endif()
