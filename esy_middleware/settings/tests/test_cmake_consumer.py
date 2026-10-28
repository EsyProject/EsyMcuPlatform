# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Verify storage-independent Settings with real C11/C++17 consumers."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(settings_consumer LANGUAGES C CXX)
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
add_subdirectory("${ROOT}/esy_common" common)
if(ENABLE_ESY_FLASH)
    add_library(esy_hal STATIC "${ROOT}/esy_test/fake/hal/fake_esy_hal_flash.c")
    target_include_directories(esy_hal PUBLIC "${ROOT}/esy_hal/include"
        "${ROOT}/esy_test/fake/hal")
    target_link_libraries(esy_hal PUBLIC esy_common_defs)
endif()
add_subdirectory("${ROOT}/esy_middleware" middleware)
foreach(forbidden IN ITEMS esy_os esy_hal_backend esy_board)
    if(TARGET ${forbidden})
        message(FATAL_ERROR "Settings unexpectedly assembled ${forbidden}")
    endif()
endforeach()
if(NOT ENABLE_ESY_FLASH)
    foreach(forbidden IN ITEMS esy_hal esy_dual_flash_store esy_file_dual_flash_store esy_file_flash)
        if(TARGET ${forbidden})
            message(FATAL_ERROR "RAM Settings unexpectedly assembled ${forbidden}")
        endif()
    endforeach()
endif()
if(ENABLE_ESY_SETTINGS)
    add_library(public_headers OBJECT public.c public.cpp)
    target_link_libraries(public_headers PRIVATE esy_settings)
    foreach(language IN ITEMS c cpp)
        add_executable(consumer_${language} consumer.${language})
        target_link_libraries(consumer_${language} PRIVATE esy_settings)
        if(ENABLE_ESY_FILE_DUAL_FLASH_STORE)
            target_link_libraries(consumer_${language} PRIVATE esy_file_dual_flash_store esy_hal)
        else()
            target_link_libraries(consumer_${language} PRIVATE esy_file)
        endif()
    endforeach()
elseif(TARGET esy_settings)
    message(FATAL_ERROR "Disabled Settings were assembled")
