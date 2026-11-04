# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-FileCopyrightText: 2026 EsyCloud
#
# SPDX-License-Identifier: Apache-2.0

function(esy_port_esp32_validate_target)
    # Native IDF publishes these values at different integration stages. Empty
    # fixture providers have no native target and retain their existing contract.
    foreach(esy_idf_target_input IN ITEMS IDF_TARGET CONFIG_IDF_TARGET)
        if(DEFINED ${esy_idf_target_input} AND
           NOT "${${esy_idf_target_input}}" STREQUAL "" AND
           NOT "${${esy_idf_target_input}}" STREQUAL "esp32")
            message(FATAL_ERROR
                "ESY_TARGET_DEVICE=ESP32 requires the classic IDF_TARGET=esp32; ${esy_idf_target_input}=${${esy_idf_target_input}}")
        endif()
    endforeach()
    if(DEFINED ENV{IDF_TARGET} AND NOT "$ENV{IDF_TARGET}" STREQUAL "" AND
       NOT "$ENV{IDF_TARGET}" STREQUAL "esp32")
        message(FATAL_ERROR
            "ESY_TARGET_DEVICE=ESP32 requires the classic IDF_TARGET=esp32; environment IDF_TARGET=$ENV{IDF_TARGET}")
    endif()
endfunction()

macro(esy_port_esp32_initialize_sdk_dependencies)
    esy_port_esp32_validate_target()
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
    esy_port_esp32_validate_target()
    # Runtime adapters belong to their consumers; HAL/backend assemble no OS target.
endfunction()

function(esy_port_esp32_configure_firmware target)
    # ESP-IDF owns component targets, final linking, Flash layout and image outputs.
    set_property(TARGET ${target} PROPERTY ESY_FIRMWARE_CONFIGURED TRUE)
    return()
endfunction()

function(esy_port_esp32_configure_network_providers)
    if(ENABLE_ESY_NETWORK AND ENABLE_ESY_NETWORK_LWIP AND
       NOT TARGET esy_network_lwip_provider)
        if(NOT TARGET idf::lwip)
            message(FATAL_ERROR "ESP32 networking requires the native idf::lwip component")
        endif()
        if(NOT CONFIG_LWIP_TCPIP_CORE_LOCKING OR NOT CONFIG_LWIP_CHECK_THREAD_SAFETY)
            message(FATAL_ERROR
                "ESP32 default raw lwIP provider requires CONFIG_LWIP_TCPIP_CORE_LOCKING=y and CONFIG_LWIP_CHECK_THREAD_SAFETY=y")
        endif()
        add_library(esy_network_lwip_provider INTERFACE)
        target_link_libraries(esy_network_lwip_provider INTERFACE idf::lwip)
    endif()
endfunction()
