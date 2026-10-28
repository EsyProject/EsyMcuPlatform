# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Verify public power target linkage, feature isolation and C/C++ object layout."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
PROJECT = '''cmake_minimum_required(VERSION 3.20)
project(power_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
include(esy_feature_import.cmake)
foreach(t esy_common_defs esy_opt_default esy_hal native_provider)
  add_library(${t} INTERFACE)
endforeach()
add_library(esy_common STATIC "${ROOT}/esy_common/device/src/esy_device.c")
target_include_directories(esy_common_defs INTERFACE "${ROOT}/esy_common/defs/include")
target_include_directories(esy_common PUBLIC "${ROOT}/esy_common/device/include")
target_link_libraries(esy_common PUBLIC esy_common_defs)
target_include_directories(esy_hal INTERFACE "${ROOT}/esy_hal/include")
target_include_directories(native_provider INTERFACE "${ROOT}/esy_middleware/os/tests/freertos")
set(FREE_RTOS_KERNEL_LIB native_provider)
add_subdirectory("${ROOT}/esy_middleware" middleware)
set(assembled "")
foreach(t esy_power_manager esy_power_hal_backend esy_power_wakeup_timer_adapter esy_power_freertos_adapter esy_power_watchdog_constraint)
  if(TARGET ${t})
    get_target_property(src ${t} SOURCES)
    string(APPEND assembled "${t}: ${src}\\n")
  endif()
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/sources.txt" "${assembled}")
if(CONSUMER_TARGET)
  add_library(power_native_provider OBJECT
    "${ROOT}/esy_middleware/power_manager/tests/cmake_consumer_provider.c"
    "${ROOT}/esy_test/fake/hal/fake_esy_hal_system.c"
    "${ROOT}/esy_test/fake/hal/fake_esy_hal_watchdog.c")
  target_link_libraries(power_native_provider PRIVATE esy_hal esy_common_defs)
  add_executable(consumer layout.c consumer.cpp $<TARGET_OBJECTS:power_native_provider>)
  target_link_libraries(consumer PRIVATE ${CONSUMER_TARGET})
endif()
'''
SURFACES = {
    'esy_power_manager': ('esy_power_manager.h', ['esy_power_manager_t', 'esy_power_lock_t', 'esy_power_transaction_t']),
    'esy_power_hal_backend': ('esy_power_hal_backend.h', ['esy_power_hal_backend_t']),
    'esy_power_wakeup_timer_adapter': ('esy_power_wakeup_timer_adapter.h', ['esy_power_result_t']),
    'esy_power_freertos_adapter': ('esy_power_tickless_adapter.h', ['esy_power_tickless_adapter_t']),
    'esy_power_watchdog_constraint': ('esy_power_watchdog_constraint.h', ['esy_power_watchdog_constraint_t']),
}
ATOMIC_NEIGHBORS = (
    ('esy_power_manager_t', 'phase'), ('esy_power_manager_t', 'magic'),
    ('esy_power_lock_t', 'usage_count'), ('esy_power_lock_t', 'manager_index'),
)
API_PROBES = {
    'esy_power_manager': 'esy_power_manager_init(nullptr, nullptr) != ESY_ERR_INVALID_ARG',
    'esy_power_hal_backend': 'esy_power_hal_backend_init(nullptr) != ESY_ERR_INVALID_ARG',
    'esy_power_wakeup_timer_adapter': 'esy_power_wakeup_deadline_timer_ops() == nullptr',
    'esy_power_freertos_adapter': 'esy_power_tickless_adapter_init(nullptr, nullptr) != ESY_ERR_INVALID_ARG',
    'esy_power_watchdog_constraint': 'esy_power_watchdog_constraint_init(nullptr, nullptr) != ESY_ERR_INVALID_ARG',
}


