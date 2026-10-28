# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0
"""Build bound-file consumers in C11/C++17 without a filesystem or OS."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(bound_file_consumer C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Werror -pedantic)
include(features.cmake)
add_library(esy_opt_default INTERFACE)
add_subdirectory("${ROOT}/esy_common" common)
if(ENABLE_ESY_FLASH)
    add_library(esy_hal STATIC "${ROOT}/esy_test/fake/hal/fake_esy_hal_flash.c")
    target_include_directories(esy_hal PUBLIC "${ROOT}/esy_hal/include" "${ROOT}/esy_test/fake/hal")
    target_link_libraries(esy_hal PUBLIC esy_common_defs)
endif()
add_subdirectory("${ROOT}/esy_middleware" middleware)
foreach(forbidden IN ITEMS esy_os esy_hal_backend esy_board esy_settings esy_coredump_core)
    if(TARGET ${forbidden})
        message(FATAL_ERROR "Unexpected dependency: ${forbidden}")
    endif()
endforeach()
if(NOT TARGET esy_file)
    message(FATAL_ERROR "Core file target missing")
endif()
set(consumer_targets esy_file)
if(ENABLE_ESY_FILE_FLASH)
    list(APPEND consumer_targets esy_file_flash)
    add_compile_definitions(TEST_FLASH)
endif()
if(ENABLE_ESY_FILE_DUAL_FLASH_STORE)
    list(APPEND consumer_targets esy_file_dual_flash_store)
    add_compile_definitions(TEST_RECORD)
endif()
foreach(language IN ITEMS c cpp)
    add_executable(consumer_${language} consumer.${language})
    target_link_libraries(consumer_${language} PRIVATE ${consumer_targets})
    if(TARGET esy_hal)
        target_link_libraries(consumer_${language} PRIVATE esy_hal)
    endif()
endforeach()
'''
SOURCE = r'''
#include "esy_middleware/file/esy_file.h"
#include "esy_middleware/file/esy_file_ram.h"
#include <string.h>
#ifdef TEST_FLASH
#include "esy_middleware/file/esy_file_flash.h"
#endif
#ifdef TEST_RECORD
#include "esy_middleware/file/esy_file_dual_flash_store.h"
#endif
#if defined(TEST_FLASH) || defined(TEST_RECORD)
#include "fake_esy_hal_flash.h"
#endif
int main(void) {
    esy_file_t file = INITIALIZER;
    esy_file_ops_t ops = INITIALIZER;
    esy_file_ram_t ram = INITIALIZER;
    uint8_t bytes[32];
    uint8_t output[8];
    esy_file_ram_static_resources_t storage = INITIALIZER;
    storage.data_buffer = bytes; storage.capacity = sizeof(bytes);
    esy_file_ram_config_t config = INITIALIZER;
    config.static_resources = &storage;
    if (esy_file_ram_init(&ram, &config) != ESY_OK ||
        esy_file_ram_get_ops(&ram, &ops) != ESY_OK ||
        esy_file_init(&file, &ops) != ESY_OK) return 1;
    if (esy_file_replace(&file, "settings", 8u) != ESY_OK ||
        esy_file_read(&file, 0u, output, 8u) != ESY_OK ||
        memcmp(output, "settings", 8u) != 0) return 2;
    if (esy_file_deinit(&file) != ESY_OK || esy_file_ram_deinit(&ram) != ESY_OK) return 3;
#if defined(TEST_FLASH) || defined(TEST_RECORD)
    uint8_t media[8192];
    uint8_t program[16];
    memset(media, 0xff, sizeof(media));
    fake_esy_hal_flash_reset();
    fake_esy_hal_flash_state_t *hal = fake_esy_hal_flash_get_state();
    hal->storage_base_addr = 0x10000u; hal->size_bytes = sizeof(media);
    hal->sector_size = 256u; hal->program_size = sizeof(program);
    hal->backing_storage = media; hal->backing_storage_size = sizeof(media);
#endif
#ifdef TEST_FLASH
    esy_file_flash_t flash = INITIALIZER;
    esy_file_flash_static_resources_t flash_storage = INITIALIZER;
    flash_storage.program_buffer = program; flash_storage.program_capacity = sizeof(program);
    esy_file_flash_config_t flash_config = INITIALIZER;
    flash_config.address = 0x10000u; flash_config.size = 4096u;
    flash_config.static_resources = &flash_storage;
    if (esy_file_flash_init(&flash, &flash_config) != ESY_OK ||
        esy_file_flash_get_ops(&flash, &ops) != ESY_OK ||
        esy_file_init(&file, &ops) != ESY_OK) return 4;
    uint32_t caps = 0u;
    if (esy_file_get_capabilities(&file, &caps) != ESY_OK ||
        (caps & ESY_FILE_CAP_ATOMIC_REPLACE) != 0u || hal->read_count != 0u) return 5;
    if (esy_file_create(&file, "dump", 4u) != ESY_OK ||
        esy_file_read(&file, 0u, output, 4u) != ESY_OK || memcmp(output, "dump", 4u) != 0 ||
        esy_file_remove(&file) != ESY_OK) return 6;
    if (esy_file_deinit(&file) != ESY_OK || esy_file_flash_deinit(&flash) != ESY_OK) return 7;
#endif
#ifdef TEST_RECORD
    esy_dual_flash_store_t record = INITIALIZER;
    esy_dual_flash_store_static_resources_t record_storage = INITIALIZER;
    record_storage.work_buffer = program; record_storage.work_buffer_size = sizeof(program);
    esy_dual_flash_store_config_t record_config = INITIALIZER;
    record_config.regions[0].address = 0x10000u; record_config.regions[0].size_bytes = 4096u;
    record_config.regions[1].address = 0x11000u; record_config.regions[1].size_bytes = 4096u;
    record_config.static_resources = &record_storage;
    if (esy_dual_flash_store_init(&record, &record_config) != ESY_OK) return 8;
    esy_file_dual_flash_store_t adapter = INITIALIZER;
    esy_file_dual_flash_store_config_t binding = INITIALIZER;
    binding.store = &record;
    if (esy_file_dual_flash_store_init(&adapter, &binding) != ESY_OK ||
        esy_file_dual_flash_store_get_ops(&adapter, &ops) != ESY_OK ||
        esy_file_init(&file, &ops) != ESY_OK) return 9;
    if (esy_file_replace(&file, "old", 3u) != ESY_OK ||
        esy_file_replace(&file, "new", 3u) != ESY_OK ||
        esy_file_read(&file, 1u, output, 2u) != ESY_OK || memcmp(output, "ew", 2u) != 0 ||
        esy_file_remove(&file) != ESY_OK) return 10;
    if (esy_file_deinit(&file) != ESY_OK || esy_file_dual_flash_store_deinit(&adapter) != ESY_OK ||
        esy_dual_flash_store_deinit(&record) != ESY_OK) return 11;
#endif
    return 0;
}
'''

class FileConsumerTests(unittest.TestCase):
    def command(self, *args, error=None):
        result = subprocess.run(args, capture_output=True, text=True)
        output = result.stdout + result.stderr
        if error is None:
            self.assertEqual(0, result.returncode, output)
        else:
            self.assertNotEqual(0, result.returncode, output)
            self.assertIn(error, output)

    def prepare(self, path, *, flash=False, record=False, provider=True):
        (path / "CMakeLists.txt").write_text(PROJECT)
        flags = {"ENABLE_ESY_FILE": True, "ENABLE_ESY_FILE_FLASH": flash,
                 "ENABLE_ESY_FILE_DUAL_FLASH_STORE": record,
                 "ENABLE_ESY_FLASH": (flash or record) and provider,
                 "ENABLE_ESY_DUAL_FLASH_STORE": record and provider}
        (path / "features.cmake").write_text("set(ESY_RTOS None)\n" + "".join(
            f"set({name} {'ON' if value else 'OFF'})\n" for name, value in flags.items()))
        for suffix in ("c", "cpp"):
            (path / f"consumer.{suffix}").write_text(SOURCE.replace("INITIALIZER", "{}" if suffix == "cpp" else "{0}"))

    def configure(self, path, error=None):
        self.command("cmake", "-S", str(path), "-B", str(path / "out"), f"-DROOT={ROOT}", error=error)

    def run_case(self, **options):
        with tempfile.TemporaryDirectory(prefix="bound file consumer ") as directory:
            path = Path(directory)
            self.prepare(path, **options)
            self.configure(path)
            self.command("cmake", "--build", str(path / "out"), "--parallel", "2")
            for language in ("c", "cpp"):
                self.command(str(path / "out" / f"consumer_{language}"))

    def test_ram_consumers_have_no_hal_or_os_dependency(self):
        self.run_case()

    def test_single_region_flash_consumer_has_no_dual_flash_store_requirement(self):
        self.run_case(flash=True)

    def test_atomic_dual_flash_store_consumers_share_the_file_contract(self):
        self.run_case(record=True)

    def test_both_providers_can_be_selected(self):
        self.run_case(flash=True, record=True)

    def test_optional_backends_reject_missing_explicit_providers(self):
        for name, options, error in (
            ("flash", {"flash": True}, "ENABLE_ESY_FILE_FLASH requires ENABLE_ESY_FLASH"),
            ("record", {"record": True}, "ENABLE_ESY_FILE_DUAL_FLASH_STORE requires ENABLE_ESY_DUAL_FLASH_STORE"),
        ):
            with self.subTest(provider=name), tempfile.TemporaryDirectory(prefix="file invalid provider ") as directory:
                path = Path(directory)
                self.prepare(path, provider=False, **options)
                self.configure(path, error)

    def test_children_require_the_parent(self):
        for child in ("ENABLE_ESY_FILE_FLASH", "ENABLE_ESY_FILE_DUAL_FLASH_STORE"):
            with self.subTest(child=child), tempfile.TemporaryDirectory(prefix="file invalid child ") as directory:
                path = Path(directory)
                self.prepare(path)
                (path / "features.cmake").write_text(f"set(ENABLE_ESY_FILE OFF)\nset({child} ON)\n")
                self.configure(path, f"{child} requires ENABLE_ESY_FILE")

if __name__ == "__main__":
    unittest.main()
