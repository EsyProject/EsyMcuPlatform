#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Verify independently selected JSON USB channels over the real CDC adapter."""

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
project(json_usb_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(esy_feature_import.cmake)
add_library(esy_opt_default INTERFACE)
target_compile_options(esy_opt_default INTERFACE -Wall -Wextra -Werror)
target_compile_definitions(esy_opt_default INTERFACE PRIVATE_OPTIONS_SENTINEL)
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
if(ENABLE_ESY_USB AND ENABLE_ESY_USB_BACKEND_TINYUSB AND PROVIDE_TINYUSB)
  add_library(tinyusb_device STATIC
    "${PLATFORM_ROOT}/esy_test/fake/third_party/tinyusb/fake_tinyusb.c")
  target_include_directories(tinyusb_device PUBLIC
    "${PLATFORM_ROOT}/esy_test/fake/third_party/tinyusb")
endif()
if(DIRECT_MODULE)
  if(ENABLE_ESY_USB AND PROVIDE_USB)
    add_subdirectory("${PLATFORM_ROOT}/esy_middleware/usb" usb)
  endif()
  add_subdirectory("${PLATFORM_ROOT}/esy_middleware/protocol" protocol)
else()
  add_subdirectory("${PLATFORM_ROOT}/esy_middleware" middleware)
endif()
foreach(unwanted esy_hal esy_hal_backend esy_board esy_os esy_common_ds
        esy_proto_bin_session esy_proto_bin_contract esy_proto_modbus_manager
        esy_network esy_proto_json_tcp_channel esy_network_lwip esy_tls)
  if(TARGET ${unwanted})
    message(FATAL_ERROR "JSON USB-only consumer assembled unrelated target ${unwanted}")
  endif()
endforeach()
if(ENABLE_ESY_PROTO_JSON_USB_CHANNEL)
  if(NOT TARGET esy_proto_json_usb_channel OR NOT TARGET Esy::proto_json_usb_channel)
    message(FATAL_ERROR "JSON USB channel target or alias is missing")
  endif()
  get_target_property(channel_links esy_proto_json_usb_channel INTERFACE_LINK_LIBRARIES)
  if(channel_links MATCHES "esy_hal|esy_os|esy_common_ds|esy_proto_bin|esy_proto_modbus|esy_network")
    message(FATAL_ERROR "JSON USB acquired a network, other family or hardware/OS dependency")
  endif()
  foreach(language c cpp)
    add_executable(consumer_${language} consumer.${language})
    target_link_libraries(consumer_${language} PRIVATE Esy::proto_json_usb_channel tinyusb_device)
    target_compile_options(consumer_${language} PRIVATE -Wall -Wextra -Werror)
  endforeach()
  if(NOT DIRECT_MODULE)
    add_executable(consumer_middleware consumer.c)
    target_link_libraries(consumer_middleware PRIVATE esy_middleware tinyusb_device)
  endif()
elseif(TARGET esy_proto_json_usb_channel)
  message(FATAL_ERROR "Disabled JSON USB channel assembled a target")
endif()
"""

CONSUMER = r"""
#include <string.h>
#include "esy_middleware/protocol/esy_proto_json_usb_channel.h"
#ifdef CORE_JSON_H_
#error "Private coreJSON types leaked into the channel public header"
#endif
#ifdef FAKE_TINYUSB_TUSB_H
#error "Private TinyUSB types leaked into the channel public header"
#endif
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private compiler options leaked into the channel consumer"
#endif
#ifdef __cplusplus
extern "C" {
#endif
#include "tusb.h"
#ifdef __cplusplus
}
#endif

typedef struct {
    unsigned received;
    int error;
} consumer_state_t;

static void receive_message(esy_proto_json_usb_channel_t *channel,
                            const esy_json_value_t *root, void *context)
{
    consumer_state_t *state = (consumer_state_t *)context;
    esy_json_value_t value;
    uint64_t integer;
    esy_proto_json_usb_channel_status_t status;
    state->received++;
    if (esy_json_object_get(root, "value", 5, &value) != ESY_OK ||
        esy_json_as_u64(&value, &integer) != ESY_OK || integer != UINT64_MAX) {
        state->error = 1;
    }
    if (esy_proto_json_usb_channel_get_status(channel, &status) != ESY_OK) state->error = 2;
    if (esy_proto_json_usb_channel_send(channel, root->data, root->length) != ESY_OK) state->error = 3;
}

int main(void)
{
    static const char k_document[] = "{\"value\":18446744073709551615}";
    esy_usb_cdc_t cdc;
    esy_usb_cdc_config_t cdc_config;
    esy_proto_json_usb_channel_t channel;
    esy_proto_json_usb_channel_config_t config;
    esy_proto_json_usb_channel_status_t status;
    esy_proto_json_usb_channel_static_resources_t resources;
    consumer_state_t receiver;
    fake_tinyusb_cdc_itf_state_t *backend;
    uint8_t receive_buffer[64];
    uint8_t transmit_buffer[68];
    uint8_t frame[64];
    unsigned iteration;
    memset(&channel, 0, sizeof(channel));
    memset(&config, 0, sizeof(config));
    memset(&status, 0, sizeof(status));
    memset(&receiver, 0, sizeof(receiver));
    memset(frame, 0, sizeof(frame));
    fake_tinyusb_reset();
    esy_usb_cdc_default_config(&cdc_config);
    cdc_config.discard_on_disconnect = true;
    if (esy_usb_cdc_init(&cdc, &cdc_config) != ESY_OK) return 1;
    backend = fake_tinyusb_cdc_get_itf(0);
    if (backend == NULL) return 2;
    resources.rx_buffer = receive_buffer;
    resources.rx_buffer_size = sizeof(receive_buffer);
    resources.tx_buffer = transmit_buffer;
    resources.tx_buffer_size = sizeof(transmit_buffer);
    config.cdc = &cdc;
    config.static_resources = &resources;
    config.on_message = receive_message;
    config.user_ctx = &receiver;
    config.rx_byte_budget = 7u;
    config.tx_byte_budget = 64u;
    if (esy_proto_json_usb_channel_init(&channel, &config) != ESY_OK) return 3;
    backend->connected = true;
    backend->line_state = 1u;
    esy_usb_cdc_task();
    frame[3] = (uint8_t)(sizeof(k_document) - 1u);
    memcpy(frame + 4u, k_document, sizeof(k_document) - 1u);
    fake_tinyusb_cdc_set_rx_data(0, frame, (uint32_t)(sizeof(k_document) + 3u));
    for (iteration = 0; iteration < 100u; ++iteration) {
        uint32_t task_count = backend->task_count;
        if (esy_proto_json_usb_channel_poll(&channel) != ESY_OK) return 4;
        if (backend->task_count != task_count) return 5;
        if (esy_proto_json_usb_channel_get_status(&channel, &status) != ESY_OK) return 6;
        if (receiver.received == 1u && !status.tx_pending) break;
        esy_usb_cdc_task();
    }
    if (iteration == 100u || receiver.error || receiver.received != 1u) return 7;
    if (backend->write_count != 1u ||
        memcmp(backend->written_data, frame, sizeof(k_document) + 3u) != 0) return 8;
    if (esy_proto_json_usb_channel_deinit(&channel) != ESY_OK) return 9;
    esy_usb_cdc_deinit(&cdc);
    return 0;
}
"""


class JsonUsbConsumerTests(unittest.TestCase):
    def configure(self, project, *, enabled=True, parent=True, codec=True,
                  usb=True, cdc=True, backend=True, direct=False,
                  provide_codec=True, provide_usb=True, tinyusb=True, provider=PROVIDER):
        (project / "CMakeLists.txt").write_text(PROJECT, encoding="utf-8")
        (project / "esy_feature_import.cmake").write_text(
            "set(ESY_RTOS None)\n"
            f"set(ENABLE_ESY_COMMON_PROTOCOL {'ON' if parent else 'OFF'})\n"
            f"set(ENABLE_ESY_COMMON_PROTOCOL_JSON {'ON' if codec else 'OFF'})\n"
            f"set(ENABLE_ESY_USB {'ON' if usb else 'OFF'})\n"
            f"set(ENABLE_ESY_USB_CDC {'ON' if cdc else 'OFF'})\n"
            f"set(ENABLE_ESY_USB_BACKEND_TINYUSB {'ON' if backend else 'OFF'})\n"
            f"set(ENABLE_ESY_PROTO_JSON_USB_CHANNEL {'ON' if enabled else 'OFF'})\n",
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
             f"-DPROVIDE_USB={'ON' if provide_usb else 'OFF'}",
             f"-DPROVIDE_TINYUSB={'ON' if tinyusb else 'OFF'}",
             # Stale cache input cannot override the ordinary feature-file set.
             "-DENABLE_ESY_PROTO_JSON_USB_CHANNEL=OFF"],
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
                    prefix="esy-json-usb-consumer-") as directory:
                project = Path(directory)
                result = self.configure(project, direct=direct)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                self.build(project)

    def test_channel_requires_each_explicit_feature(self):
        for feature, arguments in (
                ("ENABLE_ESY_COMMON_PROTOCOL", {"parent": False, "codec": False}),
                ("ENABLE_ESY_COMMON_PROTOCOL_JSON", {"codec": False}),
                ("ENABLE_ESY_USB", {"usb": False}),
                ("ENABLE_ESY_USB_CDC", {"cdc": False, "backend": False})):
            for direct in (False, True):
                with self.subTest(feature=feature, direct=direct), \
                        tempfile.TemporaryDirectory(prefix="esy-json-usb-feature-") as directory:
                    result = self.configure(Path(directory), direct=direct, **arguments)
                    self.assertNotEqual(0, result.returncode)
                    self.assertIn(f"requires {feature}", result.stdout + result.stderr)

    def test_direct_channel_requires_assembled_targets(self):
        for target, arguments in (
                ("esy_common_protocol_json", {"provide_codec": False}),
                ("esy_usb", {"provide_usb": False})):
            with self.subTest(target=target), tempfile.TemporaryDirectory(
                    prefix="esy-json-usb-target-") as directory:
                result = self.configure(Path(directory), direct=True, **arguments)
                self.assertNotEqual(0, result.returncode)
                self.assertIn(f"requires target {target}", result.stdout + result.stderr)

    def test_usb_backend_remains_consumer_selected(self):
        for arguments, diagnostic in (
                ({"backend": False}, "requires a CDC backend"),
                ({"tinyusb": False}, "consumer-provided tinyusb_device")):
            with self.subTest(arguments=arguments), tempfile.TemporaryDirectory(
                    prefix="esy-json-usb-backend-") as directory:
                result = self.configure(Path(directory), **arguments)
                self.assertNotEqual(0, result.returncode)
                self.assertIn(diagnostic, " ".join((result.stdout + result.stderr).split()))

    def test_disabled_channel_leaves_codec_and_usb_independent(self):
        for direct in (False, True):
            with self.subTest(direct=direct), tempfile.TemporaryDirectory(
                    prefix="esy-json-usb-disabled-") as directory:
                project = Path(directory)
                result = self.configure(project, enabled=False, direct=direct)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                self.build(project, enabled=False)
                commands = json.loads((project / "out/compile_commands.json").read_text())
                self.assertFalse(any("esy_proto_json_usb_channel.c" in item["file"]
                                     for item in commands))
                self.assertTrue(any("esy_usb_cdc.c" in item["file"] for item in commands))
                self.assertTrue(any("core_json.c" in item["file"] for item in commands))

    def test_all_json_and_usb_disabled_ignores_missing_provider(self):
        with tempfile.TemporaryDirectory(prefix="esy-json-usb-all-off-") as directory:
            project = Path(directory)
            result = self.configure(project, enabled=False, parent=False, codec=False,
                                    usb=False, cdc=False, backend=False, tinyusb=False,
                                    provider=project / "absent")
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.build(project, enabled=False)
            commands = json.loads((project / "out/compile_commands.json").read_text())
            self.assertFalse(any("esy_proto_json_" in item["file"] or
                                 "esy_usb_cdc" in item["file"] or "fake_tinyusb.c" in item["file"] or
                                 "core_json.c" in item["file"] for item in commands))

    def test_reconfiguration_removes_channel_and_usb_provider_sources(self):
        with tempfile.TemporaryDirectory(prefix="esy-json-usb-reconfigure-") as directory:
            project = Path(directory)
            result = self.configure(project)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.build(project)
            result = self.configure(project, enabled=False, codec=False, usb=False,
                                    cdc=False, backend=False, tinyusb=False,
                                    provider=project / "absent")
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.build(project, enabled=False)
            commands = json.loads((project / "out/compile_commands.json").read_text())
            self.assertFalse(any("esy_proto_json_" in item["file"] or
                                 "esy_usb_cdc" in item["file"] or "fake_tinyusb.c" in item["file"] or
                                 "core_json.c" in item["file"] for item in commands))


if __name__ == "__main__":
    unittest.main()
