# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

target_sources(esy_freq_synth PRIVATE
    esy_freq_synth.c
)
target_compile_features(esy_freq_synth PUBLIC c_std_11)

if(ENABLE_ESY_ADF4351 OR ENABLE_ESY_ADF4372 OR ENABLE_ESY_LMX2594 OR ENABLE_ESY_LMX2592)
    foreach(feature ENABLE_ESY_SPI ENABLE_ESY_GPIO ENABLE_ESY_SYSTEM)
        if(NOT ${feature})
            message(FATAL_ERROR "Frequency synthesizer chip drivers require ${feature}")
        endif()
    endforeach()
    if(NOT TARGET esy_hal)
        message(FATAL_ERROR "Frequency synthesizer chip drivers require the esy_hal target")
    endif()
    # Chip configuration headers expose public SPI handles; no GPIO PM API is used.
    target_link_libraries(esy_freq_synth PUBLIC esy_hal)
endif()

if (ENABLE_ESY_ADF4351)
    target_sources(esy_freq_synth PRIVATE
        esy_adf4351.c
    )
endif()

if (ENABLE_ESY_ADF4372)
    target_sources(esy_freq_synth PRIVATE
        esy_adf4372.c
    )
endif()

if (ENABLE_ESY_LMX2594)
    target_sources(esy_freq_synth PRIVATE
        esy_lmx2594.c
    )
endif()

if (ENABLE_ESY_LMX2592)
    target_sources(esy_freq_synth PRIVATE
        esy_lmx2592.c
    )
endif()
