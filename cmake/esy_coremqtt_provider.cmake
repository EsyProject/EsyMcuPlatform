# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

include_guard(GLOBAL)

# Assemble only the protocol library; transport and scheduling remain consumer-owned.
function(esy_configure_coremqtt_provider)
    if(TARGET esy_mqtt_provider)
        return()
    endif()

    if(NOT DEFINED ESY_COREMQTT_PATH AND DEFINED ENV{ESY_COREMQTT_PATH})
        set(ESY_COREMQTT_PATH "$ENV{ESY_COREMQTT_PATH}")
    endif()
    foreach(esy_mqtt_file IN ITEMS
            source/core_mqtt.c
            source/core_mqtt_state.c
            source/core_mqtt_serializer.c
            source/include/core_mqtt.h
            source/include/core_mqtt_config_defaults.h
            source/interface/transport_interface.h)
        if(NOT ESY_COREMQTT_PATH OR NOT EXISTS "${ESY_COREMQTT_PATH}/${esy_mqtt_file}")
            message(FATAL_ERROR
                "MQTT requires an existing esy_mqtt_provider target or ESY_COREMQTT_PATH containing a coreMQTT v2.3.1 checkout (missing ${esy_mqtt_file}).")
        endif()
    endforeach()

    file(STRINGS "${ESY_COREMQTT_PATH}/source/include/core_mqtt.h"
        esy_mqtt_version_line REGEX "^#define[ \t]+MQTT_LIBRARY_VERSION[ \t]+")
    if(NOT esy_mqtt_version_line MATCHES
            "^#define[ \t]+MQTT_LIBRARY_VERSION[ \t]+\"v2\\.3\\.1\"[ \t]*$")
        message(FATAL_ERROR
            "ESY_COREMQTT_PATH must provide coreMQTT v2.3.1; found '${esy_mqtt_version_line}'.")
    endif()
    if(ESY_COREMQTT_CONFIG_TARGET AND NOT TARGET ${ESY_COREMQTT_CONFIG_TARGET})
        message(FATAL_ERROR "ESY_COREMQTT_CONFIG_TARGET must name an existing configuration target.")
    endif()

    add_library(esy_mqtt_provider STATIC
        "${ESY_COREMQTT_PATH}/source/core_mqtt.c"
        "${ESY_COREMQTT_PATH}/source/core_mqtt_state.c"
        "${ESY_COREMQTT_PATH}/source/core_mqtt_serializer.c")
    target_compile_features(esy_mqtt_provider PUBLIC c_std_11)
    target_include_directories(esy_mqtt_provider SYSTEM PUBLIC
        "${ESY_COREMQTT_PATH}/source/include"
        "${ESY_COREMQTT_PATH}/source/interface")
    target_compile_options(esy_mqtt_provider PRIVATE ${ESY_PLATFORM_COMPILE_OPTIONS})
    if(ESY_COREMQTT_CONFIG_TARGET)
        target_link_libraries(esy_mqtt_provider PUBLIC ${ESY_COREMQTT_CONFIG_TARGET})
    else()
        target_compile_definitions(esy_mqtt_provider PUBLIC MQTT_DO_NOT_USE_CUSTOM_CONFIG=1)
    endif()
endfunction()
