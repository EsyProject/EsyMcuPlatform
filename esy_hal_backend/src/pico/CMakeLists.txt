# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

# esy_hal_backend/src/pico/CMakeLists.txt

foreach(esy_unavailable_feature IN ITEMS ENABLE_ESY_CAN ENABLE_ESY_ADC)
    if(${esy_unavailable_feature})
        message(FATAL_ERROR
            "${esy_unavailable_feature} has no backend for ESY_TARGET_PLATFORM=${ESY_TARGET_PLATFORM}; supported platform is STM32F4")
    endif()
endforeach()

# ---- Platform dependencies ----
set(MODULE_DEPS
    pico_time
    pico_stdio
    esy_board
    esy_opt_default
    esy_common
)

# ---- Source collection ----
set(MODULE_SRCS "")

# SDK fixed-ID claims and lazy CYW43 allocation share one nonblocking assembly
# reservation, independently of the optional generic DMA facade.
if(ENABLE_ESY_DMA OR ENABLE_ESY_UART OR ENABLE_ESY_SPI OR ENABLE_ESY_PWM OR
   ENABLE_ESY_WIFI OR ENABLE_ESY_XIO)
    list(APPEND MODULE_SRCS esy_hb_resource_assembly.c)
endif()

if(ESY_ATOMIC_IRQ_SPINLOCK_BACKEND)
    if(NOT TARGET pico_atomic OR
       (NOT ESY_TARGET_PLATFORM STREQUAL "PICO" AND NOT ESY_TARGET_PLATFORM STREQUAL "PICO_W"))
        message(FATAL_ERROR "The IRQ-spinlock atomic backend requires RP2040 and pico_atomic")
    endif()
    list(APPEND MODULE_DEPS pico_atomic)
    list(APPEND MODULE_SRCS esy_hb_atomic.c)
endif()

if(ENABLE_ESY_BOOTLOADER)
    list(APPEND MODULE_SRCS esy_hb_boot.c)
    list(APPEND MODULE_SRCS ../esy_hb_boot_direct.c)
endif()

if(ENABLE_ESY_SYSTEM)
    if(NOT TARGET pico_rand)
        message(FATAL_ERROR "Pico system random source requires the pico_rand SDK target")
    endif()
    list(APPEND MODULE_SRCS esy_hb_system.c)
    list(APPEND MODULE_DEPS pico_rand)
endif()
if(ENABLE_ESY_FAULT)
    list(APPEND MODULE_SRCS esy_hb_fault.c)
    list(APPEND MODULE_DEPS hardware_exception hardware_sync)
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/esy_hb_fault.c
        TARGET_DIRECTORY esy_hal_backend PROPERTIES COMPILE_OPTIONS
        "-mgeneral-regs-only;-ffreestanding;-fno-builtin;-fno-stack-protector;-fno-lto")
endif()

# Capture uses the private GPIO IRQ provider without enabling the public facade.
if(ENABLE_ESY_GPIO OR ENABLE_ESY_TIMER_CAPTURE)
    list(APPEND MODULE_SRCS esy_hb_gpio.c)
    list(APPEND MODULE_DEPS hardware_gpio hardware_irq)
endif()

# Peripheral-native DMA shares the physical-resource arbiter independently of
# the optional generic HAL DMA controller facade.
if(ENABLE_ESY_DMA OR ENABLE_ESY_UART OR ENABLE_ESY_SPI OR ENABLE_ESY_PWM OR
   (ENABLE_ESY_XIO AND (ENABLE_ESY_XIO_UART_TX OR ENABLE_ESY_XIO_UART_RX OR
    ENABLE_ESY_XIO_SPI)))
    list(APPEND MODULE_SRCS esy_hb_dma.c)
    list(APPEND MODULE_DEPS hardware_dma)
endif()

if(ENABLE_ESY_UART)
    if(NOT TARGET esy_common_ds)
        message(FATAL_ERROR "ENABLE_ESY_UART requires ENABLE_ESY_COMMON_DS for RX stream support")
    endif()
    list(APPEND MODULE_SRCS esy_hb_uart.c)
    list(APPEND MODULE_DEPS hardware_uart esy_common_ds)
endif()

if(ENABLE_ESY_TIMER)
    list(APPEND MODULE_SRCS esy_hb_timer.c)
    list(APPEND MODULE_DEPS hardware_timer)
endif()

if(ENABLE_ESY_TIMER_CAPTURE)
    list(APPEND MODULE_SRCS esy_hb_timer_capture.c)
    list(APPEND MODULE_DEPS hardware_timer)
endif()

if(ENABLE_ESY_I2C)
    list(APPEND MODULE_SRCS esy_hb_i2c.c)
    list(APPEND MODULE_DEPS hardware_i2c pico_i2c_slave)
endif()

if(ENABLE_ESY_MULTICORE)
    if(NOT TARGET pico_multicore)
        message(FATAL_ERROR "ENABLE_ESY_MULTICORE requires the pico_multicore SDK target")
    endif()
    list(APPEND MODULE_SRCS esy_hb_multicore.c)
    list(APPEND MODULE_DEPS pico_multicore)
endif()

if(ENABLE_ESY_PWM)
    list(APPEND MODULE_SRCS esy_hb_pwm.c)
    list(APPEND MODULE_DEPS hardware_pwm hardware_gpio hardware_clocks hardware_irq hardware_sync)
endif()

if(ENABLE_ESY_WATCHDOG)
    list(APPEND MODULE_SRCS esy_hb_watchdog.c)
    list(APPEND MODULE_DEPS hardware_watchdog)
endif()

if(ENABLE_ESY_LOW_POWER)
    list(APPEND MODULE_SRCS esy_hb_low_power.c esy_hb_wakeup_timer.c)
    list(APPEND MODULE_DEPS hardware_timer hardware_irq)
endif()

if(ENABLE_ESY_SPI)
    list(APPEND MODULE_SRCS esy_hb_spi.c)
    list(APPEND MODULE_DEPS hardware_spi hardware_clocks)
endif()

if(ENABLE_ESY_FLASH)
    list(APPEND MODULE_SRCS esy_hb_flash.c)
    list(APPEND MODULE_DEPS hardware_flash)
endif()

if(ENABLE_ESY_USB_DEV)
    list(APPEND MODULE_SRCS esy_hb_usb_dev.c)
    list(APPEND MODULE_DEPS tinyusb_device tinyusb_board pico_stdlib)
endif()

if(ENABLE_ESY_WIFI)
    if(NOT ESY_TARGET_PLATFORM STREQUAL "PICO_W" AND NOT ESY_TARGET_PLATFORM STREQUAL "PICO2_W")
        message(FATAL_ERROR "ENABLE_ESY_WIFI requires a Pico W-class platform")
    endif()
    if(NOT TARGET esy_wifi_provider)
        message(FATAL_ERROR "ENABLE_ESY_WIFI requires consumer target esy_wifi_provider")
    endif()
    list(APPEND MODULE_SRCS esy_hb_wifi.c)
    list(APPEND MODULE_DEPS esy_wifi_provider)
endif()

if(ENABLE_ESY_XIO)
    list(APPEND MODULE_SRCS esy_hb_xio.c)
    list(APPEND MODULE_DEPS hardware_pio)

    add_subdirectory(xio)
endif()

# ---- Attach sources and dependencies to esy_hal_backend ----

target_sources(esy_hal_backend PRIVATE ${MODULE_SRCS})

target_include_directories(esy_hal_backend PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(esy_hal_backend PRIVATE ${MODULE_DEPS})
