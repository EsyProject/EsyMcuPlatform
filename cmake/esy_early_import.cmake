# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-FileCopyrightText: 2026 EsyCloud
#
# SPDX-License-Identifier: Apache-2.0

include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/esy_feature_snapshot.cmake")
esy_invalidate_feature_snapshot()

# Including this entry opts into overridable project-layout conventions.
if(CMAKE_C_COMPILER_LOADED OR CMAKE_CXX_COMPILER_LOADED)
    message(FATAL_ERROR "Include esy_early_import.cmake before project().")
endif()

if(NOT DEFINED ESY_PROJECT_ROOT)
    set(ESY_PROJECT_ROOT "${CMAKE_SOURCE_DIR}")
endif()
get_filename_component(ESY_PROJECT_ROOT "${ESY_PROJECT_ROOT}" ABSOLUTE
    BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
include("${CMAKE_CURRENT_LIST_DIR}/esy_target_device.cmake")
esy_configure_target_device()

set(esy_build_config_explicit FALSE)
if(DEFINED ESY_BUILD_CONFIG_FILE)
    set(esy_build_config_explicit TRUE)
else()
    set(ESY_BUILD_CONFIG_FILE "${ESY_PROJECT_ROOT}/cmake/esy_build_config.cmake")
endif()
if(ESY_BUILD_CONFIG_FILE)
    if(EXISTS "${ESY_BUILD_CONFIG_FILE}" AND NOT IS_DIRECTORY "${ESY_BUILD_CONFIG_FILE}")
        set(esy_selected_target_fields ESY_TARGET_EXPORT_FIELDS ${ESY_TARGET_EXPORT_FIELDS})
        foreach(esy_target_field IN LISTS esy_selected_target_fields)
            set(esy_selected_value_${esy_target_field} "${${esy_target_field}}")
            get_property(esy_selected_cached_${esy_target_field}
                CACHE "${esy_target_field}" PROPERTY TYPE SET)
            get_property(esy_selected_cache_value_${esy_target_field}
                CACHE "${esy_target_field}" PROPERTY VALUE)
        endforeach()
        include("${ESY_BUILD_CONFIG_FILE}")
        foreach(esy_target_field IN LISTS esy_selected_target_fields)
            get_property(esy_final_field_cached CACHE "${esy_target_field}" PROPERTY TYPE SET)
            get_property(esy_final_field_cache_value CACHE "${esy_target_field}" PROPERTY VALUE)
            if(NOT DEFINED ${esy_target_field}
                    OR NOT "${${esy_target_field}}" STREQUAL "${esy_selected_value_${esy_target_field}}"
                    OR NOT esy_final_field_cached STREQUAL esy_selected_cached_${esy_target_field}
                    OR NOT "${esy_final_field_cache_value}" STREQUAL "${esy_selected_cache_value_${esy_target_field}}")
                message(FATAL_ERROR
                    "ESY_BUILD_CONFIG_FILE must not change platform/device selection (${esy_target_field}); provide selection inputs before early import.")
            endif()
        endforeach()
    elseif(esy_build_config_explicit OR IS_DIRECTORY "${ESY_BUILD_CONFIG_FILE}")
        message(FATAL_ERROR "ESY_BUILD_CONFIG_FILE does not name a file: ${ESY_BUILD_CONFIG_FILE}")
    endif()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/esy_build_defaults.cmake")
set(esy_project_integration_enabled TRUE)

# Dependency discovery uses the same configuration without importing an SDK.
if(CMAKE_SCRIPT_MODE_FILE)
    return()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/ports/${ESY_BUILD_PORT}/pre_project.cmake")
