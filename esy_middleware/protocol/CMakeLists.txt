# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

set(ESY_PROTO_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/include)

if(ENABLE_ESY_PROTO_JSON_TCP_CHANNEL)
    foreach(feature IN ITEMS ENABLE_ESY_COMMON_PROTOCOL ENABLE_ESY_COMMON_PROTOCOL_JSON
            ENABLE_ESY_NETWORK)
        if(NOT ${feature})
            message(FATAL_ERROR "ENABLE_ESY_PROTO_JSON_TCP_CHANNEL requires ${feature}")
        endif()
    endforeach()
    foreach(dependency IN ITEMS esy_common_defs esy_common_protocol_json esy_network)
        if(NOT TARGET ${dependency})
            message(FATAL_ERROR "esy_proto_json_tcp_channel requires target ${dependency}; enable and assemble its consumer feature gate")
        endif()
    endforeach()
    add_library(esy_proto_json_tcp_channel STATIC src/json/esy_proto_json_tcp_channel.c)
    add_library(Esy::proto_json_tcp_channel ALIAS esy_proto_json_tcp_channel)
    target_compile_features(esy_proto_json_tcp_channel PUBLIC c_std_11)
    target_include_directories(esy_proto_json_tcp_channel PUBLIC ${ESY_PROTO_INCLUDE_DIR})
    target_link_libraries(esy_proto_json_tcp_channel
        PUBLIC esy_common_protocol_json esy_network)
    if(TARGET esy_opt_default)
        target_link_libraries(esy_proto_json_tcp_channel PRIVATE esy_opt_default)
    endif()
endif()

if(ENABLE_ESY_PROTO_JSON_USB_CHANNEL)
    foreach(feature IN ITEMS ENABLE_ESY_COMMON_PROTOCOL ENABLE_ESY_COMMON_PROTOCOL_JSON
            ENABLE_ESY_USB ENABLE_ESY_USB_CDC)
        if(NOT ${feature})
            message(FATAL_ERROR "ENABLE_ESY_PROTO_JSON_USB_CHANNEL requires ${feature}")
        endif()
    endforeach()
    foreach(dependency IN ITEMS esy_common_defs esy_common_protocol_json esy_usb)
        if(NOT TARGET ${dependency})
            message(FATAL_ERROR "esy_proto_json_usb_channel requires target ${dependency}; enable and assemble its consumer feature gate")
        endif()
    endforeach()
    add_library(esy_proto_json_usb_channel STATIC src/json/esy_proto_json_usb_channel.c)
    add_library(Esy::proto_json_usb_channel ALIAS esy_proto_json_usb_channel)
    target_compile_features(esy_proto_json_usb_channel PUBLIC c_std_11)
    target_include_directories(esy_proto_json_usb_channel PUBLIC ${ESY_PROTO_INCLUDE_DIR})
    target_link_libraries(esy_proto_json_usb_channel
        PUBLIC esy_common_protocol_json esy_usb)
    if(TARGET esy_opt_default)
        target_link_libraries(esy_proto_json_usb_channel PRIVATE esy_opt_default)
    endif()
endif()

foreach(channel IN ITEMS ENABLE_ESY_PROTO_BIN_UART_CHANNEL ENABLE_ESY_PROTO_BIN_USB_CHANNEL
                         ENABLE_ESY_PROTO_BIN_TCP_CHANNEL)
    if(${channel} AND NOT ENABLE_ESY_PROTO_BIN)
        message(FATAL_ERROR "${channel} requires ENABLE_ESY_PROTO_BIN")
    endif()
endforeach()
foreach(component IN ITEMS ENABLE_ESY_PROTO_MODBUS_UART_CHANNEL ENABLE_ESY_PROTO_MODBUS_UART_WORKER
                           ENABLE_ESY_PROTO_MODBUS_TCP_CHANNEL)
    if(${component} AND NOT ENABLE_ESY_PROTO_MODBUS)
        message(FATAL_ERROR "${component} requires ENABLE_ESY_PROTO_MODBUS")
    endif()
