# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Exercise real platform feature assembly and link without disabled providers."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PROJECT = r'''cmake_minimum_required(VERSION 3.20)
project(platform_consumer C)
set(CMAKE_C_STANDARD 11)
if(NOT DEFINED ESY_TARGET_DEVICE)
  set(ESY_TARGET_DEVICE PICO2_W)
endif()
include(esy_feature_import.cmake)
foreach(t esy_opt_default esy_board pico_time pico_stdio pico_i2c_slave
          hardware_timer hardware_pio hardware_dma hardware_flash hardware_uart
          hardware_spi hardware_i2c hardware_pwm hardware_gpio hardware_clocks
          hardware_irq hardware_sync STM32_Drivers)
  add_library(${t} INTERFACE)
endforeach()
add_library(hardware_watchdog INTERFACE)
if(NOT OMIT_PICO_MULTICORE)
  add_library(pico_multicore INTERFACE)
endif()
if(NOT OMIT_PICO_RAND)
  add_library(pico_rand INTERFACE)
endif()
function(pico_generate_pio_header)
endfunction()
add_subdirectory("${ROOT}/esy_common" common)
add_subdirectory("${ROOT}/esy_hal_backend" driver)
add_subdirectory("${ROOT}/esy_hal" platform)
get_target_property(ds esy_hal_backend SOURCES)
get_target_property(ps esy_hal SOURCES)
get_target_property(deps esy_hal_backend LINK_LIBRARIES)
file(WRITE "${CMAKE_BINARY_DIR}/assembly.txt" "${ds}\n${ps}\n${deps}\nENABLE_ESY_DMA=${ENABLE_ESY_DMA}\n")
if(USE_FAKE)
  set_property(TARGET esy_hal_backend PROPERTY SOURCES "${ROOT}/esy_hal/tests/hal_test_fakes.c")
  if(ENABLE_ESY_CAN)
    target_sources(esy_hal_backend PRIVATE "${ROOT}/esy_test/fake/hal/fake_esy_hal_can_provider.c")
    target_include_directories(esy_hal_backend PRIVATE "${ROOT}/esy_test/fake/hal")
  endif()
  target_include_directories(esy_hal_backend PRIVATE "${ROOT}/esy_hal/tests" "${ROOT}/esy_hal/include" "${UNITY}")
  target_compile_definitions(esy_hal_backend PRIVATE OTA_SLOT_A_ADDR=0x10000000u OTA_SLOT_B_ADDR=0x10010000u OTA_SLOT_A_META_ADDR=0x1000F000u OTA_SLOT_B_META_ADDR=0x1001F000u OTA_SLOT_TOTAL_SIZE=0x10000u)
  if(NOT ENABLE_ESY_DMA)
    target_compile_definitions(esy_hal_backend PRIVATE esy_hal_dma_is_busy=disabled_generic_dma_is_busy)
  endif()
  foreach(direction tx rx)
    string(TOUPPER "${direction}" upper)
    if(NOT ENABLE_ESY_XIO_UART_${upper})
      foreach(api init deinit get_pin read_blocking read_dma write_blocking write_dma is_readable is_busy)
        target_compile_definitions(esy_hal_backend PRIVATE "esy_hb_xio_uart_${direction}_${api}=disabled_${direction}_${api}")
      endforeach()
    endif()
  endforeach()
endif()
target_include_directories(esy_board INTERFACE "${ROOT}/esy_hal/tests")
add_executable(consumer consumer.c "${UNITY}/unity.c")
target_include_directories(consumer PRIVATE "${UNITY}")
if(ENABLE_ESY_CAN)
  target_include_directories(consumer PRIVATE "${ROOT}/esy_test/fake/hal")
