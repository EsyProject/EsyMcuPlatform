# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

# esy_hal/src/CMakeLists.txt

set(MODULE_SRCS "")

# ---- Feature-gated HAL source assembly (ENABLE_ESY_XXX) ----

if(ENABLE_ESY_BOOTLOADER AND (NOT ENABLE_ESY_FLASH OR NOT ENABLE_ESY_SYSTEM))
    message(FATAL_ERROR "ENABLE_ESY_BOOTLOADER requires ENABLE_ESY_FLASH and ENABLE_ESY_SYSTEM")
endif()
if(ENABLE_ESY_SYSTEM)
    list(APPEND MODULE_SRCS esy_hal_system.c)
endif()
if(ENABLE_ESY_FAULT)
    list(APPEND MODULE_SRCS esy_hal_fault.c)
endif()

if(ENABLE_ESY_GPIO)
    list(APPEND MODULE_SRCS esy_hal_gpio.c esy_hal_gpio_pm.c)
endif()

if(ENABLE_ESY_DMA)
    list(APPEND MODULE_SRCS esy_hal_dma.c)
endif()

if(ENABLE_ESY_UART OR (ENABLE_ESY_XIO AND
   (ENABLE_ESY_XIO_UART_TX OR ENABLE_ESY_XIO_UART_RX)))
    list(APPEND MODULE_SRCS esy_hal_uart_io.c)
endif()

if(ENABLE_ESY_UART)
    list(APPEND MODULE_SRCS esy_hal_uart.c)
endif()

if(ENABLE_ESY_TIMER)
    if(NOT ENABLE_ESY_SYSTEM)
        message(FATAL_ERROR "ENABLE_ESY_TIMER requires ENABLE_ESY_SYSTEM")
    endif()
    list(APPEND MODULE_SRCS esy_hal_timer.c)
endif()

if(ENABLE_ESY_RTC)
    if(NOT TARGET esy_common_utils)
        message(FATAL_ERROR "ENABLE_ESY_RTC requires target esy_common_utils")
    endif()
    list(APPEND MODULE_SRCS esy_hal_rtc.c)
    target_link_libraries(esy_hal PRIVATE esy_common_utils)
endif()

if(ENABLE_ESY_I2C)
    list(APPEND MODULE_SRCS esy_hal_i2c.c)
endif()

if(ENABLE_ESY_CAN)
    list(APPEND MODULE_SRCS esy_hal_can.c)
endif()

if(ENABLE_ESY_PWM)
    list(APPEND MODULE_SRCS esy_hal_pwm.c)
endif()

if(ENABLE_ESY_MULTICORE)
    list(APPEND MODULE_SRCS esy_hal_multicore.c)
endif()

if(ENABLE_ESY_WATCHDOG)
    list(APPEND MODULE_SRCS esy_hal_watchdog.c)
endif()

if(ENABLE_ESY_ADC)
    list(APPEND MODULE_SRCS esy_hal_adc.c)
endif()

if(ENABLE_ESY_SPI OR (ENABLE_ESY_XIO AND ENABLE_ESY_XIO_SPI))
    list(APPEND MODULE_SRCS esy_hal_spi_io.c)
endif()

if(ENABLE_ESY_SPI)
    list(APPEND MODULE_SRCS esy_hal_spi.c)
endif()

if(ENABLE_ESY_TIMER_CAPTURE)
    list(APPEND MODULE_SRCS esy_hal_timer_capture.c)
endif()

if(ENABLE_ESY_FLASH)
    list(APPEND MODULE_SRCS esy_hal_flash.c)
endif()

if(ENABLE_ESY_BOOTLOADER)
    list(APPEND MODULE_SRCS esy_hal_boot.c)
endif()

if(ENABLE_ESY_LOW_POWER)
    if(NOT ENABLE_ESY_SYSTEM)
        message(FATAL_ERROR "ENABLE_ESY_LOW_POWER requires ENABLE_ESY_SYSTEM")
    endif()
    list(APPEND MODULE_SRCS esy_hal_low_power.c
                            esy_hal_wakeup_timer.c)
endif()

if(ENABLE_ESY_USB_DEV)
    list(APPEND MODULE_SRCS esy_hal_usb_dev.c)
endif()

if(ENABLE_ESY_WIFI)
    list(APPEND MODULE_SRCS esy_hal_wifi.c)
endif()

if(ENABLE_ESY_XIO)
    add_subdirectory(xio)
endif()

# ---- Attach selected sources to the top-level esy_hal target ----

target_sources(esy_hal PRIVATE ${MODULE_SRCS})