endforeach()
if(ENABLE_ESY_PROTO_MODBUS_UART_WORKER AND NOT ENABLE_ESY_PROTO_MODBUS_UART_CHANNEL)
    message(FATAL_ERROR "ENABLE_ESY_PROTO_MODBUS_UART_WORKER requires ENABLE_ESY_PROTO_MODBUS_UART_CHANNEL")
endif()
if(ENABLE_ESY_PROTO_MODBUS_UART_WORKER AND NOT ESY_RTOS STREQUAL "FreeRTOS")
    message(FATAL_ERROR "esy_proto_modbus_uart_worker requires ESY_RTOS=FreeRTOS")
endif()

function(esy_require_proto_dependencies family)
    if(NOT ENABLE_ESY_SYSTEM)
        message(FATAL_ERROR "${family} requires ENABLE_ESY_SYSTEM for deadlines")
    endif()
    foreach(dependency IN ITEMS esy_common_defs esy_common_protocol_codec
            esy_common_ds esy_common_utils esy_hal esy_opt_default)
        if(NOT TARGET ${dependency})
            message(FATAL_ERROR "${family} requires target ${dependency}; enable its consumer feature gate")
        endif()
    endforeach()
endfunction()

if(ENABLE_ESY_PROTO_BIN)
    esy_require_proto_dependencies(esy_proto_bin_session)
    # The session and OTA router share value types and the borrowed sender contract.
    add_library(esy_proto_bin_contract INTERFACE)
    target_compile_features(esy_proto_bin_contract INTERFACE c_std_11)
    target_include_directories(esy_proto_bin_contract INTERFACE ${ESY_PROTO_INCLUDE_DIR})
    target_link_libraries(esy_proto_bin_contract INTERFACE esy_common_defs esy_common_protocol_codec)

    add_library(esy_proto_bin_session STATIC src/bin/esy_proto_bin_session.c)
    target_include_directories(esy_proto_bin_session PUBLIC ${ESY_PROTO_INCLUDE_DIR})
    target_link_libraries(esy_proto_bin_session
        PUBLIC esy_proto_bin_contract esy_common_protocol_codec esy_common_ds
        PRIVATE esy_common_utils esy_hal esy_opt_default)

    if(ENABLE_ESY_PROTO_BIN_UART_CHANNEL)
        foreach(feature IN ITEMS ENABLE_ESY_UART ENABLE_ESY_DMA)
            if(NOT ${feature})
                message(FATAL_ERROR "esy_proto_bin_uart_channel requires ${feature}")
            endif()
        endforeach()
        if(NOT TARGET esy_uart_tx_scheduler)
            message(FATAL_ERROR "esy_proto_bin_uart_channel requires target esy_uart_tx_scheduler; enable ENABLE_ESY_IO_SCHEDULER and ENABLE_ESY_UART_TX_SCHEDULER")
        endif()
        add_library(esy_proto_bin_uart_channel STATIC src/bin/esy_proto_bin_uart_channel.c)
        target_include_directories(esy_proto_bin_uart_channel PUBLIC ${ESY_PROTO_INCLUDE_DIR})
        target_link_libraries(esy_proto_bin_uart_channel
            PUBLIC esy_proto_bin_session esy_hal
            PRIVATE esy_uart_tx_scheduler esy_opt_default)
    endif()

    if(ENABLE_ESY_PROTO_BIN_USB_CHANNEL)
        if(NOT TARGET esy_usb)
            message(FATAL_ERROR "esy_proto_bin_usb_channel requires target esy_usb; enable ENABLE_ESY_USB and its CDC provider")
        endif()
        add_library(esy_proto_bin_usb_channel STATIC src/bin/esy_proto_bin_usb_channel.c)
        target_include_directories(esy_proto_bin_usb_channel PUBLIC ${ESY_PROTO_INCLUDE_DIR})
        target_link_libraries(esy_proto_bin_usb_channel
            PUBLIC esy_proto_bin_session esy_usb PRIVATE esy_opt_default)
    endif()

    if(ENABLE_ESY_PROTO_BIN_TCP_CHANNEL)
        if(NOT TARGET esy_network)
            message(FATAL_ERROR "esy_proto_bin_tcp_channel requires target esy_network; enable ENABLE_ESY_NETWORK")
        endif()
        add_library(esy_proto_bin_tcp_channel STATIC src/bin/esy_proto_bin_tcp_channel.c)
        target_include_directories(esy_proto_bin_tcp_channel PUBLIC ${ESY_PROTO_INCLUDE_DIR})
        target_link_libraries(esy_proto_bin_tcp_channel
            PUBLIC esy_proto_bin_session esy_network PRIVATE esy_hal esy_opt_default)
    endif()
