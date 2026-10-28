# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

foreach(dependency esy_common_defs esy_opt_default)
    if(NOT TARGET ${dependency})
        message(FATAL_ERROR "esy_ble requires target ${dependency}")
    endif()
endforeach()

add_library(esy_ble STATIC src/esy_ble.c)
add_library(Esy::ble ALIAS esy_ble)
target_compile_features(esy_ble PUBLIC c_std_11)
target_include_directories(esy_ble PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(esy_ble PUBLIC esy_common_defs PRIVATE esy_opt_default)

if(ENABLE_ESY_BLE_BTSTACK)
    if(NOT TARGET esy_ble_btstack_provider)
        message(FATAL_ERROR
            "ENABLE_ESY_BLE_BTSTACK requires consumer target esy_ble_btstack_provider with BTstack headers, configuration and software-stack linkage")
    endif()
    add_library(esy_ble_btstack STATIC src/esy_ble_btstack.c)
    add_library(Esy::ble_btstack ALIAS esy_ble_btstack)
    target_compile_features(esy_ble_btstack PUBLIC c_std_11)
    target_link_libraries(esy_ble_btstack PUBLIC esy_ble
        PRIVATE esy_ble_btstack_provider esy_opt_default)
endif()
