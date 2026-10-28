# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Verify isolated sensor targets, feature closure and C11/C++17 consumers."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(sensor_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(esy_feature_import.cmake)
add_library(esy_opt_default INTERFACE)
target_compile_definitions(esy_opt_default INTERFACE PRIVATE_OPTIONS_SENTINEL)
add_subdirectory("${ROOT}/esy_common/defs" common_defs)
if(WITH_HAL)
  add_library(esy_hal STATIC
    "${ROOT}/esy_test/fake/hal/fake_esy_hal_i2c.c"
    "${ROOT}/esy_test/fake/hal/fake_esy_hal_system.c")
  target_include_directories(esy_hal PUBLIC "${ROOT}/esy_hal/include"
    PRIVATE "${ROOT}/esy_test/fake/hal")
  target_link_libraries(esy_hal PUBLIC esy_common_defs)
endif()
if(DIRECT_MODULE)
  if(ENABLE_ESY_SENSOR)
    add_subdirectory("${ROOT}/esy_middleware/sensor" sensor)
  endif()
else()
  add_subdirectory("${ROOT}/esy_middleware" middleware)
endif()
if(TARGET esy_os OR TARGET esy_hal_backend OR TARGET esy_board)
  message(FATAL_ERROR "Sensor unexpectedly assembled OS, backend or board targets")
endif()
if(ENABLE_ESY_SENSOR)
  if(NOT TARGET esy_sensor)
    message(FATAL_ERROR "Enabled sensor core target is missing")
  endif()
  set(sensor_target esy_sensor)
  if(ENABLE_ESY_I2C)
    if(NOT TARGET esy_sensor_i2c)
      message(FATAL_ERROR "Enabled sensor I2C target is missing")
    endif()
    set(sensor_target esy_sensor_i2c)
  elseif(TARGET esy_sensor_i2c OR TARGET esy_hal)
    message(FATAL_ERROR "Sensor core unexpectedly assembled a HAL dependency")
  endif()
  if(NOT DIRECT_MODULE)
    set(sensor_target esy_middleware)
  endif()
  file(GLOB public_probes "${CMAKE_CURRENT_LIST_DIR}/public_*")
  add_library(public_headers OBJECT ${public_probes})
  target_link_libraries(public_headers PRIVATE ${sensor_target})
  add_library(c_fixture OBJECT consumer.c)
  target_link_libraries(c_fixture PRIVATE ${sensor_target})
  add_executable(consumer consumer.cpp $<TARGET_OBJECTS:c_fixture>)
  target_link_libraries(consumer PRIVATE ${sensor_target})
  foreach(target public_headers c_fixture consumer)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    if(ENABLE_ESY_I2C)
      target_compile_definitions(${target} PRIVATE WITH_I2C)
    endif()
  endforeach()
elseif(TARGET esy_sensor OR TARGET esy_sensor_i2c)
  message(FATAL_ERROR "Disabled sensor module leaked a target")
endif()
'''

C_SOURCE = r'''
#include "esy_middleware/sensor/esy_sensor.h"
#ifdef WITH_I2C
#include "esy_middleware/sensor/esy_sensor_sht3x.h"
#include "esy_middleware/sensor/esy_sensor_mpu6050.h"
#include "esy_middleware/sensor/esy_sensor_ina219.h"
#endif
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private optimization options leaked through sensor public targets"
#endif

static esy_err_t acquire(void *ctx, esy_sensor_sample_t *sample) {
  sample->channels = ESY_SENSOR_CHANNEL_BIT(ESY_SENSOR_CHAN_TEMPERATURE);
  sample->micro[ESY_SENSOR_CHAN_TEMPERATURE] = *(const int64_t *)ctx;
  return ESY_OK;
}

size_t sensor_c_size(void) { return sizeof(esy_sensor_t); }
size_t sensor_c_alignment(void) { return _Alignof(esy_sensor_t); }

