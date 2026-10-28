# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

if(ENABLE_ESY_OTA_FLASH AND NOT ENABLE_ESY_OTA)
    message(FATAL_ERROR "ENABLE_ESY_OTA_FLASH requires ENABLE_ESY_OTA")
endif()
foreach(dependency IN ITEMS esy_common_defs esy_opt_default)
    if(NOT TARGET ${dependency})
        message(FATAL_ERROR "esy_ota_core requires target ${dependency}")
    endif()
endforeach()
add_library(esy_ota_core STATIC src/esy_ota.c)
target_compile_features(esy_ota_core PUBLIC c_std_11)
target_include_directories(esy_ota_core PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(esy_ota_core PUBLIC esy_common_defs PRIVATE esy_opt_default)

if(ENABLE_ESY_OTA_FLASH)
    foreach(feature IN ITEMS ENABLE_ESY_SYSTEM ENABLE_ESY_FLASH ENABLE_ESY_BOOTLOADER)
        if(NOT ${feature})
            message(FATAL_ERROR "esy_ota_flash requires ${feature}")
        endif()
    endforeach()
    foreach(dependency IN ITEMS esy_hal esy_bootloader esy_common_utils)
        if(NOT TARGET ${dependency})
            message(FATAL_ERROR "esy_ota_flash requires target ${dependency}")
        endif()
    endforeach()
    add_library(esy_ota_flash STATIC src/esy_ota_flash.c)
    target_compile_features(esy_ota_flash PUBLIC c_std_11)
    target_link_libraries(esy_ota_flash
        PUBLIC esy_ota_core
        PRIVATE esy_hal esy_bootloader esy_common_utils esy_opt_default)
endif()
