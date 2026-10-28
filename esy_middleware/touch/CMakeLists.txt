# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

add_library(esy_touch STATIC)

target_sources(esy_touch
    PRIVATE
        src/esy_touch.c
)

target_include_directories(esy_touch
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(esy_touch
    PUBLIC
        esy_common
    PRIVATE
        esy_opt_default
)

if (ENABLE_ESY_TOUCH_XPT2046)
    if(NOT ENABLE_ESY_GPIO)
        message(FATAL_ERROR "esy_xpt2046 requires ENABLE_ESY_GPIO")
    endif()
    add_subdirectory(src/xpt2046)
endif()

# Device registration and auxiliary GPIO power policy are a separate integration.
if(ENABLE_ESY_GPIO)
    if(NOT TARGET esy_hal)
        message(FATAL_ERROR "esy_touch_device requires the esy_hal target")
    endif()
    add_library(esy_touch_device STATIC src/esy_touch_device.c)
    target_compile_features(esy_touch_device PUBLIC c_std_11)
    target_link_libraries(esy_touch_device PUBLIC esy_touch
        PRIVATE esy_hal esy_opt_default)
    if(ENABLE_ESY_TOUCH_XPT2046)
        target_sources(esy_touch_device PRIVATE src/xpt2046/esy_xpt2046_device.c)
    endif()
endif()
