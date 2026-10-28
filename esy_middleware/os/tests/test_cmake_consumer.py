# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Verify OS target assembly, C/C++ storage ABI and ESP-IDF header selection."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
TESTS = Path(__file__).resolve().parent
PROJECT = '''cmake_minimum_required(VERSION 3.16)
project(os_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
include(esy_feature_import.cmake)
include("${ROOT}/cmake/esy_target_device.cmake")
esy_configure_target_device()
add_library(esy_opt_default INTERFACE)
add_library(esy_common INTERFACE)
add_library(esy_common_defs INTERFACE)
target_include_directories(esy_common_defs INTERFACE "${ROOT}/esy_common/defs/include")
add_library(esy_hal INTERFACE)
target_include_directories(esy_hal INTERFACE "${ROOT}/esy_hal/include")
add_library(native_provider INTERFACE)
target_include_directories(native_provider INTERFACE "${PROVIDER}")
target_compile_definitions(native_provider INTERFACE PRIVATE_NATIVE_PROVIDER)
if(DEFINED NATIVE_CORE_COUNT)
  target_compile_definitions(native_provider INTERFACE configNUMBER_OF_CORES=${NATIVE_CORE_COUNT})
endif()
if(ESY_TARGET_PLATFORM STREQUAL "ESP32")
  target_compile_definitions(native_provider INTERFACE ESY_PLATFORM_ESP32)
endif()
set(FREE_RTOS_KERNEL_LIB native_provider)
add_subdirectory("${ROOT}/esy_middleware/os" os)
get_target_property(port_sources esy_os SOURCES)
file(WRITE "${CMAKE_BINARY_DIR}/sources.txt" "${port_sources}")
add_executable(consumer layout.c consumer.cpp systick.c
  "${ROOT}/esy_test/fake/hal/fake_esy_hal_system.c")
target_link_libraries(consumer PRIVATE esy_os esy_hal)
target_include_directories(consumer PRIVATE "${ROOT}/esy_test/fake/hal"
  "${PROVIDER}" "${PROVIDER}/freertos")
if(EXPECT_TICKLESS_UNSUPPORTED)
  target_compile_definitions(consumer PRIVATE TEST_TICKLESS_UNSUPPORTED)
endif()
if(TARGET esy_os_stm32f4_interrupts)
  target_sources(consumer PRIVATE $<TARGET_OBJECTS:esy_os_stm32f4_interrupts>)
  target_compile_definitions(consumer PRIVATE TEST_STM32_SYSTICK)
  get_target_property(irq_dependencies esy_os_stm32f4_interrupts LINK_LIBRARIES)
  file(WRITE "${CMAKE_BINARY_DIR}/interrupts.txt" "${irq_dependencies}")
endif()
'''
HEADERS = sorted((ROOT / 'esy_middleware/os/include/esy_middleware/os').glob('*.h'))
TYPES = ['task', 'queue', 'mutex', 'semaphore', 'timer', 'stream_buffer',
         'msg_buffer', 'work', 'work_queue', 'delayed_work']
