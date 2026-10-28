# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Build MQTT C11/C++17 consumers and verify explicit provider assembly."""

import json
import os
import runpy
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
cached_provider = runpy.run_path(
    ROOT / "esy_test/utils/esy_provider_paths.py")["cached_provider"]
provider_input = cached_provider("coremqtt")
if not provider_input:
    raise RuntimeError("Run ./setup.sh or set ESY_COREMQTT_PATH for provider tests")
PROVIDER = Path(provider_input).resolve()

PROJECT = r"""
cmake_minimum_required(VERSION 3.20)
project(mqtt_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(esy_feature_import.cmake)
add_library(esy_opt_default INTERFACE)
target_compile_definitions(esy_opt_default INTERFACE PRIVATE_OPTIONS_SENTINEL)
add_subdirectory("${PLATFORM_ROOT}/esy_common/defs" defs)
if(ENABLE_ESY_NETWORK_LWIP)
  add_library(esy_common_ds INTERFACE)
endif()
if(ENABLE_ESY_MQTT AND NOT MISSING_PROVIDER)
  include("${PLATFORM_ROOT}/cmake/esy_coremqtt_provider.cmake")
  if(USE_CONFIG_TARGET)
    add_library(custom_mqtt_config INTERFACE)
    target_include_directories(custom_mqtt_config INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
    set(ESY_COREMQTT_CONFIG_TARGET custom_mqtt_config)
  endif()
  esy_configure_coremqtt_provider()
  esy_configure_coremqtt_provider()
endif()
if(DIRECT_MODULE)
  if(ENABLE_ESY_NETWORK)
    add_subdirectory("${PLATFORM_ROOT}/esy_middleware/network" network)
  endif()
  if(ENABLE_ESY_MQTT)
    add_subdirectory("${PLATFORM_ROOT}/esy_middleware/mqtt" mqtt)
  endif()
else()
  add_subdirectory("${PLATFORM_ROOT}/esy_middleware" middleware)
endif()
if(ENABLE_ESY_MQTT)
  foreach(lang c cpp)
    add_executable(consumer_${lang} consumer.${lang})
    target_link_libraries(consumer_${lang} PRIVATE Esy::mqtt)
  endforeach()
  if(NOT DIRECT_MODULE)
    add_executable(consumer_middleware consumer.c)
    target_link_libraries(consumer_middleware PRIVATE esy_middleware)
  endif()
  foreach(unwanted esy_os esy_hal esy_hal_backend esy_board esy_network_lwip esy_common_ds)
    if(NOT ENABLE_ESY_NETWORK_LWIP AND TARGET ${unwanted})
      message(FATAL_ERROR "Core-only MQTT assembled unrelated target ${unwanted}")
    endif()
  endforeach()
elseif(TARGET esy_mqtt OR TARGET esy_mqtt_provider OR TARGET esy_network_lwip)
  message(FATAL_ERROR "Disabled MQTT assembled a target or provider")
endif()
"""

CONSUMER = r"""
#include "esy_middleware/mqtt/esy_mqtt.h"
#if defined(ESY_NETWORK_CONNECTION_H) || defined(ESY_NETWORK_LWIP_H)
#error "Connection extensions or endpoint provider leaked into the MQTT API"
#endif
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private optimization options leaked into the consumer"
#endif
#ifdef MQTT_DO_NOT_USE_CUSTOM_CONFIG
#error "Private provider configuration leaked into the consumer"
#endif
typedef struct {
    int32_t (*send)(void *, const void *, size_t);
    int32_t (*recv)(void *, void *, size_t);
    esy_err_t (*progress)(void *);
    void *context;
} expected_basic_stream_t;
#ifdef __cplusplus
static_assert(sizeof(esy_network_stream_t) == sizeof(expected_basic_stream_t),
              "MQTT requires only the four-field basic stream contract");
#else
_Static_assert(sizeof(esy_network_stream_t) == sizeof(expected_basic_stream_t),
               "MQTT requires only the four-field basic stream contract");
#endif
int main(void)
{
    esy_mqtt_t client = {0};
    esy_mqtt_config_t config = {0};
    esy_mqtt_connect_config_t connection = {0};
    esy_mqtt_message_t message = {0};
    esy_mqtt_string_t filter = {0};
    uint16_t packet_id = 0;
    if (esy_mqtt_is_connected(&client)) return 1;
    if (esy_mqtt_init(&client, &config) != ESY_ERR_INVALID_ARG) return 2;
    if (esy_mqtt_connect(&client, &connection) == ESY_OK) return 3;
    if (esy_mqtt_publish(&client, &message, &packet_id) == ESY_OK) return 4;
    if (esy_mqtt_subscribe(&client, filter, ESY_MQTT_QOS_0, &packet_id) == ESY_OK) return 5;
    if (esy_mqtt_unsubscribe(&client, filter, &packet_id) == ESY_OK) return 6;
    if (esy_mqtt_process(&client) != ESY_ERR_STATE) return 7;
    if (esy_mqtt_disconnect(&client) != ESY_ERR_STATE) return 8;
    if (esy_mqtt_abort(&client) != ESY_ERR_STATE) return 9;
    if (esy_mqtt_deinit(&client) != ESY_ERR_STATE) return 10;
    return 0;
}
"""


