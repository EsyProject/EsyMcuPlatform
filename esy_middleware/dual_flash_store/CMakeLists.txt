# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

if(NOT ENABLE_ESY_FLASH)
    message(FATAL_ERROR "esy_dual_flash_store requires ENABLE_ESY_FLASH")
endif()
foreach(dependency esy_hal esy_common_defs esy_common_utils esy_opt_default)
    if(NOT TARGET ${dependency})
        message(FATAL_ERROR "esy_dual_flash_store requires target ${dependency}")
    endif()
endforeach()

add_library(esy_dual_flash_store STATIC src/esy_dual_flash_store.c)
target_compile_features(esy_dual_flash_store PUBLIC c_std_11)
target_include_directories(esy_dual_flash_store PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(esy_dual_flash_store PUBLIC esy_common_defs
    PRIVATE esy_hal esy_common_utils esy_opt_default)
