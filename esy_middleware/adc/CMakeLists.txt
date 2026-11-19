# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

if(ENABLE_ESY_ADC_AD7276)
    foreach(dependency esy_common_defs esy_opt_default)
        if(NOT TARGET ${dependency})
            message(FATAL_ERROR "esy_adc_ad7276 requires target ${dependency}")
        endif()
    endforeach()

    add_library(esy_adc_ad7276 STATIC src/esy_adc_ad7276.c)
    target_compile_features(esy_adc_ad7276 PUBLIC c_std_11)
    target_include_directories(esy_adc_ad7276
        PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
    target_link_libraries(esy_adc_ad7276
        PUBLIC esy_common_defs PRIVATE esy_opt_default)
endif()
