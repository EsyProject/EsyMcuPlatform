# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Verify dual Flash store feature isolation and real C11/C++17 target consumers."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(dual_flash_store_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(esy_feature_import.cmake)
add_library(esy_opt_default INTERFACE)
target_compile_definitions(esy_opt_default INTERFACE PRIVATE_OPTIONS_SENTINEL)
add_subdirectory("${ROOT}/esy_common" common)
add_library(esy_hal STATIC "${ROOT}/esy_test/fake/hal/fake_esy_hal_flash.c")
target_include_directories(esy_hal PUBLIC "${ROOT}/esy_hal/include"
  PRIVATE "${ROOT}/esy_test/fake/hal")
target_link_libraries(esy_hal PUBLIC esy_common_defs)
add_subdirectory("${ROOT}/esy_middleware" middleware)
if(TARGET esy_os)
  message(FATAL_ERROR "Dual Flash store unexpectedly assembled an OS dependency")
endif()
if(ENABLE_ESY_DUAL_FLASH_STORE)
  add_library(public_headers OBJECT public.c public.cpp)
  target_link_libraries(public_headers PRIVATE esy_dual_flash_store)
  add_library(flash_fixture OBJECT consumer.c)
  target_include_directories(flash_fixture PRIVATE "${ROOT}/esy_test/fake/hal")
  target_link_libraries(flash_fixture PRIVATE esy_dual_flash_store esy_hal)
  add_executable(consumer consumer.cpp $<TARGET_OBJECTS:flash_fixture>)
  target_link_libraries(consumer PRIVATE esy_dual_flash_store)
elseif(TARGET esy_dual_flash_store)
  message(FATAL_ERROR "Disabled dual Flash store was assembled")
endif()
'''
PUBLIC_SOURCE = r'''
#include "esy_middleware/dual_flash_store/esy_dual_flash_store.h"
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private optimization options leaked through the public target"
#endif
#ifdef ESY_HAL_FLASH_H
#error "The dual Flash store public header unexpectedly included the Flash provider"
#endif
'''
C_SOURCE = r'''
#include "esy_middleware/dual_flash_store/esy_dual_flash_store.h"
#include "fake_esy_hal_flash.h"
#include <string.h>

size_t dual_flash_store_c_size(void) { return sizeof(esy_dual_flash_store_t); }
size_t dual_flash_store_c_alignment(void) { return _Alignof(esy_dual_flash_store_t); }

int dual_flash_store_probe(void) {
  esy_dual_flash_store_t store = {0};
  esy_dual_flash_store_info_t info = {0};
  uint8_t workspace[32];
  const uint8_t payload[] = {0, 1, 2, 0x80, 0xff};
  uint8_t output[sizeof(payload)] = {0};
  size_t length = 0;
  fake_esy_hal_flash_reset();
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  flash->program_size = sizeof(workspace);
  const uint32_t region_size = flash->size_bytes / ESY_DUAL_FLASH_STORE_REGION_COUNT;
  esy_dual_flash_store_static_resources_t resources = {
      .work_buffer = workspace, .work_buffer_size = sizeof(workspace)};
  esy_dual_flash_store_config_t config = {
      .regions = {{flash->storage_base_addr, region_size},
                  {flash->storage_base_addr + region_size, region_size}},
      .static_resources = &resources};
  if (esy_dual_flash_store_init(&store, &config) != ESY_OK) return 1;
  if (esy_dual_flash_store_get_info(&store, &info) != ESY_OK || info.has_record) return 2;
  if (esy_dual_flash_store_write(&store, payload, sizeof(payload)) != ESY_OK) return 3;
  if (esy_dual_flash_store_deinit(&store) != ESY_OK) return 4;
  if (esy_dual_flash_store_init(&store, &config) != ESY_OK) return 5;
  if (esy_dual_flash_store_read(&store, output, sizeof(output), &length) != ESY_OK ||
      length != sizeof(payload) || memcmp(output, payload, sizeof(payload)) != 0)
    return 6;
  if (esy_dual_flash_store_read_range(&store, 1u, output, 3u) != ESY_OK ||
      memcmp(output, payload + 1u, 3u) != 0) return 11;
  if (esy_dual_flash_store_read_range(&store, sizeof(payload), NULL, 0u) != ESY_OK)
    return 12;
  if (esy_dual_flash_store_clear(&store) != ESY_OK) return 7;
  if (esy_dual_flash_store_get_info(&store, &info) != ESY_OK || info.has_record ||
      info.generation != 2u) return 8;
  if (esy_dual_flash_store_deinit(&store) != ESY_OK) return 9;
  if (flash->init_count != 0u || flash->deinit_count != 0u ||
      flash->safe_execute_set_count != 0u) return 10;
  return 0;
}
'''
CPP_SOURCE = r'''
#include "esy_middleware/dual_flash_store/esy_dual_flash_store.h"
extern "C" size_t dual_flash_store_c_size(void);
extern "C" size_t dual_flash_store_c_alignment(void);
extern "C" int dual_flash_store_probe(void);
int main() {
  esy_dual_flash_store_t detached{};
  if (esy_dual_flash_store_read_range(&detached, 0u, nullptr, 0u) != ESY_ERR_STATE) return 23;
  if (dual_flash_store_c_size() != sizeof(esy_dual_flash_store_t)) return 20;
  if (dual_flash_store_c_alignment() != alignof(esy_dual_flash_store_t)) return 21;
  if (esy_dual_flash_store_init(nullptr, nullptr) != ESY_ERR_INVALID_ARG) return 22;
  return dual_flash_store_probe();
}
'''


class DualFlashStoreConsumerTests(unittest.TestCase):
    def command(self, *arguments, error=None):
        result = subprocess.run(arguments, capture_output=True, text=True)
        output = result.stdout + result.stderr
        if error:
            self.assertNotEqual(0, result.returncode, output)
            self.assertIn(error, output)
        else:
            self.assertEqual(0, result.returncode, output)
        return output

    def build(self, enabled=True, flash=True, error=None):
        with tempfile.TemporaryDirectory(prefix="dual Flash store consumer ") as directory:
            path = Path(directory)
            (path / "CMakeLists.txt").write_text(PROJECT)
            (path / "esy_feature_import.cmake").write_text(
                f'set(ENABLE_ESY_DUAL_FLASH_STORE {"ON" if enabled else "OFF"})\n'
                f'set(ENABLE_ESY_FLASH {"ON" if flash else "OFF"})\n'
                'set(ESY_RTOS None)\n')
            (path / "public.c").write_text(PUBLIC_SOURCE)
            (path / "public.cpp").write_text(PUBLIC_SOURCE)
            (path / "consumer.c").write_text(C_SOURCE)
            (path / "consumer.cpp").write_text(CPP_SOURCE)
            self.command("cmake", "-S", str(path), "-B", str(path / "out"),
                         f"-DROOT={ROOT}", error=error)
            if enabled and not error:
                self.command("cmake", "--build", str(path / "out"),
                             "--target", "public_headers", "consumer", "--parallel", "2")
                self.command(str(path / "out/consumer"))

    def test_public_headers_and_real_core_link_without_os(self):
        self.build()

    def test_disabled_module_has_no_target_or_flash_requirement(self):
        self.build(enabled=False, flash=False)

    def test_flash_feature_is_required(self):
        self.build(flash=False, error="requires ENABLE_ESY_FLASH")


if __name__ == "__main__":
    unittest.main()
