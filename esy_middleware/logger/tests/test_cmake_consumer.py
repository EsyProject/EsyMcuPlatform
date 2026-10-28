# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Build and link C/C++ consumers using only the real logger target interface."""

from itertools import product
from pathlib import Path
import json
import subprocess
import tempfile
import unittest


PLATFORM_ROOT = Path(__file__).resolve().parents[3]
CMAKE_PROJECT = """\
cmake_minimum_required(VERSION 3.16)
project(logger_consumer_check LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(esy_feature_import.cmake)
add_library(esy_opt_default INTERFACE)
target_compile_definitions(esy_opt_default INTERFACE TEST_PRIVATE_OPTIONS)
add_subdirectory("${PLATFORM_ROOT}/esy_common/defs" common_defs)
if(PROVIDE_COMMON_UTILS)
    add_subdirectory("${PLATFORM_ROOT}/esy_common/utils" common_utils)
endif()
if(ENABLE_ESY_COMMON_DS)
    add_subdirectory("${PLATFORM_ROOT}/esy_common/ds" common_ds)
endif()

# Exercise production logger/common with reusable host implementations of ports.
if(PROVIDE_HAL)
add_subdirectory("${PLATFORM_ROOT}/esy_common/device" common_device)
add_library(esy_hal STATIC
    "${PLATFORM_ROOT}/esy_test/fake/hal/fake_esy_hal_system.c"
    "${PLATFORM_ROOT}/esy_test/fake/hal/fake_esy_hal_uart.c"
    "${PLATFORM_ROOT}/esy_test/fake/hal/fake_esy_hal_dma.c")
target_include_directories(esy_hal PUBLIC "${PLATFORM_ROOT}/esy_hal/include")
target_link_libraries(esy_hal PUBLIC esy_common_defs esy_common_device)
endif()
if(ENABLE_ESY_OS)
    add_library(esy_os STATIC
        "${PLATFORM_ROOT}/esy_test/fake/os/fake_esy_os.c"
        "${PLATFORM_ROOT}/esy_test/fake/os/fake_esy_os_task.c")
    target_include_directories(esy_os PUBLIC "${PLATFORM_ROOT}/esy_middleware/os/include")
    target_link_libraries(esy_os PUBLIC esy_common_defs)
endif()
if(PROVIDER STREQUAL "named" OR PROVIDER STREQUAL "custom")
    if(PROVIDER STREQUAL "custom")
        set(ESY_SEGGER_RTT_TARGET rtt_provider)
        set(provider_target rtt_provider)
    else()
        set(provider_target SEGGER_RTT)
    endif()
    add_library(${provider_target} STATIC
        "${PLATFORM_ROOT}/esy_test/fake/third_party/segger_rtt/fake_segger_rtt.c")
    target_include_directories(${provider_target} PUBLIC
        "${PLATFORM_ROOT}/esy_test/fake/third_party/segger_rtt")
    target_compile_definitions(${provider_target} INTERFACE TEST_PRIVATE_RTT)
elseif(PROVIDER STREQUAL "root")
    set(ESY_SEGGER_RTT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/provider")
endif()
add_subdirectory("${PLATFORM_ROOT}/esy_middleware/logger" logger)
if(TARGET esy_logger_os)
    message(FATAL_ERROR "A mixed Logger OS target was assembled")
endif()
get_target_property(front_sources esy_logger_core SOURCES)
list(LENGTH front_sources front_source_count)
if(NOT front_source_count EQUAL 1 OR NOT front_sources MATCHES "esy_logger.c$")
    message(FATAL_ERROR "Logger frontend acquired backend sources")
endif()
foreach(backend uart printf rtt)
    string(TOUPPER "${backend}" backend_upper)
    if(ENABLE_ESY_LOGGER_BACKEND_${backend_upper})
        if(NOT TARGET esy_logger_backend_${backend}_core)
            message(FATAL_ERROR "Enabled Logger backend has no concrete core target")
        endif()
        if(ESY_RTOS STREQUAL "FreeRTOS" AND NOT TARGET esy_logger_backend_${backend})
            message(FATAL_ERROR "Enabled Logger backend has no concrete OS target")
        endif()
    elseif(TARGET esy_logger_backend_${backend}_core OR TARGET esy_logger_backend_${backend})
        message(FATAL_ERROR "Disabled Logger backend assembled")
    endif()
endforeach()

# No include directories or unrelated libraries are supplied to the consumers.
add_executable(consumer_c consumer.c)
target_link_libraries(consumer_c PRIVATE ${CONSUMER_TARGET})
add_executable(consumer_cpp consumer.cpp)
target_link_libraries(consumer_cpp PRIVATE ${CONSUMER_TARGET})
if(EXERCISE_DUAL_UART)
    # The consumer owns HAL initialization independently of the logger.
    target_link_libraries(consumer_c PRIVATE esy_hal)
    target_link_libraries(consumer_cpp PRIVATE esy_hal)
endif()
"""
CONSUMER_SOURCE = """\
#include "esy_middleware/logger/esy_logger.h"
#include <string.h>
@HEADERS@
#if defined(TEST_PRIVATE_OPTIONS) || defined(TEST_PRIVATE_RTT)
#error "Implementation-only compile requirements leaked into the consumer"
#endif
typedef struct {
    uint8_t workspace[64];
    char output[256];
    size_t length;
} output_capture_t;
static esy_err_t capture_acquire(void *self, uint8_t **buffer, size_t *capacity) {
    esy_logger_backend_t *backend = (esy_logger_backend_t *)self;
    output_capture_t *capture = (output_capture_t *)backend->ctx;
    *buffer = capture->workspace;
    *capacity = sizeof(capture->workspace);
    return ESY_OK;
}
static esy_err_t capture_submit(void *self, const uint8_t *buffer, size_t length) {
    esy_logger_backend_t *backend = (esy_logger_backend_t *)self;
    output_capture_t *capture = (output_capture_t *)backend->ctx;
    if (capture->length + length >= sizeof(capture->output)) return ESY_ERR_NO_MEM;
    memcpy(capture->output + capture->length, buffer, length);
    capture->length += length;
    capture->output[capture->length] = 0;
    return ESY_OK;
}
int main(void) {
    output_capture_t capture = {0};
    esy_logger_backend_t custom = {capture_acquire, capture_submit, &capture};
    esy_logger_backend_binding_t capture_binding = {&custom, ESY_LOG_LEVEL_INFO};
    esy_logger_static_resources_t capture_resources = {&capture_binding, 1u};
    esy_logger_config_t capture_config = {&capture_resources, ESY_LOG_LEVEL_DEBUG};
    if (esy_logger_init(&capture_config) != ESY_OK) return 30;
    ESY_LOG_I("frontend", "value=%u", 42u);
    if (strstr(capture.output, "value=42") == NULL) return 31;
    size_t capture_length = capture.length;
    ESY_LOG_D("frontend", "filtered");
    if (capture.length != capture_length) return 32;
    if (esy_logger_detach(&custom) != ESY_OK) return 33;
    esy_logger_backend_t backend = {0};
    if (esy_logger_init(NULL) != ESY_ERR_INVALID_ARG) return 1;
    if (esy_logger_detach(&backend) != ESY_OK) return 2;
    @CALLS@
    return 0;
}
"""


