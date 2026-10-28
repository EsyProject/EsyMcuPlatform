# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

foreach(dependency esy_common_defs esy_network esy_opt_default esy_mqtt_provider)
    if(NOT TARGET ${dependency})
        message(FATAL_ERROR "esy_mqtt requires target ${dependency}; provide coreMQTT v2.3.1 through esy_mqtt_provider or cmake/esy_coremqtt_provider.cmake.")
    endif()
endforeach()

add_library(esy_mqtt STATIC src/esy_mqtt.c)
add_library(Esy::mqtt ALIAS esy_mqtt)
target_compile_features(esy_mqtt PUBLIC c_std_11)
target_include_directories(esy_mqtt PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(esy_mqtt PUBLIC esy_common_defs esy_network
    PRIVATE esy_mqtt_provider esy_opt_default)
