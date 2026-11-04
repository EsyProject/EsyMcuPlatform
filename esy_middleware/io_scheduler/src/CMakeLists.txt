# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

function(esy_add_io_scheduler_adapter target source io_feature)
    if(NOT ESY_RTOS STREQUAL "FreeRTOS")
        message(FATAL_ERROR "${target} requires ESY_RTOS=FreeRTOS")
    endif()
    foreach(feature IN ITEMS ${io_feature} ENABLE_ESY_SYSTEM)
        if(NOT ${feature})
            message(FATAL_ERROR "${target} requires ${feature}")
        endif()
    endforeach()
    foreach(dependency IN ITEMS esy_common esy_hal esy_common_ds esy_os esy_opt_default)
        if(NOT TARGET ${dependency})
            message(FATAL_ERROR "${target} requires the ${dependency} target")
        endif()
    endforeach()

    add_library(${target} STATIC ${source})
    target_include_directories(${target} PUBLIC ${ESY_IO_SCHEDULER_INCLUDE_DIR})
    target_compile_features(${target} PUBLIC c_std_11)
    target_link_libraries(${target}
        PUBLIC
            esy_common
            esy_hal
            esy_common_ds
            esy_os
        PRIVATE
            esy_opt_default
    )
endfunction()

function(esy_add_io_scheduler_core target source)
    foreach(feature IN LISTS ARGN)
        if(NOT ${feature})
            message(FATAL_ERROR "${target} requires ${feature}")
        endif()
    endforeach()
    foreach(dependency IN ITEMS esy_common esy_hal esy_common_ds esy_opt_default)
        if(NOT TARGET ${dependency})
            message(FATAL_ERROR "${target} requires the ${dependency} target")
        endif()
    endforeach()
    add_library(${target} STATIC ${source})
    target_include_directories(${target} PUBLIC ${ESY_IO_SCHEDULER_INCLUDE_DIR})
    target_compile_features(${target} PUBLIC c_std_11)
    target_link_libraries(${target} PUBLIC esy_common esy_hal esy_common_ds
        PRIVATE esy_opt_default)
endfunction()

if(ENABLE_ESY_SPI_SCHEDULER)
    if(NOT ESY_RTOS STREQUAL "None" AND NOT ESY_RTOS STREQUAL "FreeRTOS")
        message(FATAL_ERROR "SPI scheduler requires ESY_RTOS=None or ESY_RTOS=FreeRTOS")
    endif()
    esy_add_io_scheduler_core(esy_spi_scheduler_core esy_spi_scheduler_core.c
        ENABLE_ESY_SPI ENABLE_ESY_SYSTEM)
    if(ESY_RTOS STREQUAL "FreeRTOS")
        esy_add_io_scheduler_adapter(esy_spi_scheduler esy_spi_scheduler.c ENABLE_ESY_SPI)
        target_link_libraries(esy_spi_scheduler PUBLIC esy_spi_scheduler_core)
    endif()
endif()

if(ENABLE_ESY_UART_TX_SCHEDULER)
    esy_add_io_scheduler_core(esy_uart_tx_scheduler esy_uart_tx_scheduler.c
        ENABLE_ESY_UART)
    if(ESY_RTOS STREQUAL "FreeRTOS" AND ENABLE_ESY_SYSTEM)
        esy_add_io_scheduler_adapter(esy_uart_tx_scheduler_os
            esy_uart_tx_scheduler_os.c ENABLE_ESY_UART)
        target_link_libraries(esy_uart_tx_scheduler_os PUBLIC esy_uart_tx_scheduler)
    endif()
endif()

if(ENABLE_ESY_DMA_SCHEDULER)
    esy_add_io_scheduler_core(esy_dma_scheduler_core esy_dma_scheduler_core.c
        ENABLE_ESY_DMA)
endif()

if(ENABLE_ESY_PWM_SCHEDULER)
    esy_add_io_scheduler_core(esy_pwm_scheduler_core esy_pwm_scheduler_core.c
        ENABLE_ESY_PWM)
endif()
