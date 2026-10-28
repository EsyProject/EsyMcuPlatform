# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Verify optional time targets and real baremetal C11/C++17 consumers."""
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(time_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(esy_feature_import.cmake)
add_library(esy_opt_default INTERFACE)
target_compile_definitions(esy_opt_default INTERFACE PRIVATE_OPTIONS_SENTINEL)
add_subdirectory("${ROOT}/esy_common" common)
if(ENABLE_ESY_RTC)
  add_library(esy_hal_backend INTERFACE)
  add_library(esy_board INTERFACE)
  add_subdirectory("${ROOT}/esy_hal" hal)
endif()
add_subdirectory("${ROOT}/esy_middleware" middleware)
if(TARGET esy_os)
  message(FATAL_ERROR "Time unexpectedly assembled an OS dependency")
endif()
if(NOT ENABLE_ESY_RTC AND TARGET esy_hal)
  message(FATAL_ERROR "Time unexpectedly assembled a HAL dependency")
endif()
if(ENABLE_ESY_TIME)
  if(NOT TARGET esy_time)
    message(FATAL_ERROR "Enabled time target is missing")
  endif()
  add_library(public_headers OBJECT public.c public.cpp)
  target_link_libraries(public_headers PRIVATE esy_time)
  add_library(time_fixture OBJECT consumer.c)
  target_link_libraries(time_fixture PRIVATE esy_time)
  add_executable(consumer consumer.cpp $<TARGET_OBJECTS:time_fixture>)
  target_link_libraries(consumer PRIVATE esy_time)
  foreach(optional esy_sntp esy_time_rtc)
    if(TARGET ${optional})
      target_link_libraries(public_headers PRIVATE ${optional})
      target_link_libraries(time_fixture PRIVATE ${optional})
      target_link_libraries(consumer PRIVATE ${optional})
    endif()
  endforeach()
  if(ENABLE_ESY_TIME_SNTP AND NOT TARGET esy_sntp)
    message(FATAL_ERROR "Enabled SNTP target is missing")
  elseif(NOT ENABLE_ESY_TIME_SNTP AND TARGET esy_sntp)
    message(FATAL_ERROR "Disabled SNTP target was assembled")
  endif()
  if(ENABLE_ESY_TIME_RTC AND NOT TARGET esy_time_rtc)
    message(FATAL_ERROR "Enabled RTC adapter target is missing")
  elseif(NOT ENABLE_ESY_TIME_RTC AND TARGET esy_time_rtc)
    message(FATAL_ERROR "Disabled RTC adapter target was assembled")
  endif()
elseif(TARGET esy_time OR TARGET esy_sntp OR TARGET esy_time_rtc)
  message(FATAL_ERROR "Disabled time module was assembled")
endif()
'''
PUBLIC_SOURCE = r'''
#include "esy_middleware/time/esy_time.h"
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private optimization options leaked through the public target"
#endif
'''
C_SOURCE = r'''
#include "esy_middleware/time/esy_time.h"

static esy_err_t monotonic_us(void *ctx, uint64_t *microseconds) {
  *microseconds = *(const uint64_t *)ctx;
  return ESY_OK;
}

size_t time_c_size(void) { return sizeof(esy_time_t); }
size_t time_c_alignment(void) { return _Alignof(esy_time_t); }

int time_probe(void) {
  uint64_t clock_us = UINT64_C(123456789);
  esy_time_t time = {0};
  const esy_time_config_t config = {monotonic_us, &clock_us};
  const esy_timestamp_t initial = {INT64_C(1700000000), UINT32_C(250000000)};
  esy_timestamp_t now = {0};
  esy_time_status_t status = {0};
  if (esy_time_init(&time, &config) != ESY_OK) return 1;
  if (esy_time_get_status(&time, &status) != ESY_OK || status.valid) return 2;
  if (esy_time_set(&time, &initial) != ESY_OK) return 3;
  clock_us += UINT64_C(1250000);
  if (esy_time_now(&time, &now) != ESY_OK ||
      now.seconds != initial.seconds + 1 || now.nanoseconds != UINT32_C(500000000))
    return 4;
  if (esy_time_invalidate(&time) != ESY_OK ||
      esy_time_get_status(&time, &status) != ESY_OK || status.valid) return 5;
'''
RTC_SOURCE = r'''
  esy_hal_rtc_t rtc = {0};
  esy_hal_rtc_software_t software = {0};
  const esy_hal_rtc_software_config_t software_config = {
      monotonic_us, &clock_us, {2024u, 2u, 29u, 23u, 59u, 58u}};
  if (esy_hal_rtc_software_init(&software, &software_config) != ESY_OK) return 10;
  const esy_hal_rtc_config_t rtc_config = {esy_hal_rtc_software_get_ops(), &software};
  if (esy_hal_rtc_init(&rtc, &rtc_config) != ESY_OK) return 11;
  if (esy_time_rtc_load(&time, &rtc) != ESY_OK) return 12;
  clock_us += UINT64_C(2000000);
  if (esy_time_rtc_store(&time, &rtc) != ESY_OK) return 13;
  esy_datetime_t datetime = {0};
  if (esy_hal_rtc_get_datetime(&rtc, &datetime) != ESY_OK ||
      datetime.year != 2024u || datetime.month != 3u || datetime.day != 1u ||
      datetime.hour != 0u || datetime.minute != 0u || datetime.second != 0u)
    return 14;
  if (esy_hal_rtc_deinit(&rtc) != ESY_OK ||
      esy_hal_rtc_software_deinit(&software) != ESY_OK) return 15;
'''
CPP_SOURCE = r'''
#include "esy_middleware/time/esy_time.h"
extern "C" size_t time_c_size(void);
extern "C" size_t time_c_alignment(void);
extern "C" int time_probe(void);

int main() {
  if (time_c_size() != sizeof(esy_time_t)) return 20;
  if (time_c_alignment() != alignof(esy_time_t)) return 21;
  if (esy_time_init(nullptr, nullptr) != ESY_ERR_INVALID_ARG) return 22;
  return time_probe();
}
'''


class TimeConsumerTests(unittest.TestCase):
    def command(self, *arguments, error=None):
        result = subprocess.run(arguments, capture_output=True, text=True)
        output = result.stdout + result.stderr
        if error:
            self.assertNotEqual(0, result.returncode, output)
            self.assertIn(error, output)
        else:
            self.assertEqual(0, result.returncode, output)

    def build(self, enabled=True, sntp=False, rtc_adapter=False, rtc=False, error=None,
              optimized=False):
        with tempfile.TemporaryDirectory(prefix="time consumer ") as directory:
            path = Path(directory)
            (path / "CMakeLists.txt").write_text(PROJECT)
            (path / "esy_feature_import.cmake").write_text(
                f'set(ENABLE_ESY_TIME {"ON" if enabled else "OFF"})\n'
                f'set(ENABLE_ESY_TIME_SNTP {"ON" if sntp else "OFF"})\n'
                f'set(ENABLE_ESY_TIME_RTC {"ON" if rtc_adapter else "OFF"})\n'
                f'set(ENABLE_ESY_RTC {"ON" if rtc else "OFF"})\n'
                'set(ESY_RTOS None)\n')
            headers = ""
            if sntp:
                headers += '#include "esy_middleware/time/esy_sntp.h"\n'
            if rtc_adapter:
                headers += '#include "esy_middleware/time/esy_time_rtc.h"\n'
            public = headers + PUBLIC_SOURCE
            if not rtc_adapter:
                public += ('#ifdef ESY_HAL_RTC_H\n'
                           '#error "Time core unexpectedly included the RTC provider"\n'
                           '#endif\n')
            (path / "public.c").write_text(public)
            (path / "public.cpp").write_text(public)
            source = headers + C_SOURCE
            if sntp:
                source += '  if (esy_sntp_init(NULL, NULL) != ESY_ERR_INVALID_ARG) return 6;\n'
            if rtc_adapter:
                source += RTC_SOURCE
            source += '  return esy_time_deinit(&time) == ESY_OK ? 0 : 7;\n}\n'
            (path / "consumer.c").write_text(source)
            (path / "consumer.cpp").write_text(CPP_SOURCE)
            options = [f"-DROOT={ROOT}"]
            if optimized:
                flags = "-O3 -fstrict-aliasing -Wall -Wextra -Werror"
                options.extend((f"-DCMAKE_C_FLAGS={flags}", f"-DCMAKE_CXX_FLAGS={flags}"))
            self.command("cmake", "-S", str(path), "-B", str(path / "out"),
                         *options, error=error)
            if enabled and not error:
                self.command("cmake", "--build", str(path / "out"),
                             "--target", "public_headers", "consumer", "--parallel", "2")
                self.command(str(path / "out/consumer"))

    def test_core_public_headers_and_real_c_cpp_link_without_hal_or_os(self):
        self.build()

    def test_sntp_public_headers_and_link_without_hal_or_os(self):
        self.build(sntp=True)

    def test_rtc_adapter_round_trip_links_through_public_targets(self):
        self.build(sntp=True, rtc_adapter=True, rtc=True)

    def test_optimized_public_targets_support_c_cpp_integration(self):
        self.build(sntp=True, rtc_adapter=True, rtc=True, optimized=True)

    def test_disabled_module_requires_no_providers(self):
        self.build(enabled=False)

    def test_sntp_requires_the_time_feature(self):
        self.build(enabled=False, sntp=True,
                   error="ENABLE_ESY_TIME_SNTP requires ENABLE_ESY_TIME")

    def test_rtc_adapter_requires_the_time_feature(self):
        self.build(enabled=False, rtc_adapter=True,
                   error="ENABLE_ESY_TIME_RTC requires ENABLE_ESY_TIME")

    def test_rtc_adapter_requires_the_rtc_feature(self):
        self.build(rtc_adapter=True,
                   error="requires ENABLE_ESY_RTC")


if __name__ == "__main__":
    unittest.main()
