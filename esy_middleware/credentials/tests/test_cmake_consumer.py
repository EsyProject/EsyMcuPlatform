# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Check credential bundle/store consumption and optional dependency isolation."""
from pathlib import Path
import json
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(credentials_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra -Werror -pedantic)
include(esy_feature_import.cmake)
add_library(esy_opt_default INTERFACE)
target_compile_definitions(esy_opt_default INTERFACE PRIVATE_OPTIONS_SENTINEL)
add_subdirectory("${ROOT}/esy_common/defs" common_defs)
if(ENABLE_ESY_CREDENTIALS_STORE)
    add_subdirectory("${ROOT}/esy_common/utils" common_utils)
endif()
add_subdirectory("${ROOT}/esy_middleware" middleware)
foreach(forbidden IN ITEMS esy_hal esy_hal_backend esy_board esy_os esy_tls)
    if(TARGET ${forbidden})
        message(FATAL_ERROR "Credentials unexpectedly assembled ${forbidden}")
    endif()
endforeach()
if(ENABLE_ESY_CREDENTIALS)
    if(NOT TARGET esy_credentials OR NOT TARGET Esy::credentials)
        message(FATAL_ERROR "Enabled Credentials target is absent")
    endif()
    foreach(language IN ITEMS c cpp)
        add_executable(consumer_${language} consumer.${language})
        target_link_libraries(consumer_${language} PRIVATE Esy::credentials)
    endforeach()
elseif(TARGET esy_credentials OR TARGET Esy::credentials)
    message(FATAL_ERROR "Disabled Credentials target was assembled")
endif()
if(ENABLE_ESY_CREDENTIALS_STORE)
    if(NOT TARGET esy_credentials_store OR NOT TARGET Esy::credentials_store)
        message(FATAL_ERROR "Enabled Credentials store target is absent")
    endif()
    foreach(language IN ITEMS c cpp)
        add_executable(store_${language} store.${language})
        target_link_libraries(store_${language} PRIVATE Esy::credentials_store)
    endforeach()
elseif(TARGET esy_credentials_store OR TARGET Esy::credentials_store)
    message(FATAL_ERROR "Disabled Credentials store target was assembled")
endif()
if(NOT ENABLE_ESY_FILE AND TARGET esy_file)
    message(FATAL_ERROR "Credentials memory target unexpectedly assembled File")