endif()

if(ENABLE_ESY_PROTO_MODBUS)
    esy_require_proto_dependencies(esy_proto_modbus_manager)
    add_library(esy_proto_modbus_manager STATIC src/modbus/esy_proto_modbus_manager.c)
    target_include_directories(esy_proto_modbus_manager PUBLIC ${ESY_PROTO_INCLUDE_DIR})
    target_link_libraries(esy_proto_modbus_manager
        PUBLIC esy_common_protocol_codec esy_common_ds
        PRIVATE esy_common_utils esy_hal esy_opt_default)

    if(ENABLE_ESY_PROTO_MODBUS_TCP_CHANNEL)
        if(NOT TARGET esy_network)
            message(FATAL_ERROR "esy_proto_modbus_tcp_channel requires target esy_network; enable ENABLE_ESY_NETWORK")
        endif()
        add_library(esy_proto_modbus_tcp_channel STATIC src/modbus/esy_proto_modbus_tcp_channel.c)
        target_compile_features(esy_proto_modbus_tcp_channel PUBLIC c_std_11)
        target_include_directories(esy_proto_modbus_tcp_channel PUBLIC ${ESY_PROTO_INCLUDE_DIR})
        target_link_libraries(esy_proto_modbus_tcp_channel
            PUBLIC esy_proto_modbus_manager esy_network
            PRIVATE esy_common_utils esy_hal esy_opt_default)
    endif()

    if(ENABLE_ESY_PROTO_MODBUS_UART_CHANNEL)
        foreach(feature IN ITEMS ENABLE_ESY_UART ENABLE_ESY_DMA)
            if(NOT ${feature})
                message(FATAL_ERROR "esy_proto_modbus_uart_channel requires ${feature}")
            endif()
        endforeach()
        if(NOT TARGET esy_uart_tx_scheduler)
            message(FATAL_ERROR "esy_proto_modbus_uart_channel requires target esy_uart_tx_scheduler; enable ENABLE_ESY_IO_SCHEDULER and ENABLE_ESY_UART_TX_SCHEDULER")
        endif()
        add_library(esy_proto_modbus_uart_channel STATIC src/modbus/esy_proto_modbus_uart_channel.c)
        target_compile_features(esy_proto_modbus_uart_channel PUBLIC c_std_11)
        target_include_directories(esy_proto_modbus_uart_channel PUBLIC ${ESY_PROTO_INCLUDE_DIR})
        target_link_libraries(esy_proto_modbus_uart_channel
            PUBLIC esy_proto_modbus_manager esy_uart_tx_scheduler esy_hal
            PRIVATE esy_common_utils esy_opt_default)
    endif()

    if(ENABLE_ESY_PROTO_MODBUS_UART_WORKER)
        if(NOT TARGET esy_os)
            message(FATAL_ERROR "esy_proto_modbus_uart_worker requires target esy_os")
        endif()
        add_library(esy_proto_modbus_uart_worker STATIC src/modbus/esy_proto_modbus_uart_worker.c)
        target_compile_features(esy_proto_modbus_uart_worker PUBLIC c_std_11)
        target_include_directories(esy_proto_modbus_uart_worker PUBLIC ${ESY_PROTO_INCLUDE_DIR})
        target_link_libraries(esy_proto_modbus_uart_worker
            PUBLIC esy_proto_modbus_uart_channel esy_os
            PRIVATE esy_common_utils esy_hal esy_opt_default)
    endif()
endif()
