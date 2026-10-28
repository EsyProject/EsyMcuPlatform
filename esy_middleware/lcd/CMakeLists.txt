# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

add_library(esy_lcd STATIC)

target_include_directories(esy_lcd
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_sources(esy_lcd
    PRIVATE
        src/esy_lcd.c
)

target_link_libraries(esy_lcd
    PUBLIC
        esy_common
    PRIVATE
        esy_opt_default
)

if (ENABLE_ESY_LCD_ILI9488)
    if(NOT ENABLE_ESY_GPIO)
        message(FATAL_ERROR "esy_ili9488 requires ENABLE_ESY_GPIO")
    endif()
    add_subdirectory(src/ili9488)
endif()

# Device registration and auxiliary GPIO power policy are a separate integration.
if(ENABLE_ESY_GPIO)
    if(NOT TARGET esy_hal)
        message(FATAL_ERROR "esy_lcd_device requires the esy_hal target")
    endif()
    add_library(esy_lcd_device STATIC src/esy_lcd_device.c)
    target_compile_features(esy_lcd_device PUBLIC c_std_11)
    target_link_libraries(esy_lcd_device PUBLIC esy_lcd
        PRIVATE esy_hal esy_opt_default)
    if(ENABLE_ESY_LCD_ILI9488)
        target_sources(esy_lcd_device PRIVATE src/ili9488/esy_ili9488_device.c)
    endif()
endif()