endif()
'''
DIRECT_PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(credentials_missing_target LANGUAGES C)
foreach(dependency esy_common_defs esy_opt_default)
    if(NOT dependency STREQUAL OMIT_TARGET)
        add_library(${dependency} INTERFACE)
    endif()
endforeach()
if(ENABLE_ESY_CREDENTIALS_STORE)
    foreach(dependency esy_file esy_common_utils)
        if(NOT dependency STREQUAL OMIT_TARGET)
            add_library(${dependency} INTERFACE)
        endif()
    endforeach()
endif()
add_subdirectory("${ROOT}/esy_middleware/credentials" credentials)
'''
SOURCE = r'''
#include "esy_middleware/credentials/esy_credentials.h"
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private build options leaked into the public interface"
#endif
#if defined(ESY_HAL_SYSTEM_H) || defined(ESY_OS_TASK_H)
#error "Hardware or OS headers leaked into the public interface"
#endif
#ifdef ESY_FILE_H
#error "Storage headers leaked into the memory-only public interface"
#endif
int main(void)
{
    const uint8_t ca[] = "test CA";
    esy_credentials_t credentials = INITIALIZER;
    esy_credentials_lease_t lease = INITIALIZER;
    esy_credentials_config_t config = INITIALIZER, snapshot = INITIALIZER;
    config.trust_anchors.data = ca;
    config.trust_anchors.size = sizeof(ca);
    config.trust_anchors.format = ESY_CREDENTIALS_FORMAT_PEM;
    if (esy_credentials_init(&credentials, &config) != ESY_OK) return 1;
    if (esy_credentials_acquire(&credentials, &lease, &snapshot) != ESY_OK) return 2;
    if (snapshot.trust_anchors.data != ca) return 3;
    if (esy_credentials_replace(&credentials, &config) != ESY_ERR_BUSY) return 4;
    if (esy_credentials_deinit(&credentials) != ESY_ERR_BUSY) return 5;
    if (esy_credentials_release(&lease) != ESY_OK) return 6;
    if (esy_credentials_replace(&credentials, &config) != ESY_OK) return 7;
    return esy_credentials_deinit(&credentials) == ESY_OK ? 0 : 8;
}
'''
STORE_SOURCE = r'''
#include "esy_middleware/credentials/esy_credentials_store.h"
#include "esy_middleware/file/esy_file_ram.h"
#include <string.h>
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private build options leaked into the public interface"
#endif
#if defined(ESY_HAL_SYSTEM_H) || defined(ESY_OS_TASK_H)
#error "Hardware or OS headers leaked into the public interface"
#endif
int main(void)
{
    uint8_t media[2048], working[2048], scratch[2048], loaded[512];
    const uint8_t first_ca[] = "factory CA", second_ca[] = "renewed CA";
    esy_file_ram_t ram = INITIALIZER;
    esy_file_t file = INITIALIZER;
    esy_file_ops_t ops = INITIALIZER;
    esy_file_ram_static_resources_t ram_memory = INITIALIZER;
    esy_file_ram_config_t ram_config = INITIALIZER;
    ram_memory.data_buffer = media;
    ram_memory.capacity = sizeof(media);
    ram_config.static_resources = &ram_memory;
    if (esy_file_ram_init(&ram, &ram_config) != ESY_OK) return 1;
    if (esy_file_ram_get_ops(&ram, &ops) != ESY_OK) return 2;
    if (esy_file_init(&file, &ops) != ESY_OK) return 3;
    esy_credentials_store_t store = INITIALIZER;
    esy_credentials_store_static_resources_t memory = INITIALIZER;
    esy_credentials_store_config_t store_config = INITIALIZER;
    memory.working_buffer = working;
    memory.scratch_buffer = scratch;
    memory.buffer_size = sizeof(working);
    store_config.file = &file;
    store_config.static_resources = &memory;
    if (esy_credentials_store_init(&store, &store_config) != ESY_OK) return 4;
    esy_credentials_config_t input = INITIALIZER, output = INITIALIZER;
    input.trust_anchors.data = first_ca;
    input.trust_anchors.size = sizeof(first_ca);
    input.trust_anchors.format = ESY_CREDENTIALS_FORMAT_PEM;
    uint64_t first_id = 0, second_id = 0, loaded_id = 0;
    if (esy_credentials_store_stage(&store, &input, &first_id) != ESY_OK) return 5;
    esy_credentials_store_load_resources_t load_memory = INITIALIZER;
    load_memory.buffer = loaded;
    load_memory.buffer_size = sizeof(loaded);
    if (esy_credentials_store_load(&store, ESY_CREDENTIALS_STORE_CANDIDATE,
                                  &load_memory, &output, &loaded_id) != ESY_OK) return 6;
    if (loaded_id != first_id || output.trust_anchors.size != sizeof(first_ca) ||
        memcmp(output.trust_anchors.data, first_ca, sizeof(first_ca)) != 0) return 7;
    esy_credentials_t credentials = INITIALIZER;
    esy_credentials_lease_t lease = INITIALIZER;
    esy_credentials_config_t snapshot = INITIALIZER;
    if (esy_credentials_init(&credentials, &output) != ESY_OK) return 8;
    if (esy_credentials_acquire(&credentials, &lease, &snapshot) != ESY_OK) return 9;
    if (esy_credentials_store_confirm(&store, first_id) != ESY_OK) return 10;
    input.trust_anchors.data = second_ca;
    input.trust_anchors.size = sizeof(second_ca);
    if (esy_credentials_store_stage(&store, &input, &second_id) != ESY_OK) return 11;
    if (second_id <= first_id) return 12;
    if (esy_credentials_store_discard(&store, second_id) != ESY_OK) return 13;
    if (esy_credentials_store_reload(&store) != ESY_OK) return 14;
    if (memcmp(snapshot.trust_anchors.data, first_ca, sizeof(first_ca)) != 0) return 15;
    if (esy_credentials_release(&lease) != ESY_OK) return 16;
    if (esy_credentials_deinit(&credentials) != ESY_OK) return 17;
    if (esy_credentials_store_load(&store, ESY_CREDENTIALS_STORE_CONFIRMED,
                                  &load_memory, &output, &loaded_id) != ESY_OK) return 18;
    if (loaded_id != first_id) return 19;
    if (esy_credentials_store_deinit(&store) != ESY_OK) return 20;
    if (esy_file_deinit(&file) != ESY_OK) return 21;
    return esy_file_ram_deinit(&ram) == ESY_OK ? 0 : 22;
}
'''


