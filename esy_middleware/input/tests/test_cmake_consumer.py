# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Verify optional input targets and independent C11/C++17 consumers."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(input_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(esy_feature_import.cmake)
add_library(esy_opt_default INTERFACE)
target_compile_definitions(esy_opt_default INTERFACE PRIVATE_OPTIONS_SENTINEL)
add_subdirectory("${ROOT}/esy_common/defs" common_defs)
if(WITH_HAL)
    add_library(esy_hal STATIC "${ROOT}/esy_test/fake/hal/fake_esy_hal_gpio.c")
    target_include_directories(esy_hal PUBLIC "${ROOT}/esy_hal/include")
    target_link_libraries(esy_hal PUBLIC esy_common_defs)
endif()
if(DIRECT_MODULE)
    if(ENABLE_ESY_INPUT)
        add_subdirectory("${ROOT}/esy_middleware/input" input)
    endif()
else()
    add_subdirectory("${ROOT}/esy_middleware" middleware)
endif()
if(TARGET esy_os OR TARGET esy_hal_backend OR TARGET esy_board)
    message(FATAL_ERROR "Input unexpectedly assembled an OS, backend or board target")
endif()
if(ENABLE_ESY_INPUT)
    if(NOT TARGET esy_input)
        message(FATAL_ERROR "Enabled input target is missing")
    endif()
    if(ENABLE_ESY_GPIO)
        if(NOT TARGET esy_input_gpio)
            message(FATAL_ERROR "Enabled GPIO input adapter is missing")
        endif()
        set(input_target esy_input_gpio)
    else()
        if(TARGET esy_input_gpio OR TARGET esy_hal)
            message(FATAL_ERROR "Input core unexpectedly assembled GPIO/HAL support")
        endif()
        set(input_target esy_input)
    endif()
    if(NOT DIRECT_MODULE)
        set(input_target esy_middleware)
    endif()
    file(GLOB public_probes "${CMAKE_CURRENT_LIST_DIR}/public_*")
    add_library(public_headers OBJECT ${public_probes})
    target_link_libraries(public_headers PRIVATE ${input_target})
    target_compile_options(public_headers PRIVATE -Wall -Wextra -Werror)
    foreach(language c cpp)
        add_executable(consumer_${language} consumer.${language})
        target_link_libraries(consumer_${language} PRIVATE ${input_target})
        target_compile_options(consumer_${language} PRIVATE -Wall -Wextra -Werror)
        target_link_options(consumer_${language} PRIVATE "-Wl,--no-gc-sections")
        if(ENABLE_ESY_GPIO)
            target_compile_definitions(consumer_${language} PRIVATE WITH_GPIO)
            target_include_directories(consumer_${language} PRIVATE
                "${ROOT}/esy_test/fake/hal")
        endif()
    endforeach()
elseif(TARGET esy_input OR TARGET esy_input_gpio)
    message(FATAL_ERROR "Disabled input module leaked a target")
endif()
'''

CONSUMER = r'''
#include "esy_middleware/input/esy_input.h"
#ifdef WITH_GPIO
#include "esy_middleware/input/esy_input_gpio.h"
#include "fake_esy_hal_gpio.h"
#endif

#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private optimization definitions leaked through the public input target"
#endif

#ifdef __cplusplus
#define INPUT_ZERO_INIT {}
#else
#define INPUT_ZERO_INIT {0}
#endif

typedef struct {
    unsigned count;
    esy_input_event_t last;
} capture_t;

static void capture(const esy_input_event_t *event, void *arg) {
    capture_t *state = (capture_t *)arg;
    ++state->count;
    state->last = *event;
}

