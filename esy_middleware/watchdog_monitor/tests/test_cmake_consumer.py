# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Check watchdog target propagation, source isolation and required features."""
from pathlib import Path
import json
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
PROJECT = '''cmake_minimum_required(VERSION 3.20)
project(watchdog_consumer LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
if(PROBE_SANITIZER)
  add_compile_options(-fsanitize=${PROBE_SANITIZER} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${PROBE_SANITIZER})
endif()
include(esy_feature_import.cmake)
foreach(t esy_common esy_common_defs esy_opt_default native_provider)
  add_library(${t} INTERFACE)
endforeach()
target_include_directories(esy_common_defs INTERFACE "${ROOT}/esy_common/defs/include")
target_link_libraries(esy_common INTERFACE esy_common_defs)
add_library(esy_hal STATIC "${ROOT}/esy_test/fake/hal/fake_esy_hal_watchdog.c"
  "${ROOT}/esy_test/fake/hal/fake_esy_hal_system.c")
target_include_directories(esy_hal PUBLIC "${ROOT}/esy_hal/include"
  PRIVATE "${ROOT}/esy_test/fake/hal")
target_link_libraries(esy_hal PUBLIC esy_common_defs)
set(FREE_RTOS_KERNEL_LIB native_provider)
add_subdirectory("${ROOT}/esy_middleware" middleware)
if(TARGET esy_os)
  find_package(Threads REQUIRED)
  # This fixture substitutes only the OS provider, retaining real target interfaces.
  set_property(TARGET esy_os PROPERTY SOURCES "${ROOT}/esy_test/fake/os/fake_esy_os_task.c")
  target_include_directories(esy_os PRIVATE "${ROOT}/esy_test/fake/os")
  set_source_files_properties("${ROOT}/esy_test/fake/os/fake_esy_os_task.c"
    TARGET_DIRECTORY esy_os PROPERTIES
    COMPILE_DEFINITIONS "esy_os_task_enter_critical=esy_test_record_enter;esy_os_task_exit_critical=esy_test_record_exit")
  target_sources(esy_os PRIVATE "${ROOT}/esy_test/fake/os/pthread_esy_os_critical.c")
  target_link_libraries(esy_os PRIVATE Threads::Threads)
endif()
if(TARGET esy_watchdog_monitor_core)
  get_target_property(src esy_watchdog_monitor_core SOURCES)
  if(ESY_RTOS STREQUAL "FreeRTOS")
    get_target_property(adapter_src esy_watchdog_monitor SOURCES)
    string(APPEND src ";${adapter_src}")
  endif()
  file(WRITE "${CMAKE_BINARY_DIR}/sources.txt" "${src}")
  if(ESY_RTOS STREQUAL "FreeRTOS")
    add_executable(concurrency "${ROOT}/esy_middleware/watchdog_monitor/tests/watchdog_concurrency_probe.c")
    target_include_directories(concurrency PRIVATE "${ROOT}/esy_middleware/watchdog_monitor/src"
      "${ROOT}/esy_test/fake/os")
    target_link_libraries(concurrency PRIVATE esy_watchdog_monitor Threads::Threads)
  endif()
  add_executable(consumer_c consumer.c)
  add_executable(consumer_cpp consumer.cpp)
  if(ESY_RTOS STREQUAL "FreeRTOS")
    set(executor_target esy_watchdog_monitor)
  else()
    if(TARGET esy_watchdog_monitor)
      message(FATAL_ERROR "Caller-driven builds must link the core directly")
    endif()
    set(executor_target esy_watchdog_monitor_core)
  endif()
  target_link_libraries(consumer_c PRIVATE ${executor_target})
  target_link_libraries(consumer_cpp PRIVATE ${executor_target})
  add_executable(core_consumer core_consumer.c)
  target_link_libraries(core_consumer PRIVATE esy_watchdog_monitor_core)
else()
  file(WRITE "${CMAKE_BINARY_DIR}/sources.txt" "disabled")
endif()
'''
CONSUMER = '''#include "esy_middleware/watchdog_monitor/esy_watchdog_monitor.h"
static ESY_WATCHDOG_MONITOR_STATIC_STORAGE(storage, 2, 512);
int main(void) {
  esy_watchdog_monitor_static_resources_t resources;
  esy_watchdog_monitor_config_t config;
  esy_watchdog_report_t report;
  ESY_WATCHDOG_MONITOR_BIND_STATIC_RESOURCES(&resources, storage);
  esy_watchdog_monitor_default_cfg(&config);
  config.static_resources = &resources;
  if (esy_watchdog_monitor_init(&config) != ESY_OK) return 1;
  if (esy_watchdog_monitor_register("consumer", &report) != ESY_OK) return 2;
  if (esy_watchdog_monitor_enable_task_check(&report) != ESY_OK) return 3;
  esy_watchdog_monitor_report(&report);
  bool alive = true;
  if (esy_watchdog_monitor_get_task_status(&report, &alive) != ESY_OK || alive) return 4;
  esy_watchdog_monitor_unregister(&report);
  return esy_watchdog_monitor_deinit() == ESY_OK ? 0 : 5;
}
'''
CORE_CONSUMER = '''#include "esy_middleware/watchdog_monitor/esy_watchdog_monitor_core.h"
static ESY_WATCHDOG_MONITOR_CORE_STATIC_STORAGE(storage, 2);
int main(void) {
  esy_watchdog_monitor_core_resources_t resources;
  esy_watchdog_monitor_core_config_t config;
  esy_watchdog_report_t report;
  ESY_WATCHDOG_MONITOR_CORE_BIND_STATIC_RESOURCES(&resources, storage);
  esy_watchdog_monitor_core_default_cfg(&config);
  config.static_resources = &resources;
  if (esy_watchdog_monitor_core_init(&config) != ESY_OK) return 1;
  if (esy_watchdog_monitor_register("consumer", &report) != ESY_OK) return 2;
  if (esy_watchdog_monitor_core_start() != ESY_OK) return 3;
  esy_watchdog_monitor_report(&report);
  if (esy_watchdog_monitor_core_process() != ESY_OK) return 4;
  if (esy_watchdog_monitor_stop() != ESY_OK) return 5;
  return esy_watchdog_monitor_deinit() == ESY_OK ? 0 : 6;
}
'''