class CredentialsConsumerTests(unittest.TestCase):
    def command(self, *arguments, error=None):
        result = subprocess.run(arguments, capture_output=True, text=True)
        output = result.stdout + result.stderr
        if error is not None:
            self.assertNotEqual(0, result.returncode, output)
            self.assertIn(error, output)
        else:
            self.assertEqual(0, result.returncode, output)

    def build(self, enabled=True, store=False, file=False, omit_target=None,
              error=None, cache_store=None):
        with tempfile.TemporaryDirectory(prefix="credentials consumer ") as directory:
            path = Path(directory)
            (path / "CMakeLists.txt").write_text(
                DIRECT_PROJECT if omit_target else PROJECT)
            (path / "esy_feature_import.cmake").write_text(
                f'set(ENABLE_ESY_CREDENTIALS {"ON" if enabled else "OFF"})\n'
                f'set(ENABLE_ESY_CREDENTIALS_STORE {"ON" if store else "OFF"})\n'
                f'set(ENABLE_ESY_FILE {"ON" if file else "OFF"})\n'
                'set(ESY_RTOS None)\n')
            for suffix in ("c", "cpp"):
                initializer = "{}" if suffix == "cpp" else "{0}"
                (path / f"consumer.{suffix}").write_text(
                    SOURCE.replace("INITIALIZER", initializer))
                (path / f"store.{suffix}").write_text(
                    STORE_SOURCE.replace("INITIALIZER", initializer))
            arguments = ["cmake", "-S", str(path), "-B", str(path / "out"),
                         f"-DROOT={ROOT}"]
            if cache_store is not None:
                arguments.append(f"-DENABLE_ESY_CREDENTIALS_STORE={'ON' if cache_store else 'OFF'}")
            if omit_target:
                arguments.append(f"-DOMIT_TARGET={omit_target}")
                if store:
                    arguments.append("-DENABLE_ESY_CREDENTIALS_STORE=ON")
                error = (f"esy_credentials_store requires target {omit_target}"
                         if omit_target in ("esy_file", "esy_common_utils") else
                         f"esy_credentials requires target {omit_target}")
            self.command(*arguments, error=error)
            if error:
                return
            self.command("cmake", "--build", str(path / "out"), "--parallel", "2")
            if enabled:
                for suffix in ("c", "cpp"):
                    self.command(str(path / f"out/consumer_{suffix}"))
                    if store:
                        self.command(str(path / f"out/store_{suffix}"))
                commands = (path / "out/compile_commands.json").read_text()
                for forbidden in ("esy_hal/", "esy_middleware/os/", "esy_middleware/tls/"):
                    self.assertNotIn(forbidden, commands)
                self.assertEqual(store, "esy_credentials_store.c" in commands)
                for entry in json.loads(commands):
                    if Path(entry['file']).name in ('consumer.c', 'consumer.cpp'):
                        self.assertNotIn('esy_middleware/file/include', entry['command'])

    def test_c_and_cpp_consumers_use_only_public_dependencies(self):
        self.build()

    def test_disabled_feature_assembles_no_module_or_provider(self):
        self.build(enabled=False)

    def test_file_enabled_without_store_keeps_the_adapter_disabled(self):
        self.build(file=True)

    def test_store_c_and_cpp_consumers_link_without_hal_os_or_native_crypto(self):
        self.build(store=True, file=True)

    def test_store_requires_credentials_parent(self):
        self.build(enabled=False, store=True, file=True,
                   error="ENABLE_ESY_CREDENTIALS_STORE requires ENABLE_ESY_CREDENTIALS")

    def test_store_requires_file_feature(self):
        self.build(store=True,
                   error="ENABLE_ESY_CREDENTIALS_STORE requires ENABLE_ESY_FILE")

    def test_store_requires_explicit_targets_when_assembled_directly(self):
        for dependency in ("esy_file", "esy_common_utils"):
            with self.subTest(dependency=dependency):
                self.build(store=True, omit_target=dependency)

    def test_feature_file_disables_store_despite_enabled_cache(self):
        self.build(file=True, cache_store=True)

    def test_feature_file_enables_store_despite_disabled_cache(self):
        self.build(store=True, file=True, cache_store=False)

    def test_required_targets_have_explicit_diagnostics(self):
        for dependency in ("esy_common_defs", "esy_opt_default"):
            with self.subTest(dependency=dependency):
                self.build(omit_target=dependency)


if __name__ == "__main__":
    unittest.main()
