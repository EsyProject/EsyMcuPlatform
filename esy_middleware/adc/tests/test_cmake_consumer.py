# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Verify external ADC feature isolation and C11/C++17 consumers."""

from pathlib import Path
import json
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(adc_consumer LANGUAGES C CXX)
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
if(LEGACY_I2C)
  add_library(esy_hal STATIC
    "${ROOT}/esy_test/fake/hal/fake_esy_hal_i2c.c"
    "${ROOT}/esy_test/fake/hal/fake_esy_hal_system.c")
  target_include_directories(esy_hal PUBLIC "${ROOT}/esy_hal/include"
    PRIVATE "${ROOT}/esy_test/fake/hal")
  target_link_libraries(esy_hal PUBLIC esy_common_defs)
endif()
if(DIRECT_MODULE)
  add_subdirectory("${ROOT}/esy_middleware/adc" adc)
  if(ENABLE_ESY_CCD_TCD1254)
    add_subdirectory("${ROOT}/esy_middleware/ccd" ccd)
  endif()
  if(LEGACY_I2C)
    add_subdirectory("${ROOT}/esy_middleware/sensor" sensor)
  endif()
else()
  add_subdirectory("${ROOT}/esy_middleware" middleware)
endif()
if(TARGET esy_os OR TARGET esy_hal_backend OR TARGET esy_board)
  message(FATAL_ERROR "External ADC unexpectedly assembled OS, backend or board")
endif()
if(NOT LEGACY_I2C AND (TARGET esy_hal OR TARGET esy_sensor OR TARGET esy_sensor_i2c))
  message(FATAL_ERROR "External ADC unexpectedly assembled HAL or sensor targets")
endif()
if(ENABLE_ESY_ADC_AD7276)
  if(NOT TARGET esy_adc_ad7276)
    message(FATAL_ERROR "Enabled external ADC target is missing")
  endif()
  get_target_property(dependencies esy_adc_ad7276 LINK_LIBRARIES)
  if(NOT "${dependencies}" STREQUAL "esy_common_defs;esy_opt_default")
    message(FATAL_ERROR "External ADC target acquired an unexpected dependency")
  endif()
  set(consumer_target esy_adc_ad7276)
  if(DIRECT_MODULE)
    if(ENABLE_ESY_CCD_TCD1254)
      list(APPEND consumer_target esy_ccd_tcd1254)
    endif()
    if(LEGACY_I2C)
      list(APPEND consumer_target esy_sensor_i2c)
    endif()
  else()
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
    if(ENABLE_ESY_CCD_TCD1254)
      target_compile_definitions(${target} PRIVATE WITH_CCD)
    endif()
    if(LEGACY_I2C)
      target_compile_definitions(${target} PRIVATE WITH_I2C)
    endif()
  endforeach()
elseif(TARGET esy_adc_ad7276)
  message(FATAL_ERROR "Disabled external ADC target leaked")
endif()
'''

C_SOURCE = r'''
#include "esy_middleware/adc/esy_adc_ad7276.h"
#ifdef WITH_CCD
#include "esy_middleware/ccd/esy_ccd_tcd1254.h"
#endif
#ifdef WITH_I2C
#include "esy_middleware/sensor/esy_sensor_sht3x.h"
#include "esy_middleware/sensor/esy_sensor_mpu6050.h"
#include "esy_middleware/sensor/esy_sensor_ina219.h"
#endif
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private optimization options leaked through the ADC public target"
#endif

size_t adc_c_size(void) { return sizeof(esy_adc_ad7276_frame_t); }
size_t adc_c_alignment(void) { return _Alignof(esy_adc_ad7276_frame_t); }

