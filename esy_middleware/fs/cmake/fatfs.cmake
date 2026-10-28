# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

# Supply an existing official R0.16 source directory; configuration never downloads.
if(NOT ESY_FATFS_PATH OR NOT IS_DIRECTORY "${ESY_FATFS_PATH}")
    message(FATAL_ERROR "ENABLE_ESY_FS_FATFS requires ESY_FATFS_PATH containing official FatFs R0.16 source files")
endif()
set(esy_fatfs_source_names ff.c ff.h diskio.h ffunicode.c)
set(esy_fatfs_source_hashes
    dbeae3d7d22dbc070983e31af897fa5e43d29c726a0a240fe9589ffe2ec0ef1e
    42920ae4d6080bfc2a72d92a9969b5e94eeaff070e5e71d90da9ab6d7d8e158e
    1c2b29f53e061c55d43b2e412c3a61320b2621bac7561a05e275514baa44785a
    3f1632c1f1e64b0b683b0d7e51341b9870728f398604869dd8211f1819509a8e)
# ff.h includes ffconf.h relative to itself, so isolate the provider copy.
set(esy_fatfs_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/fatfs-r016")
file(MAKE_DIRECTORY "${esy_fatfs_generated_dir}")
foreach(esy_fatfs_index RANGE 0 3)
    list(GET esy_fatfs_source_names ${esy_fatfs_index} esy_fatfs_name)
    list(GET esy_fatfs_source_hashes ${esy_fatfs_index} esy_fatfs_expected_hash)
    if(NOT EXISTS "${ESY_FATFS_PATH}/${esy_fatfs_name}")
        message(FATAL_ERROR "ESY_FATFS_PATH is missing ${esy_fatfs_name}")
    endif()
    file(SHA256 "${ESY_FATFS_PATH}/${esy_fatfs_name}" esy_fatfs_actual_hash)
    if(NOT esy_fatfs_actual_hash STREQUAL esy_fatfs_expected_hash)
        message(FATAL_ERROR "ESY_FATFS_PATH/${esy_fatfs_name} must match official FatFs R0.16")
    endif()
    configure_file("${ESY_FATFS_PATH}/${esy_fatfs_name}"
        "${esy_fatfs_generated_dir}/${esy_fatfs_name}" COPYONLY)
endforeach()
configure_file("${CMAKE_CURRENT_LIST_DIR}/../config/fatfs/ffconf.h"
    "${esy_fatfs_generated_dir}/ffconf.h" COPYONLY)
add_library(esy_fs_fatfs_provider STATIC
    "${esy_fatfs_generated_dir}/ff.c" "${esy_fatfs_generated_dir}/ffunicode.c")
target_include_directories(esy_fs_fatfs_provider PUBLIC "${esy_fatfs_generated_dir}")
target_compile_features(esy_fs_fatfs_provider PUBLIC c_std_11)
target_link_libraries(esy_fs_fatfs_provider PRIVATE esy_opt_default)
add_library(esy_fs_fatfs STATIC "${CMAKE_CURRENT_LIST_DIR}/../src/backends/esy_fs_fatfs.c")
target_link_libraries(esy_fs_fatfs PUBLIC esy_fs esy_fs_fatfs_provider PRIVATE esy_opt_default)
