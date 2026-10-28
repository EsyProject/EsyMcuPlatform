# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

foreach(dependency esy_common_defs esy_opt_default)
    if(NOT TARGET ${dependency})
        message(FATAL_ERROR "esy_credentials requires target ${dependency}")
    endif()
endforeach()

add_library(esy_credentials STATIC src/esy_credentials.c)
add_library(Esy::credentials ALIAS esy_credentials)
target_compile_features(esy_credentials PUBLIC c_std_11)
target_include_directories(esy_credentials PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(esy_credentials PUBLIC esy_common_defs PRIVATE esy_opt_default)

if(ENABLE_ESY_CREDENTIALS_STORE)
    foreach(dependency esy_file esy_common_utils)
        if(NOT TARGET ${dependency})
            message(FATAL_ERROR "esy_credentials_store requires target ${dependency}")
        endif()
    endforeach()
    add_library(esy_credentials_store STATIC src/esy_credentials_store.c)
    add_library(Esy::credentials_store ALIAS esy_credentials_store)
    target_compile_features(esy_credentials_store PUBLIC c_std_11)
    target_link_libraries(esy_credentials_store PUBLIC esy_credentials esy_file
        PRIVATE esy_common_utils esy_opt_default)
endif()
