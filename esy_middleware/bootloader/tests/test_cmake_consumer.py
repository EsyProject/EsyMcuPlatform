# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Verify the bootloader target, HAL boundary, feature closure and public headers."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(boot_consumer C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
include(esy_feature_import.cmake)
add_library(esy_opt_default INTERFACE)
target_compile_definitions(esy_opt_default INTERFACE PRIVATE_OPTIONS_SENTINEL)
add_subdirectory("${ROOT}/esy_common" common)
add_library(esy_hal STATIC
  "${ROOT}/esy_hal/src/esy_hal_boot.c"
  "${ROOT}/esy_hal/src/esy_hal_flash.c"
  "${ROOT}/esy_hal/src/esy_hal_system.c"
  "${ROOT}/esy_hal/tests/hal_test_fakes.c"
  "${UNITY}/unity.c")
target_include_directories(esy_hal PUBLIC "${ROOT}/esy_hal/include"
  PRIVATE "${ROOT}/esy_hal_backend/include" "${ROOT}/esy_hal/tests" "${UNITY}")
target_compile_definitions(esy_hal PRIVATE OTA_SLOT_A_ADDR=0x10000000u
  OTA_SLOT_B_ADDR=0x10010000u OTA_SLOT_A_META_ADDR=0x1000F000u
  OTA_SLOT_B_META_ADDR=0x1001F000u OTA_SLOT_TOTAL_SIZE=0x10000u)
target_link_libraries(esy_hal PUBLIC esy_common_defs esy_common_device)
# Real boot and HAL sources are compiled without OS, RTOS, SDK or native board headers.
add_subdirectory("${ROOT}/esy_middleware" middleware)
if(TARGET esy_os)
  message(FATAL_ERROR "Bootloader unexpectedly assembled an OS dependency")
endif()
if(ENABLE_ESY_BOOTLOADER)
  add_library(boot_public OBJECT boot_public.c)
  target_link_libraries(boot_public PRIVATE esy_bootloader)
  add_executable(consumer consumer.c consumer.cpp)
  target_link_libraries(consumer PRIVATE esy_bootloader esy_hal)
elseif(TARGET esy_bootloader)
  message(FATAL_ERROR "Disabled bootloader was assembled")
endif()
'''
C_SOURCE = r'''
#include "esy_middleware/bootloader/esy_bootloader.h"
#include "esy_hal/esy_hal_boot.h"
#ifdef ESY_BOARD_DEFS_H
#error "Board declarations leaked through the public HAL or bootloader header"
#endif
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private compiler options leaked"
#endif
void setUp(void) {}
void tearDown(void) {}
int cpp_probe(void);
int main(void) {
  void (*handoff)(uint32_t) = esy_hal_boot_jump_to_app;
  if (handoff == NULL) return 3;
  esy_bootloader_layout_t layout = {0};
  esy_bootloader_config_t config = {0};
  if (esy_hal_boot_get_layout(&layout) != ESY_OK) return 1;
  if (layout.meta_size[0] != 0x1000u || layout.meta_size[1] != 0x2000u) return 4;
  if (cpp_probe() != 0) return 5;
  return esy_bootloader_init(&config) == ESY_ERR_INVALID_ARG ? 0 : 2;
}
'''
CPP_SOURCE = r'''
#include "esy_middleware/bootloader/esy_bootloader.h"
#include "esy_hal/esy_hal_boot.h"
#ifdef ESY_BOARD_DEFS_H
#error "Board declarations leaked into C++ headers"
#endif
static_assert(sizeof(esy_firmware_header_t) == 256u, "Wire format changed");
static_assert(sizeof(&esy_hal_boot_jump_to_app) == sizeof(void (*)(uint32_t)), "Handoff signature changed");
extern "C" int cpp_probe(void) {
  esy_bootloader_layout_t layout{};
  if (esy_hal_boot_get_layout(&layout) != ESY_OK) return 1;
  return layout.meta_size[0] == 0x1000u && layout.meta_size[1] == 0x2000u ? 0 : 2;
}
'''


class BootConsumerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        gem = subprocess.check_output(
            ["ruby", "-e", 'puts Gem::Specification.find_by_name("ceedling").full_gem_path'],
            text=True).strip()
        cls.unity = Path(gem) / "vendor/unity/src"

    def build(self, overrides=None, error=None):
        features = dict(ENABLE_ESY_BOOTLOADER="ON", ENABLE_ESY_FLASH="ON",
                        ENABLE_ESY_SYSTEM="ON", ESY_RTOS="None")
        features.update(overrides or {})
        with tempfile.TemporaryDirectory(prefix="boot consumer ") as d:
            p = Path(d)
            (p / "CMakeLists.txt").write_text(PROJECT)
            (p / "esy_feature_import.cmake").write_text(
                "".join(f"set({key} {value})\n" for key, value in features.items()))
            (p / "boot_public.c").write_text(
                '#include "esy_middleware/bootloader/esy_bootloader.h"\n'
                'int boot_probe(void) { return esy_bootloader_get_running_slot(); }\n')
            (p / "consumer.c").write_text(C_SOURCE)
            (p / "consumer.cpp").write_text(CPP_SOURCE)
            result = subprocess.run(
                ["cmake", "-S", str(p), "-B", str(p / "out"),
                 f"-DROOT={ROOT}", f"-DUNITY={self.unity}"], text=True, capture_output=True)
            output = result.stdout + result.stderr
            if error:
                self.assertNotEqual(0, result.returncode, output)
                self.assertIn(error, output)
                return
            self.assertEqual(0, result.returncode, output)
            if features["ENABLE_ESY_BOOTLOADER"] == "ON":
                result = subprocess.run(["cmake", "--build", str(p / "out"), "--target", "consumer", "boot_public"],
                                        text=True, capture_output=True)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                result = subprocess.run([str(p / "out/consumer")], text=True, capture_output=True)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_real_bootloader_and_hal_link_without_os_or_board_header_leaks(self):
        self.build()

    def test_disabled_bootloader_creates_no_target(self):
        self.build(dict(ENABLE_ESY_BOOTLOADER="OFF", ENABLE_ESY_FLASH="OFF",
                        ENABLE_ESY_SYSTEM="OFF"))

    def test_missing_hardware_features_fail_configuration(self):
        for feature in ("ENABLE_ESY_FLASH", "ENABLE_ESY_SYSTEM"):
            with self.subTest(feature=feature):
                self.build({feature: "OFF"}, f"esy_bootloader requires {feature}")


if __name__ == "__main__":
    unittest.main()
