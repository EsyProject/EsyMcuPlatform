# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

# esy_hal_backend/src/esp32/CMakeLists.txt

foreach(esy_unavailable_feature IN ITEMS ENABLE_ESY_CAN ENABLE_ESY_ADC)
    if(${esy_unavailable_feature})
        message(FATAL_ERROR
            "${esy_unavailable_feature} has no backend for ESY_TARGET_PLATFORM=${ESY_TARGET_PLATFORM}; supported platform is STM32F4")
    endif()
endforeach()
foreach(esy_pico_only_feature IN ITEMS ENABLE_ESY_MULTICORE ENABLE_ESY_USB_DEV ENABLE_ESY_XIO)
    if(${esy_pico_only_feature})
        message(FATAL_ERROR
            "${esy_pico_only_feature} has no backend for ESY_TARGET_PLATFORM=${ESY_TARGET_PLATFORM}; supported platforms are PICO, PICO_W, PICO2 and PICO2_W")
    endif()
endforeach()

# ---- Platform dependencies ----
set(MODULE_DEPS
    esy_board
    esy_opt_default
    esy_common
)

# ---- Source collection ----
set(MODULE_SRCS "")

# ---- Module feature gates are defined by the parent project ----

if(ENABLE_ESY_FAULT)
    if(NOT CONFIG_IDF_TARGET_ESP32)
        message(FATAL_ERROR "ESP32 fault capture requires the classic CONFIG_IDF_TARGET_ESP32 target")
    endif()
    if(NOT CONFIG_ESP_PANIC_HANDLER_IRAM)
        message(FATAL_ERROR "ESP32 fault capture requires CONFIG_ESP_PANIC_HANDLER_IRAM=y")
    endif()
    if(CONFIG_COMPILER_LTO)
        message(FATAL_ERROR "ESP32 native panic wrapping requires CONFIG_COMPILER_LTO=n")
    endif()
    foreach(esy_fault_provider IN ITEMS esp_system esp_hw_support xtensa)
        if(NOT TARGET idf::${esy_fault_provider})
            message(FATAL_ERROR "ESP32 fault capture requires native idf::${esy_fault_provider}")
        endif()
        list(APPEND MODULE_DEPS idf::${esy_fault_provider})
    endforeach()
    list(APPEND MODULE_SRCS esy_hb_fault.c)
    set(esy_fault_link_options
        "LINKER:--wrap=esp_panic_handler"
        "LINKER:--undefined=__wrap_esp_panic_handler")
    if(COMMAND idf_build_set_property)
        # Native IDF may retain pre-CMP0099 behavior for private static links.
        # Register with its final-application linker instead of relying on that
        # transitive path through a consumer component and the public HAL.
        foreach(esy_fault_link_option IN LISTS esy_fault_link_options)
            idf_build_set_property(LINK_OPTIONS "${esy_fault_link_option}" APPEND)
        endforeach()
    else()
        target_link_options(esy_hal_backend INTERFACE ${esy_fault_link_options})
    endif()
endif()

if(ENABLE_ESY_SYSTEM)
    if(NOT TARGET esy_os_context)
        message(FATAL_ERROR "ESP32 system requires the native esy_os_context port target")
    endif()
    if(NOT TARGET idf::esp_hw_support)
        message(FATAL_ERROR "ESP32 system random source requires native idf::esp_hw_support")
    endif()
    list(APPEND MODULE_DEPS esy_os_context idf::esp_hw_support)
    list(APPEND MODULE_SRCS esy_hb_system.c)
endif()

