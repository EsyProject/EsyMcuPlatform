# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

include_guard(GLOBAL)

# Assemble the parser from an existing checkout; no package download is performed.
# Existing provider owners preserve scalar roots and declare their nesting limit.
function(esy_configure_corejson_provider)
    if(TARGET esy_json_parser_provider)
        return()
    endif()

    if(NOT DEFINED ESY_COREJSON_PATH AND DEFINED ENV{ESY_COREJSON_PATH})
        set(ESY_COREJSON_PATH "$ENV{ESY_COREJSON_PATH}")
    endif()
    foreach(esy_json_file IN ITEMS source/core_json.c source/include/core_json.h LICENSE)
        if(NOT ESY_COREJSON_PATH OR NOT EXISTS "${ESY_COREJSON_PATH}/${esy_json_file}")
            message(FATAL_ERROR
                "JSON requires an existing esy_json_parser_provider target or ESY_COREJSON_PATH containing a coreJSON v3.3.1 checkout (missing ${esy_json_file}).")
        endif()
    endforeach()

    file(STRINGS "${ESY_COREJSON_PATH}/source/include/core_json.h"
        esy_json_version_line REGEX "^#define[ \t]+JSON_LIBRARY_VERSION[ \t]+")
    if(NOT esy_json_version_line MATCHES
            "^#define[ \t]+JSON_LIBRARY_VERSION[ \t]+\"v3\\.3\\.1\"[ \t]*$")
        message(FATAL_ERROR
            "ESY_COREJSON_PATH must provide coreJSON v3.3.1; found '${esy_json_version_line}'.")
    endif()

    add_library(esy_json_parser_provider STATIC "${ESY_COREJSON_PATH}/source/core_json.c")
    target_compile_features(esy_json_parser_provider PUBLIC c_std_11)
    target_include_directories(esy_json_parser_provider SYSTEM PUBLIC
        "${ESY_COREJSON_PATH}/source/include")
    target_compile_options(esy_json_parser_provider PRIVATE ${ESY_PLATFORM_COMPILE_OPTIONS})
endfunction()
