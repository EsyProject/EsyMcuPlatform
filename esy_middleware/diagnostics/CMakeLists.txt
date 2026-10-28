# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

foreach(dependency IN ITEMS esy_common_defs esy_opt_default)
    if(NOT TARGET ${dependency})
        message(FATAL_ERROR "esy_diagnostics requires target ${dependency}")
    endif()
endforeach()

add_library(esy_diagnostics STATIC src/esy_diagnostics.c)
target_compile_features(esy_diagnostics PUBLIC c_std_11)
target_include_directories(esy_diagnostics PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(esy_diagnostics PUBLIC esy_common_defs PRIVATE esy_opt_default)
