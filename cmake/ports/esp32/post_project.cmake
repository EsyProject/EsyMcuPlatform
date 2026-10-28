# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-FileCopyrightText: 2026 EsyCloud
#
# SPDX-License-Identifier: Apache-2.0

macro(esy_port_esp32_initialize_sdk_dependencies)
    if(ESY_RTOS STREQUAL "FreeRTOS")
        set(esy_freertos_provider_checked TRUE)
        set(esy_import_kernel FALSE)
        if(FREE_RTOS_KERNEL_LIB)
            if(NOT TARGET ${FREE_RTOS_KERNEL_LIB})
                message(FATAL_ERROR "FREE_RTOS_KERNEL_LIB must name an existing provider target.")
            endif()
        else()
            if(NOT TARGET idf::freertos)
                message(FATAL_ERROR "ESP32 FreeRTOS requires the native idf::freertos component target.")
            endif()
            set(FREE_RTOS_KERNEL_LIB idf::freertos)
        endif()
    endif()
endmacro()

function(esy_port_esp32_configure_runtime_targets)
    # This native OS bridge has no HAL dependency and precedes backend assembly.
    if(ENABLE_ESY_SYSTEM AND NOT TARGET esy_os_context)
        add_subdirectory("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../esy_middleware/os/src/freertos/port/esp32"
            "${CMAKE_CURRENT_BINARY_DIR}/esy_os_context")
    endif()
endfunction()

function(esy_port_esp32_configure_firmware target)
    # ESP-IDF owns component targets, final linking, Flash layout and image outputs.
    set_property(TARGET ${target} PROPERTY ESY_FIRMWARE_CONFIGURED TRUE)
    return()
endfunction()
