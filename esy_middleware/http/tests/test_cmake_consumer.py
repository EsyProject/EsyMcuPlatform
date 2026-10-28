#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Verify HTTP public consumers, provider diagnostics and disabled isolation."""

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
provider_input = cached_provider("llhttp")
if not provider_input:
    raise RuntimeError("Run ./setup.sh or set ESY_LLHTTP_PATH for provider tests")
PROVIDER = Path(provider_input).resolve()

PROJECT = r"""
cmake_minimum_required(VERSION 3.20)
project(http_consumer LANGUAGES C CXX)
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
add_subdirectory("${PLATFORM_ROOT}/esy_common/defs" defs)
if(ENABLE_ESY_HTTP_CLIENT AND NOT MISSING_PROVIDER)
  if(EXISTING_PROVIDER)
    add_library(esy_http_parser_provider STATIC
      "${REAL_PROVIDER}/src/api.c" "${REAL_PROVIDER}/src/http.c"
      "${REAL_PROVIDER}/src/llhttp.c")
    target_include_directories(esy_http_parser_provider PUBLIC "${REAL_PROVIDER}/include")
  endif()
  include("${PLATFORM_ROOT}/cmake/esy_llhttp_provider.cmake")
  esy_configure_llhttp_provider()
  esy_configure_llhttp_provider()
endif()
if(DIRECT_MODULE)
  if(ENABLE_ESY_NETWORK)
    add_subdirectory("${PLATFORM_ROOT}/esy_middleware/network" network)
  endif()
  if(ENABLE_ESY_HTTP_CLIENT)
    add_subdirectory("${PLATFORM_ROOT}/esy_middleware/http" http)
  endif()
else()
  add_subdirectory("${PLATFORM_ROOT}/esy_middleware" middleware)
endif()
if(ENABLE_ESY_HTTP_CLIENT)
  foreach(lang c cpp)
    add_executable(consumer_${lang} consumer.${lang})
    target_link_libraries(consumer_${lang} PRIVATE Esy::http_client)
  endforeach()
  if(NOT DIRECT_MODULE)
    add_executable(consumer_middleware consumer.c)
    target_link_libraries(consumer_middleware PRIVATE esy_middleware)
  endif()
  foreach(unwanted esy_os esy_hal esy_hal_backend esy_board esy_network_lwip esy_tls)
    if(TARGET ${unwanted})
      message(FATAL_ERROR "HTTP core assembled unrelated target ${unwanted}")
    endif()
  endforeach()
elseif(TARGET esy_http_client OR TARGET esy_http_parser_provider)
  message(FATAL_ERROR "Disabled HTTP assembled a target or provider")
endif()
"""

CONSUMER = r"""
#include "esy_middleware/http/esy_http_client.h"
#ifdef INCLUDE_LLHTTP_H_
#error "Private parser types leaked into HTTP public headers"
#endif
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private compiler options leaked into HTTP consumers"
#endif
int main(void)
{
    esy_http_client_t client = {0};
    esy_http_client_config_t config = {0};
    esy_http_request_t request = {ESY_HTTP_GET};
    esy_http_client_status_t status = {ESY_HTTP_IDLE};
    esy_http_client_static_resources_t resources = {0};
    ESY_HTTP_CLIENT_STATIC_STORAGE(storage, 128, 32, 64);
    ESY_HTTP_CLIENT_BIND_STATIC_RESOURCES(&resources, storage);
    if (resources.body_buffer_size != sizeof(storage.body_buffer)) return 1;
    if (esy_http_client_init(&client, &config) != ESY_ERR_INVALID_ARG) return 2;
    if (esy_http_client_submit(&client, &request) != ESY_ERR_STATE) return 3;
    if (esy_http_client_process(&client) != ESY_ERR_STATE) return 4;
    if (esy_http_client_get_status(&client, &status) != ESY_ERR_STATE) return 5;
    if (esy_http_client_cancel(&client) != ESY_ERR_STATE) return 6;
    if (esy_http_client_deinit(&client) != ESY_OK) return 7;
    return 0;
}
"""


class HttpConsumerTests(unittest.TestCase):
    def configure(self, project, *, enabled=True, network=True, direct=False,
                  missing=False, provider=PROVIDER, existing=False):
        (project / "CMakeLists.txt").write_text(PROJECT, encoding="utf-8")
        (project / "esy_feature_import.cmake").write_text(
            "set(ESY_RTOS None)\n"
            f"set(ENABLE_ESY_HTTP_CLIENT {'ON' if enabled else 'OFF'})\n"
            f"set(ENABLE_ESY_NETWORK {'ON' if network else 'OFF'})\n",
            encoding="utf-8")
        (project / "consumer.c").write_text(CONSUMER, encoding="utf-8")
        (project / "consumer.cpp").write_text(CONSUMER, encoding="utf-8")
        environment_variables = dict(
            os.environ, ESY_PROVIDER_CACHE_DIR=str(project / "empty provider cache"))
        return subprocess.run(
            ["cmake", "-S", str(project), "-B", str(project / "out"),
             f"-DPLATFORM_ROOT={ROOT}", f"-DESY_LLHTTP_PATH={provider}",
             f"-DREAL_PROVIDER={PROVIDER}",
             f"-DDIRECT_MODULE={'ON' if direct else 'OFF'}",
             f"-DMISSING_PROVIDER={'ON' if missing else 'OFF'}",
             f"-DEXISTING_PROVIDER={'ON' if existing else 'OFF'}"],
            capture_output=True, text=True, env=environment_variables)

    def assert_successful_consumer(self, *, direct=False, existing=False):
        with tempfile.TemporaryDirectory(prefix="http consumer ") as directory:
            project = Path(directory)
            provider = project / "absent" if existing else PROVIDER
            result = self.configure(project, direct=direct, existing=existing,
                                    provider=provider)
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
                for private in (str(PROVIDER), "PRIVATE_OPTIONS_SENTINEL", "/http/src",
                                "/esy_hal/", "/esy_middleware/os/"):
                    self.assertNotIn(private, command)

    def test_aggregate_c11_cpp17_consumers_without_os(self):
        self.assert_successful_consumer()

    def test_direct_module_and_idempotent_helper(self):
        self.assert_successful_consumer(direct=True)

    def test_existing_provider_takes_precedence_over_missing_checkout(self):
        self.assert_successful_consumer(existing=True)

    def test_disabled_feature_ignores_missing_provider(self):
        for network in (False, True):
            with self.subTest(network=network), tempfile.TemporaryDirectory() as directory:
                result = self.configure(Path(directory), enabled=False, network=network,
                                        provider=Path(directory) / "absent")
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_enabled_feature_requires_network_contract(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.configure(Path(directory), network=False)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("NETWORK", result.stdout + result.stderr)

    def test_enabled_module_requires_explicit_provider(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.configure(Path(directory), missing=True)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("esy_http_parser_provider", result.stdout + result.stderr)

    def test_helper_rejects_missing_checkout(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.configure(Path(directory), provider=Path(directory) / "absent")
            self.assertNotEqual(0, result.returncode)
            self.assertIn("ESY_LLHTTP_PATH", result.stdout + result.stderr)

    def test_helper_rejects_wrong_parser_version(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            provider = project / "wrong-provider"
            shutil.copytree(PROVIDER, provider)
            header = provider / "include/llhttp.h"
            header.write_text(header.read_text().replace(
                "#define LLHTTP_VERSION_PATCH 3", "#define LLHTTP_VERSION_PATCH 2"))
            result = self.configure(project, provider=provider)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("v9.4.3", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