INCLUDES = ''.join(f'#include "esy_middleware/os/{p.name}"\n' for p in HEADERS)
CHECK_PRIVATE = '#ifdef PRIVATE_NATIVE_PROVIDER\n#error "Native provider leaked"\n#endif\n'
SYSTICK_CONSUMER = '''#include <assert.h>
#include "FreeRTOS.h"
#include "task.h"
#include "esy_middleware/os/esy_os.h"
#include "esy_hal/esy_hal_system.h"
#include "fake_esy_hal_system.h"
void SysTick_Handler(void);
typedef struct {
  int scheduler_state;
  unsigned os_ticks;
  unsigned last_hal_tick;
} systick_test_ctx_t;
static systick_test_ctx_t s_ctx;
void vTaskStartScheduler(void) {}
int xTaskGetSchedulerState(void) { return s_ctx.scheduler_state; }
eSleepModeStatus eTaskConfirmSleepModeStatus(void) { return eAbortSleep; }
void vTaskStepTick(TickType_t ticks) { (void)ticks; }
BaseType_t xTaskIncrementTick(void) { return pdFALSE; }
uint32_t os_fixture_tick_core_id(void) { return 0u; }
void os_fixture_tick_isr_lock(void) {}
void os_fixture_tick_isr_unlock(void) {}
void os_fixture_disable_interrupts(void) {}
void os_fixture_yield_from_isr(int should_yield) { (void)should_yield; }
#ifdef TEST_STM32_SYSTICK
/* Deliberately an ordinary native function with no same-named macro. */
void xPortSysTickHandler(void) {
  s_ctx.last_hal_tick = esy_hal_system_get_tick();
  ++s_ctx.os_ticks;
}
int check_systick(void) {
  fake_esy_hal_system_reset();
  SysTick_Handler();
  assert(esy_hal_system_get_tick() == 1u);
  assert(s_ctx.os_ticks == 0u);
  s_ctx.scheduler_state = 1;
  SysTick_Handler();
  assert(esy_hal_system_get_tick() == 2u);
  assert(s_ctx.os_ticks == 1u);
  assert(s_ctx.last_hal_tick == 2u);
  s_ctx.scheduler_state = 2; /* A suspended scheduler still receives ticks. */
  SysTick_Handler();
  assert(esy_hal_system_get_tick() == 3u);
  assert(s_ctx.os_ticks == 2u);
  assert(s_ctx.last_hal_tick == 3u);
  return 0;
}
#else
/* Native tick owners must not need a Cortex-M SysTick handler symbol. */
static esy_err_t tickless_prepare(void *ctx, uint64_t duration) {
  (void)ctx; (void)duration; return ESY_OK;
}
static esy_err_t tickless_enter(void *ctx, esy_os_tickless_result_t *result) {
  (void)ctx; (void)result; return ESY_OK;
}
static esy_err_t tickless_callback(void *ctx) { (void)ctx; return ESY_OK; }
static esy_err_t tickless_clock(void *ctx, uint64_t *now) {
  (void)ctx; *now = 0u; return ESY_OK;
}
int check_native_tick_owner(void) {
  esy_os_init();
  esy_os_systick();
  assert(s_ctx.os_ticks == 0u);
  const esy_os_tickless_ops_t ops = {
    .prepare = tickless_prepare, .enter_atomic = tickless_enter,
    .complete = tickless_callback, .abort = tickless_callback,
    .recover_atomic = tickless_callback, .check_prepare_recovery = tickless_callback,
    .read_clock_atomic = tickless_clock,
  };
#ifdef TEST_TICKLESS_UNSUPPORTED
  assert(esy_os_set_tickless_ops(&ops, NULL) == ESY_ERR_NOT_SUPPORTED);
#else
  assert(esy_os_set_tickless_ops(&ops, NULL) == ESY_OK);
  assert(esy_os_clear_tickless_ops(&ops, NULL) == ESY_OK);
#endif
  return 0;
}
#endif
'''


