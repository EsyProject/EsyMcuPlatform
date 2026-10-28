# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Prevent hardware dependency bypasses across HAL boundaries."""

import importlib.util
from pathlib import Path
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location(
    'hardware_boundaries', Path(__file__).with_name('check_hardware_boundaries.py'))
BOUNDARIES = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BOUNDARIES)


class HardwareBoundaries(unittest.TestCase):
    def inspect(self, files):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, source in files.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(source)
            return BOUNDARIES.check(root)

    def test_middleware_uses_hal_while_hal_assembles_board_and_backend(self):
        self.assertEqual([], self.inspect({
            'esy_middleware/bootloader/src/boot.c':
                '#include "esy_hal/esy_hal_boot.h"\nvoid boot(void) { esy_hal_boot_get_layout(0); }',
            'esy_hal/src/boot.c':
                '#include "esy_board/esy_board.h"\n#include "esy_hal_backend/esy_hb_system.h"\n',
            'esy_hal/include/esy_hal/esy_hal_boot.h':
                '#include "esy_common/defs/image/esy_boot_defs.h"\n',
        }))

    def test_native_rtos_is_confined_to_the_os_port(self):
        for source in ('#include "freertos/task.h"', '#include "FreeRTOS.h"',
                       'void start(void) { xTaskCreate(0); }'):
            with self.subTest(source=source):
                self.assertTrue(self.inspect({'esy_hal_backend/src/esp32/uart.c': source}))
                self.assertEqual([], self.inspect({
                    'esy_middleware/os/src/freertos/port/esp32/context.c': source}))

    def test_middleware_cannot_import_backend_or_board_headers(self):
        for header in ('esy_hal_backend/esy_hb_flash.h', 'esy_board/esy_board.h',
                       'esy_common/defs/board/esy_board_defs.h'):
            with self.subTest(header=header):
                self.assertTrue(self.inspect({
                    'esy_middleware/bootloader/src/boot.c': f'#include "{header}"\n'}))

    def test_middleware_cannot_call_lower_api_without_including_a_header(self):
        for function in ('esy_hb_flash_read', 'esy_board_get_bootloader_layout'):
            with self.subTest(function=function):
                self.assertTrue(self.inspect({
                    'esy_middleware/bootloader/src/boot.c': f'void boot(void) {{ {function}(0); }}'}))

    def test_hal_public_header_cannot_export_board_or_backend_dependencies(self):
        self.assertTrue(self.inspect({
            'esy_hal/include/esy_hal/esy_hal_boot.h': '#include "esy_board/esy_board.h"'}))

    def test_backend_cannot_depend_on_public_hal(self):
        for source in ('#include "esy_hal/esy_hal_system.h"',
                       'void operation(void) { esy_hal_system_reset(); }'):
            with self.subTest(source=source):
                self.assertTrue(self.inspect({'esy_hal_backend/src/pico/system.c': source}))

    def test_middleware_cannot_link_board_or_backend_targets(self):
        for target in ('esy_board', 'esy_hal_backend'):
            with self.subTest(target=target):
                self.assertTrue(self.inspect({
                    'esy_middleware/bootloader/CMakeLists.txt':
                        f'target_link_libraries(esy_bootloader PRIVATE {target})'}))

    def test_comments_and_test_doubles_are_not_production_dependencies(self):
        self.assertEqual([], self.inspect({
            'esy_middleware/bootloader/src/boot.c': '/* esy_hb_flash_read(0); */\n',
            'esy_middleware/bootloader/tests/fake.c': '#include "esy_board/esy_board.h"',
        }))

    def test_common_cannot_depend_on_upper_layers_in_source_or_cmake(self):
        for header in ('esy_hal/esy_hal_spi.h', 'esy_middleware/os/esy_os.h',
                       'esy_hal_backend/esy_hb_dma.h', 'esy_board/esy_board.h'):
            with self.subTest(header=header):
                self.assertTrue(self.inspect({'esy_common/utils/src/util.c': f'#include "{header}"'}))
        for function in ('esy_hal_gpio_write', 'esy_hb_dma_start', 'esy_os_started'):
            with self.subTest(function=function):
                self.assertTrue(self.inspect({'esy_common/utils/src/util.c': f'void f(void) {{ {function}(); }}'}))
        for target in ('esy_hal', 'esy_logger', 'hardware_spi', 'pico_stdlib', 'idf::driver'):
            with self.subTest(target=target):
                self.assertTrue(self.inspect({'esy_common/utils/CMakeLists.txt':
                    f'target_link_libraries(esy_common_utils PRIVATE {target})'}))
        self.assertEqual([], self.inspect({'esy_common/utils/CMakeLists.txt':
            'target_link_libraries(esy_common_utils PRIVATE esy_common_defs esy_opt_default)'}))

    def test_native_hardware_headers_and_calls_do_not_escape_private_hal(self):
        for header in ('hardware/spi.h', 'pico/time.h', 'driver/gpio.h',
                       'esp_timer.h', 'stm32f4xx_hal.h', 'core_cm4.h'):
            for name in ('esy_common/utils/src/util.c', 'esy_middleware/protocol/src/channel.c',
                         'esy_hal/include/esy_hal/esy_hal_spi.h'):
                with self.subTest(header=header, file=name):
                    self.assertTrue(self.inspect({name: f'#include "{header}"'}))
        for function in ('gpio_put', 'HAL_SPI_Transmit', 'esp_timer_get_time', 'NVIC_EnableIRQ'):
            with self.subTest(function=function):
                self.assertTrue(self.inspect({'esy_middleware/protocol/src/channel.c':
                    f'void f(void) {{ {function}(0); }}'}))
        self.assertEqual([], self.inspect({'esy_hal_backend/src/pico/spi.c':
            '#include "hardware/spi.h"\nvoid f(void) { spi_init(0, 1); }'}))

    def test_json_codec_may_link_only_its_private_pure_software_provider(self):
        name = 'esy_common/protocol_codec/CMakeLists.txt'
        self.assertEqual([], self.inspect({name:
            'target_link_libraries(esy_common_protocol_json '
            'PUBLIC esy_common_defs PRIVATE esy_json_parser_provider esy_opt_default)'}))
        for scope in ('PUBLIC', 'INTERFACE', 'LINK_PRIVATE', ''):
            with self.subTest(scope=scope):
                self.assertTrue(self.inspect({name:
                    f'target_link_libraries(esy_common_protocol_json {scope} esy_json_parser_provider)'}))
        for name, owner in (
                ('esy_common/utils/CMakeLists.txt', 'esy_common_utils'),
                ('esy_common/utils/CMakeLists.txt', 'esy_common_protocol_json'),
                ('esy_common/protocol_codec/CMakeLists.txt', 'esy_common_protocol_codec')):
            with self.subTest(file=name, owner=owner):
                self.assertTrue(self.inspect({name:
                    f'target_link_libraries({owner} PRIVATE esy_json_parser_provider)'}))

    def test_json_provider_exception_does_not_admit_hardware_or_os_dependencies(self):
        name = 'esy_common/protocol_codec/CMakeLists.txt'
        for target in ('esy_hal', 'esy_hal_backend', 'esy_os', 'esy_board',
                       'esy_network_lwip_provider', 'esy_ble_btstack_provider',
                       'hardware_spi', 'pico_stdlib', 'idf::driver'):
            with self.subTest(target=target):
                self.assertTrue(self.inspect({name:
                    'target_link_libraries(esy_common_protocol_json '
                    f'PRIVATE esy_json_parser_provider {target})'}))
        self.assertTrue(self.inspect({
            name: 'target_link_libraries(esy_common_protocol_json PRIVATE esy_json_parser_provider)',
            'esy_common/protocol_codec/src/json.c': '#include "esy_hal/esy_hal_spi.h"',
        }))

    def test_private_headers_stay_with_their_module_and_out_of_public_headers(self):
        own = {'esy_middleware/logger/src/state_internal.h': '/* Private state. */'}
        self.assertEqual([], self.inspect({**own, 'esy_middleware/logger/src/core.c':
            '#include "state_internal.h"'}))
        for name in ('esy_middleware/protocol/src/channel.c',
                     'esy_middleware/logger/include/esy_middleware/logger/logger.h'):
            with self.subTest(file=name):
                self.assertTrue(self.inspect({**own, name: '#include "state_internal.h"'}))

    def test_private_sdk_panic_header_is_confined_to_its_version_pinned_port(self):
        header = '#include "esp_private/panic_internal.h"'
        self.assertEqual([], self.inspect({
            'esy_hal_backend/src/esp32/esy_hb_fault.c': header}))
        for name in ('esy_hal_backend/src/esp32/other.c',
                     'esy_hal_backend/include/esy_hal_backend/esy_hb_fault.h',
                     'esy_middleware/coredump/src/esy_coredump.c'):
            with self.subTest(file=name):
                self.assertTrue(self.inspect({name: header}))
        self.assertTrue(self.inspect({
            'esy_hal_backend/src/esp32/esy_hb_fault.c':
                '#include "esy_hal/src/state_internal.h"'}))

    def test_cyw43_sdk_is_private_to_hal_even_for_network_provider_glue(self):
        for name in ('esy_middleware/network/src/pico/provider.c',
                     'esy_middleware/wifi/src/runtime.c',
                     'esy_middleware/os/src/freertos/port/pico/context.c',
                     'esy_middleware/other/include/other.h'):
            for source in ('#include "pico/cyw43_arch.h"',
                           'void f(void) { cyw43_arch_poll(); }',
                           'extern void cyw43_arch_poll(void);\nvoid f(void) { cyw43_arch_poll(); }',
                           'void f(void) { cyw43_arch_lwip_begin(); }',
                           'void f(void) { cyw43_arch_lwip_end(); }'):
                with self.subTest(file=name, source=source):
                    self.assertTrue(self.inspect({name: source}))
        self.assertEqual([], self.inspect({'esy_hal_backend/src/pico/wifi.c':
            '#include "pico/cyw43_arch.h"\nvoid f(void) { cyw43_arch_poll(); }'}))

    def test_module_local_helpers_and_injected_callbacks_are_not_native_calls(self):
        self.assertEqual([], self.inspect({'esy_middleware/io_scheduler/src/core.c':
            'static void spi_submit_cb(void *ctx) { (void)ctx; }\n'
            'void run(void) { spi_submit_cb(0); ops->spi_submit(0); }'}))

    def test_qualified_internal_header_cannot_hide_behind_a_local_same_name(self):
        self.assertTrue(self.inspect({
            'esy_middleware/logger/src/state_internal.h': '/* Private logger state. */',
            'esy_middleware/protocol/src/state_internal.h': '/* Private protocol state. */',
            'esy_middleware/logger/src/core.c':
                '#include "esy_middleware/protocol/src/state_internal.h"'}))

    def test_literal_comments_cannot_hide_a_native_call_or_forge_one(self):
        name = 'esy_middleware/protocol/src/channel.c'
        self.assertEqual([], self.inspect({name: 'void f(void) { log("gpio_put(1, 1)"); }'}))
        for source in ('void f(void) { log("http://example.invalid"); gpio_put(1, 1); }',
                       'void f(void) { log("/*"); gpio_put(1, 1); /* end */ }',
                       'void f(void) { gpio_\\\nput(1, 1); }'):
            with self.subTest(source=source):
                self.assertTrue(self.inspect({name: source}))
        self.assertTrue(self.inspect({name: '// Continued comment \\\n/* still comment */\n#include "hardware/gpio.h"'}))


if __name__ == '__main__':
    unittest.main()
