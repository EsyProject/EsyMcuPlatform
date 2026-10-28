# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Link real core-lockout engines and verify consumer-owned feature gates."""
from pathlib import Path
import json
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
PROJECT = r'''
cmake_minimum_required(VERSION 3.16)
project(core_lockout_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(esy_feature_import.cmake)
add_library(esy_opt_default INTERFACE)
target_compile_definitions(esy_opt_default INTERFACE PRIVATE_OPTIONS_SENTINEL)
add_subdirectory("${PLATFORM_ROOT}/esy_common/defs" common_defs)
if(PROVIDE_COMMON)
    add_library(esy_common INTERFACE)
    target_link_libraries(esy_common INTERFACE esy_common_defs)
endif()
if(PROVIDE_PLATFORM)
    add_library(esy_hal STATIC
        "${PLATFORM_ROOT}/esy_test/fake/hal/fake_esy_hal_system.c")
    target_include_directories(esy_hal PUBLIC "${PLATFORM_ROOT}/esy_hal/include")
    target_link_libraries(esy_hal PUBLIC esy_common_defs)
endif()
if(PROVIDE_OS)
    add_library(esy_os STATIC
        "${PLATFORM_ROOT}/esy_test/fake/os/fake_esy_os.c"
        "${PLATFORM_ROOT}/esy_test/fake/os/fake_esy_os_task.c")
    target_include_directories(esy_os PUBLIC "${PLATFORM_ROOT}/esy_middleware/os/include")
    target_link_libraries(esy_os PUBLIC esy_common_defs)
endif()
if(ENABLE_ESY_CORE_LOCKOUT)
    add_subdirectory("${PLATFORM_ROOT}/esy_middleware/core_lockout" core_lockout)
    set(executors esy_core_lockout_core)
    if(ESY_RTOS STREQUAL "FreeRTOS")
        list(APPEND executors esy_core_lockout)
    elseif(TARGET esy_core_lockout)
        message(FATAL_ERROR "Caller-driven builds must link the core directly")
    endif()
    foreach(target IN LISTS executors)
        foreach(language IN ITEMS c cpp)
            add_executable(${target}_${language} ${target}.${language})
            target_include_directories(${target}_${language} PRIVATE
                "${PLATFORM_ROOT}/esy_test/fake/hal")
            target_link_libraries(${target}_${language} PRIVATE ${target} esy_hal)
        endforeach()
    endforeach()
else()
    add_subdirectory("${PLATFORM_ROOT}/esy_middleware" middleware)
    if(TARGET esy_core_lockout OR TARGET esy_core_lockout_core)
        message(FATAL_ERROR "Disabled module assembled")
    endif()
endif()
'''
CORE = r'''
#include "esy_middleware/core_lockout/esy_core_lockout_core.h"
#include "fake_esy_hal_system.h"
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private options leaked"
#endif
static void operation(void *arg) {
    unsigned *count = (unsigned *)arg;
    if (fake_esy_hal_system_get_ctx()->irq_status == 1u) ++*count;
}
int main(void) {
    fake_esy_hal_system_reset();
    unsigned count = 0;
    if (esy_core_lockout_core_init() != ESY_OK) return 1;
    if (esy_core_lockout_execute(operation, &count) != ESY_OK) return 2;
    if (count != 1 || fake_esy_hal_system_get_ctx()->irq_status != 0) return 3;
    return (int)esy_core_lockout_core_init();
}
'''
OS = CORE.replace('esy_core_lockout_core.h', 'esy_core_lockout.h').replace(
    'esy_core_lockout_core_init()', 'esy_core_lockout_init()')

