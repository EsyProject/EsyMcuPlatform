# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Verify event-bus feature closure, diagnostics and C11/C++17 consumers."""

from pathlib import Path
import json
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(event_bus_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(esy_feature_import.cmake)
add_library(esy_opt_default INTERFACE)
target_compile_definitions(esy_opt_default INTERFACE PRIVATE_OPTIONS_SENTINEL)
add_subdirectory("${ROOT}/esy_common/defs" common_defs)
if(DIRECT_MODULE)
    if(WITH_OS)
        add_library(esy_os STATIC
            "${ROOT}/esy_test/fake/os/fake_esy_os_queue.c"
            "${ROOT}/esy_test/fake/os/fake_esy_os_task.c")
        target_include_directories(esy_os PUBLIC
            "${ROOT}/esy_middleware/os/include"
            "${ROOT}/esy_test/fake/os")
        target_link_libraries(esy_os PUBLIC esy_common_defs)
    endif()
    if(ENABLE_ESY_EVENT_BUS)
        add_subdirectory("${ROOT}/esy_middleware/event_bus" event_bus)
    endif()
else()
    # Configure the real aggregate and OS source selection without native links.
    set(ESY_TARGET_DEVICE PICO)
    add_library(esy_common INTERFACE)
    target_link_libraries(esy_common INTERFACE esy_common_defs)
    add_library(esy_hal INTERFACE)
    target_include_directories(esy_hal INTERFACE "${ROOT}/esy_hal/include")
    add_library(native_provider INTERFACE)
    target_include_directories(native_provider INTERFACE
        "${ROOT}/esy_middleware/os/tests/freertos")
    set(FREE_RTOS_KERNEL_LIB native_provider)
    add_subdirectory("${ROOT}/esy_middleware" middleware)
endif()
if(ENABLE_ESY_EVENT_BUS)
    if(NOT TARGET esy_event_bus)
        message(FATAL_ERROR "Enabled event bus target is missing")
    endif()
    get_target_property(event_sources esy_event_bus SOURCES)
    if(NOT "src/esy_event_bus.c" IN_LIST event_sources)
        message(FATAL_ERROR "Event bus source is missing")
    endif()
    get_target_property(event_links esy_event_bus INTERFACE_LINK_LIBRARIES)
    if(NOT "esy_os" IN_LIST event_links OR
       NOT "esy_common_defs" IN_LIST event_links)
        message(FATAL_ERROR "Event bus public dependencies are incomplete")
    endif()
    if(NOT DIRECT_MODULE)
        get_target_property(aggregate_links esy_middleware INTERFACE_LINK_LIBRARIES)
        if(NOT "esy_event_bus" IN_LIST aggregate_links)
            message(FATAL_ERROR "Event bus missing from middleware aggregate")
        endif()
    endif()
    add_library(public_headers OBJECT public.c public.cpp)
    target_link_libraries(public_headers PRIVATE esy_event_bus)
    target_compile_options(public_headers PRIVATE -Wall -Wextra -Werror)
    if(DIRECT_MODULE)
        set(optimized_compile_flags "")
        set(optimized_link_flags "")
        if(OPTIMIZED_HOST AND CMAKE_C_COMPILER_ID STREQUAL "GNU" AND
           CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set(optimized_compile_flags -O3 -fstrict-aliasing -flto)
            set(optimized_link_flags -flto)
            foreach(optimized_target IN ITEMS esy_event_bus esy_os public_headers)
                target_compile_options(${optimized_target} PRIVATE
                    ${optimized_compile_flags})
            endforeach()
            file(WRITE "${CMAKE_BINARY_DIR}/optimized-host.txt" "GNU\n")
        endif()
        foreach(language c cpp)
            add_executable(consumer_${language} consumer.${language} layout.c)
            target_link_libraries(consumer_${language} PRIVATE esy_event_bus)
            target_compile_options(consumer_${language} PRIVATE
                -Wall -Wextra -Werror ${optimized_compile_flags})
            target_link_options(consumer_${language} PRIVATE
                "-Wl,--no-gc-sections" ${optimized_link_flags})
        endforeach()
    endif()
elseif(TARGET esy_event_bus)
    message(FATAL_ERROR "Disabled event bus leaked a target")
endif()
'''

PUBLIC_HEADER = r'''
#include "esy_middleware/event_bus/esy_event_bus.h"
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private build options leaked through the public event bus target"
#endif
'''

LAYOUT = PUBLIC_HEADER + r'''
void event_bus_c_layout(size_t *layout) {
    layout[0] = sizeof(esy_event_bus_t);
    layout[1] = _Alignof(esy_event_bus_t);
    layout[2] = sizeof(esy_event_bus_subscription_t);
    layout[3] = _Alignof(esy_event_bus_subscription_t);
}
'''

CONSUMER = PUBLIC_HEADER + r'''
#include <string.h>

#ifdef __cplusplus
#define EVENT_ZERO {}
extern "C" void event_bus_c_layout(size_t *layout);
#else
#define EVENT_ZERO {0}
void event_bus_c_layout(size_t *layout);
#endif

int main(void) {
    size_t layout[4];
    event_bus_c_layout(layout);
#ifdef __cplusplus
    if (layout[0] != sizeof(esy_event_bus_t) ||
        layout[1] != alignof(esy_event_bus_t) ||
        layout[2] != sizeof(esy_event_bus_subscription_t) ||
        layout[3] != alignof(esy_event_bus_subscription_t)) return 1;
#endif
    esy_event_bus_t bus = EVENT_ZERO;
    esy_event_bus_subscription_t slots[2];
    const esy_event_bus_static_resources_t resources = {slots, 2u};
    const esy_event_bus_config_t config = {&resources};
    ESY_ALIGNED_STORAGE(first_buffer, ESY_EVENT_BUS_QUEUE_BYTES(2u));
    ESY_ALIGNED_STORAGE(second_buffer, ESY_EVENT_BUS_QUEUE_BYTES(2u));
    esy_event_bus_subscription_static_resources_t queue_resources = {
        first_buffer.bytes, sizeof(first_buffer.bytes)
    };
    esy_event_bus_subscription_config_t subscription_config = {
        ESY_EVENT_BUS_TOPIC_BIT(0u) | ESY_EVENT_BUS_TOPIC_BIT(31u),
        2u, &queue_resources
    };
    esy_event_bus_subscription_t *first = NULL;
    esy_event_bus_subscription_t *second = NULL;
    if (esy_event_bus_init(&bus, &config) != ESY_OK) return 2;
    if (esy_event_bus_init(&bus, &config) != ESY_ERR_STATE) return 3;
    if (esy_event_bus_subscribe(&bus, &subscription_config, &first) != ESY_OK)
        return 4;
    queue_resources.queue_buffer = second_buffer.bytes;
    queue_resources.queue_buffer_size = sizeof(second_buffer.bytes);
    if (esy_event_bus_subscribe(&bus, &subscription_config, &second) != ESY_OK)
        return 5;

    esy_event_bus_event_t event = EVENT_ZERO;
    esy_event_bus_event_t received = EVENT_ZERO;
    esy_event_bus_publish_result_t report = EVENT_ZERO;
    event.topic = 31u;
    event.size = 1u;
    event.data[0] = 42u;
    if (esy_event_bus_publish(&bus, &event, &report) != ESY_OK ||
        report.matched != 2u || report.delivered != 2u || report.failed != 0u)
        return 6;
    event.data[0] = 99u;
    if (esy_event_bus_receive(first, &received, 0u) != ESY_OK ||
        received.topic != 31u || received.size != 1u || received.data[0] != 42u)
        return 7;
    if (esy_event_bus_receive(second, &received, 0u) != ESY_OK ||
        received.data[0] != 42u) return 8;

    bool need_yield = true;
    if (esy_event_bus_publish_from_isr(&bus, &event, &report, &need_yield)
        != ESY_OK || report.delivered != 2u || need_yield) return 9;
    if (esy_event_bus_receive(first, &received, 0u) != ESY_OK ||
        received.data[0] != 99u) return 10;
    if (esy_event_bus_receive(second, &received, 0u) != ESY_OK ||
        received.data[0] != 99u) return 11;

    if (esy_event_bus_unsubscribe(second) != ESY_OK) return 12;
    if (esy_event_bus_publish(&bus, &event, &report) != ESY_OK ||
        report.matched != 1u || report.delivered != 1u) return 13;
    if (esy_event_bus_deinit(&bus) != ESY_ERR_BUSY) return 14;
    if (esy_event_bus_publish(&bus, &event, &report) != ESY_ERR_STATE) return 15;
    if (esy_event_bus_unsubscribe(first) != ESY_OK ||
        esy_event_bus_deinit(&bus) != ESY_OK) return 16;
    if (esy_event_bus_deinit(&bus) != ESY_OK ||
        esy_event_bus_init(&bus, &config) != ESY_OK ||
        esy_event_bus_deinit(&bus) != ESY_OK) return 17;
    return 0;
}
'''


class EventBusConsumerTests(unittest.TestCase):
    def run_case(self, *, enabled=True, direct=True, rtos="FreeRTOS",
                 queue=True, with_os=True, expected_error=None, cached=None,
                 optimized=False):
        with tempfile.TemporaryDirectory(prefix="event-bus-consumer-") as tmp:
            directory = Path(tmp)
            (directory / "CMakeLists.txt").write_text(PROJECT)
            (directory / "layout.c").write_text(LAYOUT)
            for language in ("c", "cpp"):
                (directory / f"public.{language}").write_text(PUBLIC_HEADER)
                (directory / f"consumer.{language}").write_text(CONSUMER)
            features = {
                "ESY_RTOS": rtos,
                "ENABLE_ESY_SYSTEM": "ON",
                "ENABLE_ESY_EVENT_BUS": "ON" if enabled else "OFF",
                "ENABLE_ESY_OS_QUEUE": "ON" if queue else "OFF",
                "DIRECT_MODULE": "ON" if direct else "OFF",
                "WITH_OS": "ON" if with_os else "OFF",
                "OPTIMIZED_HOST": "ON" if optimized else "OFF",
            }
            (directory / "esy_feature_import.cmake").write_text(
                "".join(f"set({key} {value})\n" for key, value in features.items()))
            configure = subprocess.run(
                ["cmake", "-S", str(directory), "-B", str(directory / "out"),
                 "-G", "Ninja", f"-DROOT={ROOT}",
                 *([f"-DENABLE_ESY_EVENT_BUS={cached}"] if cached else [])],
                capture_output=True, text=True)
            output = configure.stdout + configure.stderr
            if expected_error:
                self.assertNotEqual(configure.returncode, 0, output)
                self.assertIn(expected_error, output)
                return
            self.assertEqual(configure.returncode, 0, output)
            if (directory / "out/optimized-host.txt").exists():
                compile_commands = json.loads(
                    (directory / "out/compile_commands.json").read_text())
                for source in ("esy_event_bus.c", "consumer.c", "consumer.cpp"):
                    command = next(entry["command"] for entry in compile_commands
                                   if Path(entry["file"]).name == source)
                    for flag in ("-O3", "-fstrict-aliasing", "-flto"):
                        self.assertIn(flag, command)
            commands = [["cmake", "--build", str(directory / "out")]]
            if enabled and direct:
                commands.extend([[str(directory / f"out/consumer_{language}")]
                                 for language in ("c", "cpp")])
            for command in commands:
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_direct_c_and_cpp_consumers(self):
        self.run_case(optimized=True)

    def test_aggregate_feature_on_and_off(self):
        for enabled in (False, True):
            with self.subTest(enabled=enabled):
                self.run_case(enabled=enabled, direct=False)

    def test_disabled_module_has_no_os_requirement(self):
        for direct in (False, True):
            with self.subTest(direct=direct):
                self.run_case(enabled=False, direct=direct, rtos="None",
                              queue=False, with_os=False)

    def test_selected_feature_file_overrides_cache(self):
        self.run_case(cached="OFF")
        self.run_case(enabled=False, rtos="None", queue=False,
                      with_os=False, cached="ON")

    def test_rejects_missing_os_and_queue(self):
        for direct in (False, True):
            with self.subTest(direct=direct, dependency="rtos"):
                self.run_case(direct=direct, rtos="None",
                              expected_error="esy_event_bus requires ESY_RTOS=FreeRTOS")
            with self.subTest(direct=direct, dependency="queue"):
                self.run_case(direct=direct, queue=False,
                              expected_error="esy_event_bus requires ENABLE_ESY_OS_QUEUE")
        self.run_case(with_os=False,
                      expected_error="esy_event_bus requires target esy_os")


if __name__ == "__main__":
    unittest.main()