endif()
'''
DIRECT_PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(settings_missing_target LANGUAGES C)
include(esy_feature_import.cmake)
foreach(dependency esy_file esy_common_utils esy_opt_default)
    if(NOT dependency STREQUAL OMIT_TARGET)
        add_library(${dependency} INTERFACE)
    endif()
endforeach()
add_subdirectory("${ROOT}/esy_middleware/settings" settings)
'''
PUBLIC_SOURCE = r'''
#include "esy_middleware/settings/esy_settings.h"
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private options leaked through the public target"
#endif
#if defined(ESY_HAL_FLASH_H) || defined(ESY_DUAL_FLASH_STORE_H) || defined(ESY_OS_TASK_H)
#error "A concrete provider leaked into the Settings public header"
#endif
'''
RAM_SETUP = r'''
#include "esy_middleware/file/esy_file_ram.h"
static int bind_file(esy_file_t *file, esy_file_ram_t *provider, uint8_t *data, size_t capacity) {
    esy_file_ram_static_resources_t resources = INITIALIZER;
    resources.data_buffer = data; resources.capacity = capacity;
    esy_file_ram_config_t config = INITIALIZER;
    config.static_resources = &resources;
    esy_file_ops_t ops = INITIALIZER;
    if (esy_file_ram_init(provider, &config) != ESY_OK) return 1;
    if (esy_file_ram_get_ops(provider, &ops) != ESY_OK) return 2;
    return esy_file_init(file, &ops) == ESY_OK ? 0 : 3;
}
'''
RAM_SOURCE = RAM_SETUP + r'''
#include "esy_middleware/settings/esy_settings.h"
#include <string.h>
int main(void) {
    esy_file_t file = INITIALIZER;
    esy_file_ram_t provider = INITIALIZER;
    esy_settings_t settings = INITIALIZER;
    uint8_t data[128], work[128], scratch[128], output[2];
    if (bind_file(&file, &provider, data, sizeof(data)) != 0) return 1;
    esy_settings_static_resources_t resources = INITIALIZER;
    resources.working_buffer = work; resources.scratch_buffer = scratch;
    resources.buffer_size = sizeof(work);
    esy_settings_config_t config = INITIALIZER;
    config.file = &file; config.max_key_length = 31u; config.static_resources = &resources;
    if (esy_settings_init(&settings, &config) != ESY_OK) return 2;
    const uint8_t value[] = {5u, 0u};
    if (esy_settings_set(&settings, "bus/address", value, sizeof(value)) != ESY_OK) return 3;
    if (esy_settings_set(&settings, "empty", NULL, 0u) != ESY_OK) return 4;
    esy_file_info_t stored = INITIALIZER;
    if (esy_file_get_info(&file, &stored) != ESY_OK || stored.exists) return 5;
    if (esy_settings_commit(&settings) != ESY_OK) return 6;
    if (esy_settings_deinit(&settings) != ESY_OK || esy_settings_init(&settings, &config) != ESY_OK) return 7;
    size_t length = 99u; bool found = false;
    if (esy_settings_get(&settings, "bus/address", output, sizeof(output), &length, &found) != ESY_OK ||
        !found || length != sizeof(value) || memcmp(value, output, length) != 0) return 8;
    if (esy_settings_get(&settings, "empty", NULL, 0u, &length, &found) != ESY_OK || !found || length != 0u) return 9;
    if (esy_settings_remove(&settings, "bus/address") != ESY_OK || esy_settings_reload(&settings) != ESY_OK) return 10;
    if (esy_settings_get(&settings, "bus/address", output, sizeof(output), &length, &found) != ESY_OK || !found) return 11;
    if (esy_settings_deinit(&settings) != ESY_OK || esy_file_deinit(&file) != ESY_OK) return 12;
    return esy_file_ram_deinit(&provider) == ESY_OK ? 0 : 13;
}
'''
FLASH_SOURCE = r'''
#include "esy_middleware/settings/esy_settings.h"
#include "esy_middleware/file/esy_file_dual_flash_store.h"
#include "fake_esy_hal_flash.h"
int main(void) {
    esy_dual_flash_store_t store = INITIALIZER;
    esy_file_dual_flash_store_t provider = INITIALIZER;
    esy_file_t file = INITIALIZER;
    esy_settings_t settings = INITIALIZER;
    uint8_t program[32], work[128], scratch[128];
    fake_esy_hal_flash_reset();
    fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
    flash->program_size = sizeof(program);
    esy_dual_flash_store_static_resources_t record_resources = INITIALIZER;
    record_resources.work_buffer = program; record_resources.work_buffer_size = sizeof(program);
    esy_dual_flash_store_config_t record = INITIALIZER;
    record.static_resources = &record_resources;
    const uint32_t region = flash->size_bytes / 2u;
    record.regions[0].address = flash->storage_base_addr;
    record.regions[0].size_bytes = region;
    record.regions[1].address = flash->storage_base_addr + region;
    record.regions[1].size_bytes = region;
    if (esy_dual_flash_store_init(&store, &record) != ESY_OK) return 1;
    esy_file_dual_flash_store_config_t binding = INITIALIZER; binding.store = &store;
    if (esy_file_dual_flash_store_init(&provider, &binding) != ESY_OK) return 2;
    esy_file_ops_t ops = INITIALIZER;
    if (esy_file_dual_flash_store_get_ops(&provider, &ops) != ESY_OK || esy_file_init(&file, &ops) != ESY_OK) return 3;
    esy_settings_static_resources_t resources = INITIALIZER;
    resources.working_buffer = work; resources.scratch_buffer = scratch; resources.buffer_size = sizeof(work);
    esy_settings_config_t config = INITIALIZER;
    config.file = &file; config.max_key_length = 16u; config.static_resources = &resources;
    if (esy_settings_init(&settings, &config) != ESY_OK) return 4;
    const uint8_t value = 42u;
    if (esy_settings_set(&settings, "address", &value, 1u) != ESY_OK || esy_settings_commit(&settings) != ESY_OK) return 5;
    if (esy_settings_deinit(&settings) != ESY_OK || esy_settings_init(&settings, &config) != ESY_OK) return 6;
    uint8_t output = 0u; size_t length = 0u; bool found = false;
    if (esy_settings_get(&settings, "address", &output, 1u, &length, &found) != ESY_OK || !found || output != 42u) return 7;
    if (esy_settings_deinit(&settings) != ESY_OK || esy_file_deinit(&file) != ESY_OK) return 8;
    if (esy_file_dual_flash_store_deinit(&provider) != ESY_OK) return 9;
    return esy_dual_flash_store_deinit(&store) == ESY_OK ? 0 : 10;
}
'''


