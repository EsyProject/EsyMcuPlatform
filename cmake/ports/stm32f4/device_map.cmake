# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

set(ESY_PORT_EXPORT_FIELDS ESY_STM32F4_DEVICE_DEFINE ESY_STM32F4_STARTUP_SOURCE)

# Shared by target options and standalone compiler-probe initialization.
set(esy_stm32_arch_options
    -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard)

function(esy_port_stm32f4_match device output)
    if(device MATCHES "^STM32F4")
        set(${output} TRUE PARENT_SCOPE)
    else()
        set(${output} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(esy_port_stm32f4_resolve device result_prefix)
    esy_resolve_stm32f4_device("${device}" esy_define esy_startup)
    set(${result_prefix}_ESY_TARGET_DEVICE "${device}" PARENT_SCOPE)
    set(${result_prefix}_ESY_TARGET_PLATFORM STM32F4 PARENT_SCOPE)
    set(${result_prefix}_ESY_BUILD_PORT stm32f4 PARENT_SCOPE)
    set(${result_prefix}_ESY_BOARD_PLATFORM_PATH stm32f4 PARENT_SCOPE)
    set(${result_prefix}_ESY_SPECS_PLATFORM_PATH stm32f4 PARENT_SCOPE)
    set(${result_prefix}_ESY_STM32F4_DEVICE_DEFINE "${esy_define}" PARENT_SCOPE)
    set(${result_prefix}_ESY_STM32F4_STARTUP_SOURCE "${esy_startup}" PARENT_SCOPE)
endfunction()

# Resolve documented part-number cores rather than guessing a CMSIS family macro.
# An optional package/temperature suffix, such as T6 or U6TR, follows the core.
function(esy_resolve_stm32f4_device device_model output_define output_startup)
    set(esy_device_rules
        "STM32F401[CRV][BC]:STM32F401xC"
        "STM32F401[CRV][DE]:STM32F401xE"
        "STM32F405[RVZ]G:STM32F405xx"
        "STM32F407[VZI][EG]:STM32F407xx"
        "STM32F410T[8B]:STM32F410Tx"
        "STM32F410C[8B]:STM32F410Cx"
        "STM32F410R[8B]:STM32F410Rx"
        "STM32F411[CRV][CE]:STM32F411xE"
        "STM32F412C[EG]:STM32F412Cx"
        "STM32F412R[EG]:STM32F412Rx"
        "STM32F412V[EG]:STM32F412Vx"
        "STM32F412Z[EG]:STM32F412Zx"
        "STM32F413[CMRVZ][GH]:STM32F413xx"
        "STM32F415[RVZ]G:STM32F415xx"
        "STM32F417[VZI][EG]:STM32F417xx"
        "STM32F423[CMRVZ]H:STM32F423xx"
        "STM32F427[VZI][GI]:STM32F427xx"
        "STM32F429[VZBNI][GI]:STM32F429xx"
        "STM32F437[VZI][GI]:STM32F437xx"
        "STM32F439[VZBNI][GI]:STM32F439xx"
        "STM32F446[MRVZ][CE]:STM32F446xx"
        "STM32F469[AINB][EGI]:STM32F469xx"
        "STM32F479[AINB][GI]:STM32F479xx"
    )
    foreach(esy_device_rule IN LISTS esy_device_rules)
        string(REPLACE ":" ";" esy_device_fields "${esy_device_rule}")
        list(GET esy_device_fields 0 esy_device_pattern)
        if(device_model MATCHES "^${esy_device_pattern}([A-Z][0-9][A-Z0-9]*)?$")
            list(GET esy_device_fields 1 esy_device_define)
            string(TOLOWER "${esy_device_define}" esy_device_startup_stem)
            set(${output_define} "${esy_device_define}" PARENT_SCOPE)
            set(${output_startup} "startup_${esy_device_startup_stem}.s" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR
        "Unsupported or ambiguous ESY_TARGET_DEVICE=${device_model}: provide a recognized "
        "STM32F4 part-number core with package and density, such as STM32F407ZG, "
        "STM32F401RE or STM32F412RG; bare families and CMSIS macros are not device models.")
endfunction()