endif()
target_link_libraries(consumer PRIVATE esy_hal)
'''


class FeatureAssembly(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        gem = subprocess.check_output(
            ['ruby', '-e', 'puts Gem::Specification.find_by_name("ceedling").full_gem_path'],
            text=True).strip()
        cls.unity = Path(gem) / 'vendor/unity/src'

    def build(self, features, source='int main(void) { return 0; }',
              error=None, expected=(), absent=(), fake=True, platform='PICO2_W',
              omit_pico_multicore=False, omit_pico_rand=False):
        with tempfile.TemporaryDirectory(prefix='esy-platform-cmake-') as temp:
            root = Path(temp)
            (root / 'CMakeLists.txt').write_text(PROJECT)
            (root / 'esy_feature_import.cmake').write_text(features)
            (root / 'consumer.c').write_text(source + '\nvoid setUp(void) {} void tearDown(void) {}\n')
            args = ['cmake', '-S', str(root), '-B', str(root / 'out'),
                    f'-DROOT={ROOT}', f'-DUNITY={self.unity}',
                    f'-DESY_TARGET_DEVICE={"STM32F407ZG" if platform == "STM32F4" else platform}',
                    f'-DOMIT_PICO_MULTICORE={"ON" if omit_pico_multicore else "OFF"}',
                    f'-DOMIT_PICO_RAND={"ON" if omit_pico_rand else "OFF"}',
                    f'-DUSE_FAKE={"ON" if fake else "OFF"}']
            result = subprocess.run(args, text=True, capture_output=True)
            if error:
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(error, result.stdout + result.stderr)
                return
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            assembly = (root / 'out/assembly.txt').read_text()
            for item in expected:
                self.assertIn(item, assembly)
            for item in absent:
                self.assertNotIn(item, assembly)
            result = subprocess.run(['cmake', '--build', str(root / 'out'),
                                     '--target', 'consumer', '-j2'],
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(root / 'out/consumer')], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_all_disabled_targets_build_without_placeholder_api(self):
        self.build('', fake=False, omit_pico_rand=True,
                   expected=('esy_hal_empty.c', 'esy_hb_empty.c'))

    def test_timer_requires_system(self):
        self.build('set(ENABLE_ESY_TIMER ON)', error='ENABLE_ESY_TIMER requires ENABLE_ESY_SYSTEM')

    def test_multicore_links_its_sdk_provider(self):
        self.build('set(ENABLE_ESY_MULTICORE ON)',
                   '#include "esy_hal/esy_hal_multicore.h"\n'
                   'int main(void) { esy_multicore_queue_t q = {0}; uint8_t b[8]; '
                   'return esy_hal_multicore_queue_init(&q, 4u, 1u, b, sizeof(b)); }',
                   expected=('esy_hb_multicore.c', 'pico_multicore'))

    def test_multicore_rejects_missing_sdk_provider(self):
        self.build('set(ENABLE_ESY_MULTICORE ON)',
                   error='ENABLE_ESY_MULTICORE requires the pico_multicore SDK target',
                   omit_pico_multicore=True)

    def test_timer_with_system_links(self):
        self.build('set(ENABLE_ESY_TIMER ON)\nset(ENABLE_ESY_SYSTEM ON)',
                   '#include "esy_hal/esy_hal_timer.h"\nint main(void) {return esy_hal_timer_init();}',
                   expected=('esy_hb_system.c', 'pico_rand'))

    def test_system_rejects_missing_random_sdk_provider(self):
        self.build('set(ENABLE_ESY_SYSTEM ON)', omit_pico_rand=True,
                   error='Pico system random source requires the pico_rand SDK target')

    def test_peripheral_apis_link_without_generic_dma(self):
        calls = {
            'UART': 'esy_hal_uart_send_blocking(NULL, NULL, 1u)',
            'SPI': 'esy_hal_spi_write_blocking(NULL, NULL, 1u)',
            'I2C': 'esy_hal_i2c_write_blocking(NULL, 0x50u, NULL, 1u)',
            'PWM': 'esy_hal_pwm_set_duty(NULL, 50.0f)',
            'ADC': 'esy_hal_adc_sample_once(NULL)',
        }
        for peripheral, call in calls.items():
            with self.subTest(peripheral=peripheral):
                name = peripheral.lower()
                self.build(f'set(ENABLE_ESY_{peripheral} ON)\n'
                           'set(ENABLE_ESY_COMMON_DS ON)\nset(ENABLE_ESY_DMA OFF)',
                           f'#include "esy_hal/esy_hal_{name}.h"\n'
                           f'int main(void) {{ return {call} == ESY_ERR_INVALID_ARG ? 0 : 1; }}',
                           expected=(f'esy_hal_{name}.c', 'ENABLE_ESY_DMA=OFF'),
                           absent=('esy_hal_dma.c',),
                           platform='STM32F4' if peripheral == 'ADC' else 'PICO2_W')

    def test_generic_dma_api_is_assembled_only_when_selected(self):
        self.build('set(ENABLE_ESY_DMA ON)',
                   '#include "esy_hal/esy_hal_dma.h"\n'
                   'int main(void) { esy_dma_config_t c; esy_hal_dma_default_cfg(&c); return 0; }',
                   expected=('esy_hal_dma.c', 'esy_hb_dma.c'))

    def test_uart_and_capture_status_lifecycles_link_through_public_target(self):
        self.build('set(ENABLE_ESY_UART ON)\nset(ENABLE_ESY_TIMER_CAPTURE ON)\n'
                   'set(ENABLE_ESY_COMMON_DS ON)\nset(ENABLE_ESY_DMA OFF)\n'
                   'set(ENABLE_ESY_GPIO OFF)',
                   '#include "esy_hal/esy_hal_uart.h"\n'
                   '#include "esy_hal/esy_hal_timer_capture.h"\n'
                   'int main(void) { esy_uart_t u = {0}; esy_uart_config_t uc;'
                   'esy_timer_capture_t c = {0}; esy_timer_capture_config_t cc;'
                   'esy_hal_uart_default_cfg(&uc); uc.phys_index = 4u;'
                   'esy_hal_timer_capture_default_cfg(&cc); cc.phys_index = 8u;'
                   'cc.inner.gpio = 21u;'
                   'if (esy_hal_uart_init(&u, &uc)) return 1;'
                   'if (esy_hal_timer_capture_init(&c, &cc)) return 2;'
                   'if (esy_hal_uart_deinit(&u)) return 3;'
                   'return esy_hal_timer_capture_deinit(&c); }',
                   expected=('esy_hal_uart.c', 'esy_hal_timer_capture.c',
                             'esy_hb_gpio.c', 'ENABLE_ESY_DMA=OFF'),
                   absent=('esy_hal_dma.c', 'esy_hal_gpio.c'))

    def test_spi_status_lifecycle_links_without_the_generic_dma_facade(self):
        self.build('set(ENABLE_ESY_SPI ON)\nset(ENABLE_ESY_DMA OFF)',
                   '#include "esy_hal/esy_hal_spi.h"\n'
                   'int main(void) { esy_spi_t s = {0}; esy_spi_config_t c;'
                   'esy_hal_spi_default_cfg(&c); c.phys_index = 3u;'
                   'if (esy_hal_spi_init(&s, &c)) return 1;'
                   'return esy_hal_spi_deinit(&s); }',
                   expected=('esy_hal_spi.c', 'esy_hb_spi.c', 'ENABLE_ESY_DMA=OFF'),
                   absent=('esy_hal_dma.c',))

    def test_xio_spi_generic_io_links_without_native_spi_or_generic_dma(self):
        self.build('set(ENABLE_ESY_XIO ON)\nset(ENABLE_ESY_XIO_SPI ON)\n'
                   'set(ENABLE_ESY_SPI OFF)\nset(ENABLE_ESY_DMA OFF)',
                   '#include "esy_hal/xio/esy_hal_xio_spi.h"\n'
                   '#include "esy_hal/esy_hal_spi.h"\n'
                   'int main(void) { esy_spi_t s = {0}; esy_xio_spi_config_t c;'
                   'uint8_t tx[2] = {1u, 2u}, rx[2];'
                   'esy_dma_config_t d = {.src=tx, .length=sizeof(tx),'
                   '.transfer_size=ESY_DMA_TRANSFER_SIZE_8};'
                   'esy_hal_xio_spi_default_cfg(&c); c.instance=0u; c.sm_index=1u;'
                   'c.pin_sck=10u; c.pin_mosi=11u; c.pin_miso=12u;'
                   'if (esy_hal_xio_spi_init(&s, &c)) return 1;'
                   'if (esy_hal_spi_write_blocking(&s, tx, 2u) != 2) return 2;'
                   'if (esy_hal_spi_read_blocking(&s, rx, 2u) != 2) return 3;'
                   'if (esy_hal_spi_transfer_blocking(&s, tx, rx, 2u) != 2) return 4;'
                   'if (esy_hal_spi_transfer_dma(&s, &d, 0, 0, 0)) return 5;'
                   'if (esy_hal_xio_spi_transfer_dma(&s, &d, 0, 0, 0)) return 6;'
                   'if (esy_hal_spi_poll(&s) || esy_hal_spi_abort(&s)) return 7;'
                   'esy_hal_xio_spi_deinit(&s); return 0; }',
                   expected=('esy_hal_xio_spi.c', 'esy_hal_spi_io.c',
                             'esy_hb_dma.c', 'ENABLE_ESY_DMA=OFF'),
                   absent=('/esy_hal_spi.c', 'esy_hb_spi.c', 'esy_hal_dma.c'))

    def test_single_direction_uart_links_without_disabled_driver(self):
        for direction in ('TX', 'RX'):
            with self.subTest(direction=direction):
                other = 'rx' if direction == 'TX' else 'tx'
                args = '&c, 0' if direction == 'TX' else '0, &c'
                self.build('set(ENABLE_ESY_XIO ON)\nset(ENABLE_ESY_DMA OFF)\n'
                           f'set(ENABLE_ESY_XIO_UART_{direction} ON)',
                           '#include "esy_hal/xio/esy_hal_xio_uart.h"\n'
                           'int main(void) { esy_uart_t u = {0}; esy_xio_uart_config_t c = {.pin=11};'
                           f'int r = esy_hal_xio_uart_init(&u, {args});'
                           'esy_hal_xio_uart_deinit(&u); if(r) return r;'
                           'r = esy_hal_xio_uart_init(&u, &c, &c);'
                           'return r == ESY_ERR_NOT_SUPPORTED && !u.ctx[0] && !u.ctx[1] ? 0 : 1; }',
                           expected=(f'esy_hal_xio_uart_{other}_disabled.c',
                                     'esy_hb_dma.c', 'ENABLE_ESY_DMA=OFF'),
                           absent=(f'esy_hb_xio_uart_{other}.c', 'esy_hal_uart.c',
                                   'esy_hal_dma.c'))

    def test_flash_geometry_queries_link_through_public_target(self):
        self.build('set(ENABLE_ESY_FLASH ON)',
                   '#include "esy_hal/esy_hal_flash.h"\n'
                   'int main(void) { esy_flash_geometry_t g; esy_flash_sector_t s;'
                   'if (esy_hal_flash_get_geometry(&g)) return 1;'
                   'if (g.base_address != 0x1000u || g.size_bytes != 4096u ||'
                   'g.program_size != 4u) return 2;'
                   'if (esy_hal_flash_get_sector(g.base_address + 1u, &s)) return 3;'
                   'return s.start_address == g.base_address &&'
                   's.size_bytes == 4096u ? 0 : 4; }',
                   expected=('esy_hal_flash.c', 'esy_hb_flash.c'))

    def test_adc_rejects_missing_backend(self):
        self.build('set(ENABLE_ESY_ADC ON)\nset(ENABLE_ESY_DMA ON)',
                   error='ENABLE_ESY_ADC has no backend')

    def test_can_links_without_os_system_or_dma(self):
        self.build('set(ENABLE_ESY_CAN ON)\nset(ESY_RTOS None)\n'
                   'set(ENABLE_ESY_SYSTEM OFF)\nset(ENABLE_ESY_DMA OFF)',
                   '#include "esy_hal/esy_hal_can.h"\n'
                   '#include "fake_esy_hal_can_provider.h"\n'
                   'int main(void) { esy_can_t can = {0}; esy_can_config_t config;'
                   'esy_can_filter_t filter = {.id=0x123u, .mask=ESY_CAN_STD_ID_MAX};'
                   'esy_can_frame_t frame = {.id=0x123u, .length=1u, .data={0x55u}};'
                   'fake_esy_hal_can_provider_reset(); esy_hal_can_default_cfg(&config);'
                   'config.phys_index=1u; if(esy_hal_can_init(&can, &config)) return 1;'
                   'if(esy_hal_can_set_filters(&can, &filter, 1u)) return 2;'
                   'if(esy_hal_can_start(&can)) return 3;'
                   'if(esy_hal_can_send(&can, &frame)) return 4;'
                   'if(esy_hal_can_stop(&can)) return 5;'
                   'return esy_hal_can_deinit(&can); }',
                   expected=('esy_hal_can.c', 'esy_hb_can.c', 'ENABLE_ESY_DMA=OFF'),
                   absent=('esy_hal_system.c', 'esy_hal_dma.c', 'esy_hb_dma.c'),
                   platform='STM32F4')

    def test_can_disabled_omits_wrapper_and_backend(self):
        self.build('set(ENABLE_ESY_CAN OFF)\nset(ESY_RTOS None)',
                   fake=False, platform='STM32F4',
                   expected=('esy_hal_empty.c', 'esy_hb_empty.c'),
                   absent=('esy_hal_can.c', 'esy_hb_can.c'))

    def test_can_rejects_unsupported_backends(self):
        for platform in ('PICO2_W', 'ESP32'):
            with self.subTest(platform=platform):
                self.build('set(ENABLE_ESY_CAN ON)', platform=platform,
                           error='ENABLE_ESY_CAN has no backend')
        self.build('set(ENABLE_ESY_CAN ON)', platform='STM32F401RE',
                   error='ENABLE_ESY_CAN requires a verified STM32F405/407/415/417')


if __name__ == '__main__':
    unittest.main()