class CoreLockoutConsumerTests(unittest.TestCase):
    def check_configuration(self, overrides=None, error=None):
        flags = dict(ENABLE_ESY_CORE_LOCKOUT='ON', ENABLE_ESY_FLASH='OFF', ENABLE_ESY_SYSTEM='ON',
                     PROVIDE_OS='ON', PROVIDE_COMMON='ON', PROVIDE_PLATFORM='ON', ESY_RTOS='FreeRTOS')
        flags.update(overrides or {})
        with tempfile.TemporaryDirectory(prefix='core lockout consumer ') as d:
            project = Path(d)
            (project / 'CMakeLists.txt').write_text(PROJECT)
            (project / 'esy_feature_import.cmake').write_text(
                ''.join(f'set({key} "{value}")\n' for key, value in flags.items()))
            targets = ('esy_core_lockout_core',)
            if flags['ESY_RTOS'] == 'FreeRTOS':
                targets += ('esy_core_lockout',)
            for target in targets:
                content = OS if target == 'esy_core_lockout' and flags['ESY_RTOS'] == 'FreeRTOS' else CORE
                for extension in ('c', 'cpp'):
                    (project / f'{target}.{extension}').write_text(content)
            result = subprocess.run(['cmake', '-S', str(project), '-B', str(project / 'out'),
                                     f'-DPLATFORM_ROOT={ROOT}'], capture_output=True, text=True)
            if error:
                self.assertNotEqual(0, result.returncode)
                self.assertIn(error, ' '.join((result.stdout + result.stderr).split()))
                return
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            result = subprocess.run(['cmake', '--build', str(project / 'out'), '-j2'], capture_output=True, text=True)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            commands = json.loads((project / 'out/compile_commands.json').read_text())
            for item in commands:
                if Path(item['file']).name in ('esy_core_lockout_core.c', 'esy_core_lockout_core.cpp'):
                    self.assertNotIn('esy_middleware/os/include', item['command'])
            if flags['ESY_RTOS'] == 'None' and flags['PROVIDE_OS'] == 'OFF':
                self.assertNotIn('esy_middleware/os', json.dumps(commands))
                self.assertFalse(any(Path(item['file']).name == 'esy_core_lockout.c' and
                                     Path(item['file']).parent.name == 'src' for item in commands))
            if flags['ENABLE_ESY_CORE_LOCKOUT'] == 'ON':
                for target in targets:
                    for language in ('c', 'cpp'):
                        self.assertEqual(0, subprocess.run([str(project / f'out/{target}_{language}')]).returncode)

    def test_public_c_and_cpp_consumers_and_core_under_freertos(self):
        self.check_configuration()

    def test_core_links_without_os(self):
        self.check_configuration(dict(ESY_RTOS='None', PROVIDE_OS='OFF'))

    def test_disabled_module_needs_no_os_or_flash(self):
        self.check_configuration(dict(ENABLE_ESY_CORE_LOCKOUT='OFF', ENABLE_ESY_FLASH='OFF',
                                      ENABLE_ESY_SYSTEM='OFF', PROVIDE_OS='OFF', ESY_RTOS='None'))

    def test_enabled_core_and_os_adapter_need_no_flash(self):
        self.check_configuration(dict(ENABLE_ESY_FLASH='OFF'))
        self.check_configuration(dict(ENABLE_ESY_FLASH='OFF', ESY_RTOS='None', PROVIDE_OS='OFF'))

    def test_missing_system_is_rejected(self):
        self.check_configuration(dict(ENABLE_ESY_SYSTEM='OFF'), 'requires ENABLE_ESY_SYSTEM')

    def test_missing_os_in_freertos_is_rejected(self):
        self.check_configuration(dict(PROVIDE_OS='OFF'), 'requires the esy_os target')

    def test_missing_core_dependencies_are_rejected(self):
        for provider, target in (('PROVIDE_PLATFORM', 'esy_hal'), ('PROVIDE_COMMON', 'esy_common')):
            with self.subTest(provider=provider):
                self.check_configuration({provider: 'OFF'}, f'requires the {target} target')

    def test_unsupported_rtos_is_rejected(self):
        self.check_configuration(dict(ESY_RTOS='Unsupported'), 'requires ESY_RTOS=None or ESY_RTOS=FreeRTOS')

if __name__ == '__main__':
    unittest.main()
