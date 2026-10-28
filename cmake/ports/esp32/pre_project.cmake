# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-FileCopyrightText: 2026 EsyCloud
#
# SPDX-License-Identifier: Apache-2.0

# The conventional scaffold supplies a component named after its directory.
# Custom component selections and native ESP-IDF discovery remain available.
if(ESY_PROJECT_ROOT AND EXISTS "${ESY_PROJECT_ROOT}/app/esp32/CMakeLists.txt")
    if(NOT DEFINED EXTRA_COMPONENT_DIRS)
        set(EXTRA_COMPONENT_DIRS "${ESY_PROJECT_ROOT}/app/esp32")
    endif()
    if(NOT DEFINED COMPONENTS)
        set(COMPONENTS esp32)
    endif()
endif()

if(COMMAND idf_build_process)
    return()
endif()
if(NOT IDF_PATH)
    if(DEFINED ENV{IDF_PATH} AND NOT "$ENV{IDF_PATH}" STREQUAL "")
        set(IDF_PATH "$ENV{IDF_PATH}")
    elseif(ESY_PLATFORM_SDK_ROOT)
        set(IDF_PATH "${ESY_PLATFORM_SDK_ROOT}")
    elseif(DEFINED ENV{ESY_PLATFORM_SDK_ROOT})
        set(IDF_PATH "$ENV{ESY_PLATFORM_SDK_ROOT}")
    endif()
endif()
if(NOT IDF_PATH OR NOT EXISTS "${IDF_PATH}/tools/cmake/project.cmake")
    message(FATAL_ERROR
        "ESP32 integration requires a local ESP-IDF checkout. Set IDF_PATH or ESY_PLATFORM_SDK_ROOT.")
endif()
include("${IDF_PATH}/tools/cmake/project.cmake")
