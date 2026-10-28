# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

# Consumer-owned feature and hardware dependencies; no OS dependency.
foreach(feature ENABLE_ESY_SYSTEM ENABLE_ESY_FLASH)
    if(NOT ${feature})
        message(FATAL_ERROR "esy_bootloader requires ${feature}")
    endif()
endforeach()
foreach(dependency esy_hal esy_common_defs esy_common_utils esy_opt_default)
    if(NOT TARGET ${dependency})
        message(FATAL_ERROR "esy_bootloader requires target ${dependency}")
    endif()
endforeach()

add_library(esy_bootloader STATIC src/esy_bootloader.c)
target_compile_features(esy_bootloader PUBLIC c_std_11)
target_include_directories(esy_bootloader PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(esy_bootloader PUBLIC esy_common_defs
    PRIVATE esy_hal esy_common_utils esy_opt_default)
