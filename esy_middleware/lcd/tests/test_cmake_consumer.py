# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Verify LCD feature closure and an optional SPI-only provider with real targets."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(lcd_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(esy_feature_import.cmake)
add_library(esy_opt_default INTERFACE)
add_subdirectory("${PLATFORM_ROOT}/esy_common" common)
if(ENABLE_ESY_GPIO)
    add_library(esy_hal STATIC
        "${PLATFORM_ROOT}/esy_test/fake/hal/fake_esy_hal_gpio.c"
        "${PLATFORM_ROOT}/esy_test/fake/hal/fake_esy_hal_gpio_pm.c"
        "${PLATFORM_ROOT}/esy_test/fake/hal/fake_esy_hal_spi.c"
        "${PLATFORM_ROOT}/esy_test/fake/hal/fake_esy_hal_dma.c"
        "${PLATFORM_ROOT}/esy_test/fake/hal/fake_esy_hal_system.c")
    target_include_directories(esy_hal PUBLIC "${PLATFORM_ROOT}/esy_hal/include")
    target_link_libraries(esy_hal PUBLIC esy_common)
endif()
if(WITH_PROVIDER)
    add_library(esy_os STATIC
        "${PLATFORM_ROOT}/esy_test/fake/os/fake_esy_os.c"
        "${PLATFORM_ROOT}/esy_test/fake/os/fake_esy_os_task.c")
    target_include_directories(esy_os PUBLIC "${PLATFORM_ROOT}/esy_middleware/os/include")
    target_link_libraries(esy_os PUBLIC esy_common)
endif()
if(WITH_PROVIDER)
    add_subdirectory("${PLATFORM_ROOT}/esy_middleware/lcd" lcd)
    add_subdirectory("${PLATFORM_ROOT}/esy_middleware/io_scheduler" io_scheduler)
else()
    add_subdirectory("${PLATFORM_ROOT}/esy_middleware" middleware)
endif()
if(ENABLE_ESY_LCD)
    file(GLOB header_probes "${CMAKE_CURRENT_LIST_DIR}/header_probe_*")
    add_library(public_headers OBJECT ${header_probes})
    target_link_libraries(public_headers PRIVATE esy_lcd)
    target_compile_options(public_headers PRIVATE -Wall -Wextra -Werror)
    add_executable(consumer consumer.c)
    target_link_libraries(consumer PRIVATE esy_lcd)
    target_link_options(consumer PRIVATE "-Wl,--no-gc-sections")
    if(WITH_DEVICE)
        target_compile_definitions(consumer PRIVATE WITH_DEVICE)
        target_link_libraries(consumer PRIVATE esy_lcd_device)
    elseif(NOT ENABLE_ESY_GPIO AND TARGET esy_lcd_device)
        message(FATAL_ERROR "GPIO-disabled LCD leaked device integration")
    endif()
    if(ENABLE_ESY_LCD_ILI9488)
        target_compile_definitions(consumer PRIVATE WITH_BACKEND)
    endif()
    if(WITH_PROVIDER)
        target_compile_definitions(consumer PRIVATE WITH_PROVIDER)
        target_link_libraries(consumer PRIVATE esy_spi_scheduler)
        if(TARGET esy_proto_modbus_manager)
            message(FATAL_ERROR "SPI-only LCD must not require Modbus")
        endif()
    endif()
elseif(TARGET esy_lcd)
    message(FATAL_ERROR "Disabled LCD leaked a target")
endif()
'''
CONSUMER = r'''
#include "esy_middleware/lcd/esy_lcd.h"
#ifdef WITH_DEVICE
#include "esy_middleware/lcd/esy_lcd_device.h"
#endif
#ifdef WITH_BACKEND
#include "esy_middleware/lcd/esy_ili9488.h"
#endif
#ifdef WITH_PROVIDER
#include "esy_middleware/io_scheduler/esy_spi_scheduler.h"
#endif
int main(void) {
    esy_lcd_t lcd = {0};
#ifdef WITH_DEVICE
    if (esy_lcd_register_device(NULL, NULL, NULL, NULL, NULL, NULL) != ESY_ERR_INVALID_ARG) return 3;
#endif
#ifdef WITH_BACKEND
    esy_ili9488_config_t cfg;
    esy_ili9488_default_config(&cfg);
#ifdef WITH_PROVIDER
    cfg.spi_submit_cb = esy_spi_scheduler_submit_prepared_with_arg;
#endif
    if (esy_ili9488_init(&lcd, &cfg) != ESY_ERR_INVALID_ARG) return 1;
    if (esy_ili9488_get_status(&lcd) != ESY_ERR_STATE) return 2;
#endif
    return esy_lcd_is_attached(&lcd) || esy_lcd_detach(&lcd) != ESY_OK;
}
'''


class LcdConsumerTests(unittest.TestCase):
    def test_feature_and_provider_matrix(self):
        for lcd, backend, provider, device in ((False, False, False, False),
                                       (True, False, False, False),
                                       (True, False, False, True),
                                       (True, True, False, True),
                                       (True, True, True, True)):
            with self.subTest(lcd=lcd, backend=backend, provider=provider):
                with tempfile.TemporaryDirectory(prefix="lcd-consumer-") as tmp:
                    root = Path(tmp)
                    (root / "CMakeLists.txt").write_text(PROJECT)
                    (root / "consumer.c").write_text(CONSUMER)
                    for index, header in enumerate(sorted((ROOT / "esy_middleware/lcd/include").rglob("*.h"))):
                        include = header.relative_to(ROOT / "esy_middleware/lcd/include")
                        for language in ("c", "cpp"):
                            (root / f"header_probe_{index}.{language}").write_text(
                                f'#include "{include.as_posix()}"\n')
                    features = {
                        "ENABLE_ESY_LCD": lcd,
                        "ENABLE_ESY_GPIO": backend or device,
                        "WITH_DEVICE": device,
                        "ENABLE_ESY_LCD_ILI9488": backend,
                        "WITH_PROVIDER": provider,
                        "ENABLE_ESY_IO_SCHEDULER": provider,
                        "ENABLE_ESY_COMMON_DS": provider,
                        "ENABLE_ESY_SPI": provider,
                        "ENABLE_ESY_SPI_SCHEDULER": provider,
                        "ENABLE_ESY_DMA": provider,
                        "ENABLE_ESY_SYSTEM": provider,
                        "ENABLE_ESY_UART_TX_SCHEDULER": False,
                        "ENABLE_ESY_PROTO_MODBUS_UART_CHANNEL": False,
                        "ENABLE_ESY_PROTO_MODBUS_UART_WORKER": False,
                        "ENABLE_ESY_PROTO_BIN": False,
                        "ENABLE_ESY_PROTO_MODBUS": False,
                    }
                    (root / "esy_feature_import.cmake").write_text(
                        f'set(ESY_RTOS {"FreeRTOS" if provider else "None"})\n' + "".join(
                            f"set({name} {'ON' if value else 'OFF'})\n"
                            for name, value in features.items()))
                    commands = [
                        ["cmake", "-S", str(root), "-B", str(root / "out"),
                         "-G", "Ninja", f"-DPLATFORM_ROOT={ROOT}"],
                        ["cmake", "--build", str(root / "out")],
                    ]
                    if lcd:
                        commands.append([str(root / "out/consumer")])
                    for command in commands:
                        result = subprocess.run(command, capture_output=True, text=True)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
