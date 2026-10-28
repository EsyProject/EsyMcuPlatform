# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

set(ESY_PORT_EXPORT_FIELDS "")

function(esy_port_pico_match device output)
    if(device MATCHES "^(PICO|PICO_W|PICO2|PICO2_W)$")
        set(${output} TRUE PARENT_SCOPE)
    else()
        set(${output} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(esy_port_pico_resolve device result_prefix)
    if(device MATCHES "^PICO2")
        set(esy_board pico2)
    else()
        set(esy_board pico)
    endif()
    set(${result_prefix}_ESY_TARGET_DEVICE "${device}" PARENT_SCOPE)
    set(${result_prefix}_ESY_TARGET_PLATFORM "${device}" PARENT_SCOPE)
    set(${result_prefix}_ESY_BUILD_PORT pico PARENT_SCOPE)
    set(${result_prefix}_ESY_BOARD_PLATFORM_PATH "${esy_board}" PARENT_SCOPE)
    set(${result_prefix}_ESY_SPECS_PLATFORM_PATH pico PARENT_SCOPE)
endfunction()
