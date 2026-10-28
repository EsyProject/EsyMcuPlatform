# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/esy_target_device.cmake")
esy_configure_target_device()
include("${CMAKE_CURRENT_LIST_DIR}/cmake/esy_port.cmake")


# Initialize with the consumer's project version before declaring the library's.
if(esy_project_integration_enabled AND NOT esy_project_import_complete)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/esy_project_import.cmake")

    # Keep resolved consumer configuration visible to subsequent sibling modules.
    get_cmake_property(esy_integration_variables VARIABLES)
    foreach(esy_integration_variable IN LISTS esy_integration_variables)
        if(esy_integration_variable MATCHES "^(APP_|ESY_|ENABLE_ESY_|ALLOW_ESY_)"
                OR esy_integration_variable MATCHES
                    "^(FREE_RTOS_KERNEL_LIB|TINYUSB_OPT_OS|LVGL_PATH|LV_BUILD_CONF_PATH|CMAKE_MODULE_PATH|esy_project_import_complete)$")
            set(${esy_integration_variable} "${${esy_integration_variable}}" PARENT_SCOPE)
        endif()
    endforeach()
    esy_call_port_hook(export_sdk_variables)
endif()

project(EsyMcuPlatform
    VERSION 0.1.0
    LANGUAGES C
)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_C_EXTENSIONS OFF)
endif()

if(NOT DEFINED ESY_BOARD_PATH)
    message(FATAL_ERROR "EsyMcuPlatform requires ESY_BOARD_PATH to be defined by the consuming project.")
endif()

if(NOT TARGET esy_opt_default)
    message(FATAL_ERROR "EsyMcuPlatform requires esy_opt_default to be defined by the consuming project.")
endif()

if(NOT TARGET esy_opt_speed)
    message(FATAL_ERROR "EsyMcuPlatform requires esy_opt_speed to be defined by the consuming project.")
endif()

# Only optional project integration enables defaults; direct consumers retain
# their explicit provider contract even if another directory loaded the helper.
if(esy_network_provider_defaults_enabled)
    esy_configure_default_network_providers()
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/esy_atomic_backend.cmake)

add_subdirectory(esy_common)
add_subdirectory("${ESY_BOARD_PATH}" "${CMAKE_CURRENT_BINARY_DIR}/esy_board")

esy_call_port_hook(configure_runtime_targets)

add_subdirectory(esy_hal_backend)
add_subdirectory(esy_hal)

add_subdirectory(esy_middleware)

set(ESY_INTERRUPT_OBJECT_TARGETS "")
set(esy_interrupt_owners "")
foreach(interrupt_owner IN ITEMS esy_hal_backend esy_os)
    if(TARGET ${interrupt_owner})
        get_property(esy_owner_interrupt_targets TARGET ${interrupt_owner}
            PROPERTY ESY_INTERRUPT_OBJECT_TARGETS)
        if(esy_owner_interrupt_targets)
            list(APPEND esy_interrupt_owners ${interrupt_owner})
        endif()
        foreach(interrupt_target IN LISTS esy_owner_interrupt_targets)
            if(NOT TARGET ${interrupt_target})
                message(FATAL_ERROR "Interrupt object target does not exist: ${interrupt_target}")
            endif()
            get_target_property(esy_interrupt_type ${interrupt_target} TYPE)
            if(NOT esy_interrupt_type STREQUAL "OBJECT_LIBRARY")
                message(FATAL_ERROR "Interrupt target must be an OBJECT library: ${interrupt_target}")
            endif()
            list(APPEND ESY_INTERRUPT_OBJECT_TARGETS ${interrupt_target})
        endforeach()
    endif()
endforeach()
list(REMOVE_DUPLICATES ESY_INTERRUPT_OBJECT_TARGETS)

if(ESY_INTERRUPT_OBJECT_TARGETS)
    add_library(esy_interrupts INTERFACE)
    add_library(Esy::interrupts ALIAS esy_interrupts)
    foreach(interrupt_target IN LISTS ESY_INTERRUPT_OBJECT_TARGETS)
        target_sources(esy_interrupts INTERFACE $<TARGET_OBJECTS:${interrupt_target}>)
    endforeach()
    target_link_libraries(esy_interrupts INTERFACE esy_hal)
    if("esy_os" IN_LIST esy_interrupt_owners)
        target_link_libraries(esy_interrupts INTERFACE esy_os)
    endif()
endif()
