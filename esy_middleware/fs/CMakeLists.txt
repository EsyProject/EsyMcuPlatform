# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

foreach(dependency IN ITEMS esy_common_defs esy_opt_default)
    if(NOT TARGET ${dependency})
        message(FATAL_ERROR "esy_fs requires target ${dependency}")
    endif()
endforeach()

add_library(esy_fs STATIC src/esy_fs.c)
add_library(Esy::fs ALIAS esy_fs)
target_compile_features(esy_fs PUBLIC c_std_11)
target_include_directories(esy_fs PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(esy_fs PUBLIC esy_common_defs PRIVATE esy_opt_default)

if(ENABLE_ESY_FS_FLASH AND NOT ENABLE_ESY_FS_LITTLEFS)
    message(FATAL_ERROR "ENABLE_ESY_FS_FLASH requires ENABLE_ESY_FS_LITTLEFS")
endif()
if(ENABLE_ESY_FS_LITTLEFS)
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/littlefs.cmake)
endif()
if(ENABLE_ESY_FS_FATFS)
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/fatfs.cmake)
endif()
