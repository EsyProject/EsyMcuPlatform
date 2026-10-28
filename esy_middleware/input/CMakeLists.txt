# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

foreach(dependency esy_common_defs esy_opt_default)
    if(NOT TARGET ${dependency})
        message(FATAL_ERROR "esy_input requires target ${dependency}")
    endif()
endforeach()

add_library(esy_input STATIC)
target_compile_features(esy_input PUBLIC c_std_11)
target_include_directories(esy_input PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(esy_input PUBLIC esy_common_defs PRIVATE esy_opt_default)

add_subdirectory(src)

if(ENABLE_ESY_GPIO)
    if(NOT TARGET esy_hal)
        message(FATAL_ERROR "esy_input_gpio requires consumer target esy_hal")
    endif()
    add_library(esy_input_gpio STATIC src/esy_input_gpio.c)
    target_compile_features(esy_input_gpio PUBLIC c_std_11)
    target_link_libraries(esy_input_gpio PUBLIC esy_input esy_hal
        PRIVATE esy_opt_default)
endif()
