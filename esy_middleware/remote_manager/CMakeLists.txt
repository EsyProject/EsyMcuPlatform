# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

if(ENABLE_ESY_REMOTE_MANAGER_PROTO_BIN AND NOT ENABLE_ESY_REMOTE_MANAGER)
    message(FATAL_ERROR "ENABLE_ESY_REMOTE_MANAGER_PROTO_BIN requires ENABLE_ESY_REMOTE_MANAGER")
endif()

foreach(dependency IN ITEMS esy_common_defs esy_opt_default)
    if(NOT TARGET ${dependency})
        message(FATAL_ERROR "esy_remote_manager requires target ${dependency}")
    endif()
endforeach()

add_library(esy_remote_manager STATIC
    src/esy_remote_manager.c
    src/esy_remote_manager_config.c
    src/esy_remote_manager_operations.c
    src/esy_remote_manager_stats.c
    src/esy_remote_manager_diagnostics.c
    src/adapters/esy_remote_manager_stats_table.c
)
target_compile_features(esy_remote_manager PUBLIC c_std_11)
target_include_directories(esy_remote_manager PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(esy_remote_manager
    PUBLIC esy_common_defs
    PRIVATE esy_opt_default
)

if(ENABLE_ESY_REMOTE_MANAGER_PROTO_BIN)
    add_library(esy_remote_manager_proto_bin STATIC
        src/adapters/esy_remote_manager_proto_bin.c
    )
    target_compile_features(esy_remote_manager_proto_bin PUBLIC c_std_11)
    target_link_libraries(esy_remote_manager_proto_bin
        PUBLIC esy_remote_manager
        PRIVATE esy_opt_default
    )
endif()

foreach(adapter IN ITEMS SETTINGS COREDUMP OTA DIAGNOSTICS)
    if(ENABLE_ESY_REMOTE_MANAGER_${adapter} AND NOT ENABLE_ESY_REMOTE_MANAGER)
        message(FATAL_ERROR "ENABLE_ESY_REMOTE_MANAGER_${adapter} requires ENABLE_ESY_REMOTE_MANAGER")
    endif()
endforeach()

if(ENABLE_ESY_REMOTE_MANAGER_SETTINGS)
    if(NOT ENABLE_ESY_SETTINGS OR NOT TARGET esy_settings)
        message(FATAL_ERROR "ENABLE_ESY_REMOTE_MANAGER_SETTINGS requires ENABLE_ESY_SETTINGS and target esy_settings")
    endif()
    add_library(esy_remote_manager_settings STATIC src/adapters/esy_remote_manager_settings.c)
    target_link_libraries(esy_remote_manager_settings PUBLIC esy_remote_manager esy_settings PRIVATE esy_opt_default)
endif()

if(ENABLE_ESY_REMOTE_MANAGER_DIAGNOSTICS)
    if(NOT ENABLE_ESY_DIAGNOSTICS OR NOT TARGET esy_diagnostics)
        message(FATAL_ERROR "ENABLE_ESY_REMOTE_MANAGER_DIAGNOSTICS requires ENABLE_ESY_DIAGNOSTICS and target esy_diagnostics")
    endif()
    add_library(esy_remote_manager_diagnostics STATIC src/adapters/esy_remote_manager_diagnostics_adapter.c)
    target_link_libraries(esy_remote_manager_diagnostics PUBLIC esy_remote_manager esy_diagnostics PRIVATE esy_opt_default)
endif()

if(ENABLE_ESY_REMOTE_MANAGER_COREDUMP)
    if(NOT ENABLE_ESY_COREDUMP OR NOT TARGET esy_coredump_core)
        message(FATAL_ERROR "ENABLE_ESY_REMOTE_MANAGER_COREDUMP requires ENABLE_ESY_COREDUMP and target esy_coredump_core")
    endif()
    add_library(esy_remote_manager_coredump_store STATIC src/adapters/esy_remote_manager_coredump_store.c)
    target_link_libraries(esy_remote_manager_coredump_store PUBLIC esy_remote_manager esy_coredump_core PRIVATE esy_opt_default)
endif()

if(ENABLE_ESY_REMOTE_MANAGER_OTA)
    if(NOT ENABLE_ESY_OTA OR NOT TARGET esy_ota_core)
        message(FATAL_ERROR "ENABLE_ESY_REMOTE_MANAGER_OTA requires ENABLE_ESY_OTA and target esy_ota_core")
    endif()
    add_library(esy_remote_manager_ota STATIC src/adapters/esy_remote_manager_ota_adapter.c)
    target_link_libraries(esy_remote_manager_ota PUBLIC esy_remote_manager esy_ota_core PRIVATE esy_opt_default)
endif()
