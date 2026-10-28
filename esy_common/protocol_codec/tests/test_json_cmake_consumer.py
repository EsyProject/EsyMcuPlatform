#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Verify real JSON consumers, provider diagnostics and feature isolation."""

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
provider_input = cached_provider("corejson")
if not provider_input:
    raise RuntimeError("Run ./setup.sh or set ESY_COREJSON_PATH for provider tests")
PROVIDER = Path(provider_input).resolve()

PROJECT = r"""
cmake_minimum_required(VERSION 3.20)
project(json_consumer LANGUAGES C CXX)
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
if(DIRECT_MODULE)
  add_subdirectory("${PLATFORM_ROOT}/esy_common/defs" defs)
  add_subdirectory("${PLATFORM_ROOT}/esy_common/utils" utils)
endif()
if(ENABLE_ESY_COMMON_PROTOCOL_JSON AND NOT MISSING_PROVIDER AND EXISTING_PROVIDER)
  add_library(esy_json_parser_provider STATIC "${REAL_PROVIDER}/source/core_json.c")
  target_include_directories(esy_json_parser_provider PUBLIC
    "${REAL_PROVIDER}/source/include")
endif()
if(IMPORT_PROVIDER)
  include("${PLATFORM_ROOT}/cmake/esy_third_party_import.cmake")
elseif(ENABLE_ESY_COMMON_PROTOCOL_JSON AND NOT MISSING_PROVIDER)
  include("${PLATFORM_ROOT}/cmake/esy_corejson_provider.cmake")
  esy_configure_corejson_provider()
  esy_configure_corejson_provider()
endif()
if(DIRECT_MODULE)
  add_subdirectory("${PLATFORM_ROOT}/esy_common/protocol_codec" protocol)
else()
  add_subdirectory("${PLATFORM_ROOT}/esy_common" common)
endif()
if(TARGET esy_common_protocol_codec)
  get_target_property(codec_links esy_common_protocol_codec LINK_LIBRARIES)
  if(codec_links MATCHES "json")
    message(FATAL_ERROR "Binary protocol codecs acquired a JSON dependency")
  endif()
endif()
if(ENABLE_ESY_COMMON_PROTOCOL_JSON)
  foreach(lang c cpp)
    add_executable(consumer_${lang} consumer.${lang})
    target_link_libraries(consumer_${lang} PRIVATE Esy::protocol_json)
  endforeach()
  if(NOT DIRECT_MODULE)
    add_executable(consumer_common consumer.c)
    target_link_libraries(consumer_common PRIVATE esy_common)
  endif()
  foreach(unwanted esy_os esy_hal esy_hal_backend esy_board)
    if(TARGET ${unwanted})
      message(FATAL_ERROR "JSON assembled unrelated target ${unwanted}")
    endif()
  endforeach()
elseif(TARGET esy_common_protocol_json OR TARGET esy_json_parser_provider)
  message(FATAL_ERROR "Disabled JSON assembled a target or provider")
endif()
"""

CONSUMER = r"""
#include "esy_common/protocol/esy_proto_json.h"
#ifdef CORE_JSON_H_
#error "Private coreJSON types leaked into JSON public headers"
#endif
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private compiler options leaked into JSON consumers"
#endif
int main(void)
{
    char output[80];
    uint8_t frames[4];
    esy_json_writer_t writer;
    esy_json_writer_resources_t resources = {output, sizeof(output), frames, sizeof(frames)};
    const char *encoded;
    size_t length;
    esy_json_value_t root;
    esy_json_value_t value;
    uint64_t integer;
    if (esy_json_writer_init(&writer, &resources) != ESY_OK) return 1;
    if (esy_json_writer_begin_object(&writer) != ESY_OK) return 2;
    if (esy_json_writer_key(&writer, "value", 5) != ESY_OK) return 3;
    if (esy_json_writer_u64(&writer, UINT64_MAX) != ESY_OK) return 4;
    if (esy_json_writer_end(&writer) != ESY_OK) return 5;
    if (esy_json_writer_finish(&writer, &encoded, &length) != ESY_OK) return 6;
    if (esy_json_parse(encoded, length, &root) != ESY_OK) return 7;
    if (esy_json_object_get(&root, "value", 5, &value) != ESY_OK) return 8;
    if (esy_json_as_u64(&value, &integer) != ESY_OK || integer != UINT64_MAX) return 9;
    return 0;
}
"""


