# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

include_guard(GLOBAL)

# Assemble pre-generated parser sources without fetching or invoking Node.js.
function(esy_configure_llhttp_provider)
    if(TARGET esy_http_parser_provider)
        return()
    endif()

    if(NOT DEFINED ESY_LLHTTP_PATH AND DEFINED ENV{ESY_LLHTTP_PATH})
        set(ESY_LLHTTP_PATH "$ENV{ESY_LLHTTP_PATH}")
    endif()
    foreach(esy_http_file IN ITEMS
            src/api.c src/http.c src/llhttp.c include/llhttp.h LICENSE-MIT)
        if(NOT ESY_LLHTTP_PATH OR NOT EXISTS "${ESY_LLHTTP_PATH}/${esy_http_file}")
            message(FATAL_ERROR
                "HTTP client requires an existing esy_http_parser_provider target or ESY_LLHTTP_PATH containing the pre-generated llhttp release/v9.4.3 checkout (missing ${esy_http_file}).")
        endif()
    endforeach()

    foreach(esy_http_component IN ITEMS MAJOR MINOR PATCH)
        file(STRINGS "${ESY_LLHTTP_PATH}/include/llhttp.h"
            esy_http_version_${esy_http_component}
            REGEX "^#define[ \t]+LLHTTP_VERSION_${esy_http_component}[ \t]+")
    endforeach()
    if(NOT esy_http_version_MAJOR MATCHES "^#define[ \t]+LLHTTP_VERSION_MAJOR[ \t]+9[ \t]*$" OR
       NOT esy_http_version_MINOR MATCHES "^#define[ \t]+LLHTTP_VERSION_MINOR[ \t]+4[ \t]*$" OR
       NOT esy_http_version_PATCH MATCHES "^#define[ \t]+LLHTTP_VERSION_PATCH[ \t]+3[ \t]*$")
        message(FATAL_ERROR "ESY_LLHTTP_PATH must provide pre-generated llhttp v9.4.3 sources.")
    endif()

    add_library(esy_http_parser_provider STATIC
        "${ESY_LLHTTP_PATH}/src/api.c"
        "${ESY_LLHTTP_PATH}/src/http.c"
        "${ESY_LLHTTP_PATH}/src/llhttp.c")
    target_compile_features(esy_http_parser_provider PUBLIC c_std_11)
    target_include_directories(esy_http_parser_provider SYSTEM PUBLIC
        "${ESY_LLHTTP_PATH}/include")
    target_compile_options(esy_http_parser_provider PRIVATE ${ESY_PLATFORM_COMPILE_OPTIONS})
endfunction()