int adc_probe(void) {
  const uint16_t raw[] = {0, UINT16_C(0x2af0), UINT16_C(0x3ffc)};
  uint16_t codes[3] = {0};
  if (esy_adc_ad7276_decode(raw, 3, ESY_ADC_AD7276_FRAME_16_CLOCKS,
                           codes, 3) != ESY_OK || codes[0] != 0 ||
      codes[1] != UINT16_C(0xabc) || codes[2] != ESY_ADC_AD7276_MAX_CODE) return 1;
#ifdef WITH_CCD
  const esy_ccd_tcd1254_config_t config = {
    .tick_hz = UINT32_C(100000000), .master_period_ticks = 50,
    .icg_delay_ns = 5000, .icg_to_sh_ns = 500, .sh_high_ns = 1000,
    .sh_period_ns = 10000, .integration_ns = 100000
  };
  esy_ccd_tcd1254_plan_t plan = {0};
  if (esy_ccd_tcd1254_plan_compile(&config, &plan) != ESY_OK) return 2;
#endif
#ifdef WITH_I2C
  if (esy_sensor_sht3x_init(NULL, NULL) != ESY_ERR_INVALID_ARG ||
      esy_sensor_mpu6050_init(NULL, NULL) != ESY_ERR_INVALID_ARG ||
      esy_sensor_ina219_init(NULL, NULL) != ESY_ERR_INVALID_ARG) return 3;
#endif
  return 0;
}
'''

CPP_SOURCE = r'''
#include "esy_middleware/adc/esy_adc_ad7276.h"
extern "C" size_t adc_c_size(void);
extern "C" size_t adc_c_alignment(void);
extern "C" int adc_probe(void);
int main() {
  if (adc_c_size() != sizeof(esy_adc_ad7276_frame_t) ||
      adc_c_alignment() != alignof(esy_adc_ad7276_frame_t)) return 10;
  if (esy_adc_ad7276_decode(nullptr, 0, ESY_ADC_AD7276_FRAME_14_CLOCKS,
                            nullptr, 0) != ESY_OK) return 11;
  return adc_probe();
}
'''


class AdcConsumerTests(unittest.TestCase):
    def command(self, arguments):
        result = subprocess.run(arguments, capture_output=True, text=True)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def run_case(self, *, enabled=True, direct=False, rtos="None",
                 mcu_adc=False, ccd=False, legacy_i2c=False):
        with tempfile.TemporaryDirectory(prefix="external adc consumer ") as temporary:
            directory = Path(temporary)
            (directory / "CMakeLists.txt").write_text(PROJECT)
            (directory / "consumer.c").write_text(C_SOURCE)
            (directory / "consumer.cpp").write_text(CPP_SOURCE)
            examples = re.findall(r"```c\n(.*?)```", (ROOT / "esy_middleware/adc/README.md")
                                  .read_text(), re.DOTALL)
            self.assertEqual(1, len(examples), "ADC README must contain its tested C example")
            (directory / "readme_example.c").write_text(examples[0])
            source = ('#include "esy_middleware/adc/esy_adc_ad7276.h"\n'
                      '#ifdef ESY_HAL_I2C_H\n#error "Unexpected HAL I2C include"\n#endif\n')
            for language in ("c", "cpp"):
                (directory / f"public_adc.{language}").write_text(source)
            values = {"ENABLE_ESY_ADC_AD7276": enabled, "ENABLE_ESY_ADC": mcu_adc,
                      "ENABLE_ESY_CCD_TCD1254": ccd, "ENABLE_ESY_SENSOR": legacy_i2c,
                      "ENABLE_ESY_I2C": legacy_i2c, "ENABLE_ESY_SYSTEM": legacy_i2c,
                      "LEGACY_I2C": legacy_i2c, "DIRECT_MODULE": direct}
            (directory / "esy_feature_import.cmake").write_text(
                f"set(ESY_RTOS {rtos})\n" + "".join(
                    f"set({name} {'ON' if value else 'OFF'})\n"
                    for name, value in values.items()))
            self.command(["cmake", "-S", str(directory), "-B", str(directory / "out"),
                          "-G", "Ninja", f"-DROOT={ROOT}"])
            self.command(["cmake", "--build", str(directory / "out")])
            database = directory / "out/compile_commands.json"
            commands = json.loads(database.read_text()) if database.exists() else []
            compiled = {Path(command["file"]).name for command in commands}
            self.assertEqual(enabled, "esy_adc_ad7276.c" in compiled)
            self.assertEqual(ccd, "esy_ccd_tcd1254.c" in compiled)
            self.assertEqual(legacy_i2c, "esy_sensor_sht3x.c" in compiled)
            if enabled:
                self.command([str(directory / "out/consumer")])

    def test_direct_and_aggregate_feature_on_off(self):
        for direct in (False, True):
            for enabled in (False, True):
                with self.subTest(direct=direct, enabled=enabled):
                    self.run_case(direct=direct, enabled=enabled)

    def test_external_adc_is_independent_of_mcu_adc_feature(self):
        for direct in (False, True):
            with self.subTest(direct=direct):
                self.run_case(direct=direct, mcu_adc=True)

    def test_direct_freertos_consumer_has_no_os_dependency(self):
        self.run_case(direct=True, rtos="FreeRTOS")

    def test_both_device_cores_coexist_without_sensor(self):
        for direct in (False, True):
            with self.subTest(direct=direct):
                self.run_case(direct=direct, ccd=True)

    def test_device_cores_coexist_with_legacy_i2c_sensors(self):
        self.run_case(ccd=True, legacy_i2c=True)


if __name__ == "__main__":
    unittest.main()
