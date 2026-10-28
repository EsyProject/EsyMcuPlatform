# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Verify public touch usage requirements without extra consumer dependencies."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(touch_consumer LANGUAGES C CXX)
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
        "${PLATFORM_ROOT}/esy_test/fake/hal/fake_esy_hal_gpio_pm.c")
    target_include_directories(esy_hal PUBLIC "${PLATFORM_ROOT}/esy_hal/include")
    target_link_libraries(esy_hal PUBLIC esy_common)
endif()
add_subdirectory("${PLATFORM_ROOT}/esy_middleware" middleware)
if(ENABLE_ESY_TOUCH)
    file(GLOB header_probes "${CMAKE_CURRENT_LIST_DIR}/header_probe_*")
    add_library(public_headers OBJECT ${header_probes})
    target_link_libraries(public_headers PRIVATE esy_touch)
    target_compile_options(public_headers PRIVATE -Wall -Wextra -Werror)
    foreach(language c cpp)
        add_executable(consumer_${language} consumer.${language})
        target_link_libraries(consumer_${language} PRIVATE esy_touch)
        target_link_options(consumer_${language} PRIVATE "-Wl,--no-gc-sections")
        if(WITH_DEVICE)
            target_compile_definitions(consumer_${language} PRIVATE WITH_DEVICE)
            target_link_libraries(consumer_${language} PRIVATE esy_touch_device)
        elseif(NOT ENABLE_ESY_GPIO AND TARGET esy_touch_device)
            message(FATAL_ERROR "GPIO-disabled touch leaked device integration")
        endif()
        if(ENABLE_ESY_TOUCH_XPT2046)
            target_compile_definitions(consumer_${language} PRIVATE WITH_BACKEND)
        endif()
    endforeach()
    get_target_property(touch_sources esy_touch SOURCES)
    if(ENABLE_ESY_TOUCH_XPT2046)
        if(NOT "${touch_sources}" MATCHES "esy_xpt2046.c")
            message(FATAL_ERROR "Enabled touch backend source is missing")
        endif()
    elseif("${touch_sources}" MATCHES "esy_xpt2046.c")
        message(FATAL_ERROR "Disabled touch backend source leaked")
    endif()
elseif(TARGET esy_touch)
    message(FATAL_ERROR "Disabled touch module leaked a target")
endif()
if(TARGET esy_os)
    message(FATAL_ERROR "Touch consumer unexpectedly requires an OS target")
endif()
'''
CONSUMER = r'''
#include "esy_middleware/touch/esy_touch.h"
#include "esy_middleware/touch/esy_touch_device.h"
#ifdef WITH_BACKEND
#include "esy_middleware/touch/esy_xpt2046.h"
#include "esy_middleware/touch/esy_xpt2046_device.h"
static esy_err_t reserve_buffers(uint8_t **tx, uint8_t **rx, size_t size, void *arg) {
    (void)size;
    (void)arg;
    *tx = NULL;
    *rx = NULL;
    return ESY_ERR_NO_MEM;
}
#endif

int main(void) {
    esy_touch_t touch = {0};
#ifdef WITH_DEVICE
    if (esy_touch_register_device(NULL, NULL, NULL, NULL, NULL, NULL) != ESY_ERR_INVALID_ARG) return 6;
#endif
    esy_touch_data_t sample;
    if (esy_touch_poll(&touch) != ESY_ERR_STATE) return 1;
    if (esy_touch_get_sample(&touch, &sample) != ESY_ERR_STATE) return 2;
#ifdef WITH_BACKEND
    esy_xpt2046_config_t config;
    esy_xpt2046_default_config(&config);
    config.spi_get_write_buffer_cb = reserve_buffers;
    if (esy_xpt2046_init(&touch, &config) != ESY_ERR_INVALID_ARG) return 3;
#endif
    if (esy_touch_detach(&touch) != ESY_OK) return 5;
    return esy_touch_is_attached(&touch) ? 4 : 0;
}
'''


class TouchConsumerTests(unittest.TestCase):
    def test_feature_and_public_dependency_matrix(self):
        for enabled, backend, device in ((False, False, False), (True, False, False),
                                         (True, False, True), (True, True, True)):
            with self.subTest(enabled=enabled, backend=backend):
                with tempfile.TemporaryDirectory(prefix="touch-consumer-") as tmp:
                    directory = Path(tmp)
                    (directory / "CMakeLists.txt").write_text(PROJECT)
                    for extension in ("c", "cpp"):
                        (directory / f"consumer.{extension}").write_text(CONSUMER)
                    for index, header in enumerate(sorted((ROOT / "esy_middleware/touch/include").rglob("*.h"))):
                        include = header.relative_to(ROOT / "esy_middleware/touch/include")
                        for language in ("c", "cpp"):
                            (directory / f"header_probe_{index}.{language}").write_text(
                                f'#include "{include.as_posix()}"\n')
                    (directory / "esy_feature_import.cmake").write_text(
                        "set(ESY_RTOS None)\n"
                        f"set(ENABLE_ESY_GPIO {'ON' if backend or device else 'OFF'})\n"
                        f"set(WITH_DEVICE {'ON' if device else 'OFF'})\n"
                        f"set(ENABLE_ESY_TOUCH {'ON' if enabled else 'OFF'})\n"
                        f"set(ENABLE_ESY_TOUCH_XPT2046 {'ON' if backend else 'OFF'})\n")
                    commands = [
                        ["cmake", "-S", str(directory), "-B", str(directory / "out"),
                         "-G", "Ninja", f"-DPLATFORM_ROOT={ROOT}"],
                        ["cmake", "--build", str(directory / "out")],
                    ]
                    if enabled:
                        commands.extend(
                            [str(directory / f"out/consumer_{extension}")]
                            for extension in ("c", "cpp"))
                    for command in commands:
                        result = subprocess.run(command, capture_output=True, text=True)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
