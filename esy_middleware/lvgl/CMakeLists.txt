# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

if(NOT ESY_RTOS STREQUAL "FreeRTOS")
    message(FATAL_ERROR "esy_lvgl requires ESY_RTOS=FreeRTOS")
endif()
if(NOT ENABLE_ESY_OS_QUEUE OR NOT ENABLE_ESY_SYSTEM)
    message(FATAL_ERROR "esy_lvgl requires ENABLE_ESY_OS_QUEUE and ENABLE_ESY_SYSTEM")
endif()
foreach(dependency lvgl esy_common esy_os esy_hal esy_lcd esy_opt_default)
    if(NOT TARGET ${dependency})
        message(FATAL_ERROR "esy_lvgl requires consumer/provider target ${dependency}")
    endif()
endforeach()

add_library(esy_lvgl STATIC)

target_include_directories(esy_lvgl
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(esy_lvgl
    PUBLIC
        lvgl
        esy_common
        esy_os
        esy_lcd
    PRIVATE
        esy_hal
        esy_opt_default
)

add_subdirectory(src)

# The touch adapter is selected through the consumer's existing touch feature.
if(ENABLE_ESY_TOUCH)
    if(NOT TARGET esy_touch)
        message(FATAL_ERROR "esy_lvgl_touch requires consumer target esy_touch")
    endif()
    add_library(esy_lvgl_touch STATIC src/esy_lvgl_touch.c)
    target_compile_features(esy_lvgl_touch PUBLIC c_std_11)
    target_link_libraries(esy_lvgl_touch PUBLIC esy_lvgl esy_touch
        PRIVATE esy_opt_default)
endif()
