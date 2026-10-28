# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

if(NOT DEFINED ESY_LITTLEFS_PATH OR ESY_LITTLEFS_PATH STREQUAL "")
    message(FATAL_ERROR "ENABLE_ESY_FS_LITTLEFS requires ESY_LITTLEFS_PATH pointing to official littlefs 2.11.x sources")
endif()
foreach(_esy_littlefs_file IN ITEMS lfs.h lfs.c lfs_util.h lfs_util.c)
    if(NOT EXISTS "${ESY_LITTLEFS_PATH}/${_esy_littlefs_file}")
        message(FATAL_ERROR "ESY_LITTLEFS_PATH is missing ${_esy_littlefs_file}")
    endif()
endforeach()
if(TARGET esy_fs_littlefs_provider)
    message(FATAL_ERROR "esy_fs_littlefs_provider already exists; this adapter owns its provider configuration")
endif()
add_library(esy_fs_littlefs_provider STATIC
    "${ESY_LITTLEFS_PATH}/lfs.c" "${ESY_LITTLEFS_PATH}/lfs_util.c")
target_include_directories(esy_fs_littlefs_provider SYSTEM PUBLIC "${ESY_LITTLEFS_PATH}")
target_compile_definitions(esy_fs_littlefs_provider PUBLIC LFS_NO_MALLOC LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR)
target_compile_features(esy_fs_littlefs_provider PUBLIC c_std_11)
target_link_libraries(esy_fs_littlefs_provider PRIVATE esy_opt_default)

add_library(esy_fs_littlefs STATIC src/backends/esy_fs_littlefs.c)
target_link_libraries(esy_fs_littlefs PUBLIC esy_fs esy_fs_littlefs_provider PRIVATE esy_opt_default)
target_compile_features(esy_fs_littlefs PUBLIC c_std_11)

if(ENABLE_ESY_FS_FLASH)
    if(NOT ENABLE_ESY_FLASH OR NOT TARGET esy_hal)
        message(FATAL_ERROR "ENABLE_ESY_FS_FLASH requires ENABLE_ESY_FLASH and the public esy_hal target")
    endif()
    add_library(esy_fs_flash STATIC src/media/esy_fs_flash.c)
    target_link_libraries(esy_fs_flash PUBLIC esy_fs_littlefs PRIVATE esy_hal esy_opt_default)
    target_compile_features(esy_fs_flash PUBLIC c_std_11)
endif()
unset(_esy_littlefs_file)
