# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Verify Wi-Fi target isolation, providers, C/C++ consumers and task concurrency."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
PROJECT = r'''cmake_minimum_required(VERSION 3.20)
project(wifi_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
include(esy_feature_import.cmake)
if(SANITIZER)
  add_compile_options(-fsanitize=${SANITIZER} -g -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${SANITIZER})
endif()
foreach(t esy_opt_default esy_board pico_time pico_stdio STM32_Drivers)
  add_library(${t} INTERFACE)
endforeach()
add_subdirectory("${ROOT}/esy_common/defs" defs)
add_subdirectory("${ROOT}/esy_common/device" device)
add_library(esy_common INTERFACE)
target_link_libraries(esy_common INTERFACE esy_common_defs esy_common_device)
if(PROVIDER)
  add_library(esy_wifi_provider INTERFACE)
  target_include_directories(esy_wifi_provider INTERFACE "${CMAKE_SOURCE_DIR}/network config")
endif()
add_subdirectory("${ROOT}/esy_hal_backend" driver)
add_subdirectory("${ROOT}/esy_hal" platform)
add_subdirectory("${ROOT}/esy_middleware" middleware)
get_target_property(sources esy_hal_backend SOURCES)
get_target_property(includes esy_hal_backend INCLUDE_DIRECTORIES)
get_target_property(deps esy_hal_backend LINK_LIBRARIES)
file(WRITE "${CMAKE_BINARY_DIR}/assembly.txt" "${sources}\n${includes}\n${deps}\n")
# Keep real target interfaces and replace only hardware implementation sources.
# Renaming the reusable HAL fake provides a deterministic HAL backend facade;
# the actual HAL Wi-Fi forwarding source remains in this build.
if(ENABLE_ESY_WIFI)
  set(fake "${ROOT}/esy_test/fake/hal/fake_esy_hal_wifi.c")
  set_property(TARGET esy_hal_backend PROPERTY SOURCES "${fake};${CMAKE_SOURCE_DIR}/provider_probe.c")
  target_include_directories(esy_hal_backend PRIVATE "${ROOT}/esy_test/fake/hal" "${ROOT}/esy_hal/include")
  set(rename)
  foreach(api default_cfg start_sta process_sta init_sta is_connected get_ipv4_addr get_network_runtime deinit)
    list(APPEND rename "esy_hal_wifi_${api}=esy_hb_wifi_${api}")
  endforeach()
  set_source_files_properties("${fake}" TARGET_DIRECTORY esy_hal_backend
    PROPERTIES COMPILE_DEFINITIONS "${rename}")
  foreach(lang c cpp)
    add_executable(consumer_${lang} consumer.${lang})
    target_include_directories(consumer_${lang} PRIVATE "${ROOT}/esy_test/fake/hal")
    target_link_libraries(consumer_${lang} PRIVATE esy_hal)
    if(TARGET esy_wifi_manager)
      target_link_libraries(consumer_${lang} PRIVATE esy_wifi_manager)
    endif()
  endforeach()
  if(TARGET esy_wifi_manager)
    find_package(Threads REQUIRED)
    add_executable(concurrency "${ROOT}/esy_middleware/wifi/tests/wifi_concurrency_probe.c")
    target_include_directories(concurrency PRIVATE "${ROOT}/esy_test/fake/hal")
    target_link_libraries(concurrency PRIVATE esy_wifi_manager esy_hal Threads::Threads)
  endif()
else()
  set_property(TARGET esy_hal_backend PROPERTY SOURCES "${CMAKE_SOURCE_DIR}/dummy.c")
  target_sources(esy_hal PRIVATE "${CMAKE_SOURCE_DIR}/dummy.c")
  if(TARGET esy_wifi_manager)
    message(FATAL_ERROR "disabled Wi-Fi unexpectedly has a manager")
  endif()
endif()
'''
CONSUMER = r'''#include "esy_hal/esy_hal_wifi.h"
#include "fake_esy_hal_wifi.h"
#include <string.h>
MANAGER_INCLUDE
int main(void) {
  esy_wifi_config_t config;
  esy_wifi_ipv4_addr_t address;
  esy_wifi_network_runtime_t runtime;
  int marker = 0;
  fake_esy_hal_wifi_reset();
  fake_esy_hal_wifi_state_t *fake = fake_esy_hal_wifi_state();
  fake->network_runtime.context = &marker;
  if (esy_hal_wifi_get_network_runtime(NULL) != ESY_ERR_INVALID_ARG) return 12;
  if (fake->runtime_calls != 0u) return 13;
  if (esy_hal_wifi_get_network_runtime(&runtime) != ESY_OK) return 14;
  if (fake->runtime_calls != 1u || runtime.context != &marker ||
      runtime.progress != fake->network_runtime.progress ||
      runtime.enter != fake->network_runtime.enter ||
      runtime.leave != fake->network_runtime.leave) return 15;
  runtime.progress(runtime.context);
  runtime.enter(runtime.context);
  runtime.leave(runtime.context);
  if (fake->progress_calls != 1u || fake->enter_calls != 1u ||
      fake->leave_calls != 1u || fake->last_context != &marker) return 16;
  fake->runtime_result = ESY_ERR_IO;
  fake->network_runtime.context = NULL;
  if (esy_hal_wifi_get_network_runtime(&runtime) != ESY_ERR_IO ||
      runtime.context != &marker ||
      runtime.progress != fake->network_runtime.progress ||
      runtime.enter != fake->network_runtime.enter ||
      runtime.leave != fake->network_runtime.leave) return 17;
  if (fake->start_calls != 0u || fake->process_calls != 0u ||
      fake->address_calls != 0u || fake->deinit_calls != 0u ||
      fake->connected) return 18;
  fake->runtime_result = ESY_OK;
  esy_hal_wifi_default_cfg(&config);
  config.ssid = "consumer";
  config.password = "password";
  MANAGER_BODY
  if (esy_hal_wifi_start_sta(&config) != ESY_OK) return 5;
  if (esy_hal_wifi_process_sta() != ESY_OK) return 6;
  if (!esy_hal_wifi_is_connected()) return 7;
  if (esy_hal_wifi_get_ipv4_addr(&address) != ESY_OK) return 8;
  if (esy_hal_wifi_get_ipv4_addr(NULL) != ESY_ERR_INVALID_ARG) return 9;
  esy_hal_wifi_deinit();
  if (esy_hal_wifi_is_connected()) return 10;
  if (esy_hal_wifi_init_sta(&config) != ESY_OK) return 11;
  esy_hal_wifi_deinit();
  return 0;
}
'''
MANAGER = r'''esy_wifi_manager_t manager;
  esy_wifi_manager_config_t mc;
  mc.sta_config = config;
  if (esy_wifi_manager_init(&manager, &mc) != ESY_OK) return 1;
  if (esy_wifi_manager_request_connect(&manager) != ESY_OK) return 2;
  if (esy_wifi_manager_process(&manager) != ESY_OK) return 3;
  if (esy_wifi_manager_deinit(&manager) != ESY_OK) return 4;'''