class WatchdogConsumerTests(unittest.TestCase):
    def run_case(self, enabled=True, watchdog=True, rtos='FreeRTOS', sanitizer='', system=True):
        with tempfile.TemporaryDirectory(prefix='watchdog consumer ') as directory:
            path = Path(directory)
            features = {'ESY_RTOS': rtos, 'ENABLE_ESY_SYSTEM': 'ON' if system else 'OFF',
                        'ENABLE_ESY_WATCHDOG': 'ON' if watchdog else 'OFF',
                        'ENABLE_ESY_WATCHDOG_MONITOR': 'ON' if enabled else 'OFF'}
            (path / 'esy_feature_import.cmake').write_text(''.join(
                f'set({key} {value})\n' for key, value in features.items()))
            (path / 'CMakeLists.txt').write_text(PROJECT)
            for suffix in ('c', 'cpp'):
                (path / f'consumer.{suffix}').write_text(CORE_CONSUMER if rtos == "None" else CONSUMER)
            (path / "core_consumer.c").write_text(CORE_CONSUMER)
            output = path / 'artifacts'
            result = subprocess.run(['cmake', '-S', str(path), '-B', str(output),
                                     f'-DROOT={ROOT}', '-DESY_TARGET_DEVICE=PICO',
                                     f'-DPROBE_SANITIZER={sanitizer}'], capture_output=True, text=True)
            valid = not enabled or (watchdog and system and rtos in ('FreeRTOS', 'None'))
            self.assertEqual(valid, result.returncode == 0, result.stdout + result.stderr)
            if not valid:
                self.assertIn('esy_watchdog_monitor requires', result.stderr)
                return
            sources = (output / 'sources.txt').read_text()
            if not enabled:
                self.assertEqual('disabled', sources)
                return
            self.assertIn('esy_watchdog_monitor_core.c', sources)
            self.assertEqual(rtos == 'FreeRTOS', 'esy_watchdog_monitor.c' in sources)
            result = subprocess.run(['cmake', '--build', str(output), '--parallel', '2'],
                                    capture_output=True, text=True)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            commands = json.loads((output / 'compile_commands.json').read_text())
            for command in commands:
                if Path(command['file']).name in ('core_consumer.c', 'esy_watchdog_monitor_core.c'):
                    self.assertNotIn('esy_middleware/os/', command['command'])
            if rtos == 'None':
                commands = (output / 'compile_commands.json').read_text()
                self.assertNotIn('esy_middleware/os/', commands)
                self.assertNotIn('FreeRTOS', commands)
            names = ['consumer_c', 'consumer_cpp', 'core_consumer']
            if rtos == 'FreeRTOS':
                names.append('concurrency')
            for name in names:
                self.assertEqual(0, subprocess.run([str(output / name)]).returncode)

    def test_public_c_and_cpp_consumers_link_and_run(self):
        self.run_case()

    def test_thread_sanitized_concurrent_operations(self):
        self.run_case(sanitizer='thread')

    def test_feature_off_needs_neither_os_nor_watchdog(self):
        self.run_case(enabled=False, watchdog=False, rtos='None')

    def test_missing_hardware_dependency_is_rejected(self):
        self.run_case(watchdog=False)

    def test_missing_system_dependency_is_rejected_without_os(self):
        self.run_case(system=False, rtos='None')

    def test_caller_driven_core_links_and_runs_without_os(self):
        self.run_case(rtos='None')

    def test_unsupported_rtos_is_rejected(self):
        self.run_case(rtos='Other')


if __name__ == '__main__':
    unittest.main()
