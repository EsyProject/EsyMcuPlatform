#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Verify independent JSON TCP consumers, feature dependencies and isolation."""

import json
import os
import runpy
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
cached_provider = runpy.run_path(
    ROOT / "esy_test/utils/esy_provider_paths.py")["cached_provider"]
provider_input = cached_provider("corejson")
if not provider_input:
    raise RuntimeError("Run ./setup.sh or set ESY_COREJSON_PATH for provider tests")
PROVIDER = Path(provider_input).resolve()

PROJECT = r"""
cmake_minimum_required(VERSION 3.20)
project(json_tcp_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(esy_feature_import.cmake)
if(PROVIDE_OPTIONS)
  add_library(esy_opt_default INTERFACE)
  target_compile_options(esy_opt_default INTERFACE -Wall -Wextra -Werror)
  target_compile_definitions(esy_opt_default INTERFACE PRIVATE_OPTIONS_SENTINEL)
endif()
if(ENABLE_ESY_COMMON_PROTOCOL_JSON)
  include("${PLATFORM_ROOT}/cmake/esy_corejson_provider.cmake")
  esy_configure_corejson_provider()
endif()
if(PROVIDE_CODEC)
  add_subdirectory("${PLATFORM_ROOT}/esy_common" common)
else()
  add_subdirectory("${PLATFORM_ROOT}/esy_common/defs" defs)
  add_subdirectory("${PLATFORM_ROOT}/esy_common/utils" utils)
endif()
if(DIRECT_MODULE)
  if(ENABLE_ESY_NETWORK AND PROVIDE_NETWORK)
    add_subdirectory("${PLATFORM_ROOT}/esy_middleware/network" network)
  endif()
  add_subdirectory("${PLATFORM_ROOT}/esy_middleware/protocol" protocol)
else()
  add_subdirectory("${PLATFORM_ROOT}/esy_middleware" middleware)
endif()
foreach(unwanted esy_hal esy_hal_backend esy_board esy_os esy_common_ds
        esy_proto_bin_session esy_proto_bin_contract esy_proto_modbus_manager
        esy_network_lwip esy_tls esy_usb esy_proto_json_usb_channel)
  if(TARGET ${unwanted})
    message(FATAL_ERROR "JSON-only consumer assembled unrelated target ${unwanted}")
  endif()
endforeach()
if(ENABLE_ESY_PROTO_JSON_TCP_CHANNEL)
  if(NOT TARGET esy_proto_json_tcp_channel OR NOT TARGET Esy::proto_json_tcp_channel)
    message(FATAL_ERROR "JSON TCP channel target or alias is missing")
  endif()
  get_target_property(channel_links esy_proto_json_tcp_channel INTERFACE_LINK_LIBRARIES)
  if(channel_links MATCHES "esy_hal|esy_os|esy_common_ds|esy_proto_bin|esy_proto_modbus")
    message(FATAL_ERROR "JSON TCP acquired another family or hardware/OS dependency")
  endif()
  foreach(language c cpp)
    add_executable(consumer_${language} consumer.${language})
    target_link_libraries(consumer_${language} PRIVATE Esy::proto_json_tcp_channel)
    target_compile_options(consumer_${language} PRIVATE -Wall -Wextra -Werror)
  endforeach()
  if(NOT DIRECT_MODULE)
    add_executable(consumer_middleware consumer.c)
    target_link_libraries(consumer_middleware PRIVATE esy_middleware)
  endif()
elseif(TARGET esy_proto_json_tcp_channel)
  message(FATAL_ERROR "Disabled JSON TCP channel assembled a target")
endif()
"""