class SettingsConsumerTests(unittest.TestCase):
    def command(self, *arguments, error=None):
        result = subprocess.run(arguments, capture_output=True, text=True)
        output = result.stdout + result.stderr
        if error is not None:
            self.assertNotEqual(0, result.returncode, output)
            self.assertIn(error, output)
        else:
            self.assertEqual(0, result.returncode, output)
        return output

    def build(self, enabled=True, file=True, flash=False, error=None, omit_target=None):
        with tempfile.TemporaryDirectory(prefix="settings file consumer ") as directory:
            path = Path(directory)
            (path / "CMakeLists.txt").write_text(DIRECT_PROJECT if omit_target else PROJECT)
            flags = {"ENABLE_ESY_SETTINGS": enabled, "ENABLE_ESY_FILE": file,
                     "ENABLE_ESY_FILE_DUAL_FLASH_STORE": file and flash,
                     "ENABLE_ESY_FILE_FLASH": False,
                     "ENABLE_ESY_DUAL_FLASH_STORE": flash, "ENABLE_ESY_FLASH": flash}
            (path / "esy_feature_import.cmake").write_text(
                "".join(f'set({name} {"ON" if value else "OFF"})\n' for name, value in flags.items()) +
                'set(ESY_RTOS None)\n')
            for suffix in ("c", "cpp"):
                (path / f"public.{suffix}").write_text(PUBLIC_SOURCE)
                initial = "{}" if suffix == "cpp" else "{0}"
                (path / f"consumer.{suffix}").write_text(
                    (FLASH_SOURCE if flash else RAM_SOURCE).replace("INITIALIZER", initial))
            arguments = ["cmake", "-S", str(path), "-B", str(path / "out"), f"-DROOT={ROOT}"]
            if omit_target:
                arguments.append(f"-DOMIT_TARGET={omit_target}")
            self.command(*arguments, error=error)
            if error is None:
                self.command("cmake", "--build", str(path / "out"), "--parallel", "2")
                if enabled:
                    self.command(str(path / "out/consumer_c"))
                    self.command(str(path / "out/consumer_cpp"))
                commands = (path / "out/compile_commands.json").read_text() if enabled else ""
                if not flash:
                    for forbidden in ("esy_hal/", "dual_flash_store/", "esy_middleware/os/"):
                        self.assertNotIn(forbidden, commands)

    def test_ram_settings_public_headers_and_runtime_have_no_hardware_or_os(self):
        self.build()

    def test_dual_flash_store_file_links_and_retains_settings_bytes(self):
        self.build(flash=True)

    def test_disabled_module_needs_no_file_or_flash(self):
        self.build(enabled=False, file=False)

    def test_disabled_module_allows_an_independent_file(self):
        self.build(enabled=False)

    def test_file_feature_is_required(self):
        self.build(file=False, error="esy_settings requires ENABLE_ESY_FILE")

    def test_required_targets_have_explicit_diagnostics(self):
        for dependency in ("esy_file", "esy_common_utils", "esy_opt_default"):
            with self.subTest(dependency=dependency):
                self.build(omit_target=dependency,
                           error=f"esy_settings requires target {dependency}")


if __name__ == "__main__":
    unittest.main()
