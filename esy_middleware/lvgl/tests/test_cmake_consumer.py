# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""LVGL feature closure and optional real-provider rendering/lifecycle smoke."""
from pathlib import Path
import argparse
import runpy
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
cached_provider = runpy.run_path(
    ROOT / "esy_test/utils/esy_provider_paths.py")["cached_provider"]
provider_input = cached_provider("lvgl")
PROVIDER = Path(provider_input).resolve() if provider_input else None
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(lvgl_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
include(esy_feature_import.cmake)
if(NOT ENABLE_ESY_LVGL)
    add_subdirectory("${PLATFORM_ROOT}/esy_middleware" middleware)
    if(TARGET esy_lvgl)
        message(FATAL_ERROR "Disabled feature leaked esy_lvgl")
    endif()
    return()
endif()
add_library(esy_opt_default INTERFACE)
add_subdirectory("${PLATFORM_ROOT}/esy_common" common)
add_library(esy_hal STATIC
    "${PLATFORM_ROOT}/esy_test/fake/hal/fake_esy_hal_system.c")
if(ENABLE_ESY_GPIO)
    target_sources(esy_hal PRIVATE
        "${PLATFORM_ROOT}/esy_test/fake/hal/fake_esy_hal_gpio.c"
        "${PLATFORM_ROOT}/esy_test/fake/hal/fake_esy_hal_gpio_pm.c")
endif()
target_include_directories(esy_hal PUBLIC "${PLATFORM_ROOT}/esy_hal/include"
    "${PLATFORM_ROOT}/esy_test/fake/hal")
target_link_libraries(esy_hal PUBLIC esy_common)
add_library(esy_os STATIC
    "${PLATFORM_ROOT}/esy_test/fake/os/fake_esy_os_task.c"
    "${PLATFORM_ROOT}/esy_test/fake/os/fake_esy_os_queue.c")
target_include_directories(esy_os PUBLIC "${PLATFORM_ROOT}/esy_middleware/os/include"
    "${PLATFORM_ROOT}/esy_test/fake/os")
target_link_libraries(esy_os PUBLIC esy_common)
if(NOT OMIT_LCD)
    add_subdirectory("${PLATFORM_ROOT}/esy_middleware/lcd" lcd)
endif()
if(ENABLE_ESY_TOUCH AND NOT OMIT_TOUCH)
    add_subdirectory("${PLATFORM_ROOT}/esy_middleware/touch" touch)
endif()
if(NOT OMIT_PROVIDER)
    if(REAL_PROVIDER)
        set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h")
        set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
        set(CONFIG_LV_USE_THORVG OFF CACHE BOOL "" FORCE)
        set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
        set(LV_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
        add_subdirectory("${REAL_PROVIDER}" provider)
    else()
        add_library(lvgl STATIC "${PLATFORM_ROOT}/esy_test/fake/third_party/lvgl/fake_lvgl.c")
        if(TEST_PROVIDER_HEADER_DIR)
            target_include_directories(lvgl PUBLIC "${TEST_PROVIDER_HEADER_DIR}")
        endif()
        target_include_directories(lvgl PUBLIC "${PLATFORM_ROOT}/esy_test/fake/third_party/lvgl")
    endif()
endif()
add_subdirectory("${PLATFORM_ROOT}/esy_middleware/lvgl" adapter)
add_executable(consumer "${PLATFORM_ROOT}/esy_middleware/lvgl/tests/integration/consumer.c")
target_link_libraries(consumer PRIVATE esy_lvgl esy_hal)
if(ENABLE_ESY_TOUCH)
    target_link_libraries(consumer PRIVATE esy_lvgl_touch)
    target_compile_definitions(consumer PRIVATE WITH_TOUCH)
elseif(TARGET esy_lvgl_touch OR TARGET esy_touch)
    message(FATAL_ERROR "No-input runtime leaked a touch target")
endif()
if(NOT ENABLE_ESY_GPIO AND (TARGET esy_lcd_device OR TARGET esy_touch_device))
    message(FATAL_ERROR "GPIO-disabled runtime leaked device integration")
endif()
add_library(header_probe OBJECT header_probe.cpp)
target_link_libraries(header_probe PRIVATE esy_lvgl)
# Deliberately retain entire facade objects to expose missing dependencies.
target_link_options(consumer PRIVATE "-Wl,--no-gc-sections")
if(REAL_PROVIDER)
    target_compile_definitions(consumer PRIVATE REAL_LVGL)
endif()
'''
CONFIG = '''#ifndef LV_CONF_H
#define LV_CONF_H
#define LV_COLOR_DEPTH 16
#define LV_USE_OS LV_OS_NONE
#define LV_MEM_SIZE (256 * 1024U)
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_ADR 0
#define LV_DRAW_BUF_STRIDE_ALIGN 1
#define LV_DRAW_BUF_ALIGN 4
#define LV_DRAW_SW_DRAW_UNIT_CNT 1
#define LV_USE_LOG 0
#define LV_USE_ASSERT 1
#define LV_USE_ASSERT_MALLOC 1
#include <stdlib.h>
#define LV_ASSERT_HANDLER abort();
#endif
'''


class ConsumerTests(unittest.TestCase):
    def invoke(self, root, *args, success=True):
        result = subprocess.run(args, capture_output=True, text=True, timeout=180)
        if success:
            self.assertEqual(result.returncode, 0, (result.stdout + result.stderr)[-16000:])
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def project(self, folder, features):
        folder.joinpath('CMakeLists.txt').write_text(PROJECT)
        folder.joinpath('lv_conf.h').write_text(CONFIG)
        folder.joinpath('header_probe.cpp').write_text(
            '#include "esy_middleware/lvgl/esy_lvgl.h"\n'
            'static_assert(sizeof(esy_lcd_t) > 0, "LVGL exposes its LCD dependency");\n')
        values = dict(ENABLE_ESY_LVGL='ON', ESY_RTOS='FreeRTOS', ENABLE_ESY_OS_QUEUE='ON',
                      ENABLE_ESY_SYSTEM='ON', ENABLE_ESY_GPIO='OFF', ENABLE_ESY_TOUCH='OFF')
        values.update(features)
        folder.joinpath('esy_feature_import.cmake').write_text(''.join(
            f'set({key} {value})\n' for key, value in values.items()))

    def configure(self, folder, **kwargs):
        return self.invoke(folder, 'cmake', '-S', str(folder), '-B', str(folder/'out'),
                           '-G', 'Ninja', f'-DPLATFORM_ROOT={ROOT}', **kwargs)

    def test_disabled_feature_needs_no_provider_or_os(self):
        for platform in ('PICO', 'PICO_W', 'PICO2_W'):
            with self.subTest(platform=platform), tempfile.TemporaryDirectory() as tmp:
                folder = Path(tmp)
                self.project(folder, {'ENABLE_ESY_LVGL': 'OFF', 'ESY_RTOS': 'None',
                                      'ESY_TARGET_DEVICE': 'STM32F407ZG' if platform == 'STM32F4' else platform, 'ENABLE_ESY_GPIO': 'OFF'})
                self.configure(folder)

    def test_missing_dependencies_fail_during_configuration(self):
        for feature, value, diagnostic in (
                ('OMIT_PROVIDER', 'ON', 'target lvgl'),
                ('OMIT_LCD', 'ON', 'target esy_lcd'),
                ('ESY_RTOS', 'None', 'ESY_RTOS=FreeRTOS'),
                ('ENABLE_ESY_OS_QUEUE', 'OFF', 'ENABLE_ESY_OS_QUEUE'),
                ('ENABLE_ESY_SYSTEM', 'OFF', 'ENABLE_ESY_SYSTEM')):
            with self.subTest(feature=feature), tempfile.TemporaryDirectory() as tmp:
                folder = Path(tmp)
                self.project(folder, {feature: value})
                self.assertIn(diagnostic, self.configure(folder, success=False))

    def test_supported_platform_configuration(self):
        for platform in ('STM32F4', 'PICO', 'PICO_W', 'PICO2', 'PICO2_W'):
            with self.subTest(platform=platform), tempfile.TemporaryDirectory() as tmp:
                folder = Path(tmp)
                self.project(folder, {'ESY_TARGET_DEVICE': 'STM32F407ZG' if platform == 'STM32F4' else platform})
                self.configure(folder)

    def build_run(self, provider=None, with_touch=False):
        with tempfile.TemporaryDirectory(prefix='lvgl-consumer-') as tmp:
            folder = Path(tmp)
            features = {'ENABLE_ESY_TOUCH': 'ON'} if with_touch else {}
            if provider:
                features['REAL_PROVIDER'] = f'"{provider}"'
            self.project(folder, features)
            self.configure(folder)
            self.invoke(folder, 'cmake', '--build', str(folder/'out'), '-j', '4')
            self.invoke(folder, str(folder/'out/consumer'))

    def test_fake_provider_links_and_runs(self):
        self.build_run()

    def test_touch_adapter_links_without_gpio(self):
        self.build_run(with_touch=True)

    def test_enabled_touch_adapter_requires_touch_target(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            self.project(folder, {'ENABLE_ESY_TOUCH':'ON', 'OMIT_TOUCH':'ON'})
            self.assertIn('target esy_touch', self.configure(folder, success=False))

    def test_unsupported_provider_configuration_fails_compilation(self):
        header = (ROOT/'esy_test/fake/third_party/lvgl/lvgl.h').read_text()
        for before, after, diagnostic in (
                ('LVGL_VERSION_MINOR 6', 'LVGL_VERSION_MINOR 5', 'verified LVGL 9.6.0'),
                ('LVGL_VERSION_PATCH 0', 'LVGL_VERSION_PATCH 1', 'verified LVGL 9.6.0'),
                ('LV_USE_OS LV_OS_NONE', 'LV_USE_OS 1', 'fixed built-in memory pool'),
                ('LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN', 'LV_USE_STDLIB_MALLOC 1',
                 'fixed built-in memory pool'),
                ('LV_MEM_ADR 0', 'LV_MEM_ADR 4096', 'fixed built-in memory pool'),
                ('LV_COLOR_DEPTH 16', 'LV_COLOR_DEPTH 32', 'RGB565, packed stride'),
                ('LV_DRAW_BUF_STRIDE_ALIGN 1', 'LV_DRAW_BUF_STRIDE_ALIGN 8',
                 'RGB565, packed stride'),
                ('LV_USE_DRAW_SW 1', 'LV_USE_DRAW_SW 0', 'RGB565, packed stride'),
                ('LV_DRAW_SW_DRAW_UNIT_CNT 1', 'LV_DRAW_SW_DRAW_UNIT_CNT 2',
                 'RGB565, packed stride')):
            with self.subTest(config=after), tempfile.TemporaryDirectory() as tmp:
                folder = Path(tmp)
                self.assertIn(before, header)
                folder.joinpath('lvgl.h').write_text(header.replace(before, after))
                self.project(folder, {'TEST_PROVIDER_HEADER_DIR': f'"{folder}"'})
                self.configure(folder)
                output = self.invoke(folder, 'cmake', '--build', str(folder/'out'),
                                     '--target', 'esy_lvgl', '-j', '4', success=False)
                self.assertIn(diagnostic, output)

    def test_real_lvgl_provider(self):
        if PROVIDER is None:
            self.skipTest('Run ./setup.sh or pass --lvgl-path for the real pinned provider')
        self.build_run(PROVIDER)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--lvgl-path', type=Path)
    args, remaining = parser.parse_known_args()
    if args.lvgl_path:
        PROVIDER = args.lvgl_path.resolve()
    unittest.main(argv=[__file__, *remaining])
