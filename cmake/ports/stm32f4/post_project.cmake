# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-FileCopyrightText: 2026 EsyCloud
#
# SPDX-License-Identifier: Apache-2.0

macro(esy_port_stm32f4_configure_build_defaults)
    if(NOT DEFINED ESY_STM32_SUPPORT_ROOT)
        set(ESY_STM32_SUPPORT_ROOT "${ESY_BOARD_PATH}/stm32f4/support")
    endif()
endmacro()

function(esy_port_stm32f4_configure_project_options)
    if(ENABLE_ESY_HAL_FULL_ASSERT)
        target_compile_definitions(esy_project_options INTERFACE USE_FULL_ASSERT)
    endif()
endfunction()

macro(esy_port_stm32f4_initialize_sdk_dependencies)
    if(ESY_RTOS STREQUAL "FreeRTOS")
        esy_select_existing_freertos_provider()
        if(esy_import_kernel)
            esy_require_freertos_sources()
            if(NOT TARGET freertos_config)
                add_library(freertos_config INTERFACE)
                target_include_directories(freertos_config SYSTEM INTERFACE "${ESY_RTOS_CONFIG_DIR}")
                if(ESY_STM32_SUPPORT_ROOT)
                    target_include_directories(freertos_config SYSTEM INTERFACE
                        "${ESY_STM32_SUPPORT_ROOT}/CMSIS/Include"
                        "${ESY_STM32_SUPPORT_ROOT}/CMSIS/Device/ST/STM32F4xx/Include"
                        "${ESY_STM32_SUPPORT_ROOT}/STM32F4xx_HAL_Driver/Inc")
                endif()
                target_compile_definitions(freertos_config INTERFACE "${ESY_STM32F4_DEVICE_DEFINE}")
                target_compile_options(freertos_config INTERFACE ${ESY_PLATFORM_COMPILE_OPTIONS})
            endif()
            if(NOT FREERTOS_PORT)
                set(FREERTOS_PORT GCC_ARM_CM4F)
            endif()
            add_subdirectory("${FREERTOS_KERNEL_PATH}" "${CMAKE_BINARY_DIR}/third_party/freertos")
            set(FREE_RTOS_KERNEL_LIB freertos_kernel)
        endif()
    endif()
endmacro()

function(esy_port_stm32f4_configure_coredump_target target)
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE -mgeneral-regs-only)
    endif()
endfunction()

function(esy_port_stm32f4_configure_firmware target)
    get_target_property(esy_firmware_type ${target} TYPE)
    if(NOT esy_firmware_type STREQUAL "EXECUTABLE")
        message(FATAL_ERROR "ESY_FIRMWARE_TARGET must be an executable for ESY_TARGET_PLATFORM=${ESY_TARGET_PLATFORM}.")
    endif()
    set(esy_firmware_slot DEFAULT)
    if(DEFINED ESY_APP_SLOT)
        string(TOUPPER "${ESY_APP_SLOT}" esy_firmware_slot)
        if(NOT esy_firmware_slot MATCHES "^(A|B)$")
            message(FATAL_ERROR "ESY_APP_SLOT must be A or B, or unset for the default image.")
        endif()
    endif()
    set(ESY_LINK_SCRIPT "${ESY_LINKER_SCRIPT_${esy_firmware_slot}}")
    if(NOT IS_ABSOLUTE "${ESY_LINK_SCRIPT}" OR NOT EXISTS "${ESY_LINK_SCRIPT}" OR IS_DIRECTORY "${ESY_LINK_SCRIPT}")
        message(FATAL_ERROR "ESY_LINKER_SCRIPT_${esy_firmware_slot} must name an existing absolute linker script.")
    endif()
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${ESY_LINK_SCRIPT}")
    set_target_properties(${target} PROPERTIES
        SUFFIX ".elf"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

    if(NOT CMAKE_OBJCOPY)
        message(FATAL_ERROR "STM32F4 image generation requires CMAKE_OBJCOPY from the toolchain.")
    endif()
    target_link_options(${target} PRIVATE
        "-T${ESY_LINK_SCRIPT}" --specs=nano.specs -Wl,--print-memory-usage
        "-Wl,-Map=${CMAKE_BINARY_DIR}/${target}.map")
    target_link_libraries(${target} PRIVATE c m)
    set(esy_firmware_image_name "${target}")
    if(NOT esy_firmware_slot STREQUAL "DEFAULT")
        string(TOLOWER "${esy_firmware_slot}" esy_firmware_slot_lower)
        string(APPEND esy_firmware_image_name "_slot_${esy_firmware_slot_lower}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_OBJCOPY}" -O ihex "$<TARGET_FILE:${target}>"
            "${CMAKE_BINARY_DIR}/${esy_firmware_image_name}.hex"
        COMMAND "${CMAKE_OBJCOPY}" -O binary "$<TARGET_FILE:${target}>"
            "${CMAKE_BINARY_DIR}/${esy_firmware_image_name}.bin"
        COMMENT "Generating firmware HEX and BIN images"
        VERBATIM)
    set_property(TARGET ${target} APPEND PROPERTY ADDITIONAL_CLEAN_FILES
        "${CMAKE_BINARY_DIR}/${target}.map"
        "${CMAKE_BINARY_DIR}/${esy_firmware_image_name}.hex"
        "${CMAKE_BINARY_DIR}/${esy_firmware_image_name}.bin")
endfunction()