static int core_smoke(void) {
    capture_t events = INPUT_ZERO_INIT;
    esy_input_button_t button = INPUT_ZERO_INIT;
    esy_input_button_config_t config = INPUT_ZERO_INIT;
    config.source_id = 7u;
    config.code = 42u;
    config.on_event = capture;
    config.event_arg = &events;
    if (esy_input_button_init(&button, &config) != ESY_OK) return 1;
    if (esy_input_button_init(&button, &config) != ESY_ERR_STATE) return 2;
    if (esy_input_button_sample(&button, false, 0u) != ESY_OK ||
        esy_input_button_sample(&button, true, 1u) != ESY_OK ||
        esy_input_button_sample(&button, false, 2u) != ESY_OK) return 3;
    if (events.count != 3u || events.last.type != ESY_INPUT_EVENT_CLICK ||
        events.last.source_id != 7u || events.last.code != 42u ||
        events.last.value != 1 || events.last.timestamp_ms != 2u) return 4;
    if (esy_input_button_deinit(&button) != ESY_OK ||
        esy_input_button_sample(&button, false, 3u) != ESY_ERR_STATE ||
        esy_input_button_init(&button, &config) != ESY_OK ||
        esy_input_button_deinit(&button) != ESY_OK) return 5;

    events.count = 0u;
    esy_input_encoder_t encoder = INPUT_ZERO_INIT;
    esy_input_encoder_config_t encoder_config = INPUT_ZERO_INIT;
    encoder_config.source_id = 9u;
    encoder_config.code = 72u;
    encoder_config.edges_per_step = 4u;
    encoder_config.on_event = capture;
    encoder_config.event_arg = &events;
    if (esy_input_encoder_init(&encoder, &encoder_config) != ESY_OK) return 6;
    const uint8_t states[] = {0u, 1u, 3u, 2u, 0u};
    for (unsigned i = 0u; i < sizeof(states); ++i) {
        if (esy_input_encoder_sample(&encoder, states[i], i) != ESY_OK) return 7;
    }
    if (events.count != 1u || events.last.type != ESY_INPUT_EVENT_RELATIVE ||
        events.last.source_id != 9u || events.last.code != 72u ||
        events.last.value != 1) return 8;
    if (esy_input_encoder_deinit(&encoder) != ESY_OK) return 9;
    return 0;
}

#ifdef WITH_GPIO
static int gpio_smoke(void) {
    fake_esy_hal_gpio_reset();
    esy_gpio_t gpio = INPUT_ZERO_INIT;
    esy_gpio_config_t gpio_config;
    esy_hal_gpio_default_cfg(&gpio_config);
    gpio_config.pin = 3u;
    if (esy_hal_gpio_init(&gpio, &gpio_config) != ESY_OK) return 20;
    capture_t events = INPUT_ZERO_INIT;
    esy_input_button_t button = INPUT_ZERO_INIT;
    esy_input_button_config_t config = INPUT_ZERO_INIT;
    config.on_event = capture;
    config.event_arg = &events;
    esy_input_gpio_button_t provider = INPUT_ZERO_INIT;
    provider.gpio = &gpio;
    provider.active_low = true;
    if (esy_input_button_init(&button, &config) != ESY_OK) return 21;
    fake_esy_hal_gpio_set_read_value_for_pin(3u, true);
    if (esy_input_gpio_button_poll(&button, &provider, 0u) != ESY_OK) return 22;
    fake_esy_hal_gpio_set_read_value_for_pin(3u, false);
    if (esy_input_gpio_button_poll(&button, &provider, 1u) != ESY_OK) return 23;
    fake_esy_hal_gpio_set_read_value_for_pin(3u, true);
    if (esy_input_gpio_button_poll(&button, &provider, 2u) != ESY_OK) return 24;
    if (events.count != 3u || events.last.type != ESY_INPUT_EVENT_CLICK) return 25;
    if (esy_input_button_deinit(&button) != ESY_OK ||
        !esy_hal_gpio_is_initialized(&gpio)) return 26;

    esy_gpio_t phase_a = INPUT_ZERO_INIT;
    esy_gpio_t phase_b = INPUT_ZERO_INIT;
    gpio_config.pin = 4u;
    if (esy_hal_gpio_init(&phase_a, &gpio_config) != ESY_OK) return 27;
    gpio_config.pin = 5u;
    if (esy_hal_gpio_init(&phase_b, &gpio_config) != ESY_OK) return 28;
    events.count = 0u;
    esy_input_encoder_t encoder = INPUT_ZERO_INIT;
    esy_input_encoder_config_t encoder_config = INPUT_ZERO_INIT;
    encoder_config.edges_per_step = 4u;
    encoder_config.on_event = capture;
    encoder_config.event_arg = &events;
    esy_input_gpio_encoder_t encoder_provider = INPUT_ZERO_INIT;
    encoder_provider.phase_a = &phase_a;
    encoder_provider.phase_b = &phase_b;
    if (esy_input_encoder_init(&encoder, &encoder_config) != ESY_OK) return 29;
    const uint8_t states[] = {0u, 1u, 3u, 2u, 0u};
    for (unsigned i = 0u; i < sizeof(states); ++i) {
        fake_esy_hal_gpio_set_read_value_for_pin(4u, (states[i] & 2u) != 0u);
        fake_esy_hal_gpio_set_read_value_for_pin(5u, (states[i] & 1u) != 0u);
        if (esy_input_gpio_encoder_poll(&encoder, &encoder_provider, i) != ESY_OK)
            return 30;
    }
    if (events.count != 1u || events.last.type != ESY_INPUT_EVENT_RELATIVE ||
        events.last.value != 1) return 31;
    if (esy_input_encoder_deinit(&encoder) != ESY_OK ||
        !esy_hal_gpio_is_initialized(&phase_a) ||
        !esy_hal_gpio_is_initialized(&phase_b)) return 32;
    if (esy_hal_gpio_deinit(&phase_a) != ESY_OK ||
        esy_hal_gpio_deinit(&phase_b) != ESY_OK ||
        esy_hal_gpio_deinit(&gpio) != ESY_OK) return 33;
    return 0;
}
#endif