class PowerConsumerTests(unittest.TestCase):
    def command(self, *args, success=True):
        result = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(success, result.returncode == 0, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def configure(self, target=None, rtos='None', power=True, low=True, watchdog=False):
        with tempfile.TemporaryDirectory(prefix='power consumer ') as directory:
            path = Path(directory)
            features = {'ESY_RTOS': rtos, 'ENABLE_ESY_SYSTEM': 'ON',
                        'ENABLE_ESY_POWER_MANAGER': 'ON' if power else 'OFF',
                        'ENABLE_ESY_LOW_POWER': 'ON' if low else 'OFF',
                        'ENABLE_ESY_WATCHDOG': 'ON' if watchdog else 'OFF',
                        'ENABLE_ESY_WATCHDOG_MONITOR': 'ON' if watchdog else 'OFF'}
            (path / 'esy_feature_import.cmake').write_text(''.join(f'set({k} {v})\n' for k, v in features.items()))
            (path / 'CMakeLists.txt').write_text(PROJECT)
            if target:
                header, types = SURFACES[target]
                include = f'#include <stddef.h>\n#include "esy_middleware/power_manager/{header}"\n'
                c = include + 'void layout(size_t *out) {\n'
                cpp = include + 'extern "C" void layout(size_t *);\nint main() { size_t v[32]; layout(v);\n'
                for i, typename in enumerate(types):
                    c += f'out[{2*i}] = sizeof({typename}); out[{2*i+1}] = _Alignof({typename});\n'
                    cpp += f'if(v[{2*i}] != sizeof({typename}) || v[{2*i+1}] != alignof({typename})) return {i+1};\n'
                for i, (typename, member) in enumerate(ATOMIC_NEIGHBORS, 2 * len(types)):
                    c += f'out[{i}] = offsetof({typename}, {member});\n'
                    cpp += f'if(v[{i}] != offsetof({typename}, {member})) return {i+1};\n'
                # A real public call forces the linker to extract the target's
                # implementation archive, including its transitive references.
                cpp += f'if ({API_PROBES[target]}) return 100;\n'
                (path / 'layout.c').write_text(c + '}\n')
                (path / 'consumer.cpp').write_text(cpp + 'return 0; }\n')
            valid = not power or low
            output = self.command('cmake', '-S', str(path), '-B', str(path/'artifacts'),
                                  f'-DROOT={ROOT}', '-DESY_TARGET_DEVICE=PICO',
                                  f'-DCONSUMER_TARGET={target or ""}', success=valid)
            if not valid:
                self.assertIn('ENABLE_ESY_POWER_MANAGER requires ENABLE_ESY_LOW_POWER', output)
                return
            sources = (path/'artifacts/sources.txt').read_text()
            self.assertEqual(power, 'esy_power_manager:' in sources)
            self.assertEqual(power and rtos == 'FreeRTOS', 'esy_power_freertos_adapter:' in sources)
            self.assertEqual(power and watchdog, 'esy_power_watchdog_constraint:' in sources)
            if not power:
                self.assertEqual('', sources)
            if target:
                self.command('cmake', '--build', str(path/'artifacts'), '--target', 'consumer', '--parallel', '2')
                self.command(str(path/'artifacts/consumer'))

    def test_every_public_target_propagates_headers_and_c_cpp_layout(self):
        for target in SURFACES:
            with self.subTest(target=target):
                self.configure(target, rtos='FreeRTOS', watchdog=True)

    def test_no_rtos_has_no_tickless_dependency(self):
        self.configure('esy_power_manager')

    def test_watchdog_constraint_without_os_propagates_core_headers(self):
        self.configure('esy_power_watchdog_constraint', rtos='None', watchdog=True)

    def test_disabled_module_has_no_targets(self):
        self.configure(power=False, low=False)

    def test_low_power_dependency_is_required(self):
        self.configure(low=False)

    def test_watchdog_adapter_is_optional(self):
        self.configure('esy_power_freertos_adapter', rtos='FreeRTOS')


if __name__ == '__main__':
    unittest.main()
