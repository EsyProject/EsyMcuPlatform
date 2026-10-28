# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

foreach(esy_pico_only_feature IN ITEMS ENABLE_ESY_MULTICORE ENABLE_ESY_USB_DEV ENABLE_ESY_XIO)
    if(${esy_pico_only_feature})
        message(FATAL_ERROR
            "${esy_pico_only_feature} has no backend for ESY_TARGET_PLATFORM=${ESY_TARGET_PLATFORM}; supported platforms are PICO, PICO_W, PICO2 and PICO2_W")
    endif()
endforeach()
if(ENABLE_ESY_CAN AND NOT ESY_TARGET_DEVICE MATCHES "^STM32F(405|407|415|417)[A-Z0-9]*$")
    message(FATAL_ERROR
        "ENABLE_ESY_CAN requires a verified STM32F405/407/415/417 bxCAN device; ESY_TARGET_DEVICE=${ESY_TARGET_DEVICE}")
endif()

# ---- Platform dependencies ----
set(MODULE_DEPS
    STM32_Drivers
    esy_board
    esy_opt_default
    esy_common
)

# ---- Source collection ----
set(MODULE_SRCS "")
set(INTERRUPT_SRCS "")

# Module feature gates are defined by the parent project.

if(ENABLE_ESY_BOOTLOADER)
    list(APPEND MODULE_SRCS esy_hb_boot.c)
    list(APPEND MODULE_SRCS ../esy_hb_boot_direct.c)
endif()

if(ENABLE_ESY_SYSTEM)
    list(APPEND MODULE_SRCS esy_hb_system.c)
    if(NOT ESY_RTOS STREQUAL "FreeRTOS")
        list(APPEND INTERRUPT_SRCS stm32f4xx_systick_baremetal.c)
    endif()
endif()
if(ENABLE_ESY_FAULT)
    if(NOT ESY_TARGET_DEVICE MATCHES "^STM32F(405|407|415|417)[A-Z0-9]*$")
        message(FATAL_ERROR "ENABLE_ESY_FAULT requires a verified STM32F405/407/415/417 SRAM layout")
    endif()
    list(APPEND MODULE_SRCS esy_hb_fault.c)
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/esy_hb_fault.c
        TARGET_DIRECTORY esy_hal_backend PROPERTIES COMPILE_OPTIONS
        "-mgeneral-regs-only;-ffreestanding;-fno-builtin;-fno-stack-protector;-fno-lto")
endif()

# SPI chip-select operations use the private GPIO provider.
if(ENABLE_ESY_GPIO OR ENABLE_ESY_SPI)
    list(APPEND MODULE_SRCS esy_hb_gpio.c)
endif()

if(ENABLE_ESY_GPIO)
    list(APPEND INTERRUPT_SRCS stm32f4xx_it_gpio.c)
endif()

if(ENABLE_ESY_TIMER)
    list(APPEND MODULE_SRCS esy_hb_timer.c)
    list(APPEND INTERRUPT_SRCS stm32f4xx_it_timer.c)
endif()

if(ENABLE_ESY_TIMER_CAPTURE)
    list(APPEND MODULE_SRCS esy_hb_timer_capture.c)
endif()

if(ENABLE_ESY_WATCHDOG)
    list(APPEND MODULE_SRCS esy_hb_watchdog.c)
endif()

if(ENABLE_ESY_PWM)
    list(APPEND MODULE_SRCS esy_hb_pwm.c)
endif()

# Native peripheral transfers share this arbiter and its vectors even when the
# consumer does not select the generic HAL DMA controller facade.
if(ENABLE_ESY_DMA OR ENABLE_ESY_UART OR ENABLE_ESY_SPI OR ENABLE_ESY_PWM OR
   ENABLE_ESY_ADC)
    list(APPEND MODULE_SRCS esy_hb_dma.c)
    list(APPEND INTERRUPT_SRCS stm32f4xx_it_dma.c)
endif()

if(ENABLE_ESY_I2C)
    list(APPEND MODULE_SRCS esy_hb_i2c.c)
endif()

if(ENABLE_ESY_CAN)
    list(APPEND MODULE_SRCS esy_hb_can.c)
endif()

if(ENABLE_ESY_UART)
    if(NOT TARGET esy_common_ds)
        message(FATAL_ERROR "ENABLE_ESY_UART requires ENABLE_ESY_COMMON_DS for RX stream support")
    endif()
    list(APPEND MODULE_SRCS esy_hb_uart.c)
    list(APPEND INTERRUPT_SRCS stm32f4xx_it_uart.c)
    list(APPEND MODULE_DEPS esy_common_ds)
endif()

if(ENABLE_ESY_LOW_POWER)
    list(APPEND MODULE_SRCS esy_hb_low_power.c esy_hb_wakeup_timer.c)
endif()

if(ENABLE_ESY_ADC)
    list(APPEND MODULE_SRCS esy_hb_adc.c)
    list(APPEND INTERRUPT_SRCS stm32f4xx_it_adc.c)
endif()

if(ENABLE_ESY_SPI)
    list(APPEND MODULE_SRCS esy_hb_spi.c)
endif()

if(ENABLE_ESY_FLASH)
    if(NOT ESY_TARGET_DEVICE MATCHES "^STM32F(405|407|415|417)[A-Z0-9]*$")
        message(FATAL_ERROR
            "ENABLE_ESY_FLASH supports only STM32F405/407/415/417 single-bank layouts; "
            "ESY_TARGET_DEVICE=${ESY_TARGET_DEVICE} has no verified Flash geometry")
    endif()
    list(APPEND MODULE_SRCS esy_hb_flash.c)
endif()

if(ENABLE_ESY_WIFI)
    message(FATAL_ERROR "ENABLE_ESY_WIFI is not implemented for ESY_TARGET_PLATFORM=STM32F4")
endif()

# ---- Attach sources and dependencies to esy_hal_backend ----

target_sources(esy_hal_backend PRIVATE ${MODULE_SRCS})

target_include_directories(esy_hal_backend PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(esy_hal_backend PRIVATE ${MODULE_DEPS})

# Strong vector entries must reach the executable as objects so that startup
# weak aliases cannot suppress their extraction from a static archive.
if(INTERRUPT_SRCS)
    add_library(esy_hb_stm32f4_interrupts OBJECT ${INTERRUPT_SRCS})
    target_link_libraries(esy_hb_stm32f4_interrupts PRIVATE
        esy_hal_backend
        STM32_Drivers
        esy_common
        esy_opt_default
    )
    esy_register_interrupt_target(esy_hal_backend esy_hb_stm32f4_interrupts)
endif()