int main(void) {
    int result = core_smoke();
    if (result != 0) return result;
#ifdef WITH_GPIO
    result = gpio_smoke();
#endif
    return result;
}
'''


class InputConsumerTests(unittest.TestCase):
    def run_case(self, *, enabled=True, gpio=False, direct=False,
                 rtos="None", with_hal=None, expected_error=None):
        if with_hal is None:
            with_hal = gpio and enabled
        with tempfile.TemporaryDirectory(prefix="input-consumer-") as tmp:
            directory = Path(tmp)
            (directory / "CMakeLists.txt").write_text(PROJECT)
            for language in ("c", "cpp"):
                (directory / f"consumer.{language}").write_text(CONSUMER)
            headers = ["esy_input.h"]
            if gpio:
                headers.append("esy_input_gpio.h")
            for index, header in enumerate(headers):
                for language in ("c", "cpp"):
                    (directory / f"public_{index}.{language}").write_text(
                        f'#include "esy_middleware/input/{header}"\n'
                        '#ifdef PRIVATE_OPTIONS_SENTINEL\n'
                        '#error "Private options leaked to public headers"\n'
                        '#endif\n')
            (directory / "esy_feature_import.cmake").write_text(
                f"set(ESY_RTOS {rtos})\n"
                f"set(ENABLE_ESY_INPUT {'ON' if enabled else 'OFF'})\n"
                f"set(ENABLE_ESY_GPIO {'ON' if gpio else 'OFF'})\n"
                f"set(WITH_HAL {'ON' if with_hal else 'OFF'})\n"
                f"set(DIRECT_MODULE {'ON' if direct else 'OFF'})\n")
            configure = subprocess.run(
                ["cmake", "-S", str(directory), "-B", str(directory / "out"),
                 "-G", "Ninja", f"-DROOT={ROOT}"],
                capture_output=True, text=True)
            output = configure.stdout + configure.stderr
            if expected_error:
                self.assertNotEqual(configure.returncode, 0, output)
                self.assertIn(expected_error, output)
                return
            self.assertEqual(configure.returncode, 0, output)
            commands = [["cmake", "--build", str(directory / "out")]]
            if enabled:
                commands.extend([str(directory / f"out/consumer_{language}")]
                                for language in ("c", "cpp"))
            for command in commands:
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_aggregate_feature_and_gpio_matrix(self):
        for enabled, gpio in ((False, False), (False, True),
                              (True, False), (True, True)):
            with self.subTest(enabled=enabled, gpio=gpio):
                self.run_case(enabled=enabled, gpio=gpio)

    def test_direct_core_is_independent_of_rtos_selection(self):
        for rtos in ("None", "FreeRTOS"):
            with self.subTest(rtos=rtos):
                self.run_case(direct=True, rtos=rtos)

    def test_direct_gpio_adapter_does_not_require_os(self):
        self.run_case(direct=True, gpio=True, rtos="FreeRTOS")

    def test_gpio_adapter_requires_public_hal_target(self):
        self.run_case(gpio=True, with_hal=False,
                      expected_error="esy_input_gpio requires consumer target esy_hal")


if __name__ == "__main__":
    unittest.main()