class LoggerCMakeConsumerTests(unittest.TestCase):
    def run_command(self, *command):
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(0, result.returncode,
                         f"Command failed: {command}\n{result.stdout}{result.stderr}")

    def check_consumer(self, backends=(), provider="named", overrides=None, error=None,
                       exercise_dual_uart=False, consumer_target="esy_logger", used_backends=None):
        features = dict(ESY_RTOS="FreeRTOS", ENABLE_ESY_COMMON_DS="ON", ENABLE_ESY_OS="ON",
                        ENABLE_ESY_SYSTEM="ON", ENABLE_ESY_UART="ON", ENABLE_ESY_DMA="ON",
                        PROVIDER=provider, PROVIDE_HAL="ON", PROVIDE_COMMON_UTILS="ON",
                        CONSUMER_TARGET=consumer_target)
        for backend in ("uart", "printf", "rtt"):
            features[f"ENABLE_ESY_LOGGER_BACKEND_{backend.upper()}"] = (
                "ON" if backend in backends else "OFF")
        features.update(overrides or {})
        consumed = backends if used_backends is None else used_backends
        core_mode = features["ESY_RTOS"] == "None" or consumer_target.endswith("_core")
        suffix = lambda b: "_core" if core_mode and b != "rtt" else ""
        init_suffix = "_core" if core_mode else ""
        headers = "\n".join(
            f'#include "esy_middleware/logger/esy_logger_backend_{b}{suffix(b)}.h"'
            for b in consumed)
        calls = "\n".join(
            f"esy_logger_backend_{b}{suffix(b)}_config_t {b}_cfg;\n"
            f"esy_logger_backend_{b}{suffix(b)}_default_cfg(&{b}_cfg);\n"
            f"if (esy_logger_backend_{b}{init_suffix}_init(&backend, &{b}_cfg) != ESY_ERR_INVALID_ARG) return 3;\n"
            f"if (esy_logger_backend_{b}_deinit(&backend) != ESY_OK) return 4;"
            for b in consumed)
        if exercise_dual_uart:
            self.assertTrue(core_mode)
            self.assertIn("uart", backends)
            features["EXERCISE_DUAL_UART"] = "ON"
            headers += '\n#include "esy_hal/esy_hal_uart.h"'
            calls += "\n" + "\n".join((
                "esy_uart_t uarts[2] = {0};",
                "esy_uart_config_t uart_config;",
                "esy_hal_uart_default_cfg(&uart_config);",
                "uart_config.phys_index = 0u;",
                "if (esy_hal_uart_init(&uarts[0], &uart_config) != ESY_OK) return 10;",
                "uart_config.phys_index = 1u;",
                "if (esy_hal_uart_init(&uarts[1], &uart_config) != ESY_OK) return 11;",
                "ESY_LOGGER_BACKEND_UART_CORE_STATIC_STORAGE(first, 4u, 32u) = {0};",
                "ESY_LOGGER_BACKEND_UART_CORE_STATIC_STORAGE(second, 4u, 32u) = {0};",
                "esy_logger_backend_uart_core_resources_t first_resources;",
                "esy_logger_backend_uart_core_resources_t second_resources;",
                "ESY_LOGGER_BACKEND_UART_CORE_BIND_STATIC_RESOURCES(&first_resources, first);",
                "ESY_LOGGER_BACKEND_UART_CORE_BIND_STATIC_RESOURCES(&second_resources, second);",
                "esy_logger_backend_t outputs[2] = {0};",
                "uart_cfg.block_size = 32u;",
                "uart_cfg.pool_block_count = 4u;",
                "uart_cfg.dma_short_threshold = 32u;",
                "uart_cfg.uart = &uarts[0];",
                "uart_cfg.static_resources = &first_resources;",
                "if (esy_logger_backend_uart_core_init(&outputs[0], &uart_cfg) != ESY_OK) return 12;",
                "uart_cfg.uart = &uarts[1];",
                "uart_cfg.static_resources = &second_resources;",
                "if (esy_logger_backend_uart_core_init(&outputs[1], &uart_cfg) != ESY_OK) return 13;",
                "if (outputs[0].ctx == outputs[1].ctx) return 14;",
                "esy_logger_backend_binding_t bindings[2] = {0};",
                "bindings[0].backend = &outputs[0];",
                "bindings[0].filter_level = ESY_LOG_LEVEL_INFO;",
                "bindings[1].backend = &outputs[1];",
                "bindings[1].filter_level = ESY_LOG_LEVEL_DEBUG;",
                "esy_logger_static_resources_t resources = {0};",
                "resources.bindings = bindings;",
                "resources.binding_count = 2u;",
                "esy_logger_config_t config = {0};",
                "config.static_resources = &resources;",
                "config.filter_level = ESY_LOG_LEVEL_DEBUG;",
                "if (esy_logger_init(&config) != ESY_OK) return 15;",
                'ESY_LOG_I("consumer", "both UARTs");',
                "if (esy_logger_backend_uart_core_process(&outputs[0], 4u) != ESY_OK) return 16;",
                "if (esy_logger_backend_uart_core_process(&outputs[1], 4u) != ESY_OK) return 17;",
                "if (esy_logger_backend_uart_deinit(&outputs[0]) != ESY_OK) return 18;",
                'ESY_LOG_D("consumer", "second UART");',
                "if (esy_logger_backend_uart_core_process(&outputs[1], 4u) != ESY_OK) return 19;",
                "if (esy_logger_backend_uart_deinit(&outputs[1]) != ESY_OK) return 20;",
                "if (esy_hal_uart_deinit(&uarts[0]) != ESY_OK) return 21;",
                "if (esy_hal_uart_deinit(&uarts[1]) != ESY_OK) return 22;"))
        if core_mode and "rtt" in consumed:
            calls += "\n" + "\n".join((
                "ESY_LOGGER_BACKEND_RTT_STATIC_STORAGE(rtt_storage, 32u);",
                "esy_logger_backend_rtt_static_resources_t rtt_resources;",
                "ESY_LOGGER_BACKEND_RTT_BIND_STATIC_RESOURCES(&rtt_resources, rtt_storage);",
                "rtt_cfg.static_resources = &rtt_resources;",
                "if (esy_logger_backend_rtt_core_init(&backend, &rtt_cfg) != ESY_OK) return 5;",
                "esy_logger_backend_binding_t binding = {0};",
                "binding.backend = &backend;",
                "binding.filter_level = ESY_LOG_LEVEL_INFO;",
                "esy_logger_static_resources_t frontend_resources = {0};",
                "frontend_resources.bindings = &binding;",
                "frontend_resources.binding_count = 1u;",
                "esy_logger_config_t frontend_config = {0};",
                "frontend_config.static_resources = &frontend_resources;",
                "frontend_config.filter_level = ESY_LOG_LEVEL_INFO;",
                "if (esy_logger_init(&frontend_config) != ESY_OK) return 6;",
                'esy_log("consumer", ESY_LOG_LEVEL_INFO, "caller-driven RTT");',
                "if (esy_logger_backend_rtt_deinit(&backend) != ESY_OK) return 7;"))
        with tempfile.TemporaryDirectory(prefix="logger consumer ") as directory:
            project = Path(directory)
            (project / "CMakeLists.txt").write_text(CMAKE_PROJECT)
            (project / "esy_feature_import.cmake").write_text(
                "".join(f"set({key} {value})\n" for key, value in features.items()))
            source = CONSUMER_SOURCE.replace("@HEADERS@", headers).replace("@CALLS@", calls)
            for name in ("consumer.c", "consumer.cpp"):
                (project / name).write_text(source)
            if provider == "root":
                # Synthetic provider owned by this test, not vendor support code.
                rtt = project / "provider" / "RTT"
                rtt.mkdir(parents=True)
                fake = PLATFORM_ROOT / "esy_test/fake/third_party/segger_rtt"
                for name in ("SEGGER_RTT.h", "fake_segger_rtt.h"):
                    (rtt / name).write_text((fake / name).read_text())
                (rtt / "SEGGER_RTT.c").write_text((fake / "fake_segger_rtt.c").read_text())
                config = project / "provider" / "Config"
                config.mkdir()
                (config / "SEGGER_RTT_Conf.h").write_text("/* Test provider configuration. */\n")
            configure = ("cmake", "-S", str(project), "-B", str(project / "build"),
                         f"-DPLATFORM_ROOT={PLATFORM_ROOT}")
            if error is not None:
                result = subprocess.run(configure, capture_output=True, text=True)
                self.assertNotEqual(0, result.returncode)
                self.assertIn(error, result.stdout + result.stderr)
                return
            self.run_command(*configure)
            self.run_command("cmake", "--build", str(project / "build"),
                             "--target", "consumer_c", "consumer_cpp", "--parallel", "2")
            if core_mode and features["ENABLE_ESY_OS"] == "OFF":
                self.assertNotIn("esy_middleware/os", (project / "build/compile_commands.json").read_text())
            commands = json.loads((project / "build/compile_commands.json").read_text())
            for entry in commands:
                name = Path(entry["file"]).name
                if name in ("consumer.c", "consumer.cpp", "esy_logger.c"):
                    for private in ("esy_common/ds/include", "segger_rtt", "provider/RTT", "provider/Config"):
                        self.assertNotIn(private, entry["command"])
                    for private in (() if exercise_dual_uart and name != "esy_logger.c" else ("esy_hal/include",)):
                        self.assertNotIn(private, entry["command"])
                    if name == "esy_logger.c" or core_mode or consumed == ("rtt",):
                        self.assertNotIn("esy_middleware/os/include", entry["command"])
            for name in ("consumer_c", "consumer_cpp"):
                self.run_command(str(project / "build" / name))
                symbols = subprocess.run(["nm", "-g", str(project / "build" / name)],
                                         capture_output=True, text=True, check=True).stdout
                links = (project / "build/CMakeFiles" / f"{name}.dir" / "link.txt").read_text()
                if consumer_target != "esy_logger":
                    for backend in ("uart", "printf", "rtt"):
                        if backend not in consumed:
                            self.assertNotIn(f"esy_logger_backend_{backend}_", symbols)
                            self.assertNotIn(f"libesy_logger_backend_{backend}", links)
                            self.assertFalse(list((project / "build").rglob(
                                f"libesy_logger_backend_{backend}*.a")))
                    if "rtt" not in consumed:
                        self.assertNotIn("SEGGER_RTT", symbols)
                        self.assertNotIn("librtt_provider", links)
                        self.assertNotIn("libSEGGER_RTT", links)
                    if not consumed:
                        for private in ("esy_hal_", "esy_os_", "esy_block_", "SEGGER_RTT"):
                            self.assertNotIn(private, symbols)
                        for private in ("libesy_hal", "libesy_os", "libesy_common_ds",
                                        "libesy_common_utils", "libesy_common_device"):
                            self.assertNotIn(private, links)
                if core_mode:
                    self.assertNotIn("libesy_os", links)
                    self.assertNotIn("esy_os_", symbols)

    def test_all_eight_backend_combinations(self):
        for mode in ("None", "FreeRTOS"):
            for enabled in product((False, True), repeat=3):
                backends = tuple(b for b, on in zip(("uart", "printf", "rtt"), enabled) if on)
                with self.subTest(backends=backends, mode=mode):
                    self.check_consumer(backends, overrides=dict(ESY_RTOS=mode,
                        ENABLE_ESY_OS="OFF" if mode == "None" else "ON"))

    def test_frontend_without_hal_rtos_system_data_structures_or_provider(self):
        self.check_consumer(provider="missing", consumer_target="esy_logger_core",
            overrides=dict(ESY_RTOS="None", ENABLE_ESY_OS="OFF", PROVIDE_HAL="OFF",
                ENABLE_ESY_SYSTEM="OFF", ENABLE_ESY_COMMON_DS="OFF", PROVIDE_COMMON_UTILS="OFF"))

    def test_selected_frontend_does_not_link_enabled_backends(self):
        self.check_consumer(("uart", "printf", "rtt"), consumer_target="esy_logger_core",
                            used_backends=())

    def test_each_backend_target_excludes_other_enabled_backends(self):
        for mode in ("None", "FreeRTOS"):
            for backend in ("uart", "printf", "rtt"):
                with self.subTest(mode=mode, backend=backend):
                    target = f"esy_logger_backend_{backend}" + ("_core" if mode == "None" else "")
                    self.check_consumer(("uart", "printf", "rtt"),
                        consumer_target=target, used_backends=(backend,),
                        overrides=dict(ESY_RTOS=mode, ENABLE_ESY_OS="OFF" if mode == "None" else "ON"))

    def test_caller_driven_backends_without_any_os_target(self):
        for backends in (("uart",), ("printf",), ("rtt",), ("uart", "printf", "rtt")):
            with self.subTest(backends=backends):
                self.check_consumer(backends, overrides=dict(ESY_RTOS="None", ENABLE_ESY_OS="OFF"))

    def test_other_rtt_provider_forms(self):
        for provider in ("custom", "root"):
            with self.subTest(provider=provider):
                self.check_consumer(("rtt",), provider)

    def test_caller_driven_dual_uart_consumers_without_os(self):
        self.check_consumer(("uart",), overrides=dict(ESY_RTOS="None", ENABLE_ESY_OS="OFF"),
                            exercise_dual_uart=True)

    def test_missing_backend_dependencies_fail_at_configure(self):
        cases = (
            ("rtt", "ENABLE_ESY_OS", "OFF", "require the esy_os target"),
            ("printf", "ENABLE_ESY_SYSTEM", "OFF", "requires ENABLE_ESY_SYSTEM"),
            ("uart", "ENABLE_ESY_COMMON_DS", "OFF", "require ENABLE_ESY_COMMON_DS"),
            ("printf", "ENABLE_ESY_COMMON_DS", "OFF", "require ENABLE_ESY_COMMON_DS"),
            ("uart", "ENABLE_ESY_UART", "OFF", "requires ENABLE_ESY_UART and ENABLE_ESY_DMA"),
            ("uart", "ENABLE_ESY_DMA", "OFF", "requires ENABLE_ESY_UART and ENABLE_ESY_DMA"),
            ("rtt", "PROVIDER", "missing", "requires SEGGER RTT"),
            ("rtt", "PROVIDE_HAL", "OFF", "requires target esy_hal"),
            ("uart", "PROVIDE_COMMON_UTILS", "OFF", "require target esy_common_utils"),
            ("printf", "ESY_RTOS", "unsupported", "require ESY_RTOS=None or ESY_RTOS=FreeRTOS"),
        )
        for backend, key, value, error in cases:
            with self.subTest(backend=backend, missing=key):
                self.check_consumer((backend,), overrides={key: value}, error=error)


if __name__ == "__main__":
    unittest.main()