if(ENABLE_ESY_BOOTLOADER)
    if(NOT ENABLE_ESY_SYSTEM OR NOT ENABLE_ESY_FLASH)
        message(FATAL_ERROR "ENABLE_ESY_BOOTLOADER requires ENABLE_ESY_SYSTEM and ENABLE_ESY_FLASH")
    endif()
    if(NOT TARGET idf::app_update)
        message(FATAL_ERROR "ESP32 boot requires the native idf::app_update component")
    endif()
    list(APPEND MODULE_DEPS idf::app_update)
    if(NOT CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
        message(FATAL_ERROR "ESP32 managed boot requires CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y")
    endif()
    list(APPEND MODULE_SRCS esy_hb_boot.c)
endif()

# Capture uses the private GPIO IRQ provider without enabling the public facade.
if(ENABLE_ESY_GPIO OR ENABLE_ESY_TIMER_CAPTURE)
    list(APPEND MODULE_SRCS esy_hb_gpio.c)
endif()

if(ENABLE_ESY_TIMER)
    list(APPEND MODULE_SRCS esy_hb_timer.c)
endif()

if(ENABLE_ESY_TIMER_CAPTURE)
    if(NOT TARGET idf::esp_driver_gpio)
        message(FATAL_ERROR
            "ESP32 timer capture requires native idf::esp_driver_gpio")
    endif()
    if(NOT TARGET idf::esp_timer)
        message(FATAL_ERROR
            "ESP32 timer capture requires native idf::esp_timer")
    endif()
    list(APPEND MODULE_DEPS idf::esp_driver_gpio idf::esp_timer)
    list(APPEND MODULE_SRCS esy_hb_timer_capture.c)
endif()

if(ENABLE_ESY_WATCHDOG)
    list(APPEND MODULE_SRCS esy_hb_watchdog.c)
endif()

if(ENABLE_ESY_PWM)
    if(NOT TARGET idf::esp_driver_ledc)
        message(FATAL_ERROR
            "ESP32 PWM requires the native idf::esp_driver_ledc component")
    endif()
    if(NOT TARGET idf::esp_driver_gpio)
        message(FATAL_ERROR
            "ESP32 PWM's native LEDC headers require idf::esp_driver_gpio")
    endif()
    list(APPEND MODULE_DEPS idf::esp_driver_ledc idf::esp_driver_gpio)
    list(APPEND MODULE_SRCS esy_hb_pwm.c)
endif()

if(ENABLE_ESY_LOW_POWER)
    message(FATAL_ERROR "ENABLE_ESY_LOW_POWER is not implemented for ESY_TARGET_PLATFORM=ESP32")
endif()

# UART/PWM HAL ownership queries use the backend DMA query without requiring
# the optional generic HAL DMA controller facade.
if(ENABLE_ESY_DMA OR ENABLE_ESY_UART OR ENABLE_ESY_PWM)
    list(APPEND MODULE_SRCS esy_hb_dma.c)
endif()

if(ENABLE_ESY_I2C)
    if(NOT TARGET idf::esp_driver_i2c)
        message(FATAL_ERROR
            "ESP32 I2C requires the native idf::esp_driver_i2c component")
    endif()
    if(NOT TARGET idf::esp_driver_gpio)
        message(FATAL_ERROR
            "ESP32 I2C pin validation requires idf::esp_driver_gpio")
    endif()
    if(NOT TARGET idf::esp_timer)
        message(FATAL_ERROR
            "ESP32 I2C transaction deadlines require native idf::esp_timer")
    endif()
    list(APPEND MODULE_DEPS idf::esp_driver_i2c idf::esp_driver_gpio idf::esp_timer)
    list(APPEND MODULE_SRCS esy_hb_i2c.c)
endif()

if(ENABLE_ESY_UART)
    if(NOT TARGET idf::esp_driver_uart)
        message(FATAL_ERROR
            "ESP32 UART requires the native idf::esp_driver_uart component")
    endif()
    list(APPEND MODULE_DEPS idf::esp_driver_uart)
    list(APPEND MODULE_SRCS esy_hb_uart.c)
endif()

if(ENABLE_ESY_SPI)
    if(NOT TARGET idf::esp_driver_spi)
        message(FATAL_ERROR
            "ESP32 SPI requires the native idf::esp_driver_spi component")
    endif()
    if(NOT TARGET idf::esp_driver_gpio)
        message(FATAL_ERROR
            "ESP32 SPI chip-select control requires idf::esp_driver_gpio")
    endif()
    list(APPEND MODULE_DEPS idf::esp_driver_spi idf::esp_driver_gpio)
    list(APPEND MODULE_SRCS esy_hb_spi.c)
endif()

if(ENABLE_ESY_FLASH)
    list(APPEND MODULE_SRCS esy_hb_flash.c)
endif()

if(ENABLE_ESY_WIFI)
    message(FATAL_ERROR "ENABLE_ESY_WIFI is not implemented for ESY_TARGET_PLATFORM=ESP32")
endif()

# ---- Attach sources and dependencies to esy_hal_backend ----

target_sources(esy_hal_backend PRIVATE ${MODULE_SRCS})

target_include_directories(esy_hal_backend PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(esy_hal_backend PRIVATE ${MODULE_DEPS})
