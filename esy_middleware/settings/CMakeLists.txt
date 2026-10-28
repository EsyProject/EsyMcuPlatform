# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

if(NOT ENABLE_ESY_FILE)
    message(FATAL_ERROR "esy_settings requires ENABLE_ESY_FILE")
endif()
foreach(dependency esy_file esy_common_utils esy_opt_default)
    if(NOT TARGET ${dependency})
        message(FATAL_ERROR "esy_settings requires target ${dependency}")
    endif()
endforeach()

add_library(esy_settings STATIC src/esy_settings.c)
target_compile_features(esy_settings PUBLIC c_std_11)
target_include_directories(esy_settings PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(esy_settings PUBLIC esy_file
    PRIVATE esy_common_utils esy_opt_default)