int sensor_probe(void) {
  esy_sensor_t sensor = {0};
  const esy_sensor_driver_t driver = {
    .channels = ESY_SENSOR_CHANNEL_BIT(ESY_SENSOR_CHAN_TEMPERATURE),
    .fetch = acquire
  };
  int64_t temperature = INT64_C(23500000), actual = 0;
  if (esy_sensor_init(&sensor, &driver, &temperature) != ESY_OK) return 1;
  if (esy_sensor_sample_fetch(&sensor) != ESY_OK) return 2;
  if (esy_sensor_channel_get(&sensor, ESY_SENSOR_CHAN_TEMPERATURE, &actual) != ESY_OK ||
      actual != temperature) return 3;
  if (esy_sensor_deinit(&sensor) != ESY_OK) return 4;
#ifdef WITH_I2C
  if (esy_sensor_sht3x_init(NULL, NULL) != ESY_ERR_INVALID_ARG ||
      esy_sensor_mpu6050_init(NULL, NULL) != ESY_ERR_INVALID_ARG ||
      esy_sensor_ina219_init(NULL, NULL) != ESY_ERR_INVALID_ARG) return 5;
#endif
  return 0;
}
'''

CPP_SOURCE = r'''
#include "esy_middleware/sensor/esy_sensor.h"
extern "C" size_t sensor_c_size(void);
extern "C" size_t sensor_c_alignment(void);
extern "C" int sensor_probe(void);
int main() {
  if (sensor_c_size() != sizeof(esy_sensor_t) ||
      sensor_c_alignment() != alignof(esy_sensor_t)) return 10;
  esy_sensor_unit_t unit = ESY_SENSOR_UNIT_AMPERE;
  if (esy_sensor_channel_unit(ESY_SENSOR_CHAN_TEMPERATURE, &unit) != ESY_OK ||
      unit != ESY_SENSOR_UNIT_CELSIUS) return 11;
  if (esy_sensor_init(nullptr, nullptr, nullptr) != ESY_ERR_INVALID_ARG) return 12;
  return sensor_probe();
}
'''


class SensorConsumerTests(unittest.TestCase):
    def command(self, arguments, error=None):
        result = subprocess.run(arguments, capture_output=True, text=True)
        output = result.stdout + result.stderr
        if error:
            self.assertNotEqual(0, result.returncode, output)
            self.assertIn(error, output)
        else:
            self.assertEqual(0, result.returncode, output)

    def run_case(self, *, enabled=True, i2c=False, system=None, direct=False,
                 rtos="None", with_hal=None, error=None):
        if system is None:
            system = i2c
        if with_hal is None:
            with_hal = enabled and i2c
        with tempfile.TemporaryDirectory(prefix="sensor consumer ") as temporary:
            directory = Path(temporary)
            (directory / "CMakeLists.txt").write_text(PROJECT)
            (directory / "consumer.c").write_text(C_SOURCE)
            (directory / "consumer.cpp").write_text(CPP_SOURCE)
            headers = ["esy_sensor.h"]
            if i2c:
                headers.extend(("esy_sensor_sht3x.h", "esy_sensor_mpu6050.h",
                                "esy_sensor_ina219.h"))
            for index, header in enumerate(headers):
                source = f'#include "esy_middleware/sensor/{header}"\n'
                if not i2c:
                    source += ('#ifdef ESY_HAL_I2C_H\n'
                               '#error "Sensor core unexpectedly included HAL I2C"\n'
                               '#endif\n')
                for language in ("c", "cpp"):
                    (directory / f"public_{index}.{language}").write_text(source)
            values = {"ENABLE_ESY_SENSOR": enabled, "ENABLE_ESY_I2C": i2c,
                      "ENABLE_ESY_SYSTEM": system, "WITH_HAL": with_hal,
                      "DIRECT_MODULE": direct}
            (directory / "esy_feature_import.cmake").write_text(
                f"set(ESY_RTOS {rtos})\n" + "".join(
                    f"set({name} {'ON' if value else 'OFF'})\n"
                    for name, value in values.items()))
            self.command(["cmake", "-S", str(directory), "-B", str(directory / "out"),
                          "-G", "Ninja", f"-DROOT={ROOT}"], error=error)
            if error:
                return
            self.command(["cmake", "--build", str(directory / "out")])
            if enabled:
                self.command([str(directory / "out/consumer")])

    def test_aggregate_feature_and_i2c_matrix(self):
        for enabled, i2c in ((False, False), (False, True),
                             (True, False), (True, True)):
            with self.subTest(enabled=enabled, i2c=i2c):
                self.run_case(enabled=enabled, i2c=i2c)

    def test_direct_core_has_no_hal_or_os_dependency(self):
        for rtos in ("None", "FreeRTOS"):
            with self.subTest(rtos=rtos):
                self.run_case(direct=True, rtos=rtos)

    def test_direct_i2c_adapter_links_without_os(self):
        self.run_case(direct=True, i2c=True, rtos="FreeRTOS")

    def test_i2c_adapter_requires_system_support(self):
        self.run_case(i2c=True, system=False,
                      error="esy_sensor_i2c requires ENABLE_ESY_SYSTEM")

    def test_i2c_adapter_requires_a_public_hal_target(self):
        self.run_case(i2c=True, with_hal=False,
                      error="esy_sensor_i2c requires consumer target esy_hal")


if __name__ == "__main__":
    unittest.main()
