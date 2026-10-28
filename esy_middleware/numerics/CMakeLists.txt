# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

# Numerical limits are consumer inputs, not independent feature switches.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/include/esy_middleware/numerics/esy_numerics_config.h")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/include/esy_middleware/numerics/esy_numerics_config.h"
    NUMERICS_DEFAULTS REGEX "^#define ESY_NUMERICS_BUILD_[A-Z_]+ [0-9]+$")
foreach(DEFAULT IN LISTS NUMERICS_DEFAULTS)
    string(REGEX REPLACE "^#define ESY_NUMERICS_BUILD_([A-Z_]+) ([0-9]+)$" "\\1;\\2" PAIR "${DEFAULT}")
    list(GET PAIR 0 SUFFIX)
    list(GET PAIR 1 VALUE)
    if(NOT DEFINED ESY_${SUFFIX})
        set(ESY_${SUFFIX} "${VALUE}")
    endif()
    if(NOT "${ESY_${SUFFIX}}" MATCHES "^(0|[1-9][0-9]*)$")
        message(FATAL_ERROR "ESY_${SUFFIX} must be a nonnegative decimal integer")
    endif()
endforeach()
if(ESY_FIT_MAX_M LESS 1 OR ESY_LINALG_MAX_M LESS ESY_FIT_MAX_M OR
   ESY_LINALG_MAX_M GREATER 46340 OR ESY_FIT_MAX_N LESS 1 OR
   ESY_FIT_MAX_N GREATER 2147483647 OR ESY_FIT_MAX_DEG GREATER_EQUAL ESY_FIT_MAX_M)
    message(FATAL_ERROR "Inconsistent esy_numerics numerical limits")
endif()

set(NUMERICS_GENERATED_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
configure_file(src/esy_numerics_build_config.h.in
    "${NUMERICS_GENERATED_INCLUDE}/esy_middleware/numerics/esy_numerics_build_config.h" @ONLY)

add_library(esy_numerics STATIC)
target_include_directories(esy_numerics PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/include" "${NUMERICS_GENERATED_INCLUDE}")
target_compile_definitions(esy_numerics PUBLIC ESY_NUMERICS_CONFIGURED_BUILD=1)
target_link_libraries(esy_numerics PUBLIC esy_common_defs PRIVATE esy_opt_speed)
# MSVC provides the math functions in its C runtime; embedded GCC and Unix use m.
if(NOT MSVC)
    target_link_libraries(esy_numerics PRIVATE m)
endif()
add_subdirectory(src)