class MqttConsumerTests(unittest.TestCase):
    def configure(self, project, *, enabled=True, child=False, direct=False,
                  missing=False, provider=PROVIDER, custom=False):
        (project / "CMakeLists.txt").write_text(PROJECT)
        (project / "esy_feature_import.cmake").write_text(
            f"set(ENABLE_ESY_MQTT {'ON' if enabled else 'OFF'})\n"
            f"set(ENABLE_ESY_NETWORK {'ON' if enabled else 'OFF'})\n"
            f"set(ENABLE_ESY_NETWORK_LWIP {'ON' if child else 'OFF'})\n")
        (project / "consumer.c").write_text(CONSUMER)
        (project / "consumer.cpp").write_text(CONSUMER)
        (project / "core_mqtt_config.h").write_text(
            "#define MQTT_SEND_TIMEOUT_MS 20U\n"
            "#define MQTT_RECV_POLLING_TIMEOUT_MS 10U\n")
        environment_variables = dict(
            os.environ, ESY_PROVIDER_CACHE_DIR=str(project / "empty provider cache"))
        return subprocess.run(
            ["cmake", "-S", str(project), "-B", str(project / "out"),
             f"-DPLATFORM_ROOT={ROOT}", f"-DESY_COREMQTT_PATH={provider}",
             f"-DDIRECT_MODULE={'ON' if direct else 'OFF'}",
             f"-DMISSING_PROVIDER={'ON' if missing else 'OFF'}",
             f"-DUSE_CONFIG_TARGET={'ON' if custom else 'OFF'}"],
            capture_output=True, text=True, env=environment_variables)

    def assert_successful_consumer(self, *, direct=False, custom=False):
        with tempfile.TemporaryDirectory(prefix="mqtt consumer ") as directory:
            project = Path(directory)
            result = self.configure(project, direct=direct, custom=custom)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            result = subprocess.run(["cmake", "--build", str(project / "out")],
                                    capture_output=True, text=True)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            names = ["consumer_c", "consumer_cpp"]
            if not direct:
                names.append("consumer_middleware")
            for name in names:
                result = subprocess.run([str(project / "out" / name)],
                                        capture_output=True, text=True)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            commands = json.loads((project / "out/compile_commands.json").read_text())
            consumers = [entry for entry in commands
                         if Path(entry["file"]).name in ("consumer.c", "consumer.cpp")]
            self.assertEqual(len(names), len(consumers))
            for entry in consumers:
                command = entry.get("command", " ".join(entry.get("arguments", [])))
                for private in (str(PROVIDER), "PRIVATE_OPTIONS_SENTINEL", "MQTT_DO_NOT_USE_CUSTOM_CONFIG",
                                "/esy_hal/", "/esy_middleware/os/", "/mqtt/src"):
                    self.assertNotIn(private, command)

    def test_aggregate_target_links_real_provider_without_os(self):
        self.assert_successful_consumer()

    def test_direct_module_and_idempotent_provider_helper(self):
        self.assert_successful_consumer(direct=True)

    def test_explicit_configuration_target(self):
        self.assert_successful_consumer(custom=True)

    def test_disabled_feature_ignores_missing_provider_and_enabled_child(self):
        for child in (False, True):
            with self.subTest(child=child), tempfile.TemporaryDirectory() as directory:
                result = self.configure(Path(directory), enabled=False, child=child,
                                        provider=Path(directory) / "absent")
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_enabled_feature_without_provider_target_fails_clearly(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.configure(Path(directory), missing=True)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("esy_mqtt_provider", result.stdout + result.stderr)

    def test_helper_rejects_missing_provider_checkout(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.configure(Path(directory), provider=Path(directory) / "absent")
            self.assertNotEqual(0, result.returncode)
            self.assertIn("ESY_COREMQTT_PATH", result.stdout + result.stderr)

    def test_helper_rejects_wrong_provider_version(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            provider = project / "wrong-provider"
            shutil.copytree(PROVIDER / "source", provider / "source")
            header = provider / "source/include/core_mqtt.h"
            header.write_text(header.read_text().replace('"v2.3.1"', '"v2.3.0"'))
            result = self.configure(project, provider=provider)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("v2.3.1", result.stdout + result.stderr)

    def test_lwip_adapter_requires_explicit_transport_provider(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.configure(Path(directory), child=True)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("esy_network_lwip_provider", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