CONSUMER = r"""
#include <string.h>
#include "esy_middleware/protocol/esy_proto_json_tcp_channel.h"
#ifdef CORE_JSON_H_
#error "Private coreJSON types leaked into the channel public header"
#endif
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private compiler options leaked into the channel consumer"
#endif

typedef struct {
    uint8_t input[64];
    uint8_t output[64];
    size_t input_length;
    size_t input_offset;
    size_t output_length;
    unsigned received;
    int error;
} consumer_state_t;

static int32_t prefix_send(void *context, const void *data, size_t size)
{
    consumer_state_t *state = (consumer_state_t *)context;
    size_t accepted = size > 3u ? 3u : size;
    if (accepted > sizeof(state->output) - state->output_length) return ESY_ERR_NO_MEM;
    memcpy(state->output + state->output_length, data, accepted);
    state->output_length += accepted;
    return (int32_t)accepted;
}

static int32_t fragment_recv(void *context, void *data, size_t size)
{
    consumer_state_t *state = (consumer_state_t *)context;
    size_t available = state->input_length - state->input_offset;
    size_t count = size < available ? size : available;
    if (count > 2u) count = 2u;
    memcpy(data, state->input + state->input_offset, count);
    state->input_offset += count;
    return (int32_t)count;
}

static esy_err_t connection_status(void *context, esy_network_connection_status_t *status)
{
    consumer_state_t *state = (consumer_state_t *)context;
    memset(status, 0, sizeof(*status));
    status->state = ESY_NETWORK_CONNECTION_OPEN;
    status->session_id = 7u;
    status->send_capacity = 64u;
    status->rx_available = (uint32_t)(state->input_length - state->input_offset);
    return ESY_OK;
}

static void receive_message(esy_proto_json_tcp_channel_t *channel,
                            const esy_json_value_t *root, void *context)
{
    consumer_state_t *state = (consumer_state_t *)context;
    esy_json_value_t value;
    uint64_t integer;
    esy_proto_json_tcp_channel_status_t status;
    state->received++;
    if (esy_json_object_get(root, "value", 5, &value) != ESY_OK ||
        esy_json_as_u64(&value, &integer) != ESY_OK || integer != UINT64_MAX) {
        state->error = 1;
    }
    if (esy_proto_json_tcp_channel_get_status(channel, &status) != ESY_OK) state->error = 2;
    if (esy_proto_json_tcp_channel_send(channel, root->data, root->length) != ESY_OK) state->error = 3;
}

int main(void)
{
    static const char k_document[] = "{\"value\":18446744073709551615}";
    esy_proto_json_tcp_channel_t channel;
    esy_proto_json_tcp_channel_config_t config;
    esy_proto_json_tcp_channel_status_t status;
    esy_proto_json_tcp_channel_static_resources_t resources;
    consumer_state_t provider;
    uint8_t receive_buffer[64];
    uint8_t transmit_buffer[68];
    unsigned iteration;
    memset(&channel, 0, sizeof(channel));
    memset(&config, 0, sizeof(config));
    memset(&status, 0, sizeof(status));
    memset(&provider, 0, sizeof(provider));
    if (esy_proto_json_tcp_channel_init(NULL, NULL) != ESY_ERR_INVALID_ARG) return 1;
    if (esy_proto_json_tcp_channel_init(&channel, &config) != ESY_ERR_INVALID_ARG) return 2;
    if (esy_proto_json_tcp_channel_get_status(&channel, &status) != ESY_ERR_STATE) return 3;
    if (esy_proto_json_tcp_channel_deinit(&channel) != ESY_OK) return 4;
    if (esy_proto_json_tcp_channel_poll(&channel) != ESY_ERR_STATE) return 5;
    if (esy_proto_json_tcp_channel_send(&channel, "{}", 2) != ESY_ERR_STATE) return 6;
    provider.input[3] = (uint8_t)(sizeof(k_document) - 1u);
    memcpy(provider.input + ESY_PROTO_JSON_TCP_HEADER_BYTES,
           k_document, sizeof(k_document) - 1u);
    provider.input_length = ESY_PROTO_JSON_TCP_HEADER_BYTES + sizeof(k_document) - 1u;
    resources.rx_buffer = receive_buffer;
    resources.rx_buffer_size = sizeof(receive_buffer);
    resources.tx_buffer = transmit_buffer;
    resources.tx_buffer_size = sizeof(transmit_buffer);
    config.connection.stream.context = &provider;
    config.connection.stream.send = prefix_send;
    config.connection.stream.recv = fragment_recv;
    config.connection.get_status = connection_status;
    config.static_resources = &resources;
    config.on_message = receive_message;
    config.user_ctx = &provider;
    config.rx_byte_budget = 7u;
    config.tx_byte_budget = 5u;
    if (esy_proto_json_tcp_channel_init(&channel, &config) != ESY_OK) return 7;
    for (iteration = 0; iteration < 100u; ++iteration) {
        if (esy_proto_json_tcp_channel_poll(&channel) != ESY_OK) return 8;
        if (esy_proto_json_tcp_channel_get_status(&channel, &status) != ESY_OK) return 9;
        if (provider.received == 1u && !status.tx_pending) break;
    }
    if (iteration == 100u || provider.error || provider.received != 1u) return 10;
    if (provider.output_length != provider.input_length ||
        memcmp(provider.output, provider.input, provider.input_length) != 0) return 11;
    if (esy_proto_json_tcp_channel_deinit(&channel) != ESY_OK) return 12;
    return 0;
}
"""