class OSCMakeConsumerTests(unittest.TestCase):
    def command(self, *args, success=True):
        result = subprocess.run(args, capture_output=True, text=True)
        if success:
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        else:
            self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def consumer(self, enabled=True, overrides=None, error=None, esp=False, missing=False,
                 systick_self_macro=False, native_cores=None, unsupported_tickless=False):
        with tempfile.TemporaryDirectory(prefix='os consumer ') as directory:
            path = Path(directory)
            features = {'ESY_RTOS': 'FreeRTOS', 'ENABLE_ESY_SYSTEM': 'ON',
                        'ESY_TARGET_DEVICE': 'ESP32' if esp else 'PICO'}
            for name in ['QUEUE', 'MUTEX', 'SEMAPHORE', 'TIMER', 'WORK', 'STREAM_BUFFER', 'MSG_BUFFER']:
                features['ENABLE_ESY_OS_' + name] = 'ON' if enabled else 'OFF'
            features.update(overrides or {})
            (path / 'esy_feature_import.cmake').write_text(''.join(f'set({k} {v})\n' for k, v in features.items()))
            provider = path / 'provider'
            target = provider / 'freertos' if esp else provider
            target.mkdir(parents=True)
            if not missing:
                for header in (TESTS / 'freertos').glob('*.h'):
                    (target / header.name).write_text(header.read_text())
                if systick_self_macro:
                    with (target / 'FreeRTOS.h').open('a') as header:
                        header.write('\n#define xPortSysTickHandler xPortSysTickHandler\n')
            (path / 'CMakeLists.txt').write_text(PROJECT)
            (path / 'systick.c').write_text(SYSTICK_CONSUMER)
            c = INCLUDES + CHECK_PRIVATE + 'void get_layout(size_t *out) {\n'
            cpp = INCLUDES + CHECK_PRIVATE + 'extern "C" void get_layout(size_t *);\n#ifdef TEST_STM32_SYSTICK\nextern "C" int check_systick(void);\n#else\nextern "C" int check_native_tick_owner(void);\n#endif\nint main() { size_t layout[30]; get_layout(layout);\n'
            for i, name in enumerate(TYPES):
                t = f'esy_os_{name}_static_storage_t'
                c += f'out[{i*3}] = sizeof({t}); out[{i*3+1}] = _Alignof({t});\nstruct holder_{i} {{ char c; {t} storage; }}; out[{i*3+2}] = offsetof(struct holder_{i}, storage);\n'
                cpp += f'struct holder_{i} {{ char c; {t} storage; }}; if (layout[{i*3}] != sizeof({t}) || layout[{i*3+1}] != alignof({t}) || layout[{i*3+2}] != offsetof(holder_{i}, storage)) return {i+1};\n'
            (path / 'layout.c').write_text(c + '}\n')
            (path / 'consumer.cpp').write_text(cpp + '#ifdef TEST_STM32_SYSTICK\nif (check_systick()) return 99;\n#else\nif (check_native_tick_owner()) return 99;\n#endif\nreturn 0; }\n')
            output = self.command('cmake', '-S', str(path), '-B', str(path / 'build'),
                                  f'-DROOT={ROOT}', f'-DPROVIDER={provider}',
                                  *([f'-DNATIVE_CORE_COUNT={native_cores}'] if native_cores else []),
                                  f'-DEXPECT_TICKLESS_UNSUPPORTED={"ON" if unsupported_tickless else "OFF"}',
                                  success=error is None)
            if error:
                self.assertIn(error, output)
                return
            output = self.command('cmake', '--build', str(path / 'build'), '--parallel', '2', success=not missing)
            self.assertFalse((path / 'build/os/src/freertos/freertos_compat').exists())
            if missing:
                self.assertIn('freertos/FreeRTOS.h', output)
                self.assertNotIn('include nested depth', output)
                return
            self.command(str(path / 'build/consumer'))
            sources = (path / 'build/sources.txt').read_text()
            self.assertEqual(10 if enabled else 3, sources.count('.c'))
            irq_manifest = path / 'build/interrupts.txt'
            self.assertEqual(features['ESY_TARGET_DEVICE'].startswith('STM32F4'), irq_manifest.exists())
            if irq_manifest.exists():
                self.assertEqual({'esy_os', 'esy_hal', 'esy_common_defs', 'esy_opt_default'},
                                 set(irq_manifest.read_text().split(';')))

    def test_all_wrappers_and_c_cpp_layout(self):
        self.consumer()

    def test_optional_wrappers_off(self):
        self.consumer(enabled=False)

    def test_stm32_systick_before_and_after_scheduler_start(self):
        for self_macro in (False, True):
            with self.subTest(self_macro=self_macro):
                self.consumer(enabled=False, overrides={'ESY_TARGET_DEVICE': 'STM32F407ZG'},
                              systick_self_macro=self_macro)

    def test_pico_native_tick_owners_do_not_link_cortex_m_systick(self):
        for platform in ('PICO', 'PICO_W', 'PICO2', 'PICO2_W'):
            with self.subTest(platform=platform):
                self.consumer(enabled=False, overrides={'ESY_TARGET_DEVICE': platform})

    def test_work_requires_queue_and_timer(self):
        for dependency in ['QUEUE', 'TIMER']:
            self.consumer(overrides={'ENABLE_ESY_OS_' + dependency: 'OFF'}, error='ENABLE_ESY_OS_WORK requires')

    def test_platform_system_required(self):
        self.consumer(overrides={'ENABLE_ESY_SYSTEM': 'OFF'}, error='esy_os requires ENABLE_ESY_SYSTEM')

    def test_esp_prefixed_provider(self):
        self.consumer(esp=True)

    def test_unverified_smp_tickless_providers_fail_closed(self):
        for esp in (False, True):
            with self.subTest(esp=esp):
                self.consumer(enabled=False, esp=esp, native_cores=2,
                              unsupported_tickless=True)

    def test_missing_esp_provider_fails_without_recursive_include(self):
        self.consumer(esp=True, missing=True)


if __name__ == '__main__':
    unittest.main()
