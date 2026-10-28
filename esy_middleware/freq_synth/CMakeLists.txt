# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

add_library(esy_freq_synth STATIC)

target_include_directories(esy_freq_synth
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(esy_freq_synth
    PUBLIC
        esy_common_defs
        esy_common_device
    PRIVATE
        esy_opt_default
)

add_subdirectory(src)

# Device registration and auxiliary GPIO power policy are a separate integration.
if(ENABLE_ESY_GPIO)
    if(NOT TARGET esy_hal)
        message(FATAL_ERROR "esy_freq_synth_device requires the esy_hal target")
    endif()
    add_library(esy_freq_synth_device STATIC src/esy_freq_synth_device.c)
    target_compile_features(esy_freq_synth_device PUBLIC c_std_11)
    target_link_libraries(esy_freq_synth_device PUBLIC esy_freq_synth
        PRIVATE esy_hal esy_opt_default)
    foreach(chip ADF4351 ADF4372)
        if(ENABLE_ESY_${chip})
            string(TOLOWER "${chip}" chip_source)
            target_sources(esy_freq_synth_device PRIVATE src/esy_${chip_source}_device.c)
        endif()
    endforeach()
endif()
