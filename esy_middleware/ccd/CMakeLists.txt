# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

if(NOT ENABLE_ESY_CCD_TCD1254)
    return()
endif()

foreach(dependency esy_common_defs esy_opt_default)
    if(NOT TARGET ${dependency})
        message(FATAL_ERROR "esy_ccd_tcd1254 requires target ${dependency}")
    endif()
endforeach()

add_library(esy_ccd_tcd1254 STATIC src/esy_ccd_tcd1254.c)
target_compile_features(esy_ccd_tcd1254 PUBLIC c_std_11)
target_include_directories(esy_ccd_tcd1254
    PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(esy_ccd_tcd1254
    PUBLIC esy_common_defs PRIVATE esy_opt_default)
