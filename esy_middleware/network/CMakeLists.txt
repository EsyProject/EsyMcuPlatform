# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

add_library(esy_network INTERFACE)
add_library(Esy::network ALIAS esy_network)
target_compile_features(esy_network INTERFACE c_std_11)
target_include_directories(esy_network INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(esy_network INTERFACE esy_common_defs)

if(ENABLE_ESY_NETWORK_LWIP)
    foreach(dependency esy_common_ds esy_network_lwip_provider esy_opt_default)
        if(NOT TARGET ${dependency})
            message(FATAL_ERROR "esy_network_lwip requires target ${dependency}; provide lwIP raw API and network configuration explicitly.")
        endif()
    endforeach()
    add_library(esy_network_lwip STATIC src/esy_network_lwip.c)
    target_link_libraries(esy_network_lwip PUBLIC esy_network
        PRIVATE esy_common_ds esy_network_lwip_provider esy_opt_default)
endif()
