# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Verify CCD feature isolation and C11/C++17 consumers."""

from pathlib import Path
import json
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(ccd_consumer LANGUAGES C CXX)
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
add_subdirectory("${ROOT}/esy_common/defs" common_defs)
if(DIRECT_MODULE)
  add_subdirectory("${ROOT}/esy_middleware/ccd" ccd)
else()
  add_subdirectory("${ROOT}/esy_middleware" middleware)
endif()
foreach(unexpected IN ITEMS esy_hal esy_os esy_hal_backend esy_board esy_sensor esy_sensor_i2c)
  if(TARGET ${unexpected})
    message(FATAL_ERROR "CCD unexpectedly assembled ${unexpected}")
  endif()
endforeach()
if(ENABLE_ESY_CCD_TCD1254)
  if(NOT TARGET esy_ccd_tcd1254)
    message(FATAL_ERROR "Enabled CCD target is missing")
  endif()
  get_target_property(dependencies esy_ccd_tcd1254 LINK_LIBRARIES)
  if(NOT "${dependencies}" STREQUAL "esy_common_defs;esy_opt_default")
    message(FATAL_ERROR "CCD target acquired an unexpected dependency")
  endif()
  set(consumer_target esy_ccd_tcd1254)
  if(NOT DIRECT_MODULE)
    set(consumer_target esy_middleware)
  endif()
  file(GLOB public_probes "${CMAKE_CURRENT_LIST_DIR}/public_*")
  add_library(public_headers OBJECT ${public_probes})
  add_library(c_fixture OBJECT consumer.c)
  add_library(readme_example OBJECT readme_example.c)
  add_executable(consumer consumer.cpp $<TARGET_OBJECTS:c_fixture>)
  foreach(target public_headers c_fixture readme_example consumer)
    target_link_libraries(${target} PRIVATE ${consumer_target})
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  endforeach()
elseif(TARGET esy_ccd_tcd1254)
  message(FATAL_ERROR "Disabled CCD target leaked")
endif()
'''

C_SOURCE = r'''
#include "esy_middleware/ccd/esy_ccd_tcd1254.h"
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private optimization options leaked through the CCD public target"
#endif

void ccd_c_layout(size_t layout[8]) {
  layout[0] = sizeof(esy_ccd_tcd1254_config_t);
  layout[1] = _Alignof(esy_ccd_tcd1254_config_t);
  layout[2] = sizeof(esy_ccd_tcd1254_plan_t);
  layout[3] = _Alignof(esy_ccd_tcd1254_plan_t);
  layout[4] = sizeof(esy_ccd_tcd1254_readout_t);
  layout[5] = _Alignof(esy_ccd_tcd1254_readout_t);
  layout[6] = sizeof(esy_ccd_tcd1254_t);
  layout[7] = _Alignof(esy_ccd_tcd1254_t);
}