class JsonConsumerTests(unittest.TestCase):
    def configure(self, project, *, enabled=True, parent=True, direct=False,
                  missing=False, provider=PROVIDER, existing=False,
                  imported=False, environment=False):
        (project / "CMakeLists.txt").write_text(PROJECT, encoding="utf-8")
        (project / "esy_feature_import.cmake").write_text(
            "set(ESY_RTOS None)\n"
            f"set(ENABLE_ESY_COMMON_PROTOCOL {'ON' if parent else 'OFF'})\n"
            f"set(ENABLE_ESY_COMMON_PROTOCOL_JSON {'ON' if enabled else 'OFF'})\n",
            encoding="utf-8")
        (project / "consumer.c").write_text(CONSUMER, encoding="utf-8")
        (project / "consumer.cpp").write_text(CONSUMER, encoding="utf-8")
        arguments = ["cmake", "-S", str(project), "-B", str(project / "out"),
                     f"-DPLATFORM_ROOT={ROOT}", f"-DREAL_PROVIDER={PROVIDER}",
                     f"-DDIRECT_MODULE={'ON' if direct else 'OFF'}",
                     f"-DMISSING_PROVIDER={'ON' if missing else 'OFF'}",
                     f"-DEXISTING_PROVIDER={'ON' if existing else 'OFF'}",
                     f"-DIMPORT_PROVIDER={'ON' if imported else 'OFF'}",
                     # These stale cache values must not override the feature file.
                     "-DENABLE_ESY_COMMON_PROTOCOL=OFF",
                     "-DENABLE_ESY_COMMON_PROTOCOL_JSON=OFF"]
        environment_variables = dict(os.environ,
                                     ESY_PROVIDER_CACHE_DIR=str(project / "empty provider cache"))
        if environment:
            environment_variables["ESY_COREJSON_PATH"] = str(provider)
        else:
            arguments.append(f"-DESY_COREJSON_PATH={provider}")
        return subprocess.run(arguments, capture_output=True, text=True,
                              check=False, env=environment_variables)

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

    def test_public_c_and_cpp_consumers_without_os_or_hal(self):
        for direct, imported in ((False, False), (True, False), (False, True)):
            with self.subTest(direct=direct, imported=imported), \
                    tempfile.TemporaryDirectory(prefix="esy-json-consumer-") as directory:
                project = Path(directory)
                result = self.configure(project, direct=direct, imported=imported)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                self.build(project)

    def test_existing_provider_takes_precedence_over_missing_path(self):
        with tempfile.TemporaryDirectory(prefix="esy-json-existing-") as directory:
            project = Path(directory)
            result = self.configure(project, existing=True,
                                    provider=project / "absent")
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.build(project)

    def test_environment_provider_input(self):
        with tempfile.TemporaryDirectory(prefix="esy-json-env-") as directory:
            project = Path(directory)
            result = self.configure(project, environment=True)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.build(project)

    def test_disabled_feature_ignores_missing_provider(self):
        for parent, imported in ((False, False), (True, False), (True, True)):
            with self.subTest(parent=parent, imported=imported), \
                    tempfile.TemporaryDirectory(prefix="esy-json-disabled-") as directory:
                project = Path(directory)
                result = self.configure(project, enabled=False, parent=parent,
                                        provider=project / "absent", imported=imported)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                self.build(project, enabled=False)
                commands = json.loads((project / "out/compile_commands.json").read_text())
                self.assertFalse(any("core_json.c" in item["file"] or
                                     "esy_proto_json_" in item["file"] for item in commands))

    def test_reconfiguration_removes_json_sources(self):
        with tempfile.TemporaryDirectory(prefix="esy-json-reconfigure-") as directory:
            project = Path(directory)
            result = self.configure(project)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.build(project)
            result = self.configure(project, enabled=False, provider=project / "absent")
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.build(project, enabled=False)
            commands = json.loads((project / "out/compile_commands.json").read_text())
            self.assertFalse(any("core_json.c" in item["file"] or
                                 "esy_proto_json_" in item["file"] for item in commands))

    def test_child_feature_requires_parent(self):
        for direct in (False, True):
            with self.subTest(direct=direct), \
                    tempfile.TemporaryDirectory(prefix="esy-json-parent-") as directory:
                result = self.configure(Path(directory), parent=False, direct=direct)
                self.assertNotEqual(0, result.returncode)
                self.assertIn("requires ENABLE_ESY_COMMON_PROTOCOL", result.stdout + result.stderr)

    def test_enabled_module_requires_explicit_provider(self):
        with tempfile.TemporaryDirectory(prefix="esy-json-provider-") as directory:
            result = self.configure(Path(directory), missing=True)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("esy_json_parser_provider", result.stdout + result.stderr)

    def test_helper_rejects_missing_checkout(self):
        with tempfile.TemporaryDirectory(prefix="esy-json-missing-") as directory:
            project = Path(directory)
            result = self.configure(project, provider=project / "absent")
            self.assertNotEqual(0, result.returncode)
            self.assertIn("ESY_COREJSON_PATH", result.stdout + result.stderr)

    def test_helper_rejects_wrong_parser_version(self):
        with tempfile.TemporaryDirectory(prefix="esy-json-version-") as directory:
            project = Path(directory)
            provider = project / "wrong-provider"
            for relative in ("source/core_json.c", "source/include/core_json.h", "LICENSE"):
                destination = provider / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(PROVIDER / relative, destination)
            header = provider / "source/include/core_json.h"
            header.write_text(header.read_text().replace('"v3.3.1"', '"v3.3.0"'))
            result = self.configure(project, provider=provider)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("v3.3.1", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