class WifiConsumerTests(unittest.TestCase):
    def run_case(self, manager=True, wifi=True, provider=True, platform='PICO2_W', sanitizer=''):
        with tempfile.TemporaryDirectory(prefix='wifi consumer ') as directory:
            path = Path(directory)
            features = {'ENABLE_ESY_WIFI_MANAGER': manager, 'ENABLE_ESY_WIFI': wifi}
            (path / 'esy_feature_import.cmake').write_text(''.join(
                f'set({key} {"ON" if value else "OFF"})\n' for key, value in features.items())
                + f'set(ESY_TARGET_DEVICE {"STM32F407ZG" if platform == "STM32F4" else platform})\nset(ESY_RTOS None)\n')
            (path / 'CMakeLists.txt').write_text(PROJECT)
            (path / 'network config').mkdir()
            (path / 'network config/provider_marker.h').write_text('#define PROVIDER_VALUE 42\n')
            (path / 'provider_probe.c').write_text('#include "provider_marker.h"\n'
                '_Static_assert(PROVIDER_VALUE == 42, "provider requirements missing");\n')
            (path / 'dummy.c').write_text('typedef int wifi_disabled_fixture_t;\n')
            body = CONSUMER.replace('MANAGER_INCLUDE',
                '#include "esy_middleware/wifi/esy_wifi_manager.h"' if manager else '')
            body = body.replace('MANAGER_BODY', MANAGER if manager else '')
            for suffix in ('c', 'cpp'):
                (path / f'consumer.{suffix}').write_text(body)
            out = path / 'artifacts'
            command = ['cmake', '-S', str(path), '-B', str(out), f'-DROOT={ROOT}',
                       f'-DPROVIDER={"ON" if provider else "OFF"}', f'-DSANITIZER={sanitizer}']
            result = subprocess.run(command, capture_output=True, text=True)
            valid = (not manager or wifi) and (not wifi or
                    (provider and platform in ('PICO_W', 'PICO2_W')))
            self.assertEqual(valid, result.returncode == 0, result.stdout + result.stderr)
            if not valid:
                expected = ('ENABLE_ESY_WIFI_MANAGER requires' if not wifi else
                            'not implemented' if platform in ('ESP32', 'STM32F4') else
                            'Pico W-class' if platform not in ('PICO_W', 'PICO2_W') else
                            'requires consumer target esy_wifi_provider')
                self.assertIn(expected, result.stderr)
                return
            assembly = (out / 'assembly.txt').read_text()
            self.assertNotIn('/specs', assembly)
            self.assertNotIn('/portable', assembly)
            self.assertEqual(wifi, 'esy_hb_wifi.c' in assembly)
            self.assertEqual(wifi, 'esy_wifi_provider' in assembly)
            result = subprocess.run(['cmake', '--build', str(out), '--parallel', '2'],
                                    capture_output=True, text=True)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            names = ('consumer_c', 'consumer_cpp') if wifi else ()
            if manager:
                names += ('concurrency',)
            for name in names:
                result = subprocess.run([str(out / name)], capture_output=True, text=True, timeout=60)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_c_and_cpp_consumers_with_arbitrary_provider_layout(self):
        self.run_case()

    def test_concurrent_requests_queries_processing_and_pm_under_tsan(self):
        self.run_case(sanitizer='thread')

    def test_memory_and_undefined_behavior_sanitizers(self):
        self.run_case(sanitizer='address,undefined')

    def test_manager_disabled_keeps_direct_platform_api_on_pico_w(self):
        self.run_case(manager=False, platform='PICO_W')

    def test_all_disabled_needs_no_network_provider(self):
        self.run_case(manager=False, wifi=False, provider=False)

    def test_missing_wifi_feature_is_rejected(self):
        self.run_case(wifi=False)

    def test_missing_provider_is_rejected(self):
        self.run_case(provider=False)

    def test_unsupported_platforms_are_rejected(self):
        for platform in ('PICO', 'PICO2', 'STM32F4', 'ESP32'):
            with self.subTest(platform=platform):
                self.run_case(platform=platform)


if __name__ == '__main__':
    unittest.main()