int ccd_probe(void) {
  const esy_ccd_tcd1254_config_t config = {
    .tick_hz = UINT32_C(100000000), .master_period_ticks = 50,
    .icg_delay_ns = 5000, .icg_to_sh_ns = 500, .sh_high_ns = 1000,
    .sh_period_ns = 10000, .integration_ns = 100000
  };
  esy_ccd_tcd1254_plan_t plan = {0};
  esy_ccd_tcd1254_t ccd = {0};
  esy_ccd_tcd1254_readout_t readout = {0};
  bool usable = true;
  if (esy_ccd_tcd1254_plan_compile(&config, &plan) != ESY_OK ||
      plan.master_period_ticks != config.master_period_ticks) return 1;
  if (esy_ccd_tcd1254_init(&ccd, &config) != ESY_OK ||
      esy_ccd_tcd1254_begin(&ccd, &readout) != ESY_OK || !readout.discard ||
      esy_ccd_tcd1254_complete(&ccd, readout.token,
          ESY_CCD_TCD1254_LINE_SAMPLES, &usable) != ESY_OK || usable) return 2;
  if (esy_ccd_tcd1254_begin(&ccd, &readout) != ESY_OK ||
      esy_ccd_tcd1254_cancel(&ccd, readout.token) != ESY_OK ||
      esy_ccd_tcd1254_deinit(&ccd) != ESY_OK) return 3;
  return 0;
}
'''

CPP_SOURCE = r'''
#include "esy_middleware/ccd/esy_ccd_tcd1254.h"
extern "C" void ccd_c_layout(size_t layout[8]);
extern "C" int ccd_probe(void);
int main() {
  size_t c_layout[8] = {};
  const size_t cpp_layout[8] = {
    sizeof(esy_ccd_tcd1254_config_t), alignof(esy_ccd_tcd1254_config_t),
    sizeof(esy_ccd_tcd1254_plan_t), alignof(esy_ccd_tcd1254_plan_t),
    sizeof(esy_ccd_tcd1254_readout_t), alignof(esy_ccd_tcd1254_readout_t),
    sizeof(esy_ccd_tcd1254_t), alignof(esy_ccd_tcd1254_t)
  };
  ccd_c_layout(c_layout);
  for (size_t index = 0; index < 8; ++index)
    if (c_layout[index] != cpp_layout[index]) return 10;
  const esy_ccd_tcd1254_config_t config = {
    100000000, 50, 5000, 500, 1000, 10000, 100000
  };
  esy_ccd_tcd1254_plan_t plan = {};
  if (esy_ccd_tcd1254_plan_compile(&config, &plan) != ESY_OK) return 11;
  return ccd_probe();
}
'''


class CcdConsumerTests(unittest.TestCase):
    def command(self, arguments):
        result = subprocess.run(arguments, capture_output=True, text=True)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def run_case(self, *, enabled=True, direct=False, rtos="None"):
        with tempfile.TemporaryDirectory(prefix="ccd consumer ") as temporary:
            directory = Path(temporary)
            (directory / "CMakeLists.txt").write_text(PROJECT)
            (directory / "consumer.c").write_text(C_SOURCE)
            (directory / "consumer.cpp").write_text(CPP_SOURCE)
            examples = re.findall(r"```c\n(.*?)```", (ROOT / "esy_middleware/ccd/README.md")
                                  .read_text(), re.DOTALL)
            self.assertEqual(1, len(examples), "CCD README must contain its tested C example")
            (directory / "readme_example.c").write_text(examples[0])
            source = ('#include "esy_middleware/ccd/esy_ccd_tcd1254.h"\n'
                      '#ifdef ESY_HAL_I2C_H\n#error "Unexpected HAL I2C include"\n#endif\n')
            for language in ("c", "cpp"):
                (directory / f"public_ccd.{language}").write_text(source)
            (directory / "esy_feature_import.cmake").write_text(
                f"set(ESY_RTOS {rtos})\n"
                f"set(ENABLE_ESY_CCD_TCD1254 {'ON' if enabled else 'OFF'})\n"
                "set(ENABLE_ESY_SENSOR OFF)\nset(ENABLE_ESY_ADC OFF)\n"
                "set(ENABLE_ESY_ADC_AD7276 OFF)\n"
                f"set(DIRECT_MODULE {'ON' if direct else 'OFF'})\n")
            self.command(["cmake", "-S", str(directory), "-B", str(directory / "out"),
                          "-G", "Ninja", f"-DROOT={ROOT}"])
            self.command(["cmake", "--build", str(directory / "out")])
            database = directory / "out/compile_commands.json"
            commands = json.loads(database.read_text()) if database.exists() else []
            compiled = {Path(command["file"]).name for command in commands}
            self.assertEqual(enabled, "esy_ccd_tcd1254.c" in compiled)
            self.assertNotIn("esy_adc_ad7276.c", compiled)
            self.assertNotIn("esy_sensor.c", compiled)
            if enabled:
                self.command([str(directory / "out/consumer")])

    def test_direct_and_aggregate_feature_on_off(self):
        for direct in (False, True):
            for enabled in (False, True):
                with self.subTest(direct=direct, enabled=enabled):
                    self.run_case(direct=direct, enabled=enabled)

    def test_direct_freertos_consumer_has_no_os_dependency(self):
        self.run_case(direct=True, rtos="FreeRTOS")


if __name__ == "__main__":
    unittest.main()