class JsonTcpConsumerTests(unittest.TestCase):
    def configure(self, project, *, enabled=True, parent=True, codec=True,
                  network=True, direct=False, provide_codec=True,
                  provide_network=True, options=True, provider=PROVIDER):
        (project / "CMakeLists.txt").write_text(PROJECT, encoding="utf-8")
        (project / "esy_feature_import.cmake").write_text(
            "set(ESY_RTOS None)\n"
            f"set(ENABLE_ESY_COMMON_PROTOCOL {'ON' if parent else 'OFF'})\n"
            f"set(ENABLE_ESY_COMMON_PROTOCOL_JSON {'ON' if codec else 'OFF'})\n"
            f"set(ENABLE_ESY_NETWORK {'ON' if network else 'OFF'})\n"
            f"set(ENABLE_ESY_PROTO_JSON_TCP_CHANNEL {'ON' if enabled else 'OFF'})\n",
            encoding="utf-8")
        for language in ("c", "cpp"):
            (project / f"consumer.{language}").write_text(CONSUMER, encoding="utf-8")
        environment_variables = dict(
            os.environ, ESY_PROVIDER_CACHE_DIR=str(project / "empty provider cache"))
        return subprocess.run(
            ["cmake", "-S", str(project), "-B", str(project / "out"),
             f"-DPLATFORM_ROOT={ROOT}", f"-DESY_COREJSON_PATH={provider}",
             f"-DDIRECT_MODULE={'ON' if direct else 'OFF'}",
             f"-DPROVIDE_CODEC={'ON' if provide_codec else 'OFF'}",
             f"-DPROVIDE_NETWORK={'ON' if provide_network else 'OFF'}",
             f"-DPROVIDE_OPTIONS={'ON' if options else 'OFF'}",
             # Stale cache input cannot override the ordinary feature-file set.
             "-DENABLE_ESY_PROTO_JSON_TCP_CHANNEL=OFF"],
            capture_output=True, text=True, check=False, env=environment_variables)

    def build(self, project, *, enabled=True):
        result = subprocess.run(["cmake", "--build", str(project / "out"), "-j2"],
                                capture_output=True, text=True, check=False)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        if enabled:
            for consumer in (project / "out").glob("consumer_*"):
                if consumer.is_file():
                    result = subprocess.run([str(consumer)], capture_output=True,
                                            text=True, check=False)
                    self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_public_c_and_cpp_direct_and_aggregate_consumers(self):
        for direct in (False, True):
            with self.subTest(direct=direct), tempfile.TemporaryDirectory(
                    prefix="esy-json-tcp-consumer-") as directory:
                project = Path(directory)
                result = self.configure(project, direct=direct)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                self.build(project)

    def test_channel_does_not_require_project_option_target(self):
        with tempfile.TemporaryDirectory(prefix="esy-json-tcp-options-") as directory:
            project = Path(directory)
            result = self.configure(project, direct=True, options=False)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.build(project)

    def test_channel_requires_each_explicit_feature(self):
        for feature, arguments in (
                ("ENABLE_ESY_COMMON_PROTOCOL", {"parent": False, "codec": False}),
                ("ENABLE_ESY_COMMON_PROTOCOL_JSON", {"codec": False}),
                ("ENABLE_ESY_NETWORK", {"network": False})):
            for direct in (False, True):
                with self.subTest(feature=feature, direct=direct), \
                        tempfile.TemporaryDirectory(prefix="esy-json-tcp-feature-") as directory:
                    result = self.configure(Path(directory), direct=direct, **arguments)
                    self.assertNotEqual(0, result.returncode)
                    self.assertIn(f"requires {feature}", result.stdout + result.stderr)

    def test_direct_channel_requires_assembled_targets(self):
        for target, arguments in (
                ("esy_common_protocol_json", {"provide_codec": False}),
                ("esy_network", {"provide_network": False})):
            with self.subTest(target=target), tempfile.TemporaryDirectory(
                    prefix="esy-json-tcp-target-") as directory:
                result = self.configure(Path(directory), direct=True, **arguments)
                self.assertNotEqual(0, result.returncode)
                self.assertIn(f"requires target {target}", result.stdout + result.stderr)

    def test_disabled_channel_leaves_codec_independent(self):
        for direct in (False, True):
            with self.subTest(direct=direct), tempfile.TemporaryDirectory(
                    prefix="esy-json-tcp-disabled-") as directory:
                project = Path(directory)
                result = self.configure(project, enabled=False, direct=direct)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                self.build(project, enabled=False)
                commands = json.loads((project / "out/compile_commands.json").read_text())
                self.assertFalse(any("esy_proto_json_tcp_channel.c" in item["file"]
                                     for item in commands))
                self.assertTrue(any("core_json.c" in item["file"] for item in commands))

    def test_all_json_disabled_ignores_missing_provider(self):
        with tempfile.TemporaryDirectory(prefix="esy-json-tcp-all-off-") as directory:
            project = Path(directory)
            result = self.configure(project, enabled=False, parent=False, codec=False,
                                    network=False, provider=project / "absent")
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.build(project, enabled=False)
            commands = json.loads((project / "out/compile_commands.json").read_text())
            self.assertFalse(any("esy_proto_json_" in item["file"] or
                                 "core_json.c" in item["file"] for item in commands))

    def test_reconfiguration_removes_channel_and_provider_sources(self):
        with tempfile.TemporaryDirectory(prefix="esy-json-tcp-reconfigure-") as directory:
            project = Path(directory)
            result = self.configure(project)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.build(project)
            result = self.configure(project, enabled=False, codec=False,
                                    provider=project / "absent")
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.build(project, enabled=False)
            commands = json.loads((project / "out/compile_commands.json").read_text())
            self.assertFalse(any("esy_proto_json_" in item["file"] or
                                 "core_json.c" in item["file"] for item in commands))


if __name__ == "__main__":
    unittest.main()
