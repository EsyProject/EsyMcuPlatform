# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

set(ESY_PORT_EXPORT_FIELDS "")

function(esy_port_esp32_match device output)
    if(device STREQUAL "ESP32")
        set(${output} TRUE PARENT_SCOPE)
    else()
        set(${output} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(esy_port_esp32_resolve device result_prefix)
    set(${result_prefix}_ESY_TARGET_DEVICE "${device}" PARENT_SCOPE)
    set(${result_prefix}_ESY_TARGET_PLATFORM ESP32 PARENT_SCOPE)
    set(${result_prefix}_ESY_BUILD_PORT esp32 PARENT_SCOPE)
    set(${result_prefix}_ESY_BOARD_PLATFORM_PATH esp32 PARENT_SCOPE)
    set(${result_prefix}_ESY_SPECS_PLATFORM_PATH esp32 PARENT_SCOPE)
endfunction()
