# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Exercise optional defaults and overrides with independent CMake consumers."""

from pathlib import Path
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
EARLY = ROOT / "cmake/esy_early_import.cmake"
TARGET_RESOLVER = ROOT / "cmake/esy_target_device.cmake"
PROJECT_IMPORT = ROOT / "cmake/esy_project_import.cmake"
THIRD_PARTY_IMPORT = ROOT / "cmake/esy_third_party_import.cmake"
FIRMWARE = ROOT / "cmake/esy_firmware.cmake"
FEATURE_SNAPSHOT = ROOT / "cmake/esy_feature_snapshot.cmake"
VERSION_TEMPLATE = ROOT / "cmake/version.h.in"
STM32_PRE_PROJECT = ROOT / "cmake/ports/stm32f4/pre_project.cmake"
STM32_DEVICE_CASES = (
    ("STM32F401CB", "STM32F401xC"),
    ("STM32F401RC", "STM32F401xC"),
    ("STM32F401RD", "STM32F401xE"),
    ("STM32F401RE", "STM32F401xE"),
    ("STM32F405RG", "STM32F405xx"),
    ("STM32F407ZG", "STM32F407xx"),
    ("STM32F410T8", "STM32F410Tx"),
    ("STM32F410CB", "STM32F410Cx"),
    ("STM32F410R8", "STM32F410Rx"),
    ("STM32F411CC", "STM32F411xE"),
    ("STM32F411RE", "STM32F411xE"),
    ("STM32F412CE", "STM32F412Cx"),
    ("STM32F412RG", "STM32F412Rx"),
    ("STM32F412VE", "STM32F412Vx"),
    ("STM32F412ZG", "STM32F412Zx"),
    ("STM32F413ZH", "STM32F413xx"),
    ("STM32F415RG", "STM32F415xx"),
    ("STM32F417IG", "STM32F417xx"),
    ("STM32F423CH", "STM32F423xx"),
    ("STM32F427ZI", "STM32F427xx"),
    ("STM32F429NI", "STM32F429xx"),
    ("STM32F437VG", "STM32F437xx"),
    ("STM32F439BG", "STM32F439xx"),
    ("STM32F446RE", "STM32F446xx"),
    ("STM32F469NI", "STM32F469xx"),
    ("STM32F479AG", "STM32F479xx"),
    ("stm32f401ret6", "STM32F401xE"),
    ("STM32F412CGU6TR", "STM32F412Cx"),
)
IGNORED_PRIVATE_CONFIGURATIONS = (
    'message(FATAL_ERROR "Private configuration must not be loaded by the library")\n',
    'set(ESY_TARGET_DEVICE PICO2)\n',
    'set(ESY_PLATFORM_SDK_ROOT "SDK selected by a private file")\n',
)
PICO_SDK_FIXTURE = r'''
if(CMAKE_C_COMPILER_LOADED)
  message(FATAL_ERROR "SDK import ran after compiler detection")
endif()
if(EXPECT_EXPLICIT_TOOLCHAIN AND PICO_COMPILER)
  message(FATAL_ERROR "SDK compiler selection would replace the explicit toolchain")
endif()
file(APPEND "${FIXTURE_EVENT_LOG}" "sdk-import\n")
function(pico_sdk_init)
  if(NOT CMAKE_C_COMPILER_LOADED)
    message(FATAL_ERROR "SDK initialization ran before project()")
  endif()
  file(APPEND "${FIXTURE_EVENT_LOG}" "sdk-init\n")
  foreach(provider pico_stdlib pico_time pico_stdio pico_rand pico_atomic)
    if(NOT TARGET ${provider})
      add_library(${provider} INTERFACE)
    endif()
  endforeach()
endfunction()
function(pico_set_linker_script target script)
  set_property(TARGET ${target} PROPERTY FIXTURE_LINKER_SCRIPT "${script}")
endfunction()
function(pico_add_extra_outputs target)
  set_property(TARGET ${target} PROPERTY FIXTURE_EXTRA_OUTPUTS enabled)
endfunction()
function(pico_enable_stdio_uart target enabled)
  set_property(TARGET ${target} PROPERTY FIXTURE_STDIO_uart "${enabled}")
endfunction()
function(pico_enable_stdio_usb target enabled)
  set_property(TARGET ${target} PROPERTY FIXTURE_STDIO_usb "${enabled}")
endfunction()
function(pico_enable_stdio_rtt target enabled)
  set_property(TARGET ${target} PROPERTY FIXTURE_STDIO_rtt "${enabled}")
endfunction()
'''


def cmake_path(path):
    return str(path).replace("\\", "/")


class CmakeIntegrationTests(unittest.TestCase):
    def test_common_only_links_real_c_and_cpp_consumers_with_optional_optimization(self):
        for optimization_target in (False, True):
            with self.subTest(optimization_target=optimization_target), \
                    tempfile.TemporaryDirectory(prefix="esy common only ") as temporary:
                directory = Path(temporary)
                consumer = r'''
#include "esy_common/protocol/esy_proto_tlv.h"
#include "esy_common/ds/esy_list.h"
#include "esy_common/utils/time/esy_datetime.h"
int main(void) {
  esy_list_t list;
  esy_list_node_t node = {0};
  uint8_t payload[8];
  uint16_t length = 0, offset = 0;
  esy_tlv_item_t item;
  if (esy_list_init(&list) != ESY_OK) return 1;
  esy_list_push_back(&list, &node);
  esy_list_clear(&list);
  if (esy_list_count(&list) != 0) return 2;
  if (!esy_tlv_append_u32(payload, sizeof(payload), &length, 42, 123)) return 3;
  if (!esy_tlv_next(payload, length, &offset, &item) || item.label != 42 ||
      item.len != 4 || item.value[0] != 123 || offset != length) return 4;
  const esy_datetime_t epoch = {1970u, 1u, 1u, 0u, 0u, 0u};
  esy_timestamp_t timestamp = {0};
  if (esy_datetime_to_timestamp(&epoch, &timestamp) != ESY_OK ||
      timestamp.seconds != 0 || timestamp.nanoseconds != 0u) return 5;
  return 0;
}
'''
                (directory / "main.c").write_text(consumer)
                (directory / "main.cpp").write_text(consumer)
                provider = ("add_library(esy_opt_default INTERFACE)\n"
                            if optimization_target else "")
                (directory / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(common_only LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(ENABLE_ESY_COMMON_DS ON)
set(ENABLE_ESY_COMMON_PROTOCOL ON)
{provider}
add_subdirectory("{cmake_path(ROOT / 'esy_common')}" common)
add_executable(consumer_c main.c)
add_executable(consumer_cpp main.cpp)
target_link_libraries(consumer_c PRIVATE Esy::common)
target_link_libraries(consumer_cpp PRIVATE Esy::common)
''')
                self.configure(directory)
                self.run_command(["cmake", "--build", str(directory / "out")], directory)
                for executable in ("consumer_c", "consumer_cpp"):
                    self.run_command([str(directory / "out" / executable)], directory)

    def test_current_feature_inputs_and_third_party_use_names_are_preserved(self):
        with tempfile.TemporaryDirectory(prefix="esy current features ") as temporary:
            directory = Path(temporary)
            values = {
                "ESY_RTOS": "None",
                "ENABLE_ESY_LOGGER": "ON",
                "ENABLE_ESY_DUAL_FLASH_STORE": "ON",
                "ENABLE_ESY_FILE_DUAL_FLASH_STORE": "OFF",
                "ENABLE_ESY_NUMERICS": "ON",
                "ENABLE_ESY_NUMERICS_QUANTIZE": "",
                "ENABLE_ESY_PROTO_BIN_UART_CHANNEL": "ON",
                "ENABLE_ESY_PROTO_BIN_USB_CHANNEL": "OFF",
                "ENABLE_ESY_PROTO_BIN_TCP_CHANNEL": "",
                "ENABLE_ESY_PROTO_MODBUS_UART_CHANNEL": "ON",
                "ENABLE_ESY_SPI_SCHEDULER": "OFF",
                "ENABLE_ESY_UART_TX_SCHEDULER": "ON",
                "USE_FULL_ASSERT": "ON",
                "LV_USE_LOG": "OFF",
            }
            checks = "".join(
                f'if(NOT "${{{name}}}" STREQUAL "{value}")\n'
                f'  message(FATAL_ERROR "Project import changed {name}")\n'
                'endif()\n' for name, value in values.items())
            source = self.prepare_pico_consumer(directory,
                f'include("{cmake_path(PROJECT_IMPORT)}")\n' + checks)
            (source / "esy_feature_import.cmake").write_text("".join(
                f'set({name} "{value}")\n' for name, value in values.items()))
            self.configure(source)
            snapshot = json.loads((source / "out/esy_features.json").read_text())
            for name, value in values.items():
                if name.startswith("ENABLE_ESY_"):
                    self.assertIs(value == "ON", snapshot["features"][name])

    def test_reusable_aggregate_leaves_third_party_rtos_variables_alone(self):
        with tempfile.TemporaryDirectory(prefix="esy unrelated os variable ") as temporary:
            directory = Path(temporary)
            (directory / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(third_party_os_variable LANGUAGES C)
set(ESY_RTOS None)
set(RTOS third_party_selection)
add_subdirectory("{cmake_path(ROOT / 'esy_common')}" common)
if(NOT RTOS STREQUAL "third_party_selection" OR NOT ESY_RTOS STREQUAL "None")
  message(FATAL_ERROR "Reusable assembly changed an unrelated third-party input")
endif()
''')
            self.configure(directory)

    def test_feature_snapshot_records_effective_normal_values_over_cache(self):
        for enabled in (False, True):
            with self.subTest(enabled=enabled), \
                    tempfile.TemporaryDirectory(prefix="esy effective features ") as temporary:
                directory = Path(temporary)
                source = self.prepare_pico_consumer(directory,
                    'add_library(lvgl INTERFACE)\n' +
                    f'include("{cmake_path(PROJECT_IMPORT)}")')
                feature = source / "esy_feature_import.cmake"
                feature.write_text(f'set(ENABLE_ESY_LVGL {"ON" if enabled else "OFF"})\n')
                cached = "OFF" if enabled else "ON"
                self.configure(source, extra_args=(f"-DENABLE_ESY_LVGL:BOOL={cached}",))
                snapshot = json.loads((source / "out/esy_features.json").read_text())
                self.assertEqual(1, snapshot["schema_version"])
                self.assertEqual("PICO2_W", snapshot["target_device"])
                self.assertEqual(str(source.resolve()), snapshot["project_root"])
                self.assertEqual(str(feature.resolve()), snapshot["feature_import_file"])
                self.assertIs(enabled, snapshot["features"]["ENABLE_ESY_LVGL"])
                self.assertIn(f"ENABLE_ESY_LVGL:BOOL={cached}",
                              (source / "out/CMakeCache.txt").read_text())

    def test_feature_snapshot_normalizes_cmake_boolean_values(self):
        with tempfile.TemporaryDirectory(prefix="esy normalized features ") as temporary:
            directory = Path(temporary)
            source = self.prepare_pico_consumer(directory,
                f'include("{cmake_path(PROJECT_IMPORT)}")')
            values = {"ON": True, "YES": True, "TRUE": True, "1": True,
                      "OFF": False, "NO": False, "FALSE": False, "0": False,
                      "": False, "missing-NOTFOUND": False, "custom": True}
            (source / "esy_feature_import.cmake").write_text(
                "".join(f'set(ENABLE_ESY_PROBE_{index} "{value}")\n'
                        for index, value in enumerate(values)) +
                'set(CONFIG_LV_BUILD_DEMOS OFF)\n')
            self.configure(source)
            snapshot = json.loads((source / "out/esy_features.json").read_text())
            self.assertEqual({f"ENABLE_ESY_PROBE_{index}": enabled
                              for index, enabled in enumerate(values.values())},
                             snapshot["features"])

    def test_feature_snapshot_encodes_quoted_source_and_feature_paths(self):
        with tempfile.TemporaryDirectory(prefix='esy snapshot "quoted" ') as temporary:
            source = Path(temporary)
            feature = source / 'features "quoted"\t.cmake'
            feature.write_text("set(ENABLE_ESY_PROBE ON)\n")
            (source / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(snapshot_paths LANGUAGES NONE)
set(ESY_TARGET_DEVICE PICO2_W)
set(ESY_FEATURE_IMPORT_FILE [==[{feature}]==])
include("{cmake_path(FEATURE_SNAPSHOT)}")
esy_generate_feature_snapshot()
''')
            self.configure(source)
            snapshot = json.loads((source / "out/esy_features.json").read_text())
            self.assertEqual(str(source.resolve()), snapshot["project_root"])
            self.assertEqual(str(feature.resolve()), snapshot["feature_import_file"])

    def test_feature_snapshot_json_strings_escape_backslashes_and_controls(self):
        with tempfile.TemporaryDirectory(prefix="esy snapshot escaping ") as temporary:
            directory = Path(temporary)
            script = directory / "escape.cmake"
            value = 'quoted " path \\ with\ttab\nand\rreturn'
            script.write_text(f'''
cmake_minimum_required(VERSION 3.20)
include("{cmake_path(FEATURE_SNAPSHOT)}")
set(value [==[{value}]==])
esy_feature_snapshot_json_string("${{value}}" encoded)
file(WRITE "{cmake_path(directory / 'value.json')}" "${{encoded}}")
''')
            self.run_command(["cmake", "-P", str(script)], directory)
            self.assertEqual(value, json.loads((directory / "value.json").read_text()))

    def test_feature_snapshot_supports_an_explicitly_disabled_feature_file(self):
        with tempfile.TemporaryDirectory(prefix="esy no feature file ") as temporary:
            directory = Path(temporary)
            source = self.prepare_pico_consumer(directory,
                f'include("{cmake_path(PROJECT_IMPORT)}")',
                before_project='set(ESY_FEATURE_IMPORT_FILE "")\n')
            (source / "esy_feature_import.cmake").write_text(
                'message(FATAL_ERROR "Disabled feature file was read")\n')
            self.configure(source)
            snapshot = json.loads((source / "out/esy_features.json").read_text())
            self.assertEqual("", snapshot["feature_import_file"])
            self.assertEqual({}, snapshot["features"])

    def test_feature_snapshot_isolates_independent_consumers(self):
        with tempfile.TemporaryDirectory(prefix="esy separate snapshots ") as temporary:
            directory = Path(temporary)
            snapshots = []
            for name, enabled in (("first", True), ("second", False)):
                source = self.prepare_pico_consumer(directory / name,
                    f'include("{cmake_path(PROJECT_IMPORT)}")')
                (source / "esy_feature_import.cmake").write_text(
                    f'set(ENABLE_ESY_TOUCH {"ON" if enabled else "OFF"})\n')
                self.configure(source)
                path = source / "out/esy_features.json"
                snapshot = json.loads(path.read_text())
                self.assertEqual(str(source.resolve()), snapshot["project_root"])
                self.assertIs(enabled, snapshot["features"]["ENABLE_ESY_TOUCH"])
                snapshots.append((path, snapshot))
            for path, expected in snapshots:
                self.assertEqual(expected, json.loads(path.read_text()))

    def test_feature_snapshot_is_removed_before_failed_early_reconfiguration(self):
        with tempfile.TemporaryDirectory(prefix="esy failed early snapshot ") as temporary:
            directory = Path(temporary)
            source = self.prepare_pico_consumer(directory,
                f'include("{cmake_path(PROJECT_IMPORT)}")')
            self.configure(source)
            snapshot = source / "out/esy_features.json"
            self.assertTrue(snapshot.is_file())
            cmake = source / "CMakeLists.txt"
            cmake.write_text(cmake.read_text().replace(
                "set(ESY_TARGET_DEVICE PICO2_W)", "set(ESY_TARGET_DEVICE UNKNOWN)"))
            self.configure(source, error="Unsupported ESY_TARGET_DEVICE: UNKNOWN")
            self.assertFalse(snapshot.exists())

    def test_feature_snapshot_is_removed_before_failed_project_import(self):
        with tempfile.TemporaryDirectory(prefix="esy failed import snapshot ") as temporary:
            source = Path(temporary)
            feature = source / "features.cmake"
            feature.write_text("set(ENABLE_ESY_TOUCH OFF)\n")
            (source / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(snapshot_without_early LANGUAGES C)
set(ESY_TARGET_DEVICE PICO2_W)
set(ESY_BOARD_PATH "${{CMAKE_CURRENT_SOURCE_DIR}}/board")
set(ESY_FEATURE_IMPORT_FILE "${{CMAKE_CURRENT_SOURCE_DIR}}/features.cmake")
add_library(pico_stdlib INTERFACE)
include("{cmake_path(PROJECT_IMPORT)}")
''')
            self.configure(source)
            snapshot = source / "out/esy_features.json"
            self.assertTrue(snapshot.is_file())
            feature.write_text('message(FATAL_ERROR "Feature configuration failed")\n')
            self.configure(source, error="Feature configuration failed")
            self.assertFalse(snapshot.exists())

    def test_failed_generation_does_not_make_a_snapshot_a_success_marker(self):
        with tempfile.TemporaryDirectory(prefix="esy failed generate snapshot ") as temporary:
            directory = Path(temporary)
            source = self.prepare_pico_consumer(directory,
                f'include("{cmake_path(PROJECT_IMPORT)}")')
            feature = source / "esy_feature_import.cmake"
            feature.write_text("set(ENABLE_ESY_TOUCH OFF)\n")
            self.configure(source)
            snapshot = source / "out/esy_features.json"
            self.assertIs(False, json.loads(snapshot.read_text())["features"]["ENABLE_ESY_TOUCH"])
            feature.write_text("set(ENABLE_ESY_TOUCH ON)\n")
            cmake = source / "CMakeLists.txt"
            cmake.write_text(cmake.read_text() +
                '\nadd_executable(broken_image main.c)\n'
                'target_link_libraries(broken_image PRIVATE missing::provider)\n')
            result = self.configure(source, error="missing::provider")
            self.assertNotEqual(0, result.returncode)
            # file(GENERATE) may have written the attempted configuration before
            # another generator failed. A report is never a success marker.
            if snapshot.exists():
                self.assertIs(True,
                    json.loads(snapshot.read_text())["features"]["ENABLE_ESY_TOUCH"])

    def test_script_dependency_discovery_does_not_invalidate_a_build_snapshot(self):
        with tempfile.TemporaryDirectory(prefix="esy script snapshot ") as temporary:
            directory = Path(temporary)
            snapshot = directory / "esy_features.json"
            content = '{"existing_build": true}\n'
            snapshot.write_text(content)
            script = directory / "discover.cmake"
            script.write_text(f'''
cmake_minimum_required(VERSION 3.20)
set(ESY_TARGET_DEVICE PICO2_W)
include("{cmake_path(EARLY)}")
''')
            self.run_command(["cmake", "-P", str(script)], directory)
            self.assertEqual(content, snapshot.read_text())

    def run_command(self, command, directory, error=None):
        result = subprocess.run(command, cwd=directory, capture_output=True, text=True)
        output = result.stdout + result.stderr
        if error is None:
            self.assertEqual(0, result.returncode, output)
        else:
            self.assertNotEqual(0, result.returncode, output)
            self.assertIn(error, output)
        return result

    def prepare_pico_consumer(self, directory, body, before_project="", bind_board=True,
                              project_version="1.2.3",
                              selection_inputs="set(ESY_TARGET_DEVICE PICO2_W)"):
        sdk = directory / "sdk provider outside consumer"
        (sdk / "external").mkdir(parents=True)
        (sdk / "external/pico_sdk_import.cmake").write_text(PICO_SDK_FIXTURE)
        source = directory / "firmware source with spaces"
        source.mkdir()
        (source / "main.c").write_text("int main(void) { return 0; }\n")
        board_input = (f'set(ESY_BOARD_PATH "{cmake_path(directory / "board outside consumer")}")'
                       if bind_board else "")
        version_input = f"VERSION {project_version}" if project_version else ""
        (source / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.20)
set(FIXTURE_EVENT_LOG "{cmake_path(directory / 'events.txt')}")
{selection_inputs}
set(PICO_SDK_PATH "{cmake_path(sdk)}")
{before_project}
include("{cmake_path(EARLY)}")
project(UnrelatedProjectName {version_input} LANGUAGES C CXX)
file(APPEND "${{FIXTURE_EVENT_LOG}}" "project\n")
{board_input}
{body}
''')
        return source

    def configure(self, source, error=None, extra_args=()):
        return self.run_command(
            ["cmake", "-S", str(source), "-B", str(source / "out"), "-G", "Ninja", *extra_args],
            source, error)

    def platform_assembly(self, directory, board_body="add_library(esy_board INTERFACE)\n"):
        board = directory / "consumer board for library assembly"
        board.mkdir(exist_ok=True)
        (board / "CMakeLists.txt").write_text(board_body)
        return (f'set(ESY_BOARD_PATH "{cmake_path(board)}")\n'
                f'add_subdirectory("{cmake_path(ROOT)}" platform)\n')

    def prepare_capability_consumer(self, directory, platform, features, backend_only=False,
                                    device=None, native_app_update=False, native_uart=None,
                                    native_pwm=None, native_i2c=False,
                                    native_capture=False, native_spi=False, native_os_context=False,
                                    native_freertos=False, native_random=True):
        (directory / "esy_feature_import.cmake").write_text(features)
        native_provider = ("add_library(idf_app_update INTERFACE)\n"
                           "add_library(idf::app_update ALIAS idf_app_update)\n"
                           if native_app_update else "")
        if native_os_context:
            native_provider += "add_library(esy_os_context INTERFACE)\n"
        if platform == "ESP32" and native_random:
            native_provider += ("add_library(idf_random_provider INTERFACE)\n"
                                "add_library(idf::esp_hw_support ALIAS idf_random_provider)\n")
        if native_freertos:
            native_provider += ("set(ESY_RTOS FreeRTOS)\n"
                                "add_library(native_freertos INTERFACE)\n"
                                "set(FREE_RTOS_KERNEL_LIB native_freertos)\n")
        if native_uart:
            native_provider += ("add_library(idf_uart_provider INTERFACE)\n"
                                f"add_library({native_uart} ALIAS idf_uart_provider)\n")
        if native_pwm:
            native_provider += ("add_library(idf_pwm_provider INTERFACE)\n"
                                f"add_library({native_pwm} ALIAS idf_pwm_provider)\n")
        if native_i2c:
            native_provider += ("add_library(idf_i2c_provider INTERFACE)\n"
                                "add_library(idf::esp_driver_i2c ALIAS idf_i2c_provider)\n")
        if native_spi:
            native_provider += ("add_library(idf_spi_provider INTERFACE)\n"
                                "add_library(idf::esp_driver_spi ALIAS idf_spi_provider)\n")
        if native_pwm == "idf::esp_driver_ledc" or native_i2c or native_capture or native_spi:
            native_provider += ("add_library(idf_gpio_provider INTERFACE)\n"
                                "add_library(idf::esp_driver_gpio ALIAS idf_gpio_provider)\n")
        if native_capture or native_i2c:
            native_provider += ("add_library(idf_timer_provider INTERFACE)\n"
                                "add_library(idf::esp_timer ALIAS idf_timer_provider)\n")
        assembly = (f'add_subdirectory("{cmake_path(ROOT / "esy_hal_backend")}" backend)\n'
                    if backend_only else self.platform_assembly(directory))
        (directory / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(capability_consumer C)
set(CMAKE_C_STANDARD 11)
set(ESY_TARGET_DEVICE {device or ('STM32F407ZG' if platform == 'STM32F4' else platform)})
include(esy_feature_import.cmake)
foreach(provider esy_opt_default esy_opt_speed STM32_Drivers pico_time pico_stdio pico_rand
                 pico_stdlib pico_multicore hardware_watchdog hardware_dma hardware_pio tinyusb_device tinyusb_board)
  add_library(${{provider}} INTERFACE)
endforeach()
function(pico_generate_pio_header)
endfunction()
{native_provider}
{assembly}
foreach(target esy_hal_backend esy_hal esy_hb_stm32f4_interrupts esy_os_context)
  if(TARGET ${{target}})
    get_target_property(sources ${{target}} SOURCES)
    file(WRITE "${{CMAKE_BINARY_DIR}}/${{target}}-sources.txt" "${{sources}}\n")
    get_target_property(dependencies ${{target}} LINK_LIBRARIES)
    file(WRITE "${{CMAKE_BINARY_DIR}}/${{target}}-dependencies.txt" "${{dependencies}}\n")
  endif()
endforeach()
file(WRITE "${{CMAKE_BINARY_DIR}}/generic-dma-gate.txt" "${{ENABLE_ESY_DMA}}")
file(WRITE "${{CMAKE_BINARY_DIR}}/public-gpio-gate.txt" "${{ENABLE_ESY_GPIO}}")
''')

    def test_calendar_rtc_facade_follows_its_gate_without_native_providers(self):
        for platform in ("PICO2_W", "STM32F4", "ESP32"):
            for enabled in (False, True):
                with self.subTest(platform=platform, enabled=enabled), \
                        tempfile.TemporaryDirectory(prefix="esy portable RTC facade ") as temporary:
                    directory = Path(temporary)
                    self.prepare_capability_consumer(directory, platform,
                        f"set(ENABLE_ESY_RTC {'ON' if enabled else 'OFF'})\n")
                    self.configure(directory)
                    hal = (directory / "out/esy_hal-sources.txt").read_text()
                    backend = (directory / "out/esy_hal_backend-sources.txt").read_text()
                    if enabled:
                        self.assertIn("esy_hal_rtc.c", hal)
                    else:
                        self.assertNotIn("esy_hal_rtc.c", hal)
                    self.assertNotIn("esy_hb_rtc.c", backend)
                    self.assertNotIn("esy_hal_system.c", hal)

    def test_capture_irq_provider_does_not_enable_the_public_gpio_facade(self):
        for platform in ("PICO2_W", "ESP32"):
            for backend_only in (False, True):
                for enabled in (False, True):
                    with self.subTest(platform=platform, backend_only=backend_only,
                                      enabled=enabled), tempfile.TemporaryDirectory(
                            prefix="esy capture without GPIO facade ") as temporary:
                        directory = Path(temporary)
                        features = ("set(ENABLE_ESY_GPIO OFF)\n"
                                    f"set(ENABLE_ESY_TIMER_CAPTURE {'ON' if enabled else 'OFF'})\n")
                        self.prepare_capability_consumer(directory, platform, features,
                            backend_only, native_capture=platform == "ESP32" and enabled)
                        self.configure(directory)
                        sources = (directory / "out/esy_hal_backend-sources.txt").read_text()
                        dependencies = (directory / "out/esy_hal_backend-dependencies.txt").read_text()
                        for source in ("esy_hb_gpio.c", "esy_hb_timer_capture.c"):
                            if enabled:
                                self.assertIn(source, sources)
                            else:
                                self.assertNotIn(source, sources)
                        providers = (("hardware_gpio", "hardware_irq") if platform == "PICO2_W"
                                     else ("idf::esp_driver_gpio", "idf::esp_timer"))
                        for provider in providers:
                            if enabled:
                                self.assertIn(provider, dependencies)
                            else:
                                self.assertNotIn(provider, dependencies)
                        self.assertEqual("OFF", (directory / "out/public-gpio-gate.txt").read_text())
                        if not backend_only:
                            hal = (directory / "out/esy_hal-sources.txt").read_text()
                            self.assertNotIn("esy_hal_gpio.c", hal)
                            if enabled:
                                self.assertIn("esy_hal_timer_capture.c", hal)
                            else:
                                self.assertNotIn("esy_hal_timer_capture.c", hal)

    def test_esp32_capture_requires_its_native_gpio_and_timer_providers(self):
        for backend_only in (False, True):
            for provider, fixture_target in (("esp_driver_gpio", "gpio"), ("esp_timer", "timer")):
                with self.subTest(backend_only=backend_only, provider=provider), \
                        tempfile.TemporaryDirectory(prefix="esy capture missing provider ") as temporary:
                    directory = Path(temporary)
                    self.prepare_capability_consumer(directory, "ESP32",
                        "set(ENABLE_ESY_TIMER_CAPTURE ON)\nset(ENABLE_ESY_GPIO OFF)\n",
                        backend_only, native_capture=True)
                    source = directory / "CMakeLists.txt"
                    source.write_text(source.read_text().replace(
                        f"add_library(idf::{provider} ALIAS idf_{fixture_target}_provider)", ""))
                    self.configure(directory, f"capture requires native idf::{provider}")

    def test_native_peripheral_dma_does_not_enable_the_generic_facade(self):
        for platform in ("PICO2_W", "STM32F4", "ESP32"):
            with self.subTest(platform=platform), tempfile.TemporaryDirectory(
                    prefix="esy native peripheral DMA ") as temporary:
                directory = Path(temporary)
                features = ("set(ENABLE_ESY_COMMON_DS ON)\nset(ENABLE_ESY_DMA OFF)\n"
                            "set(ENABLE_ESY_UART ON)\nset(ENABLE_ESY_SPI ON)\n"
                            "set(ENABLE_ESY_PWM ON)\nset(ENABLE_ESY_I2C ON)\n")
                if platform == "STM32F4":
                    features += "set(ENABLE_ESY_ADC ON)\n"
                self.prepare_capability_consumer(directory, platform, features,
                    native_uart="idf::esp_driver_uart" if platform == "ESP32" else None,
                    native_pwm="idf::esp_driver_ledc" if platform == "ESP32" else None,
                    native_i2c=platform == "ESP32", native_spi=platform == "ESP32")
                self.configure(directory)
                backend = (directory / "out/esy_hal_backend-sources.txt").read_text()
                hal = (directory / "out/esy_hal-sources.txt").read_text()
                self.assertIn("esy_hb_dma.c", backend)
                self.assertNotIn("esy_hal_dma.c", hal)
                self.assertEqual("OFF", (directory / "out/generic-dma-gate.txt").read_text())
                if platform == "STM32F4":
                    vectors = (directory / "out/esy_hb_stm32f4_interrupts-sources.txt").read_text()
                    self.assertIn("stm32f4xx_it_dma.c", vectors)

    def test_stm32_spi_chip_select_provider_does_not_enable_public_gpio_or_exti(self):
        for backend_only in (False, True):
            for enabled in (False, True):
                with self.subTest(backend_only=backend_only, enabled=enabled), \
                        tempfile.TemporaryDirectory(prefix="esy SPI without GPIO facade ") as temporary:
                    directory = Path(temporary)
                    features = ("set(ENABLE_ESY_GPIO OFF)\nset(ENABLE_ESY_DMA OFF)\n"
                                f"set(ENABLE_ESY_SPI {'ON' if enabled else 'OFF'})\n")
                    self.prepare_capability_consumer(directory, "STM32F4", features,
                                                    backend_only, device="STM32F407ZG")
                    self.configure(directory)
                    backend = (directory / "out/esy_hal_backend-sources.txt").read_text()
                    if enabled:
                        self.assertIn("esy_hb_gpio.c", backend)
                        self.assertIn("esy_hb_spi.c", backend)
                    else:
                        self.assertNotIn("esy_hb_gpio.c", backend)
                        self.assertNotIn("esy_hb_spi.c", backend)
                    vectors = directory / "out/esy_hb_stm32f4_interrupts-sources.txt"
                    if vectors.exists():
                        self.assertNotIn("stm32f4xx_it_gpio.c", vectors.read_text())
                    self.assertEqual("OFF", (directory / "out/public-gpio-gate.txt").read_text())
                    if not backend_only:
                        hal = (directory / "out/esy_hal-sources.txt").read_text()
                        self.assertNotIn("esy_hal_gpio.c", hal)

    def test_i2c_alone_does_not_assemble_the_shared_dma_backend(self):
        for platform in ("PICO2_W", "STM32F4", "ESP32"):
            with self.subTest(platform=platform), tempfile.TemporaryDirectory(
                    prefix="esy I2C without shared DMA ") as temporary:
                directory = Path(temporary)
                self.prepare_capability_consumer(directory, platform,
                    "set(ENABLE_ESY_I2C ON)\nset(ENABLE_ESY_DMA OFF)\n",
                    native_i2c=platform == "ESP32")
                self.configure(directory)
                backend = (directory / "out/esy_hal_backend-sources.txt").read_text()
                hal = (directory / "out/esy_hal-sources.txt").read_text()
                self.assertIn("esy_hb_i2c.c", backend)
                self.assertNotIn("esy_hb_dma.c", backend)
                self.assertNotIn("esy_hal_dma.c", hal)
                self.assertFalse((directory / "out/esy_hb_stm32f4_interrupts-sources.txt").exists())

    def test_stm32_logical_timer_vectors_follow_the_timer_gate(self):
        for enabled in (False, True):
            with self.subTest(enabled=enabled), tempfile.TemporaryDirectory(
                    prefix="esy logical timer vectors ") as temporary:
                directory = Path(temporary)
                features = ("set(ENABLE_ESY_SYSTEM ON)\n"
                            f"set(ENABLE_ESY_TIMER {'ON' if enabled else 'OFF'})\n")
                self.prepare_capability_consumer(directory, "STM32F4", features,
                                                device="STM32F407ZG")
                self.configure(directory)
                vectors = (directory / "out/esy_hb_stm32f4_interrupts-sources.txt").read_text()
                if enabled:
                    self.assertIn("stm32f4xx_it_timer.c", vectors)
                else:
                    self.assertNotIn("stm32f4xx_it_timer.c", vectors)

    def test_esp32_system_only_does_not_require_boot_or_app_update(self):
        for backend_only in (False, True):
            with self.subTest(backend_only=backend_only), \
                    tempfile.TemporaryDirectory(prefix="esy system without OTA provider ") as tmp:
                directory = Path(tmp)
                self.prepare_capability_consumer(directory, "ESP32",
                    "set(ENABLE_ESY_SYSTEM ON)\n", backend_only, native_os_context=True)
                self.configure(directory)
                dependencies = (directory / "out/esy_hal_backend-dependencies.txt").read_text()
                self.assertIn("esy_os_context", dependencies)
                self.assertIn("idf::esp_hw_support", dependencies)
                self.assertNotIn("app_update", dependencies)
                sources = (directory / "out/esy_hal_backend-sources.txt").read_text()
                self.assertIn("esy_hb_system.c", sources)
                self.assertNotIn("esy_hb_boot.c", sources)

    def test_system_random_provider_is_explicit_and_only_required_when_system_is_enabled(self):
        for enabled in (False, True):
            with self.subTest(enabled=enabled), tempfile.TemporaryDirectory(
                    prefix="esy system random provider ") as tmp:
                directory = Path(tmp)
                self.prepare_capability_consumer(directory, "ESP32",
                    f"set(ENABLE_ESY_SYSTEM {'ON' if enabled else 'OFF'})\n",
                    backend_only=True, native_os_context=True, native_random=False)
                self.configure(directory,
                    "ESP32 system random source requires native idf::esp_hw_support"
                    if enabled else None)

    def test_pico_system_privately_links_random_without_selecting_flash(self):
        with tempfile.TemporaryDirectory(prefix="esy Pico random provider ") as tmp:
            directory = Path(tmp)
            self.prepare_capability_consumer(directory, "PICO2_W",
                "set(ENABLE_ESY_SYSTEM ON)\nset(ENABLE_ESY_FLASH OFF)\n")
            self.configure(directory)
            dependencies = (directory / "out/esy_hal_backend-dependencies.txt").read_text()
            self.assertIn("pico_rand", dependencies)
            sources = (directory / "out/esy_hal_backend-sources.txt").read_text()
            self.assertNotIn("esy_hb_flash.c", sources)

    def test_esp32_system_assembles_native_os_context_without_app_update(self):
        with tempfile.TemporaryDirectory(prefix="esy native system context ") as tmp:
            directory = Path(tmp)
            self.prepare_capability_consumer(directory, "ESP32",
                "set(ENABLE_ESY_SYSTEM ON)\n", native_freertos=True)
            self.configure(directory)
            dependencies = (directory / "out/esy_os_context-dependencies.txt").read_text()
            self.assertIn("native_freertos", dependencies)
            self.assertNotIn("app_update", dependencies)
            sources = (directory / "out/esy_os_context-sources.txt").read_text()
            self.assertIn("esy_os_context.c", sources)
            self.assertNotIn("app_update", (directory /
                "out/esy_hal_backend-dependencies.txt").read_text())

    def test_esp32_system_requires_os_context_independently_of_app_update(self):
        for backend_only in (False, True):
            for app_update in (False, True):
                with self.subTest(backend_only=backend_only, app_update=app_update), \
                        tempfile.TemporaryDirectory(prefix="esy system context provider ") as tmp:
                    directory = Path(tmp)
                    self.prepare_capability_consumer(directory, "ESP32",
                        "set(ENABLE_ESY_SYSTEM ON)\nset(ESY_RTOS FreeRTOS)\n", backend_only,
                        native_app_update=app_update)
                    error = ("requires the native esy_os_context port target" if backend_only
                             else "requires the native FreeRTOS provider target")
                    self.configure(directory, error)

    def test_esp32_boot_requires_native_app_update_provider(self):
        for backend_only in (False, True):
            with self.subTest(backend_only=backend_only), \
                    tempfile.TemporaryDirectory(prefix="esy native handoff provider ") as tmp:
                directory = Path(tmp)
                self.prepare_capability_consumer(directory, "ESP32",
                    "set(ENABLE_ESY_SYSTEM ON)\nset(ENABLE_ESY_FLASH ON)\n"
                    "set(ENABLE_ESY_BOOTLOADER ON)\n"
                    "set(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE ON)\n", backend_only,
                    native_os_context=True)
                self.configure(directory, "requires the native idf::app_update component")

    def test_stm32_adc_vector_follows_the_adc_gate(self):
        for enabled in (False, True):
            with self.subTest(enabled=enabled), tempfile.TemporaryDirectory(
                    prefix="esy ADC interrupt vector ") as temporary:
                directory = Path(temporary)
                features = ("set(ENABLE_ESY_SYSTEM ON)\nset(ENABLE_ESY_DMA ON)\n"
                            f"set(ENABLE_ESY_ADC {'ON' if enabled else 'OFF'})\n")
                self.prepare_capability_consumer(directory, "STM32F4", features,
                                                device="STM32F407ZG")
                self.configure(directory)
                vectors = (directory / "out/esy_hb_stm32f4_interrupts-sources.txt").read_text()
                if enabled:
                    self.assertIn("stm32f4xx_it_adc.c", vectors)
                else:
                    self.assertNotIn("stm32f4xx_it_adc.c", vectors)

    def test_esp32_uart_requires_native_sdk_provider(self):
        for backend_only in (False, True):
            with self.subTest(backend_only=backend_only), tempfile.TemporaryDirectory(
                    prefix="esy native UART provider ") as temporary:
                directory = Path(temporary)
                self.prepare_capability_consumer(directory, "ESP32",
                    "set(ENABLE_ESY_UART ON)\nset(ENABLE_ESY_DMA ON)\n", backend_only)
                self.configure(directory, "UART requires the native idf::esp_driver_uart")

    def test_esp32_uart_provider_follows_the_feature_gate(self):
        provider = "idf::esp_driver_uart"
        for enabled in (False, True):
            with self.subTest(provider=provider, enabled=enabled), \
                    tempfile.TemporaryDirectory(prefix="esy gated UART provider ") as temporary:
                directory = Path(temporary)
                features = (f"set(ENABLE_ESY_UART {'ON' if enabled else 'OFF'})\n"
                            "set(ENABLE_ESY_DMA ON)\n")
                self.prepare_capability_consumer(directory, "ESP32", features,
                                                native_uart=provider)
                self.configure(directory)
                dependencies = (directory / "out/esy_hal_backend-dependencies.txt").read_text()
                sources = (directory / "out/esy_hal_backend-sources.txt").read_text()
                if enabled:
                    self.assertIn(provider, dependencies)
                    self.assertIn("esy_hb_uart.c", sources)
                else:
                    self.assertNotIn(provider, dependencies)
                    self.assertNotIn("esy_hb_uart.c", sources)

    def test_esp32_i2c_requires_native_sdk_provider(self):
        for backend_only in (False, True):
            with self.subTest(backend_only=backend_only), tempfile.TemporaryDirectory(
                    prefix="esy native I2C provider ") as temporary:
                directory = Path(temporary)
                self.prepare_capability_consumer(directory, "ESP32",
                    "set(ENABLE_ESY_I2C ON)\nset(ENABLE_ESY_DMA ON)\n", backend_only)
                self.configure(directory, "I2C requires the native idf::esp_driver_i2c component")

    def test_esp32_spi_requires_native_sdk_provider(self):
        for backend_only in (False, True):
            with self.subTest(backend_only=backend_only), tempfile.TemporaryDirectory(
                    prefix="esy native SPI provider ") as temporary:
                directory = Path(temporary)
                self.prepare_capability_consumer(directory, "ESP32",
                    "set(ENABLE_ESY_SPI ON)\nset(ENABLE_ESY_DMA OFF)\n", backend_only)
                self.configure(directory, "SPI requires the native idf::esp_driver_spi component")

    def test_esp32_spi_provider_follows_the_feature_gate(self):
        for backend_only in (False, True):
            for enabled in (False, True):
                for provider in (False, True):
                    if enabled and not provider:
                        continue
                    with self.subTest(backend_only=backend_only, enabled=enabled,
                                      provider=provider), tempfile.TemporaryDirectory(
                            prefix="esy gated SPI provider ") as temporary:
                        directory = Path(temporary)
                        features = (f"set(ENABLE_ESY_SPI {'ON' if enabled else 'OFF'})\n"
                                    "set(ENABLE_ESY_DMA OFF)\n")
                        self.prepare_capability_consumer(directory, "ESP32", features,
                                                        backend_only, native_spi=provider)
                        self.configure(directory)
                        dependencies = (directory / "out/esy_hal_backend-dependencies.txt").read_text()
                        sources = (directory / "out/esy_hal_backend-sources.txt").read_text()
                        if enabled:
                            self.assertIn("idf::esp_driver_spi", dependencies)
                            self.assertIn("idf::esp_driver_gpio", dependencies)
                            self.assertIn("esy_hb_spi.c", sources)
                        else:
                            self.assertNotIn("idf::esp_driver_spi", dependencies)
                            self.assertNotIn("idf::esp_driver_gpio", dependencies)
                            self.assertNotIn("esy_hb_spi.c", sources)

    def test_esp32_spi_provider_requires_gpio_headers(self):
        for backend_only in (False, True):
            with self.subTest(backend_only=backend_only), tempfile.TemporaryDirectory(
                    prefix="esy SPI GPIO provider ") as temporary:
                directory = Path(temporary)
                self.prepare_capability_consumer(directory, "ESP32",
                    "set(ENABLE_ESY_SPI ON)\nset(ENABLE_ESY_DMA OFF)\n",
                    backend_only, native_spi=True)
                source = directory / "CMakeLists.txt"
                source.write_text(source.read_text().replace(
                    "add_library(idf::esp_driver_gpio ALIAS idf_gpio_provider)", ""))
                self.configure(directory, "SPI chip-select control requires idf::esp_driver_gpio")

    def test_esp32_i2c_provider_follows_the_feature_gate(self):
        for backend_only in (False, True):
            for enabled in (False, True):
                for provider in (False, True):
                    if enabled and not provider:
                        continue
                    with self.subTest(backend_only=backend_only, enabled=enabled,
                                      provider=provider), tempfile.TemporaryDirectory(
                            prefix="esy gated I2C provider ") as temporary:
                        directory = Path(temporary)
                        features = (f"set(ENABLE_ESY_I2C {'ON' if enabled else 'OFF'})\n"
                                    "set(ENABLE_ESY_DMA ON)\n")
                        self.prepare_capability_consumer(directory, "ESP32", features,
                                                        backend_only, native_i2c=provider)
                        self.configure(directory)
                        dependencies = (directory / "out/esy_hal_backend-dependencies.txt").read_text()
                        sources = (directory / "out/esy_hal_backend-sources.txt").read_text()
                        if enabled:
                            self.assertIn("idf::esp_driver_i2c", dependencies)
                            self.assertIn("idf::esp_driver_gpio", dependencies)
                            self.assertIn("idf::esp_timer", dependencies)
                            self.assertIn("esy_hb_i2c.c", sources)
                        else:
                            self.assertNotIn("idf::esp_driver_i2c", dependencies)
                            self.assertNotIn("idf::esp_driver_gpio", dependencies)
                            self.assertNotIn("idf::esp_timer", dependencies)
                            self.assertNotIn("esy_hb_i2c.c", sources)

    def test_esp32_i2c_requires_native_deadline_provider(self):
        for backend_only in (False, True):
            with self.subTest(backend_only=backend_only), tempfile.TemporaryDirectory(
                    prefix="esy I2C deadline provider ") as temporary:
                directory = Path(temporary)
                self.prepare_capability_consumer(directory, "ESP32",
                    "set(ENABLE_ESY_I2C ON)\n", backend_only, native_i2c=True)
                source = directory / "CMakeLists.txt"
                source.write_text(source.read_text().replace(
                    "add_library(idf::esp_timer ALIAS idf_timer_provider)", ""))
                self.configure(directory, "I2C transaction deadlines require native idf::esp_timer")

    def test_esp32_pwm_requires_native_sdk_provider(self):
        for backend_only in (False, True):
            with self.subTest(backend_only=backend_only), tempfile.TemporaryDirectory(
                    prefix="esy native PWM provider ") as temporary:
                directory = Path(temporary)
                self.prepare_capability_consumer(directory, "ESP32",
                    "set(ENABLE_ESY_PWM ON)\nset(ENABLE_ESY_DMA ON)\n", backend_only)
                self.configure(directory, "PWM requires the native idf::esp_driver_ledc")

    def test_esp32_pwm_provider_follows_the_feature_gate(self):
        provider = "idf::esp_driver_ledc"
        for enabled in (False, True):
            with self.subTest(provider=provider, enabled=enabled), \
                    tempfile.TemporaryDirectory(prefix="esy gated PWM provider ") as temporary:
                directory = Path(temporary)
                features = (f"set(ENABLE_ESY_PWM {'ON' if enabled else 'OFF'})\n"
                            "set(ENABLE_ESY_DMA ON)\n")
                self.prepare_capability_consumer(directory, "ESP32", features,
                                                native_pwm=provider)
                self.configure(directory)
                dependencies = (directory / "out/esy_hal_backend-dependencies.txt").read_text()
                sources = (directory / "out/esy_hal_backend-sources.txt").read_text()
                if enabled:
                    self.assertIn(provider, dependencies)
                    self.assertIn("esy_hb_pwm.c", sources)
                    self.assertIn("idf::esp_driver_gpio", dependencies)
                else:
                    self.assertNotIn(provider, dependencies)
                    self.assertNotIn("idf::esp_driver_gpio", dependencies)
                    self.assertNotIn("esy_hb_pwm.c", sources)

    def test_esp32_pwm_modular_provider_requires_gpio_headers(self):
        with tempfile.TemporaryDirectory(prefix="esy LEDC GPIO headers ") as temporary:
            directory = Path(temporary)
            self.prepare_capability_consumer(directory, "ESP32",
                "set(ENABLE_ESY_PWM ON)\nset(ENABLE_ESY_DMA ON)\n",
                native_pwm="idf::esp_driver_ledc")
            source = directory / "CMakeLists.txt"
            source.write_text(source.read_text().replace(
                "add_library(idf::esp_driver_gpio ALIAS idf_gpio_provider)", ""))
            self.configure(directory, "LEDC headers require idf::esp_driver_gpio")

    def test_esp32_i2c_provider_requires_gpio_headers(self):
        with tempfile.TemporaryDirectory(prefix="esy I2C GPIO headers ") as temporary:
            directory = Path(temporary)
            self.prepare_capability_consumer(directory, "ESP32",
                "set(ENABLE_ESY_I2C ON)\nset(ENABLE_ESY_DMA ON)\n", native_i2c=True)
            source = directory / "CMakeLists.txt"
            source.write_text(source.read_text().replace(
                "add_library(idf::esp_driver_gpio ALIAS idf_gpio_provider)", ""))
            self.configure(directory, "I2C pin validation requires idf::esp_driver_gpio")

    def test_esp32_managed_boot_requires_native_rollback(self):
        for backend_only in (False, True):
            with self.subTest(backend_only=backend_only), \
                    tempfile.TemporaryDirectory(prefix="esy native rollback contract ") as tmp:
                directory = Path(tmp)
                self.prepare_capability_consumer(directory, "ESP32",
                    "set(ENABLE_ESY_SYSTEM ON)\nset(ENABLE_ESY_FLASH ON)\n"
                    "set(ENABLE_ESY_BOOTLOADER ON)\n", backend_only,
                    native_app_update=True, native_os_context=True)
                self.configure(directory, "requires CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y")

    def test_boot_backend_and_handoff_obey_feature_gate_on_every_port(self):
        for platform in ("PICO", "PICO_W", "PICO2", "PICO2_W", "STM32F4", "ESP32"):
            for enabled in (False, True):
                with self.subTest(platform=platform, enabled=enabled), \
                        tempfile.TemporaryDirectory(prefix="esy native boot assembly ") as tmp:
                    directory = Path(tmp)
                    features = ("set(ENABLE_ESY_SYSTEM ON)\nset(ENABLE_ESY_FLASH ON)\n"
                                "set(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE ON)\n"
                                f"set(ENABLE_ESY_BOOTLOADER {'ON' if enabled else 'OFF'})\n")
                    self.prepare_capability_consumer(directory, platform, features,
                        native_app_update=platform == "ESP32" and enabled,
                        native_os_context=platform == "ESP32")
                    self.configure(directory)
                    backend = (directory / "out/esy_hal_backend-sources.txt").read_text()
                    hal = (directory / "out/esy_hal-sources.txt").read_text()
                    for name, sources in (("esy_hb_boot.c", backend), ("esy_hal_boot.c", hal)):
                        if enabled:
                            self.assertIn(name, sources)
                        else:
                            self.assertNotIn(name, sources)
                    if platform != "ESP32":
                        self.assertEqual(enabled, "esy_hb_boot_direct.c" in backend)
                    dependencies = (directory / "out/esy_hal_backend-dependencies.txt").read_text()
                    self.assertEqual(platform == "ESP32" and enabled,
                                     "idf::app_update" in dependencies)

    def test_pico_only_capabilities_reject_other_platforms_during_configuration(self):
        for platform in ("STM32F4", "ESP32"):
            for feature in ("ENABLE_ESY_MULTICORE", "ENABLE_ESY_USB_DEV", "ENABLE_ESY_XIO"):
                for backend_only in (False, True):
                    with self.subTest(platform=platform, feature=feature, backend_only=backend_only), \
                            tempfile.TemporaryDirectory(prefix="esy unsupported capability ") as tmp:
                        directory = Path(tmp)
                        self.prepare_capability_consumer(directory, platform,
                                                        f"set({feature} ON)\n", backend_only)
                        result = self.configure(directory, f"{feature} has no backend")
                        self.assertIn(f"ESY_TARGET_PLATFORM={platform}",
                                      result.stdout + result.stderr)

    def test_pico_only_capabilities_keep_all_pico_backends_and_hal_sources(self):
        features = "".join(f"set({feature} ON)\n" for feature in (
            "ENABLE_ESY_MULTICORE", "ENABLE_ESY_USB_DEV", "ENABLE_ESY_XIO", "ENABLE_ESY_DMA",
            "ENABLE_ESY_XIO_SPI", "ENABLE_ESY_XIO_UART_RX", "ENABLE_ESY_XIO_UART_TX"))
        for platform in ("PICO", "PICO_W", "PICO2", "PICO2_W"):
            with self.subTest(platform=platform), \
                    tempfile.TemporaryDirectory(prefix="esy supported capability ") as tmp:
                directory = Path(tmp)
                self.prepare_capability_consumer(directory, platform, features)
                self.configure(directory)
                self.assertIn('pico_multicore',
                              (directory / 'out/esy_hal_backend-dependencies.txt').read_text())
                for target, prefix in (("esy_hal_backend", "esy_hb"), ("esy_hal", "esy_hal")):
                    sources = (directory / f"out/{target}-sources.txt").read_text()
                    for capability in ("multicore", "usb_dev", "xio_spi", "xio_uart_rx", "xio_uart_tx"):
                        self.assertIn(f"{prefix}_{capability}.c", sources)

    def test_adc_platform_constraint_applies_to_full_and_direct_backend_consumers(self):
        for platform in ("PICO", "PICO_W", "PICO2", "PICO2_W", "STM32F4", "ESP32"):
            for backend_only in (False, True):
                with self.subTest(platform=platform, backend_only=backend_only), \
                        tempfile.TemporaryDirectory(prefix="esy adc capability ") as tmp:
                    directory = Path(tmp)
                    self.prepare_capability_consumer(directory, platform,
                                                    "set(ENABLE_ESY_ADC ON)\n"
                                                    "set(ENABLE_ESY_DMA ON)\n", backend_only)
                    if platform != "STM32F4":
                        result = self.configure(directory, "ENABLE_ESY_ADC has no backend")
                        self.assertIn(f"ESY_TARGET_PLATFORM={platform}",
                                      result.stdout + result.stderr)
                    else:
                        self.configure(directory)
                        sources = (directory / "out/esy_hal_backend-sources.txt").read_text()
                        self.assertIn("esy_hb_adc.c", sources)
                        if not backend_only:
                            sources = (directory / "out/esy_hal-sources.txt").read_text()
                            self.assertIn("esy_hal_adc.c", sources)

    def test_stm32_flash_supports_verified_single_bank_device_families(self):
        for device in ("STM32F405RG", "STM32F407ZG", "STM32F415RG", "STM32F417ZG"):
            for backend_only in (False, True):
                with self.subTest(device=device, backend_only=backend_only), \
                        tempfile.TemporaryDirectory(prefix="esy supported flash geometry ") as tmp:
                    directory = Path(tmp)
                    self.prepare_capability_consumer(directory, "STM32F4",
                                                    "set(ENABLE_ESY_FLASH ON)\n", backend_only,
                                                    device=device)
                    self.configure(directory)
                    sources = (directory / "out/esy_hal_backend-sources.txt").read_text()
                    self.assertIn("esy_hb_flash.c", sources)
                    if not backend_only:
                        sources = (directory / "out/esy_hal-sources.txt").read_text()
                        self.assertIn("esy_hal_flash.c", sources)

    def test_stm32_flash_rejects_unverified_device_geometry(self):
        for device in ("STM32F401RE", "STM32F411CE", "STM32F429ZI",
                       "STM32F446RE", "STM32F469NI"):
            for backend_only in (False, True):
                with self.subTest(device=device, backend_only=backend_only), \
                        tempfile.TemporaryDirectory(prefix="esy unsupported flash geometry ") as tmp:
                    directory = Path(tmp)
                    self.prepare_capability_consumer(directory, "STM32F4",
                                                    "set(ENABLE_ESY_FLASH ON)\n", backend_only,
                                                    device=device)
                    result = self.configure(directory, "has no verified Flash geometry")
                    self.assertIn(f"ESY_TARGET_DEVICE={device or ''}",
                                  result.stdout + result.stderr)

    def test_disabled_stm32_flash_does_not_restrict_other_devices(self):
        for device in (None, "STM32F401RE", "STM32F429ZI", "STM32F446RE", "STM32F469NI"):
            for features in ("", "set(ENABLE_ESY_FLASH OFF)\n"):
                for backend_only in (False, True):
                    with self.subTest(device=device, features=features, backend_only=backend_only), \
                            tempfile.TemporaryDirectory(prefix="esy disabled flash geometry ") as tmp:
                        directory = Path(tmp)
                        self.prepare_capability_consumer(directory, "STM32F4", features,
                                                        backend_only, device=device)
                        self.configure(directory)
                        sources = (directory / "out/esy_hal_backend-sources.txt").read_text()
                        self.assertNotIn("esy_hb_flash.c", sources)

    def test_disabled_platform_specific_capabilities_add_no_sources_on_any_platform(self):
        for platform in ("PICO", "PICO_W", "PICO2", "PICO2_W", "STM32F4", "ESP32"):
            for state in ("undefined", "OFF"):
                with self.subTest(platform=platform, state=state), \
                        tempfile.TemporaryDirectory(prefix="esy disabled capability ") as tmp:
                    directory = Path(tmp)
                    features = ("" if state == "undefined" else "".join(
                        f"set({feature} OFF)\n" for feature in
                        ("ENABLE_ESY_MULTICORE", "ENABLE_ESY_USB_DEV", "ENABLE_ESY_XIO",
                         "ENABLE_ESY_ADC")))
                    # Child gates remain inactive without their XIO parent.
                    features += "".join(f"set({feature} ON)\n" for feature in
                                        ("ENABLE_ESY_XIO_SPI", "ENABLE_ESY_XIO_UART_RX",
                                         "ENABLE_ESY_XIO_UART_TX"))
                    self.prepare_capability_consumer(directory, platform, features)
                    self.configure(directory)
                    for target in ("esy_hal_backend", "esy_hal"):
                        sources = (directory / f"out/{target}-sources.txt").read_text()
                        self.assertIn("empty.c", sources)
                        for capability in ("multicore", "usb_dev", "xio", "adc"):
                            self.assertNotIn(capability, sources)

    def run_early(self, directory, inputs, error=None, extra_args=(), after_early=""):
        source = directory / "selection.cmake"
        output = directory / "selection.txt"
        source.write_text(f'set(ESY_PROJECT_ROOT "{cmake_path(directory)}")\n' + inputs + f'''
include("{cmake_path(EARLY)}")
{after_early}
file(WRITE "{cmake_path(output)}"
  "ESY_TARGET_PLATFORM=${{ESY_TARGET_PLATFORM}}\nESY_TARGET_DEVICE=${{ESY_TARGET_DEVICE}}\n"
  "BOARD=${{ESY_BOARD_PLATFORM_PATH}}\nSPECS=${{ESY_SPECS_PLATFORM_PATH}}\n"
  "DEFINE=${{ESY_STM32F4_DEVICE_DEFINE}}\n"
  "STARTUP=${{ESY_STM32F4_STARTUP_SOURCE}}\nSDK=${{ESY_PLATFORM_SDK_ROOT}}\n")
''')
        self.run_command(["cmake", *extra_args, "-P", str(source)], directory, error)
        if error is not None:
            return {}
        return dict(line.split("=", 1) for line in output.read_text().splitlines())

    def copy_device_catalog(self, directory):
        catalog = directory / "catalog/esy_target_device.cmake"
        catalog.parent.mkdir(parents=True)
        shutil.copyfile(TARGET_RESOLVER, catalog)
        for descriptor in (ROOT / "cmake/ports").glob("*/device_map.cmake"):
            destination = catalog.parent / "ports" / descriptor.parent.name / "device_map.cmake"
            destination.parent.mkdir(parents=True)
            shutil.copyfile(descriptor, destination)
        return catalog

    def add_device_descriptor(self, catalog, name, *, device="TESTCHIP42", fields=None,
                              extra_fields=()):
        defaults = {
            "ESY_TARGET_DEVICE": "${device}", "ESY_TARGET_PLATFORM": "TESTCHIP",
            "ESY_BUILD_PORT": name, "ESY_BOARD_PLATFORM_PATH": name,
            "ESY_SPECS_PLATFORM_PATH": name,
        }
        defaults.update(fields or {})
        directory = catalog.parent / "ports" / name
        directory.mkdir(parents=True)
        result_body = "".join(
            f'  set(${{result_prefix}}_{key} "{value}" PARENT_SCOPE)\n'
            for key, value in defaults.items() if value is not None)
        (directory / "device_map.cmake").write_text(f'''
set(ESY_PORT_EXPORT_FIELDS "{';'.join(extra_fields)}")
set(ESY_DESCRIPTOR_PRIVATE "must stay inside descriptor loading")
function(esy_port_{name}_match device output)
  if(device STREQUAL "{device}")
    set(${{output}} TRUE PARENT_SCOPE)
  else()
    set(${{output}} FALSE PARENT_SCOPE)
  endif()
endfunction()
function(esy_port_{name}_resolve device result_prefix)
{result_body}endfunction()
''')
        (directory / "pre_project.cmake").write_text(
            'message(FATAL_ERROR "SDK import must not run during device resolution")\n')

    def resolve_catalog(self, catalog, device, error=None):
        output = catalog.parent / "mapping.txt"
        self.run_command(["cmake", f"-DESY_TARGET_DEVICE={device}",
                          f"-DESY_TARGET_MAP_FILE={output}", "-P", str(catalog)],
                         catalog.parent, error)
        return {} if error else dict(line.split("=", 1) for line in output.read_text().splitlines())

    def test_device_catalog_discovers_a_new_port_without_changing_the_central_resolver(self):
        with tempfile.TemporaryDirectory(prefix="esy new device descriptor ") as tmp:
            catalog = self.copy_device_catalog(Path(tmp))
            original = catalog.read_bytes()
            self.add_device_descriptor(catalog, "testchip", extra_fields=("ESY_TESTCHIP_DEFINE",),
                                       fields={"ESY_TESTCHIP_DEFINE": "TESTCHIP42_NATIVE"})
            fields = self.resolve_catalog(catalog, "testchip42")
            self.assertEqual("TESTCHIP42", fields["ESY_TARGET_DEVICE"])
            self.assertEqual("TESTCHIP", fields["ESY_TARGET_PLATFORM"])
            for field in ("ESY_BUILD_PORT", "ESY_BOARD_PLATFORM_PATH", "ESY_SPECS_PLATFORM_PATH"):
                self.assertEqual("testchip", fields[field])
            self.assertEqual("TESTCHIP42_NATIVE", fields["ESY_TESTCHIP_DEFINE"])
            self.assertEqual("", fields["ESY_STM32F4_DEVICE_DEFINE"])
            self.assertEqual("", fields["ESY_STM32F4_STARTUP_SOURCE"])
            self.assertEqual(original, catalog.read_bytes())

    def test_device_catalog_rejects_overlapping_port_matches_before_parsing(self):
        with tempfile.TemporaryDirectory(prefix="esy overlapping device descriptors ") as tmp:
            catalog = self.copy_device_catalog(Path(tmp))
            self.add_device_descriptor(catalog, "overlap", device="PICO2_W", fields={
                "ESY_TARGET_PLATFORM": None})
            self.resolve_catalog(catalog, "PICO2_W", "Ambiguous ESY_TARGET_DEVICE=PICO2_W")

    def test_device_catalog_exports_in_sorted_descriptor_order(self):
        with tempfile.TemporaryDirectory(prefix="esy sorted device descriptors ") as tmp:
            catalog = self.copy_device_catalog(Path(tmp))
            for name in ("zeta", "alpha"):
                field = f"ESY_{name.upper()}_REVISION"
                self.add_device_descriptor(catalog, name, device=f"{name.upper()}42",
                                           fields={field: "one"}, extra_fields=(field,))
            fields = self.resolve_catalog(catalog, "PICO2_W")
            names = list(fields)
            self.assertLess(names.index("ESY_ALPHA_REVISION"), names.index("ESY_ZETA_REVISION"))
            self.assertEqual("", fields["ESY_ALPHA_REVISION"])
            self.assertEqual("", fields["ESY_ZETA_REVISION"])

    def test_device_catalog_rejects_missing_invalid_and_inherited_outputs(self):
        invalid = (
            ({"ESY_TARGET_PLATFORM": None}, (), "omitted required output ESY_TARGET_PLATFORM"),
            ({"ESY_TARGET_PLATFORM": ""}, (), "invalid ESY_TARGET_PLATFORM"),
            ({"ESY_TARGET_PLATFORM": "testchip"}, (), "invalid ESY_TARGET_PLATFORM"),
            ({"ESY_TARGET_DEVICE": "PICO"}, (), "changed normalized ESY_TARGET_DEVICE"),
            ({"ESY_BUILD_PORT": "pico"}, (), "must return its own ESY_BUILD_PORT"),
            ({"ESY_BOARD_PLATFORM_PATH": "../board"}, (), "invalid ESY_BOARD_PLATFORM_PATH"),
            ({"ESY_SPECS_PLATFORM_PATH": ""}, (), "invalid ESY_SPECS_PLATFORM_PATH"),
            ({}, ("ESY_TESTCHIP_DEFINE",), "omitted required output ESY_TESTCHIP_DEFINE"),
            ({}, ("CMAKE_TOOLCHAIN_FILE",), "invalid extra output"),
            ({}, ("ESY_RTOS",), "invalid extra output"),
            ({"ESY_TESTCHIP_DEFINE": "one;two"}, ("ESY_TESTCHIP_DEFINE",), "must be a scalar"),
        )
        for fields, extra_fields, diagnostic in invalid:
            with self.subTest(fields=fields), tempfile.TemporaryDirectory(
                    prefix="esy invalid device descriptor ") as tmp:
                catalog = self.copy_device_catalog(Path(tmp))
                self.add_device_descriptor(catalog, "testchip", fields=fields,
                                           extra_fields=extra_fields)
                script = catalog.parent / "invalid.cmake"
                script.write_text(f'''
include("{cmake_path(catalog)}")
set(esy_result_ESY_TARGET_PLATFORM "INHERITED_STALE_VALUE")
esy_resolve_target_device(TESTCHIP42)
''')
                self.run_command(["cmake", "-P", str(script)], catalog.parent, diagnostic)

    def test_device_catalog_repeated_resolution_clears_other_ports_outputs_and_derived_cache(self):
        with tempfile.TemporaryDirectory(prefix="esy repeated device resolution ") as tmp:
            catalog = self.copy_device_catalog(Path(tmp))
            self.add_device_descriptor(catalog, "testchip", extra_fields=("ESY_TESTCHIP_DEFINE",),
                                       fields={"ESY_TESTCHIP_DEFINE": "TESTCHIP42_NATIVE"})
            script = catalog.parent / "repeated.cmake"
            script.write_text(f'''
include("{cmake_path(catalog)}")
foreach(field ESY_TARGET_PLATFORM ESY_BUILD_PORT ESY_BOARD_PLATFORM_PATH
              ESY_SPECS_PLATFORM_PATH ESY_STM32F4_DEVICE_DEFINE
              ESY_STM32F4_STARTUP_SOURCE ESY_TESTCHIP_DEFINE)
  set(${{field}} "stale derived input" CACHE STRING "Stale fixture")
endforeach()
set(ESY_TARGET_DEVICE STM32F401RET6)
esy_configure_target_device()
if(NOT ESY_STM32F4_DEVICE_DEFINE STREQUAL "STM32F401xE")
  message(FATAL_ERROR "STM32 alias mapping changed")
endif()
set(ESY_TARGET_DEVICE testchip42)
esy_configure_target_device()
if(NOT ESY_STM32F4_DEVICE_DEFINE STREQUAL "" OR
   NOT ESY_STM32F4_STARTUP_SOURCE STREQUAL "" OR
   NOT ESY_TESTCHIP_DEFINE STREQUAL "TESTCHIP42_NATIVE")
  message(FATAL_ERROR "A previous port output survived device selection")
endif()
set(ESY_TARGET_PLATFORM PICO_W CACHE STRING "Stale matching platform" FORCE)
set(ESY_TARGET_DEVICE pico_w)
esy_configure_target_device()
if(NOT ESY_TESTCHIP_DEFINE STREQUAL "" OR
   NOT ESY_STM32F4_DEVICE_DEFINE STREQUAL "")
  message(FATAL_ERROR "An inactive port output was not cleared")
endif()
get_property(selected_device CACHE ESY_TARGET_DEVICE PROPERTY VALUE)
get_property(selected_type CACHE ESY_TARGET_DEVICE PROPERTY TYPE)
if(NOT ESY_TARGET_DEVICE STREQUAL "PICO_W" OR
   NOT selected_device STREQUAL "PICO_W" OR
   NOT selected_type STREQUAL "STRING")
  message(FATAL_ERROR "Normalized target device or its cache entry was lost")
endif()
foreach(field IN LISTS ESY_TARGET_EXPORT_FIELDS)
  if(field MATCHES "^ESY_TARGET_DEVICE$")
    continue()
  endif()
  get_property(cached CACHE ${{field}} PROPERTY TYPE SET)
  if(cached)
    message(FATAL_ERROR "Derived field remained cached: ${{field}}")
  endif()
endforeach()
if(DEFINED ESY_DESCRIPTOR_PRIVATE OR DEFINED ESY_PORT_EXPORT_FIELDS)
  message(FATAL_ERROR "Descriptor loading leaked its private scope")
endif()
''')
            self.run_command(["cmake", "-P", str(script)], catalog.parent)

    def test_script_mode_derives_all_target_fields_from_device_without_sdk(self):
        cases = (
            ("pico", "PICO", "pico", "pico"),
            ("PICO_W", "PICO_W", "pico", "pico"),
            ("pico2", "PICO2", "pico2", "pico"),
            ("pico2_w", "PICO2_W", "pico2", "pico"),
            ("stm32f405rg", "STM32F4", "stm32f4", "stm32f4"),
            ("STM32F407ZG", "STM32F4", "stm32f4", "stm32f4"),
            ("esp32", "ESP32", "esp32", "esp32"),
        )
        with tempfile.TemporaryDirectory(prefix="esy selection ") as tmp:
            directory = Path(tmp)
            for raw, platform, board, specs in cases:
                with self.subTest(device=raw):
                    actual = self.run_early(directory, f'set(ESY_TARGET_DEVICE "{raw}")\n')
                    self.assertEqual(platform, actual["ESY_TARGET_PLATFORM"])
                    self.assertEqual(raw.upper(), actual["ESY_TARGET_DEVICE"])
                    self.assertEqual(board, actual["BOARD"])
                    self.assertEqual(specs, actual["SPECS"])

    def test_machine_readable_resolver_matches_early_import_without_sdk(self):
        with tempfile.TemporaryDirectory(prefix="esy shared device resolver ") as tmp:
            directory = Path(tmp)
            output = directory / "target mapping.txt"
            cases = ["pico", "PICO_W", "Pico2", "pico2_w", "esp32"]
            cases.extend(model for model, _ in STM32_DEVICE_CASES)
            for device in cases:
                with self.subTest(device=device):
                    early = self.run_early(directory, f'set(ESY_TARGET_DEVICE "{device}")\n')
                    self.run_command(["cmake", f"-DESY_TARGET_DEVICE={device}",
                                      f"-DESY_TARGET_MAP_FILE={output}",
                                      "-P", str(TARGET_RESOLVER)], directory)
                    fields = dict(line.split("=", 1) for line in output.read_text().splitlines())
                    self.assertEqual(early["ESY_TARGET_DEVICE"], fields["ESY_TARGET_DEVICE"])
                    self.assertEqual(early["ESY_TARGET_PLATFORM"], fields["ESY_TARGET_PLATFORM"])
                    self.assertEqual(early["BOARD"], fields["ESY_BOARD_PLATFORM_PATH"])
                    self.assertEqual(early["SPECS"], fields["ESY_SPECS_PLATFORM_PATH"])
                    self.assertEqual(early["DEFINE"], fields["ESY_STM32F4_DEVICE_DEFINE"])
                    self.assertEqual(early["STARTUP"], fields["ESY_STM32F4_STARTUP_SOURCE"])

    def test_machine_readable_resolver_rejects_missing_unknown_and_ambiguous_devices(self):
        with tempfile.TemporaryDirectory(prefix="esy rejected device resolver ") as tmp:
            directory = Path(tmp)
            for device in ("", "RP2350", "PICO3", "ESP32_S3", "STM32F4", "STM32F407XX"):
                with self.subTest(device=device):
                    self.run_command(["cmake", f"-DESY_TARGET_DEVICE={device}",
                                      f"-DESY_TARGET_MAP_FILE={directory / 'mapping.txt'}",
                                      "-P", str(TARGET_RESOLVER)], directory, "ESY_TARGET_DEVICE")

    def test_direct_library_requires_device_instead_of_derived_platform(self):
        with tempfile.TemporaryDirectory(prefix="esy direct device contract ") as tmp:
            directory = Path(tmp)
            (directory / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(direct_selection C)
set(ESY_TARGET_PLATFORM PICO2_W)
add_subdirectory("{cmake_path(ROOT)}" platform)
''')
            self.configure(directory, "ESY_TARGET_DEVICE must be explicitly provided")

    def test_project_metadata_does_not_select_a_platform(self):
        with tempfile.TemporaryDirectory(prefix="esy ignored selection files ") as tmp:
            directory = Path(tmp)
            config = directory / ".project.conf"
            config.write_text("ESY_TARGET_PLATFORM=PICO2_W\nESY_TARGET_DEVICE=PICO2_W\n")
            self.run_early(directory, "", "ESY_TARGET_DEVICE must be explicitly provided")

    def test_stm32_device_selection_uses_explicit_cmsis_groups(self):
        with tempfile.TemporaryDirectory(prefix="esy stm32 device groups ") as tmp:
            directory = Path(tmp)
            for model, expected_define in STM32_DEVICE_CASES:
                with self.subTest(model=model):
                    actual = self.run_early(directory,
                        f"set(ESY_TARGET_DEVICE {model})\n")
                    self.assertEqual(model.upper(), actual["ESY_TARGET_DEVICE"])
                    self.assertEqual(expected_define, actual["DEFINE"])
                    self.assertEqual(f"startup_{expected_define.lower()}.s", actual["STARTUP"])

    def test_stm32_device_selection_rejects_unknown_and_ambiguous_parts(self):
        with tempfile.TemporaryDirectory(prefix="esy unknown stm32 devices ") as tmp:
            directory = Path(tmp)
            for model in ("STM32F401", "STM32F401R", "STM32F401ZE", "STM32F401RF",
                          "STM32F407", "STM32F407XX", "STM32F407RG", "STM32F407ZGBAD",
                          "STM32F410", "STM32F410T", "STM32F410T9", "STM32F410ZB",
                          "STM32F411RB", "STM32F412", "STM32F412KG", "STM32F413RE",
                          "STM32F446CE",
                          "STM32F499ZG"):
                with self.subTest(model=model):
                    self.run_early(directory,
                        f"set(ESY_TARGET_DEVICE {model})\n",
                        "Unsupported or ambiguous ESY_TARGET_DEVICE")

    @unittest.skipUnless(os.environ.get("ESY_TEST_STM32_SUPPORT_ROOT"),
                         "Set ESY_TEST_STM32_SUPPORT_ROOT to verify native CMSIS/startup files")
    def test_stm32_device_groups_exist_in_the_selected_vendor_checkout(self):
        support = Path(os.environ["ESY_TEST_STM32_SUPPORT_ROOT"]).resolve()
        cmsis = support / "CMSIS/Device/ST/STM32F4xx"
        umbrella = (cmsis / "Include/stm32f4xx.h").read_text()
        for model, expected_define in STM32_DEVICE_CASES:
            with self.subTest(model=model, cmsis=expected_define):
                self.assertIn(f"defined({expected_define})", umbrella.replace("defined (", "defined("))
                self.assertTrue((cmsis / f"Include/{expected_define.lower()}.h").is_file())
                self.assertTrue((cmsis / f"Source/Templates/gcc/startup_{expected_define.lower()}.s").is_file())

    def test_explicit_selection_ignores_missing_invalid_conflicting_and_changed_config_files(self):
        contents = (None, "ESY_TARGET_PLATFORM=STM32F4\nESY_TARGET_DEVICE=STM32F407ZG\n",
                    "ESY_TARGET_PLATFORM=PICO\nESY_TARGET_PLATFORM=PICO2\n",
                    "not valid configuration\n")
        with tempfile.TemporaryDirectory(prefix="esy ignored config contents ") as tmp:
            directory = Path(tmp)
            config = directory / ".project.conf"
            for content in contents:
                with self.subTest(content=content):
                    if content is None:
                        config.unlink(missing_ok=True)
                    else:
                        config.write_text(content)
                    actual = self.run_early(directory,
                        'set(ESY_TARGET_DEVICE pico2_w)\n')
                    self.assertEqual("PICO2_W", actual["ESY_TARGET_PLATFORM"])
                    self.assertEqual("PICO2_W", actual["ESY_TARGET_DEVICE"])

    def test_cli_selection_is_normalized_without_reading_or_tracking_configuration_files(self):
        with tempfile.TemporaryDirectory(prefix="esy explicit cli selection ") as tmp:
            directory = Path(tmp)
            source = self.prepare_pico_consumer(directory, f'''
include("{cmake_path(PROJECT_IMPORT)}")
if(NOT ESY_TARGET_PLATFORM STREQUAL "PICO_W" OR NOT ESY_TARGET_DEVICE STREQUAL "PICO_W")
  message(FATAL_ERROR "The explicit CLI platform/device was not normalized")
endif()
get_property(configuration_inputs DIRECTORY PROPERTY CMAKE_CONFIGURE_DEPENDS)
if("${{CMAKE_CURRENT_SOURCE_DIR}}/.project.conf" IN_LIST configuration_inputs)
  message(FATAL_ERROR "The library registered a template configuration dependency")
endif()
''', selection_inputs="")
            config = source / ".project.conf"
            config.write_text("ESY_TARGET_PLATFORM=STM32F4\nESY_TARGET_DEVICE=STM32F407ZG\n")
            arguments = ("-DESY_TARGET_DEVICE=pico_w",)
            self.configure(source, extra_args=arguments)
            config.write_text("ESY_TARGET_PLATFORM=ESP32\nESY_TARGET_PLATFORM=unknown\n")
            self.configure(source, extra_args=arguments)
            config.unlink()
            self.configure(source, extra_args=arguments)
            cache = (source / "out/CMakeCache.txt").read_text()
            self.assertNotIn("ESY_TARGET_PLATFORM:", cache)
            self.assertIn("ESY_TARGET_DEVICE:STRING=PICO_W", cache)

    def test_device_reconfiguration_clears_derived_platform_cache(self):
        with tempfile.TemporaryDirectory(prefix="esy derived platform reconfiguration ") as tmp:
            directory = Path(tmp)
            source = self.prepare_pico_consumer(directory, f'''
include("{cmake_path(PROJECT_IMPORT)}")
if(DEFINED CACHE{{ESY_TARGET_PLATFORM}})
  message(FATAL_ERROR "A derived platform was persisted in the build cache")
endif()
file(WRITE "${{CMAKE_BINARY_DIR}}/selection.txt" "${{ESY_TARGET_PLATFORM}}|${{ESY_TARGET_DEVICE}}")
''', selection_inputs="")
            for raw, canonical in (("pico", "PICO"), ("pico2", "PICO2"),
                                   ("pico2_w", "PICO2_W"), ("pico_w", "PICO_W")):
                with self.subTest(device=raw):
                    self.configure(source, extra_args=(f"-DESY_TARGET_DEVICE={raw}",
                                                       "-DESY_TARGET_PLATFORM=ESP32"))
                    self.assertEqual(f"{canonical}|{canonical}",
                                     (source / "out/selection.txt").read_text())
                    cache = (source / "out/CMakeCache.txt").read_text()
                    self.assertNotIn("ESY_TARGET_PLATFORM:", cache)
                    self.assertIn(f"ESY_TARGET_DEVICE:STRING={canonical}", cache)

    def test_cached_device_remains_the_single_input_on_reconfigure(self):
        with tempfile.TemporaryDirectory(prefix="esy explicit device reconfiguration ") as tmp:
            directory = Path(tmp)
            source = self.prepare_pico_consumer(directory, f'''
include("{cmake_path(PROJECT_IMPORT)}")
file(WRITE "${{CMAKE_BINARY_DIR}}/selection.txt" "${{ESY_TARGET_PLATFORM}}|${{ESY_TARGET_DEVICE}}")
''', selection_inputs="")
            self.configure(source, extra_args=("-DESY_TARGET_DEVICE=pico",))
            self.configure(source)
            self.assertEqual("PICO|PICO", (source / "out/selection.txt").read_text())
            self.configure(source, extra_args=("-DESY_TARGET_DEVICE=pico2",))
            self.assertEqual("PICO2|PICO2", (source / "out/selection.txt").read_text())

    def test_private_project_environment_path_is_completely_ignored(self):
        with tempfile.TemporaryDirectory(prefix="esy ignored private settings ") as tmp:
            directory = Path(tmp)
            (directory / ".local").mkdir()
            private_file = directory / ".local/project_env.cmake"
            for content in IGNORED_PRIVATE_CONFIGURATIONS:
                with self.subTest(content=content):
                    private_file.write_text(content)
                    actual = self.run_early(directory,
                        'set(ESY_TARGET_DEVICE PICO)\n'
                        'set(ESY_PLATFORM_SDK_ROOT "explicit SDK")\n')
                    self.assertEqual("PICO", actual["ESY_TARGET_PLATFORM"])
                    self.assertEqual("PICO", actual["ESY_TARGET_DEVICE"])
                    self.assertEqual("explicit SDK", actual["SDK"])

    def test_consumer_can_explicitly_include_its_own_sdk_configuration_before_early_import(self):
        with tempfile.TemporaryDirectory(prefix="esy consumer-owned SDK settings ") as tmp:
            directory = Path(tmp)
            sdk = directory / "sdk provider outside consumer"
            configuration = directory / "locations chosen by the application.cmake"
            configuration.write_text(f'set(ESY_PLATFORM_SDK_ROOT "{cmake_path(sdk)}")\n')
            source = self.prepare_pico_consumer(directory, f'''
include("{cmake_path(PROJECT_IMPORT)}")
add_executable(configured_sdk_consumer main.c)
target_link_libraries(configured_sdk_consumer PRIVATE esy_project_options)
''', before_project=f'''
unset(PICO_SDK_PATH)
include("{cmake_path(configuration)}")
''')
            (source / ".local").mkdir()
            (source / ".local/project_env.cmake").write_text(IGNORED_PRIVATE_CONFIGURATIONS[0])
            with mock.patch.dict(os.environ, {"PICO_SDK_PATH": ""}):
                self.configure(source)
            self.run_command(["cmake", "--build", str(source / "out"), "--target",
                              "configured_sdk_consumer"], source)
            self.run_command([str(source / "out/configured_sdk_consumer")], source)
            self.assertEqual(["sdk-import", "project", "sdk-init"],
                             (directory / "events.txt").read_text().splitlines())

    def test_private_environment_cannot_supply_missing_explicit_selection(self):
        with tempfile.TemporaryDirectory(prefix="esy ignored private selection ") as tmp:
            directory = Path(tmp)
            (directory / ".local").mkdir()
            (directory / ".local/project_env.cmake").write_text(
                "set(ESY_TARGET_DEVICE STM32F407ZG)\n")
            self.run_early(directory, "", "ESY_TARGET_DEVICE must be explicitly provided")
            self.run_early(directory,
                "set(ESY_TARGET_PLATFORM STM32F4)\n", "ESY_TARGET_DEVICE must be explicitly provided")

    def test_sdk_paths_can_be_supplied_through_cli_or_supported_environment_variables(self):
        for mode in ("cli_path", "cli_alias", "environment_path", "environment_alias"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix="esy explicit SDK input ") as tmp:
                directory = Path(tmp)
                sdk = directory / "sdk provider outside consumer"
                source = self.prepare_pico_consumer(directory, f'''
include("{cmake_path(PROJECT_IMPORT)}")
if(NOT PICO_SDK_PATH STREQUAL "{cmake_path(sdk)}")
  message(FATAL_ERROR "The explicit SDK input was not used")
endif()
''', before_project="unset(PICO_SDK_PATH)")
                environment = os.environ.copy()
                for variable in ("PICO_SDK_PATH", "ESY_PLATFORM_SDK_ROOT"):
                    environment.pop(variable, None)
                variable = "PICO_SDK_PATH" if mode.endswith("_path") else "ESY_PLATFORM_SDK_ROOT"
                arguments = ()
                if mode.startswith("cli_"):
                    arguments = (f"-D{variable}={sdk}",)
                else:
                    environment[variable] = str(sdk)
                with mock.patch.dict(os.environ, environment, clear=True):
                    self.configure(source, extra_args=arguments)
                self.assertEqual(["sdk-import", "project", "sdk-init"],
                                 (directory / "events.txt").read_text().splitlines())

    def test_device_is_required_even_when_a_derived_platform_is_present(self):
        with tempfile.TemporaryDirectory(prefix="esy absent selection ") as tmp:
            directory = Path(tmp)
            for inputs in ("", 'set(ESY_TARGET_DEVICE "")\n',
                           "set(ESY_TARGET_PLATFORM PICO)\n",
                           "set(ESY_TARGET_PLATFORM STM32F4)\n"):
                with self.subTest(inputs=inputs):
                    self.run_early(directory, inputs, "ESY_TARGET_DEVICE must be explicitly provided")

    def test_unknown_device_selection_fails_before_sdk_import(self):
        with tempfile.TemporaryDirectory(prefix="esy invalid selection ") as tmp:
            directory = Path(tmp)
            for device in ("unknown", "RP2350", "ESP32_S3", "PICO3", "STM32F4"):
                with self.subTest(device=device):
                    self.run_early(directory, f"set(ESY_TARGET_DEVICE {device})\n", "ESY_TARGET_DEVICE")

    def test_helpers_reject_the_wrong_project_order(self):
        with tempfile.TemporaryDirectory(prefix="esy wrong order ") as tmp:
            directory = Path(tmp)
            self.run_early(directory,
                           f'include("{cmake_path(PROJECT_IMPORT)}")\n', "after project()")
            source = directory / "late early import"
            source.mkdir()
            (source / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(wrong_order C)
set(ESY_TARGET_DEVICE PICO)
include("{cmake_path(EARLY)}")
''')
            self.configure(source, "before project()")

    def test_library_assembly_initializes_once_and_exports_consumer_configuration(self):
        for explicit in ("none", "before", "after"):
            with self.subTest(explicit=explicit), tempfile.TemporaryDirectory(prefix="esy merged import ") as tmp:
                directory = Path(tmp)
                import_call = f'include("{cmake_path(PROJECT_IMPORT)}")\n'
                source = self.prepare_pico_consumer(directory,
                    (import_call if explicit == "before" else "") +
                    self.platform_assembly(directory) +
                    (import_call if explicit == "after" else "") + '''
if(NOT PROJECT_NAME STREQUAL "UnrelatedProjectName" OR NOT PROJECT_VERSION STREQUAL "7.29.3")
  message(FATAL_ERROR "Library assembly changed the consumer project identity")
endif()
if(NOT DEFINED ENABLE_ESY_TOUCH OR ENABLE_ESY_TOUCH OR
   NOT DEFINED ENABLE_ESY_WIFI OR ENABLE_ESY_WIFI OR
   NOT ESY_RTOS STREQUAL "None" OR NOT APP_HOST_COMM_TRANSPORT STREQUAL "UART" OR
   NOT APP_LOGGER_BACKEND STREQUAL "RTT" OR NOT APP_LOGGER_OUTPUTS STREQUAL "RTT" OR
   NOT DEFINED APP_ENABLE_STATUS_DISPLAY OR APP_ENABLE_STATUS_DISPLAY)
  message(FATAL_ERROR "Normal consumer configuration did not reach the caller")
endif()
if(DEFINED CACHE{ENABLE_ESY_TOUCH} OR DEFINED CACHE{ENABLE_ESY_WIFI} OR
   DEFINED CACHE{APP_HOST_COMM_TRANSPORT} OR DEFINED CACHE{APP_LOGGER_OUTPUTS} OR
   DEFINED CACHE{APP_ENABLE_STATUS_DISPLAY} OR DEFINED CACHE{APP_LOGGER_BACKEND})
  message(FATAL_ERROR "Library assembly cached the consumer's normal feature values")
endif()
add_subdirectory(sibling)
add_executable(merged_version version.c)
target_link_libraries(merged_version PRIVATE esy_project_options)
''', project_version="7.29.3")
                (source / "esy_feature_import.cmake").write_text('''
set(ESY_RTOS None)
set(ENABLE_ESY_TOUCH OFF)
set(ENABLE_ESY_WIFI OFF)
set(APP_HOST_COMM_TRANSPORT UART)
set(APP_LOGGER_OUTPUTS RTT)
set(APP_LOGGER_BACKEND RTT)
set(APP_ENABLE_STATUS_DISPLAY OFF)
file(APPEND "${ESY_PROJECT_ROOT}/feature-loads.txt" "loaded\\n")
''')
                sibling = source / "sibling"
                sibling.mkdir()
                (sibling / "CMakeLists.txt").write_text('''
if(NOT ESY_RTOS STREQUAL "None" OR NOT DEFINED ENABLE_ESY_TOUCH OR
   ENABLE_ESY_TOUCH OR NOT APP_HOST_COMM_TRANSPORT STREQUAL "UART" OR
   NOT APP_LOGGER_BACKEND STREQUAL "RTT" OR NOT APP_LOGGER_OUTPUTS STREQUAL "RTT" OR
   NOT DEFINED APP_ENABLE_STATUS_DISPLAY OR APP_ENABLE_STATUS_DISPLAY)
  message(FATAL_ERROR "Sibling modules lost resolved consumer configuration")
endif()
''')
                (source / "version.c").write_text(
                    '#include "version.h"\n'
                    '_Static_assert(APP_VERSION == 72903, "Expected the consumer version");\n'
                    'int main(void) { return APP_VERSION == 72903 ? 0 : 1; }\n')
                self.configure(source)
                self.run_command(["cmake", "--build", str(source / "out"), "--target", "merged_version"], source)
                self.run_command([str(source / "out/merged_version")], source)
                self.assertEqual("loaded\n", (source / "feature-loads.txt").read_text())
                self.assertEqual(["sdk-import", "project", "sdk-init"],
                                 (directory / "events.txt").read_text().splitlines())

    def test_conventional_consumer_builds_without_a_build_config(self):
        with tempfile.TemporaryDirectory(prefix="esy default build ") as tmp:
            directory = Path(tmp)
            source = self.prepare_pico_consumer(directory, f'''
include("{cmake_path(PROJECT_IMPORT)}")
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT CMAKE_C_STANDARD EQUAL 11 OR
   NOT CMAKE_CXX_STANDARD EQUAL 17 OR NOT CMAKE_C_STANDARD_REQUIRED OR
   NOT CMAKE_EXPORT_COMPILE_COMMANDS)
  message(FATAL_ERROR "Conventional language/build defaults were not supplied")
endif()
if(NOT ESY_BOARD_PATH STREQUAL "${{CMAKE_SOURCE_DIR}}/esy_board" OR
   NOT ESY_SPECS_DIR STREQUAL "${{CMAKE_SOURCE_DIR}}/specs/pico" OR
   NOT ESY_RTOS_CONFIG_DIR STREQUAL "${{ESY_SPECS_DIR}}/include/specs")
  message(FATAL_ERROR "Conventional board/specs paths were not supplied")
endif()
if(NOT ESY_VERSION_TEMPLATE STREQUAL "{cmake_path(VERSION_TEMPLATE)}" OR
   DEFINED ESY_FIRMWARE_COMPILE_DEFINITIONS OR DEFINED ESY_APP_SLOT OR
   DEFINED EXTRA_COMPONENT_DIRS OR DEFINED COMPONENTS)
  message(FATAL_ERROR "Missing library version template or unexpected provider/product policy")
endif()
if(NOT DEFINED ENABLE_ESY_TOUCH OR ENABLE_ESY_TOUCH)
  message(FATAL_ERROR "The consumer's conventional feature file was not read")
endif()
add_executable(default_consumer consumer.c consumer.cpp)
target_link_libraries(default_consumer PRIVATE esy_project_options)
''', bind_board=False)
            # This consumer supplies product files, but no CMake override file.
            (source / "esy_feature_import.cmake").write_text("set(ENABLE_ESY_TOUCH OFF)\nset(ESY_RTOS None)\n")
            specs = source / "specs/pico/include/specs"
            specs.mkdir(parents=True)
            (specs / "selection.h").write_text("#define CONVENTIONAL_SPEC_VALUE 31\n")
            (source / "consumer.c").write_text('''
#include "selection.h"
#include "version.h"
#if __STDC_VERSION__ < 201112L || !defined(ESY_PLATFORM_PICO2_W)
#error "Missing conventional C standard or platform definition"
#endif
_Static_assert(APP_VERSION_MAJOR == 1 && APP_VERSION_MINOR == 2 && APP_VERSION_PATCH == 3,
               "The consumer project owns the version components");
_Static_assert(APP_VERSION == 10203 && APP_VERSION * 2 == 20406,
               "The application version is usable in arithmetic expressions");
int main(void) { return CONVENTIONAL_SPEC_VALUE == 31 ? 0 : 1; }
''')
            (source / "consumer.cpp").write_text('''
#include "specs/selection.h"
#include "version.h"
static_assert(__cplusplus >= 201703L, "Expected the conventional C++17 standard");
static_assert(CONVENTIONAL_SPEC_VALUE == 31, "Missing specs include propagation");
static_assert(APP_VERSION == 10203 && APP_VERSION * 2 == 20406,
              "The generated application version must support C++ expressions");
''')
            self.assertFalse((source / "cmake/esy_build_config.cmake").exists())
            self.configure(source)
            self.run_command(["cmake", "--build", str(source / "out"), "--target",
                              "default_consumer"], source)
            self.run_command([str(source / "out/default_consumer")], source)
            self.assertFalse((source / "cmake/version.h.in").exists())
            self.assertTrue((source / "out/generated/version.h").is_file())
            self.assertTrue((source / "out/compile_commands.json").is_file())

    def test_cli_values_and_explicit_empty_defaults_are_preserved(self):
        with tempfile.TemporaryDirectory(prefix="esy explicit cli defaults ") as tmp:
            directory = Path(tmp)
            source = self.prepare_pico_consumer(directory, f'''
include("{cmake_path(PROJECT_IMPORT)}")
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release" OR NOT CMAKE_C_STANDARD EQUAL 99 OR
   NOT CMAKE_CXX_STANDARD EQUAL 20 OR CMAKE_C_EXTENSIONS OR
   CMAKE_C_STANDARD_REQUIRED OR CMAKE_EXPORT_COMPILE_COMMANDS)
  message(FATAL_ERROR "CLI language/build overrides were overwritten")
endif()
foreach(input ESY_FEATURE_IMPORT_FILE ESY_SPECS_DIR ESY_RTOS_CONFIG_DIR
              ESY_VERSION_TEMPLATE GENERATED_INCLUDE_DIR ESY_DEFAULT_COMPILE_OPTIONS
              ESY_SPEED_COMPILE_OPTIONS ESY_DEFAULT_LINK_OPTIONS)
  if(NOT DEFINED ${{input}} OR NOT "${{${{input}}}}" STREQUAL "")
    message(FATAL_ERROR "An explicit empty override was replaced: ${{input}}")
  endif()
endforeach()
if(NOT LV_BUILD_CONF_PATH STREQUAL "")
  message(FATAL_ERROR "A disabled specs path created a graphics config path")
endif()
''')
            (source / "esy_feature_import.cmake").write_text('message(FATAL_ERROR "disabled features")\n')
            (source / "cmake").mkdir()
            (source / "cmake/version.h.in").write_text("disabled version\n")
            arguments = ["-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_STANDARD=99",
                         "-DCMAKE_CXX_STANDARD=20", "-DCMAKE_C_EXTENSIONS=OFF",
                         "-DCMAKE_C_STANDARD_REQUIRED=OFF", "-DCMAKE_EXPORT_COMPILE_COMMANDS=OFF"]
            arguments += [f"-D{variable}=" for variable in (
                "ESY_FEATURE_IMPORT_FILE", "ESY_SPECS_DIR", "ESY_RTOS_CONFIG_DIR",
                "ESY_VERSION_TEMPLATE", "GENERATED_INCLUDE_DIR", "ESY_DEFAULT_COMPILE_OPTIONS",
                "ESY_SPEED_COMPILE_OPTIONS", "ESY_DEFAULT_LINK_OPTIONS")]
            self.configure(source, extra_args=arguments)
            self.assertFalse((source / "out/compile_commands.json").exists())

    def test_release_optimization_defaults_are_available_without_overrides(self):
        with tempfile.TemporaryDirectory(prefix="esy release profile ") as tmp:
            directory = Path(tmp)
            source = self.prepare_pico_consumer(directory, f'''
include("{cmake_path(PROJECT_IMPORT)}")
foreach(input ESY_DEFAULT_COMPILE_OPTIONS ESY_SPEED_COMPILE_OPTIONS)
  if(NOT "${{${{input}}}}" MATCHES "CONFIG:Release")
    message(FATAL_ERROR "Conventional configuration-specific optimization is missing")
  endif()
endforeach()
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  message(FATAL_ERROR "Explicit Release was replaced by Debug")
endif()
''')
            self.configure(source, extra_args=("-DCMAKE_BUILD_TYPE=Release",))

    def test_multi_configuration_generator_does_not_receive_a_build_type(self):
        with tempfile.TemporaryDirectory(prefix="esy multi configuration ") as tmp:
            directory = Path(tmp)
            source = self.prepare_pico_consumer(directory, f'''
include("{cmake_path(PROJECT_IMPORT)}")
if(CMAKE_BUILD_TYPE OR NOT CMAKE_CONFIGURATION_TYPES)
  message(FATAL_ERROR "Multi-configuration generation acquired a single build type")
endif()
''')
            self.run_command(["cmake", "-S", str(source), "-B", str(source / "out"),
                              "-G", "Ninja Multi-Config"], source)

    def test_override_file_runs_after_selection_and_before_sdk_import(self):
        with tempfile.TemporaryDirectory(prefix="esy early overrides ") as tmp:
            directory = Path(tmp)
            source = self.prepare_pico_consumer(directory, f'''
include("{cmake_path(PROJECT_IMPORT)}")
if(NOT ESY_DEFAULT_COMPILE_OPTIONS STREQUAL "-DCONSUMER_PROFILE=1" OR
   NOT ESY_SPECS_DIR STREQUAL "${{ESY_PROJECT_ROOT}}/alternate settings/pico")
  message(FATAL_ERROR "Consumer build overrides were replaced by defaults")
endif()
''')
            sdk = directory / "sdk provider outside consumer"
            sdk_import = sdk / "external/pico_sdk_import.cmake"
            sdk_import.write_text('''
if(NOT OVERRIDE_BEFORE_SDK OR NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  message(FATAL_ERROR "Consumer override ran after SDK import")
endif()
''' + PICO_SDK_FIXTURE)
            (source / "cmake").mkdir()
            (source / "cmake/esy_build_config.cmake").write_text('''
if(NOT ESY_TARGET_PLATFORM STREQUAL "PICO2_W" OR NOT ESY_SPECS_PLATFORM_PATH STREQUAL "pico")
  message(FATAL_ERROR "Consumer override ran before selection normalization")
endif()
set(OVERRIDE_BEFORE_SDK ON)
set(CMAKE_BUILD_TYPE Release CACHE STRING "Consumer profile")
set(ESY_SPECS_DIR "${ESY_PROJECT_ROOT}/alternate settings/${ESY_SPECS_PLATFORM_PATH}")
set(ESY_DEFAULT_COMPILE_OPTIONS -DCONSUMER_PROFILE=1)
''')
            self.configure(source)

    def test_nonstandard_project_root_drives_selection_paths_and_version(self):
        with tempfile.TemporaryDirectory(prefix="esy separate project root ") as tmp:
            directory = Path(tmp)
            consumer_root = directory / "product files outside CMake source"
            (consumer_root / "cmake").mkdir(parents=True)
            (consumer_root / "esy_feature_import.cmake").write_text("set(ENABLE_ESY_TOUCH OFF)\n")
            (consumer_root / "cmake/version.h.in").write_text(
                '#define CONVENTIONAL_VERSION "@PROJECT_VERSION@"\n')
            source = self.prepare_pico_consumer(directory, f'''
include("{cmake_path(PROJECT_IMPORT)}")
if(NOT ESY_TARGET_PLATFORM STREQUAL "PICO" OR
   NOT ESY_SPECS_DIR STREQUAL "{cmake_path(consumer_root)}/specs/pico" OR
   NOT ESY_FEATURE_IMPORT_FILE STREQUAL "{cmake_path(consumer_root)}/esy_feature_import.cmake")
  message(FATAL_ERROR "The explicit project root was ignored")
endif()
add_executable(version_consumer version.c)
target_link_libraries(version_consumer PRIVATE esy_project_options)
''', before_project=f'''
set(ESY_PROJECT_ROOT "{cmake_path(consumer_root)}")
set(ESY_VERSION_TEMPLATE "${{ESY_PROJECT_ROOT}}/cmake/version.h.in")
''',
                selection_inputs="set(ESY_TARGET_DEVICE PICO)")
            (source / "version.c").write_text('''
#include "version.h"
int main(void) { return CONVENTIONAL_VERSION[0] == '1' ? 0 : 1; }
''')
            self.configure(source)
            self.run_command(["cmake", "--build", str(source / "out"), "--target",
                              "version_consumer"], source)
            self.run_command([str(source / "out/version_consumer")], source)
            self.assertIn('"1.2.3"', (source / "out/generated/version.h").read_text())

    def test_library_version_template_zero_fills_components_without_implicit_local_overrides(self):
        for version, components in ((None, (0, 0, 0)), ("7", (7, 0, 0)),
                                    ("7.4", (7, 4, 0)), ("7.4.2.9", (7, 4, 2)),
                                    ("08.09.01", (8, 9, 1)), ("01.010.00", (1, 10, 0)),
                                    ("000.00.000", (0, 0, 0)), ("010.08.009", (10, 8, 9))):
            with self.subTest(version=version), tempfile.TemporaryDirectory(prefix="esy version parts ") as tmp:
                directory = Path(tmp)
                source = self.prepare_pico_consumer(directory, f'''
include("{cmake_path(PROJECT_IMPORT)}")
if(NOT "${{PROJECT_VERSION}}" STREQUAL "{version or ''}")
  message(FATAL_ERROR "Version generation changed the consumer project version")
endif()
add_executable(version_parts version.c version.cpp)
target_link_libraries(version_parts PRIVATE esy_project_options)
''', project_version=version)
                (source / "cmake").mkdir()
                (source / "cmake/version.h.in").write_text('#error "A custom template requires explicit selection"\n')
                major, minor, patch = components
                expression = (f"APP_VERSION_MAJOR == {major} && APP_VERSION_MINOR == {minor} && "
                              f"APP_VERSION_PATCH == {patch} && APP_VERSION == "
                              f"{major * 10000 + minor * 100 + patch}")
                (source / "version.c").write_text(
                    '#include "version.h"\n'
                    f'_Static_assert({expression}, "Consumer version components must be decimal C integers");\n'
                    'int main(void) { return 0; }\n')
                (source / "version.cpp").write_text(
                    '#include "version.h"\n'
                    f'static_assert({expression}, "Consumer version components must be decimal C++ integers");\n')
                self.configure(source)
                self.run_command(["cmake", "--build", str(source / "out"), "--target",
                                  "version_parts"], source)
                self.run_command([str(source / "out/version_parts")], source)

    def test_explicit_version_template_selects_custom_header_and_output_directory(self):
        with tempfile.TemporaryDirectory(prefix="esy explicit version template ") as tmp:
            directory = Path(tmp)
            custom_template = directory / "custom header outside consumer.in"
            custom_template.write_text('#define SELECTED_VERSION "@PROJECT_VERSION@"\n'
                                       '#define SELECTED_MAJOR "@PROJECT_VERSION_MAJOR@"\n'
                                       '#define SELECTED_MINOR "@PROJECT_VERSION_MINOR@"\n'
                                       '#define SELECTED_PATCH "@PROJECT_VERSION_PATCH@"\n'
                                       '#define EXPLICIT_TEMPLATE_SELECTED 1\n')
            generated = directory / "generated version outside build"
            source = self.prepare_pico_consumer(directory, f'''
include("{cmake_path(PROJECT_IMPORT)}")
add_executable(custom_version version.c)
target_link_libraries(custom_version PRIVATE esy_project_options)
''', before_project=f'''
set(ESY_VERSION_TEMPLATE "{cmake_path(custom_template)}")
set(GENERATED_INCLUDE_DIR "{cmake_path(generated)}")
''', project_version="08.010.00")
            (source / "version.c").write_text('''
#include "version.h"
#include <string.h>
#if !EXPLICIT_TEMPLATE_SELECTED || defined(APP_VERSION)
#error "The explicit version template was replaced by a default"
#endif
int main(void) {
  return strcmp(SELECTED_VERSION, "08.010.00") || strcmp(SELECTED_MAJOR, "08") ||
         strcmp(SELECTED_MINOR, "010") || strcmp(SELECTED_PATCH, "00");
}
''')
            self.configure(source)
            self.run_command(["cmake", "--build", str(source / "out"), "--target",
                              "custom_version"], source)
            self.run_command([str(source / "out/custom_version")], source)
            self.assertTrue((generated / "version.h").is_file())
            self.assertFalse((source / "out/generated/version.h").exists())

    def test_direct_library_assembly_does_not_generate_a_version_header(self):
        with tempfile.TemporaryDirectory(prefix="esy direct assembly version ") as tmp:
            directory = Path(tmp)
            (directory / "board").mkdir()
            (directory / "board/CMakeLists.txt").write_text("add_library(esy_board INTERFACE)\n")
            (directory / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(direct_consumer VERSION 8.7.6 LANGUAGES C)
set(ESY_TARGET_DEVICE PICO)
set(ESY_BOARD_PATH "${{CMAKE_CURRENT_SOURCE_DIR}}/board")
add_library(esy_opt_default INTERFACE)
add_library(esy_opt_speed INTERFACE)
add_subdirectory("{cmake_path(ROOT)}" platform)
if(DEFINED ESY_VERSION_TEMPLATE OR DEFINED GENERATED_INCLUDE_DIR OR TARGET esy_project_options OR
   DEFINED APP_HOST_COMM_TRANSPORT OR DEFINED APP_LOGGER_OUTPUTS OR
   DEFINED APP_LOGGER_BACKEND OR DEFINED APP_ENABLE_STATUS_DISPLAY)
  message(FATAL_ERROR "Direct library assembly activated optional integration defaults")
endif()
''')
            self.configure(directory)
            self.assertFalse(list((directory / "out").rglob("version.h")))
            self.assertFalse((directory / "out/esy_features.json").exists())

    def test_optional_build_config_distinguishes_absence_disable_and_invalid_path(self):
        with tempfile.TemporaryDirectory(prefix="esy optional override ") as tmp:
            directory = Path(tmp)
            # An absent conventional file is valid; an explicit missing file is an error.
            self.run_early(directory, "set(ESY_TARGET_DEVICE PICO)\n")
            self.run_early(directory,
                'set(ESY_TARGET_DEVICE PICO)\n'
                'set(ESY_BUILD_CONFIG_FILE "missing override.cmake")\n',
                "ESY_BUILD_CONFIG_FILE")
            (directory / "cmake").mkdir()
            (directory / "cmake/esy_build_config.cmake").write_text(
                'message(FATAL_ERROR "disabled override")\n')
            self.run_early(directory,
                'set(ESY_TARGET_DEVICE PICO)\nset(ESY_BUILD_CONFIG_FILE "")\n')

    def test_build_override_cannot_replace_device_or_derived_platform(self):
        for variable in ("ESY_TARGET_PLATFORM", "ESY_TARGET_DEVICE"):
            for storage in ("normal", "cache", "unset_cache"):
                with self.subTest(variable=variable, storage=storage), tempfile.TemporaryDirectory(prefix="esy immutable selection ") as tmp:
                    directory = Path(tmp)
                    (directory / "cmake").mkdir()
                    if storage == "unset_cache":
                        if variable != "ESY_TARGET_DEVICE":
                            continue
                        change = f"unset({variable} CACHE)\n"
                    else:
                        suffix = ' CACHE STRING "Conflicting selection" FORCE' if storage == "cache" else ""
                        change = f"set({variable} PICO2{suffix})\n"
                    (directory / "cmake/esy_build_config.cmake").write_text(change)
                    self.run_early(directory,
                        'if(POLICY CMP0126)\n  cmake_policy(SET CMP0126 NEW)\nendif()\n'
                        'set(ESY_TARGET_DEVICE PICO)\n', "selection")

    def test_disabled_optional_providers_do_not_require_conventional_source_trees(self):
        with tempfile.TemporaryDirectory(prefix="esy unused providers ") as tmp:
            directory = Path(tmp)
            source = self.prepare_pico_consumer(directory, f'''
include("{cmake_path(PROJECT_IMPORT)}")
if(TARGET lvgl OR TARGET FreeRTOS-Kernel OR TARGET SEGGER_RTT OR TARGET esy_mqtt_provider)
  message(FATAL_ERROR "A disabled provider was assembled")
endif()
''')
            (source / "esy_feature_import.cmake").write_text(
                'set(ESY_RTOS None)\nset(ENABLE_ESY_LVGL OFF)\nset(ENABLE_ESY_NUMERICS OFF)\n'
                'set(ENABLE_ESY_MQTT OFF)\nset(ESY_COREMQTT_PATH "missing MQTT checkout")\n'
                'set(ENABLE_ESY_LOGGER_BACKEND_RTT OFF)\n')
            self.configure(source)
            self.assertFalse((source / "external/FreeRTOS-Kernel").exists())
            self.assertFalse((source / "external/lvgl").exists())

    def test_pico_sdk_import_preserves_an_explicit_consumer_toolchain(self):
        with tempfile.TemporaryDirectory(prefix="esy consumer toolchain ") as tmp:
            directory = Path(tmp)
            toolchain = directory / "consumer selected toolchain.cmake"
            toolchain.write_text("set(CMAKE_SYSTEM_NAME Generic)\n")
            source = self.prepare_pico_consumer(directory, f'''
if(NOT CMAKE_TOOLCHAIN_FILE STREQUAL "{cmake_path(toolchain)}" OR
   NOT PICO_COMPILER STREQUAL "consumer_compiler_selector")
  message(FATAL_ERROR "Consumer toolchain settings were not preserved")
endif()
include("{cmake_path(PROJECT_IMPORT)}")
''', before_project=f'''
set(EXPECT_EXPLICIT_TOOLCHAIN ON)
set(CMAKE_TOOLCHAIN_FILE "{cmake_path(toolchain)}")
set(PICO_COMPILER consumer_compiler_selector)
''')
            self.configure(source)

    def test_explicit_project_paths_propagate_without_global_settings(self):
        with tempfile.TemporaryDirectory(prefix="esy explicit configuration ") as tmp:
            directory = Path(tmp)
            specs = directory / "configuration family root"
            (specs / "include/specs").mkdir(parents=True)
            (specs / "include/specs/selection.h").write_text("#define SPEC_VALUE 7\n")
            rtos = directory / "native config headers unrelated to specs"
            rtos.mkdir()
            (rtos / "FreeRTOSConfig.h").write_text("#define NATIVE_CONFIG_VALUE 9\n")
            version = directory / "consumer version template.in"
            version.write_text('#define FIXTURE_VERSION "@PROJECT_VERSION@"\n')
            feature = directory / "consumer feature choices.cmake"
            feature.write_text('set(ENABLE_ESY_TOUCH OFF)\nset(ENABLE_ESY_QEMU ON)\n'
                               'file(APPEND "${FIXTURE_EVENT_LOG}" "features\\n")\n')
            generated = directory / "generated include files"
            source = self.prepare_pico_consumer(directory, f'''
set(ESY_FEATURE_IMPORT_FILE "{cmake_path(feature)}")
set(ESY_SPECS_DIR "{cmake_path(specs)}")
set(ESY_RTOS_CONFIG_DIR "{cmake_path(rtos)}")
set(ESY_VERSION_TEMPLATE "{cmake_path(version)}")
set(GENERATED_INCLUDE_DIR "{cmake_path(generated)}")
set(ESY_DEFAULT_COMPILE_OPTIONS -DDEFAULT_POLICY=1)
set(ESY_SPEED_COMPILE_OPTIONS -DSPEED_POLICY=1)
set(ESY_DEFAULT_LINK_OPTIONS -Wl,--as-needed)
set(ESY_PROJECT_COMPILE_DEFINITIONS CONSUMER_POLICY=1)
include("{cmake_path(PROJECT_IMPORT)}")
if(ENABLE_ESY_TOUCH OR NOT ENABLE_ESY_QEMU)
  message(FATAL_ERROR "Consumer feature values changed")
endif()
get_property(global_includes DIRECTORY PROPERTY INCLUDE_DIRECTORIES)
get_property(global_definitions DIRECTORY PROPERTY COMPILE_DEFINITIONS)
if(global_includes OR global_definitions)
  message(FATAL_ERROR "Project settings leaked into directory-global properties")
endif()
add_executable(header_consumer headers.c)
target_link_libraries(header_consumer PRIVATE esy_project_options)
file(GENERATE OUTPUT "${{CMAKE_BINARY_DIR}}/options.txt" CONTENT
  "$<TARGET_PROPERTY:esy_opt_default,INTERFACE_COMPILE_OPTIONS>\n$<TARGET_PROPERTY:esy_opt_speed,INTERFACE_COMPILE_OPTIONS>\n$<TARGET_PROPERTY:esy_opt_default,INTERFACE_LINK_OPTIONS>\n$<TARGET_PROPERTY:esy_opt_default,INTERFACE_LINK_LIBRARIES>\n")
''')
            (source / "headers.c").write_text('''
#include "specs/selection.h"
#include "selection.h"
#include "FreeRTOSConfig.h"
#include "version.h"
#if !defined(ESY_PLATFORM_PICO2_W) || !defined(ENABLE_ESY_QEMU) || !CONSUMER_POLICY
#error "Missing consumer-selected platform/project definitions"
#endif
int main(void) {
  return SPEC_VALUE == 7 && NATIVE_CONFIG_VALUE == 9 && FIXTURE_VERSION[0] == '1' ? 0 : 1;
}
''')
            self.configure(source)
            self.run_command(["cmake", "--build", str(source / "out"), "--target",
                              "header_consumer"], source)
            self.run_command([str(source / "out/header_consumer")], source)
            self.assertEqual(["sdk-import", "project", "features", "sdk-init"],
                             (directory / "events.txt").read_text().splitlines())
            options = (source / "out/options.txt").read_text()
            for value in ("DEFAULT_POLICY=1", "SPEED_POLICY=1", "--as-needed",
                          "esy_project_options"):
                self.assertIn(value, options)

    def test_existing_optimization_targets_keep_consumer_options(self):
        with tempfile.TemporaryDirectory(prefix="esy preserved targets ") as tmp:
            directory = Path(tmp)
            source = self.prepare_pico_consumer(directory, f'''
add_library(esy_opt_default INTERFACE)
add_library(esy_opt_speed INTERFACE)
target_compile_options(esy_opt_default INTERFACE -DKEEP_DEFAULT=1)
target_compile_options(esy_opt_speed INTERFACE -DKEEP_SPEED=1)
set(ESY_DEFAULT_COMPILE_OPTIONS -DREPLACEMENT_MUST_NOT_APPLY=1)
set(ESY_SPEED_COMPILE_OPTIONS -DREPLACEMENT_MUST_NOT_APPLY=1)
include("{cmake_path(PROJECT_IMPORT)}")
foreach(option_target esy_opt_default esy_opt_speed)
  get_target_property(options ${{option_target}} INTERFACE_COMPILE_OPTIONS)
  get_target_property(links ${{option_target}} INTERFACE_LINK_LIBRARIES)
  if(options MATCHES "REPLACEMENT_MUST_NOT_APPLY" OR NOT links MATCHES "esy_project_options")
    message(FATAL_ERROR "Existing consumer target was replaced or lost shared settings")
  endif()
endforeach()
get_target_property(default_options esy_opt_default INTERFACE_COMPILE_OPTIONS)
get_target_property(speed_options esy_opt_speed INTERFACE_COMPILE_OPTIONS)
if(NOT default_options MATCHES "KEEP_DEFAULT" OR NOT speed_options MATCHES "KEEP_SPEED")
  message(FATAL_ERROR "Consumer optimization options were discarded")
endif()
''')
            self.configure(source)

    def test_existing_sdk_and_software_providers_remain_consumer_owned(self):
        with tempfile.TemporaryDirectory(prefix="esy existing providers ") as tmp:
            directory = Path(tmp)
            source = self.prepare_pico_consumer(directory, f'''
add_library(pico_stdlib INTERFACE)
add_library(FreeRTOS-Kernel INTERFACE)
add_library(lvgl INTERFACE)
add_library(esy_mqtt_provider INTERFACE)
add_library(consumer_rtt INTERFACE)
target_compile_definitions(lvgl INTERFACE LVGL_CONSUMER_SENTINEL)
target_compile_definitions(esy_mqtt_provider INTERFACE MQTT_CONSUMER_SENTINEL)
target_compile_definitions(FreeRTOS-Kernel INTERFACE RTOS_CONSUMER_SENTINEL)
target_compile_definitions(consumer_rtt INTERFACE RTT_CONSUMER_SENTINEL)
set(ESY_RTOS FreeRTOS)
set(ENABLE_ESY_LVGL ON)
set(ENABLE_ESY_MQTT ON)
set(ESY_COREMQTT_PATH "missing MQTT checkout")
set(ENABLE_ESY_NUMERICS ON)
set(ENABLE_ESY_LOGGER ON)
set(ENABLE_ESY_LOGGER_BACKEND_RTT ON)
set(ESY_SEGGER_RTT_TARGET consumer_rtt)
# A numeric zero is a valid explicit TinyUSB OPT_OS_NONE selection.
set(TINYUSB_OPT_OS 0)
include("{cmake_path(PROJECT_IMPORT)}")
foreach(provider lvgl FreeRTOS-Kernel consumer_rtt esy_mqtt_provider)
  get_target_property(definitions ${{provider}} INTERFACE_COMPILE_DEFINITIONS)
  if(NOT definitions MATCHES "CONSUMER_SENTINEL")
    message(FATAL_ERROR "Consumer provider was replaced")
  endif()
endforeach()
if(NOT FREE_RTOS_KERNEL_LIB STREQUAL "FreeRTOS-Kernel")
  message(FATAL_ERROR "Existing native OS provider was not reused")
endif()
if(NOT TINYUSB_OPT_OS STREQUAL "0")
  message(FATAL_ERROR "The consumer's TinyUSB OS selection was overwritten")
endif()
''')
            self.configure(source)
            self.assertEqual(["sdk-import", "project"],
                             (directory / "events.txt").read_text().splitlines())

    def test_default_pico_network_providers_propagate_configuration_to_sdk_sources(self):
        for wifi, tcp in ((False, False), (True, False), (False, True), (True, True)):
            for rtos in ("None", "FreeRTOS"):
                with self.subTest(wifi=wifi, tcp=tcp, rtos=rtos), tempfile.TemporaryDirectory(prefix="esy network defaults ") as tmp:
                    directory = Path(tmp)
                    specs = directory / "network configuration outside consumer"
                    (specs / "include/specs").mkdir(parents=True)
                    (specs / "include/specs/lwipopts.h").write_text("#define NETWORK_OPTION_VALUE 31\n")
                    native = directory / "native configuration outside specs"
                    native.mkdir()
                    (native / "FreeRTOSConfig.h").write_text("#define NATIVE_CONFIG_VALUE 17\n")
                    kernel = directory / "native kernel public headers"
                    kernel.mkdir()
                    (kernel / "FreeRTOS.h").write_text("#define NATIVE_KERNEL_VALUE 19\n")
                    sdk_source = directory / "SDK interface source.c"
                    sdk_source.write_text('''
#include "lwipopts.h"
#if NETWORK_USES_FREERTOS
#include "FreeRTOS.h"
#include "FreeRTOSConfig.h"
_Static_assert(NATIVE_CONFIG_VALUE == 17 && NATIVE_KERNEL_VALUE == 19,
               "Network SDK sources require the selected native provider/config");
#endif
int network_sdk_probe(void) { return NETWORK_OPTION_VALUE; }
''')
                    native_input = f'''
add_library(network_kernel INTERFACE)
target_include_directories(network_kernel INTERFACE "{cmake_path(kernel)}")
set(FREE_RTOS_KERNEL_LIB network_kernel)
''' if rtos == "FreeRTOS" else "set(FREE_RTOS_KERNEL_LIB unused_native_provider)"
                    providers = (["esy_wifi_provider"] if wifi else []) + (
                        ["esy_network_lwip_provider"] if tcp else [])
                    source = self.prepare_pico_consumer(directory, f'''
add_library(pico_cyw43_arch_lwip_poll INTERFACE)
target_sources(pico_cyw43_arch_lwip_poll INTERFACE "{cmake_path(sdk_source)}")
target_compile_definitions(pico_cyw43_arch_lwip_poll INTERFACE
  NETWORK_USES_FREERTOS={int(rtos == 'FreeRTOS')})
{native_input}
set(ESY_SPECS_DIR "{cmake_path(specs)}")
set(ESY_RTOS_CONFIG_DIR "{cmake_path(native)}")
include("{cmake_path(PROJECT_IMPORT)}")
if(TARGET esy_wifi_provider OR TARGET esy_network_lwip_provider)
  message(FATAL_ERROR "Default network providers were created before library assembly")
endif()
{self.platform_assembly(directory)}
if(TARGET specs)
  message(FATAL_ERROR "Fixture must not supply a consumer-named configuration target")
endif()
if({'NOT ' if wifi else ''}TARGET esy_wifi_provider OR
   {'NOT ' if tcp else ''}TARGET esy_network_lwip_provider)
  message(FATAL_ERROR "Network provider assembly did not follow the selected gates")
endif()
foreach(provider esy_wifi_provider esy_network_lwip_provider)
  if(TARGET ${{provider}})
    get_target_property(links ${{provider}} INTERFACE_LINK_LIBRARIES)
    if(NOT "esy_project_options" IN_LIST links OR
       NOT "pico_cyw43_arch_lwip_poll" IN_LIST links)
      message(FATAL_ERROR "Default network provider lost SDK/configuration requirements")
    endif()
    if("unused_native_provider" IN_LIST links)
      message(FATAL_ERROR "Bare-metal networking acquired an unused native RTOS provider")
    endif()
  endif()
endforeach()
add_executable(network_consumer main.c)
target_link_libraries(network_consumer PRIVATE {' '.join(providers)})
''')
                    (source / "esy_feature_import.cmake").write_text(
                        f"set(ESY_RTOS {rtos})\nset(ENABLE_ESY_SYSTEM {'ON' if rtos == 'FreeRTOS' or tcp else 'OFF'})\n"
                        f"set(ENABLE_ESY_WIFI {'ON' if wifi else 'OFF'})\n"
                        f"set(ENABLE_ESY_COMMON_DS ON)\nset(ENABLE_ESY_NETWORK {'ON' if tcp else 'OFF'})\n"
                        f"set(ENABLE_ESY_NETWORK_LWIP {'ON' if tcp else 'OFF'})\n")
                    (source / "main.c").write_text(
                        "int network_sdk_probe(void);\n"
                        "int main(void) { return network_sdk_probe() == 31 ? 0 : 1; }\n"
                        if providers else "int main(void) { return 0; }\n")
                    self.configure(source)
                    self.run_command(["cmake", "--build", str(source / "out"), "--target",
                                      "network_consumer"], source)
                    self.run_command([str(source / "out/network_consumer")], source)

    def test_explicit_pico_network_providers_take_precedence_without_poll_sdk_target(self):
        for timing in ("before_helper", "after_helper"):
            with self.subTest(timing=timing), tempfile.TemporaryDirectory(prefix="esy custom network providers ") as tmp:
                directory = Path(tmp)
                provider_definitions = '''
foreach(provider esy_wifi_provider esy_network_lwip_provider)
  add_library(${provider} INTERFACE)
  target_compile_definitions(${provider} INTERFACE CONSUMER_${provider}=1)
endforeach()
'''
                source = self.prepare_pico_consumer(directory, f'''
{provider_definitions if timing == 'before_helper' else ''}
include("{cmake_path(PROJECT_IMPORT)}")
{provider_definitions if timing == 'after_helper' else ''}
{self.platform_assembly(directory)}
foreach(provider esy_wifi_provider esy_network_lwip_provider)
  get_target_property(links ${{provider}} INTERFACE_LINK_LIBRARIES)
  if(links)
    message(FATAL_ERROR "Consumer network provider acquired default dependencies")
  endif()
endforeach()
add_executable(custom_network main.c)
target_link_libraries(custom_network PRIVATE esy_wifi_provider esy_network_lwip_provider)
''')
                (source / "esy_feature_import.cmake").write_text(
                    "set(ESY_RTOS None)\nset(ENABLE_ESY_WIFI ON)\n"
                    "set(ENABLE_ESY_SYSTEM ON)\nset(ENABLE_ESY_COMMON_DS ON)\n"
                    "set(ENABLE_ESY_NETWORK ON)\nset(ENABLE_ESY_NETWORK_LWIP ON)\n")
                (source / "main.c").write_text('''
#if !CONSUMER_esy_wifi_provider || !CONSUMER_esy_network_lwip_provider
#error "Explicit network providers were replaced"
#endif
int main(void) { return 0; }
''')
                self.configure(source)
                self.run_command(["cmake", "--build", str(source / "out"), "--target",
                                  "custom_network"], source)

    def test_disabled_network_does_not_assemble_an_unused_lwip_provider(self):
        with tempfile.TemporaryDirectory(prefix="esy disabled network providers ") as tmp:
            directory = Path(tmp)
            source = self.prepare_pico_consumer(directory, f'''
include("{cmake_path(PROJECT_IMPORT)}")
{self.platform_assembly(directory)}
if(TARGET esy_network_lwip_provider OR TARGET esy_wifi_provider)
  message(FATAL_ERROR "Disabled networking assembled a provider")
endif()
''')
            (source / "esy_feature_import.cmake").write_text(
                "set(ESY_RTOS None)\nset(ENABLE_ESY_WIFI OFF)\n"
                "set(ENABLE_ESY_NETWORK OFF)\nset(ENABLE_ESY_NETWORK_LWIP ON)\n")
            self.configure(source)

    def test_pico_network_provider_defaults_do_not_apply_to_other_platforms(self):
        for platform in ("PICO", "PICO2", "STM32F4", "ESP32"):
            with self.subTest(platform=platform), tempfile.TemporaryDirectory(prefix="esy network platform ") as tmp:
                directory = Path(tmp)
                board_probe = '''
if(TARGET esy_wifi_provider OR TARGET esy_network_lwip_provider)
  message(FATAL_ERROR "Pico W provider defaults leaked onto another platform")
endif()
message(FATAL_ERROR "Board observed no Pico W network defaults")
'''
                (directory / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(network_platform C)
set(ESY_TARGET_DEVICE {'STM32F407ZG' if platform == 'STM32F4' else platform})
set(ESY_BOARD_PATH "${{CMAKE_CURRENT_SOURCE_DIR}}/unused board input")
set(ESY_FEATURE_IMPORT_FILE "${{CMAKE_CURRENT_SOURCE_DIR}}/features.cmake")
add_library(pico_stdlib INTERFACE)
include("{cmake_path(PROJECT_IMPORT)}")
{self.platform_assembly(directory, board_probe)}
''')
                (directory / "features.cmake").write_text(
                    "set(ESY_RTOS None)\nset(ENABLE_ESY_WIFI ON)\n"
                    "set(ENABLE_ESY_SYSTEM ON)\nset(ENABLE_ESY_COMMON_DS ON)\n"
                    "set(ENABLE_ESY_NETWORK ON)\nset(ENABLE_ESY_NETWORK_LWIP ON)\n")
                self.configure(directory, "Board observed no Pico W network defaults")

    def test_helper_in_another_directory_does_not_enable_direct_consumer_defaults(self):
        with tempfile.TemporaryDirectory(prefix="esy isolated network defaults ") as tmp:
            directory = Path(tmp)
            helper_user = directory / "separate helper consumer"
            helper_user.mkdir()
            (helper_user / "CMakeLists.txt").write_text(
                f'include("{cmake_path(PROJECT_IMPORT)}")\n')
            (directory / "direct_features.cmake").write_text(
                "set(ESY_RTOS None)\nset(ENABLE_ESY_WIFI ON)\n")
            (directory / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(isolated_direct_consumer C)
set(ESY_TARGET_DEVICE PICO2_W)
set(ESY_BOARD_PATH "${{CMAKE_CURRENT_SOURCE_DIR}}/unused board input")
add_library(pico_stdlib INTERFACE)
add_subdirectory("{cmake_path(helper_user)}" helper_user)
if(NOT COMMAND esy_configure_default_network_providers)
  message(FATAL_ERROR "The sibling helper did not load its provider function")
endif()
if(DEFINED esy_network_provider_defaults_enabled)
  message(FATAL_ERROR "A sibling consumer leaked its integration opt-in")
endif()
include("${{CMAKE_CURRENT_SOURCE_DIR}}/direct_features.cmake")
{self.platform_assembly(directory)}
''')
            self.configure(directory, "requires consumer target esy_wifi_provider")

    def test_pico_network_defaults_report_a_missing_sdk_poll_provider(self):
        for gate, provider in (("ENABLE_ESY_WIFI", "esy_wifi_provider"),
                               ("ENABLE_ESY_NETWORK_LWIP", "esy_network_lwip_provider")):
            with self.subTest(gate=gate), tempfile.TemporaryDirectory(prefix="esy missing poll SDK ") as tmp:
                directory = Path(tmp)
                source = self.prepare_pico_consumer(directory,
                    f'include("{cmake_path(PROJECT_IMPORT)}")\n'
                    + self.platform_assembly(directory))
                (source / "esy_feature_import.cmake").write_text(
                    "set(ESY_RTOS None)\nset(ENABLE_ESY_SYSTEM ON)\n"
                    f"set(ENABLE_ESY_NETWORK ON)\nset({gate} ON)\n")
                result = self.configure(source, "pico_cyw43_arch_lwip_poll")
                self.assertIn(provider, result.stdout + result.stderr)

    def test_project_only_network_import_uses_explicit_configuration_paths(self):
        with tempfile.TemporaryDirectory(prefix="esy project-only network ") as tmp:
            directory = Path(tmp)
            configuration = directory / "network config chosen by consumer"
            (configuration / "include/specs").mkdir(parents=True)
            (configuration / "include/specs/lwipopts.h").write_text("#define NETWORK_OPTION_VALUE 43\n")
            (directory / "sdk.c").write_text(
                '#include "lwipopts.h"\nint network_sdk_probe(void) { return NETWORK_OPTION_VALUE; }\n')
            (directory / "main.c").write_text(
                'int network_sdk_probe(void);\nint main(void) { return network_sdk_probe() == 43 ? 0 : 1; }\n')
            (directory / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(project_only_network C)
set(ESY_TARGET_DEVICE PICO_W)
set(ESY_BOARD_PATH "${{CMAKE_CURRENT_SOURCE_DIR}}/board")
set(ESY_SPECS_DIR "{cmake_path(configuration)}")
set(ESY_FEATURE_IMPORT_FILE "${{CMAKE_CURRENT_SOURCE_DIR}}/features.cmake")
add_library(pico_stdlib INTERFACE)
add_library(pico_cyw43_arch_lwip_poll INTERFACE)
target_sources(pico_cyw43_arch_lwip_poll INTERFACE "${{CMAKE_CURRENT_SOURCE_DIR}}/sdk.c")
include("{cmake_path(PROJECT_IMPORT)}")
{self.platform_assembly(directory)}
add_executable(project_only_network main.c)
target_link_libraries(project_only_network PRIVATE esy_wifi_provider)
''')
            (directory / "features.cmake").write_text("set(ESY_RTOS None)\nset(ENABLE_ESY_WIFI ON)\n")
            self.configure(directory)
            self.run_command(["cmake", "--build", str(directory / "out"), "--target",
                              "project_only_network"], directory)
            self.run_command([str(directory / "out/project_only_network")], directory)

    def test_direct_provider_import_requires_configuration_options_for_network_defaults(self):
        with tempfile.TemporaryDirectory(prefix="esy missing network options ") as tmp:
            directory = Path(tmp)
            (directory / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(missing_network_options C)
set(ESY_TARGET_DEVICE PICO_W)
set(ESY_RTOS None)
set(ENABLE_ESY_WIFI ON)
add_library(pico_stdlib INTERFACE)
add_library(pico_cyw43_arch_lwip_poll INTERFACE)
add_library(esy_opt_default INTERFACE)
add_library(esy_opt_speed INTERFACE)
include("{cmake_path(THIRD_PARTY_IMPORT)}")
{self.platform_assembly(directory)}
''')
            self.configure(directory, "network providers require esy_project_options")

    def test_freertos_import_uses_the_explicit_native_config_directory(self):
        for import_mode in ("assembly", "helper"):
            with self.subTest(import_mode=import_mode), tempfile.TemporaryDirectory(prefix="esy native provider ") as tmp:
                directory = Path(tmp)
                kernel = directory / "kernel outside consumer"
                (kernel / "include").mkdir(parents=True)
                (kernel / "include/FreeRTOS.h").write_text("/* Provider fixture. */\n")
                native_config = directory / "exact native config headers"
                native_config.mkdir()
                (native_config / "FreeRTOSConfig.h").write_text("/* Consumer fixture. */\n")
                port = kernel / "portable/ThirdParty/GCC/RP2350_ARM_NTZ"
                port.mkdir(parents=True)
                (port / "CMakeLists.txt").write_text(f'''
if(NOT FREERTOS_CONFIG_FILE_DIRECTORY STREQUAL "{cmake_path(native_config)}")
  message(FATAL_ERROR "RTOS configuration path was inferred or nested again")
endif()
if(NOT FREERTOS_HEAP STREQUAL "4")
  message(FATAL_ERROR "The consumer's heap selection was overwritten")
endif()
if(NOT TINYUSB_OPT_OS STREQUAL "OPT_OS_PICO")
  message(FATAL_ERROR "Pico FreeRTOS USB scheduling default was not supplied")
endif()
add_library(FreeRTOS-Kernel INTERFACE)
target_include_directories(FreeRTOS-Kernel INTERFACE "${{FREERTOS_CONFIG_FILE_DIRECTORY}}")
''')
                initialization = (self.platform_assembly(directory) if import_mode == "assembly"
                                  else f'include("{cmake_path(PROJECT_IMPORT)}")\n')
                source = self.prepare_pico_consumer(directory, f'''
set(ESY_RTOS FreeRTOS)
set(ENABLE_ESY_SYSTEM ON)
set(FREERTOS_HEAP 4 CACHE STRING "Consumer heap policy")
set(FREERTOS_KERNEL_PATH "{cmake_path(kernel)}")
set(ESY_RTOS_CONFIG_DIR "{cmake_path(native_config)}")
add_library(tinyusb_common INTERFACE)
{initialization}
if(NOT TARGET FreeRTOS-Kernel OR NOT FREE_RTOS_KERNEL_LIB STREQUAL "FreeRTOS-Kernel")
  message(FATAL_ERROR "The requested native provider was not assembled")
endif()
get_target_property(tinyusb_links tinyusb_common INTERFACE_LINK_LIBRARIES)
if(NOT tinyusb_links MATCHES "FreeRTOS-Kernel")
  message(FATAL_ERROR "Selected native provider did not reach TinyUSB")
endif()
''')
                self.configure(source)

    def test_provider_environment_defaults_preserve_explicit_cli_paths(self):
        for explicit in (False, True):
            with self.subTest(explicit=explicit), tempfile.TemporaryDirectory(prefix="esy environment providers ") as tmp:
                directory = Path(tmp)
                environment_root = directory / "environment providers"
                explicit_root = directory / "command line providers"
                selected_root = explicit_root if explicit else environment_root
                source = self.prepare_pico_consumer(directory, f'''
include("{cmake_path(PROJECT_IMPORT)}")
if(NOT FREERTOS_KERNEL_PATH STREQUAL "{cmake_path(selected_root)}/kernel" OR
   NOT LVGL_PATH STREQUAL "{cmake_path(selected_root)}/graphics")
  message(FATAL_ERROR "Provider environment/CLI precedence was not preserved")
endif()
''')
                arguments = ([f"-DFREERTOS_KERNEL_PATH={explicit_root}/kernel",
                              f"-DLVGL_PATH={explicit_root}/graphics"] if explicit else [])
                with mock.patch.dict(os.environ, {
                    "FREERTOS_KERNEL_PATH": str(environment_root / "kernel"),
                    "LVGL_PATH": str(environment_root / "graphics"),
                }):
                    self.configure(source, extra_args=arguments)

    def test_enabled_provider_explicit_empty_path_does_not_fall_back_to_environment(self):
        for provider in ("FREERTOS_KERNEL_PATH", "LVGL_PATH"):
            with self.subTest(provider=provider), tempfile.TemporaryDirectory(prefix="esy disabled provider fallback ") as tmp:
                directory = Path(tmp)
                provider_root = directory / "valid environment source tree"
                provider_root.mkdir()
                native_config = directory / "native config"
                native_config.mkdir()
                (native_config / "FreeRTOSConfig.h").write_text("/* Consumer configuration. */\n")
                if provider == "FREERTOS_KERNEL_PATH":
                    (provider_root / "include").mkdir()
                    (provider_root / "include/FreeRTOS.h").write_text("/* Provider fixture. */\n")
                    port = provider_root / "portable/ThirdParty/GCC/RP2350_ARM_NTZ"
                    port.mkdir(parents=True)
                    (port / "CMakeLists.txt").write_text("add_library(FreeRTOS-Kernel INTERFACE)\n")
                    selection = f'set(ESY_RTOS FreeRTOS)\nset(ESY_RTOS_CONFIG_DIR "{cmake_path(native_config)}")'
                else:
                    (provider_root / "stub.c").write_text("int graphics_probe(void) { return 0; }\n")
                    (provider_root / "CMakeLists.txt").write_text("add_library(lvgl STATIC stub.c)\n")
                    selection = "set(ENABLE_ESY_LVGL ON)"
                source = self.prepare_pico_consumer(directory, selection + f'''
include("{cmake_path(PROJECT_IMPORT)}")
''')
                with mock.patch.dict(os.environ, {provider: str(provider_root)}):
                    self.configure(source, error=provider, extra_args=(f"-D{provider}=",))

    def test_lvgl_provider_preserves_normal_and_cache_configuration_inputs(self):
        for mode in ("normal", "shadow_cache", "cache"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix="esy lvgl configuration ") as tmp:
                directory = Path(tmp)
                lvgl = directory / "graphics provider"
                lvgl.mkdir()
                normal_config = directory / "normal graphics configuration.h"
                cache_config = directory / "cached graphics configuration.h"
                normal_config.write_text("/* Consumer normal configuration. */\n")
                cache_config.write_text("/* Consumer cached configuration. */\n")
                selected = cache_config if mode == "cache" else normal_config
                (lvgl / "stub.c").write_text("int graphics_configuration_probe(void) { return 0; }\n")
                (lvgl / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.12.4)
set(LV_BUILD_CONF_PATH "" CACHE PATH "Vendor graphics config description")
if(NOT LV_BUILD_CONF_PATH STREQUAL "{cmake_path(selected)}")
  message(FATAL_ERROR "Provider cache declaration discarded the selected header")
endif()
add_library(lvgl STATIC stub.c)
''')
                normal_input = (f'set(LV_BUILD_CONF_PATH "{cmake_path(normal_config)}")'
                                if mode != "cache" else "")
                source = self.prepare_pico_consumer(directory, f'''
set(ENABLE_ESY_LVGL ON)
set(LVGL_PATH "{cmake_path(lvgl)}")
{normal_input}
get_property(cache_before CACHE LV_BUILD_CONF_PATH PROPERTY TYPE SET)
if(cache_before)
  get_property(original_cache_value CACHE LV_BUILD_CONF_PATH PROPERTY VALUE)
endif()
include("{cmake_path(PROJECT_IMPORT)}")
if(NOT LV_BUILD_CONF_PATH STREQUAL "{cmake_path(selected)}")
  message(FATAL_ERROR "Provider import discarded the parent header setting")
endif()
if(cache_before AND NOT "$CACHE{{LV_BUILD_CONF_PATH}}" STREQUAL "${{original_cache_value}}")
  message(FATAL_ERROR "Provider import changed the consumer's cached header value")
endif()
''')
                arguments = ((f"-DLV_BUILD_CONF_PATH={cache_config}",)
                             if mode != "normal" else ())
                self.configure(source, extra_args=arguments)

    def test_lvgl_provider_preserves_normal_choices_over_vendor_options(self):
        for cached in (False, True):
            with self.subTest(cached=cached), \
                    tempfile.TemporaryDirectory(prefix="esy lvgl plain choices ") as temporary:
                directory = Path(temporary)
                lvgl = directory / "graphics provider"
                lvgl.mkdir()
                (lvgl / "stub.c").write_text("int graphics_options(void) { return 0; }\n")
                (lvgl / "CMakeLists.txt").write_text('''
cmake_minimum_required(VERSION 3.12.4)
option(CONFIG_LV_BUILD_DEMOS "Vendor demos" ON)
option(CONFIG_LV_BUILD_EXAMPLES "Vendor examples" ON)
if(CONFIG_LV_BUILD_DEMOS OR CONFIG_LV_BUILD_EXAMPLES)
  message(FATAL_ERROR "Vendor options discarded the consumer's normal choices")
endif()
add_library(lvgl STATIC stub.c)
''')
                source = self.prepare_pico_consumer(directory, f'''
set(ENABLE_ESY_LVGL ON)
set(CONFIG_LV_BUILD_DEMOS OFF)
set(CONFIG_LV_BUILD_EXAMPLES OFF)
set(LVGL_PATH "{cmake_path(lvgl)}")
include("{cmake_path(PROJECT_IMPORT)}")
if(CONFIG_LV_BUILD_DEMOS OR CONFIG_LV_BUILD_EXAMPLES)
  message(FATAL_ERROR "Provider import changed the consumer's normal choices")
endif()
''')
                arguments = ("-DCONFIG_LV_BUILD_DEMOS:BOOL=ON",
                             "-DCONFIG_LV_BUILD_EXAMPLES:BOOL=ON") if cached else ()
                self.configure(source, extra_args=arguments)

    def test_new_lvgl_and_rtt_providers_use_explicit_external_sources(self):
        with tempfile.TemporaryDirectory(prefix="esy software sources ") as tmp:
            directory = Path(tmp)
            lvgl = directory / "graphics provider sources"
            lvgl.mkdir()
            config = directory / "graphics config owned by consumer.h"
            config.write_text("/* Consumer configuration fixture. */\n")
            (lvgl / "stub.c").write_text("int provider_probe(void) { return 0; }\n")
            (lvgl / "CMakeLists.txt").write_text(f'''
if(NOT LV_BUILD_CONF_PATH STREQUAL "{cmake_path(config)}")
  message(FATAL_ERROR "The selected graphics configuration was changed")
endif()
add_library(lvgl STATIC stub.c)
''')
            rtt = directory / "trace provider sources"
            (rtt / "RTT").mkdir(parents=True)
            (rtt / "Config").mkdir()
            (rtt / "RTT/SEGGER_RTT.c").write_text("int trace_provider_probe(void) { return 0; }\n")
            (rtt / "RTT/SEGGER_RTT.h").write_text("/* Provider fixture. */\n")
            (rtt / "Config/SEGGER_RTT_Conf.h").write_text("/* Provider fixture. */\n")
            source = self.prepare_pico_consumer(directory, f'''
set(ENABLE_ESY_LVGL ON)
set(LVGL_PATH "{cmake_path(lvgl)}")
set(LV_BUILD_CONF_PATH "{cmake_path(config)}")
set(ENABLE_ESY_LOGGER ON)
set(ENABLE_ESY_LOGGER_BACKEND_RTT ON)
set(ESY_SEGGER_RTT_ROOT "{cmake_path(rtt)}")
include("{cmake_path(PROJECT_IMPORT)}")
if(NOT TARGET lvgl OR NOT TARGET ${{ESY_SEGGER_RTT_TARGET}})
  message(FATAL_ERROR "Explicit software providers were not assembled")
endif()
if(TARGET pico_stdio_rtt)
  message(FATAL_ERROR "Logging unexpectedly selected an additional stdio owner")
endif()
get_target_property(rtt_includes ${{ESY_SEGGER_RTT_TARGET}} INTERFACE_INCLUDE_DIRECTORIES)
if(NOT rtt_includes MATCHES "trace provider sources/RTT" OR
   NOT rtt_includes MATCHES "trace provider sources/Config")
  message(FATAL_ERROR "RTT include requirements were lost")
endif()
''')
            self.configure(source)

    def test_disabling_logger_ignores_cached_rtt_selection_and_preserves_other_providers(self):
        with tempfile.TemporaryDirectory(prefix="esy disabled logger rtt ") as tmp:
            directory = Path(tmp)
            rtt = directory / "logger RTT sources"
            (rtt / "RTT").mkdir(parents=True)
            (rtt / "Config").mkdir()
            files = (rtt / "RTT/SEGGER_RTT.c", rtt / "RTT/SEGGER_RTT.h",
                     rtt / "Config/SEGGER_RTT_Conf.h")
            for path in files:
                path.write_text("/* Logger-only RTT provider fixture. */\n")
            source = self.prepare_pico_consumer(directory, f'''
set(ESY_SEGGER_RTT_ROOT "{cmake_path(rtt)}")
add_library(consumer_rtt INTERFACE)
add_library(pico_stdio_rtt INTERFACE)
foreach(provider consumer_rtt pico_stdio_rtt)
  target_compile_definitions(${{provider}} INTERFACE INDEPENDENT_RTT_PROVIDER)
endforeach()
include("{cmake_path(PROJECT_IMPORT)}")
if(NOT ENABLE_ESY_LOGGER_BACKEND_RTT)
  message(FATAL_ERROR "The test must retain the cached RTT child gate")
endif()
if(ENABLE_ESY_LOGGER)
  if(NOT TARGET esy_third_party_segger_rtt)
    message(FATAL_ERROR "Enabled RTT logger did not assemble its provider")
  endif()
elseif(TARGET esy_third_party_segger_rtt)
  message(FATAL_ERROR "Disabled logger assembled its RTT provider")
endif()
foreach(provider consumer_rtt pico_stdio_rtt)
  get_target_property(definitions ${{provider}} INTERFACE_COMPILE_DEFINITIONS)
  if(NOT definitions STREQUAL "INDEPENDENT_RTT_PROVIDER")
    message(FATAL_ERROR "Logger selection changed an independent RTT provider")
  endif()
endforeach()
''')
            self.configure(source, extra_args=("-DENABLE_ESY_LOGGER=ON",
                                               "-DENABLE_ESY_LOGGER_BACKEND_RTT=ON"))
            for path in files:
                path.unlink()
            self.configure(source, extra_args=("-DENABLE_ESY_LOGGER=OFF",))
            cache = (source / "out/CMakeCache.txt").read_text()
            self.assertIn("ENABLE_ESY_LOGGER:UNINITIALIZED=OFF", cache)
            self.assertIn("ENABLE_ESY_LOGGER_BACKEND_RTT:UNINITIALIZED=ON", cache)

    def test_enabled_missing_software_providers_fail_without_private_fallbacks(self):
        cases = (
            ('set(ESY_RTOS FreeRTOS)\nset(FREERTOS_KERNEL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/missing")',
             "FREERTOS_KERNEL_PATH"),
            ('set(ENABLE_ESY_LVGL ON)\nset(LVGL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/missing")',
             "LVGL_PATH"),
            ('set(ENABLE_ESY_MQTT ON)\nset(ESY_COREMQTT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/missing")',
             "ESY_COREMQTT_PATH"),
            ('set(ENABLE_ESY_LOGGER ON)\nset(ENABLE_ESY_LOGGER_BACKEND_RTT ON)\nset(ESY_SEGGER_RTT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/missing")',
             "ESY_SEGGER_RTT_ROOT"),
        )
        for setup, error in cases:
            with self.subTest(setup=setup), tempfile.TemporaryDirectory(prefix="esy missing providers ") as tmp:
                directory = Path(tmp)
                source = self.prepare_pico_consumer(directory,
                    setup + f'\ninclude("{cmake_path(PROJECT_IMPORT)}")\n')
                self.configure(source, error)

    def test_explicit_empty_feature_and_version_paths_disable_conventions(self):
        with tempfile.TemporaryDirectory(prefix="esy disabled optional files ") as tmp:
            directory = Path(tmp)
            source = self.prepare_pico_consumer(directory, f'''
include("{cmake_path(PROJECT_IMPORT)}")
if(ENABLE_ESY_TOUCH OR ESY_FEATURE_IMPORT_FILE OR ESY_VERSION_TEMPLATE)
  message(FATAL_ERROR "Disabled optional files were discovered")
endif()
''', before_project='''
set(ESY_FEATURE_IMPORT_FILE "")
set(ESY_VERSION_TEMPLATE "")
''')
            (source / "esy_feature_import.cmake").write_text('message(FATAL_ERROR "disabled feature file")\n')
            (source / "cmake").mkdir()
            (source / "cmake/version.h.in").write_text("disabled version template\n")
            self.configure(source)
            self.assertFalse((source / "out/generated/version.h").exists())

    def test_project_import_without_early_keeps_the_existing_explicit_contract(self):
        with tempfile.TemporaryDirectory(prefix="esy custom assembly ") as tmp:
            directory = Path(tmp)
            (directory / "esy_feature_import.cmake").write_text('message(FATAL_ERROR "unrequested file")\n')
            (directory / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(custom_consumer C)
set(ESY_TARGET_DEVICE PICO)
set(ESY_BOARD_PATH "${{CMAKE_CURRENT_SOURCE_DIR}}/custom board")
add_library(pico_stdlib INTERFACE)
include("{cmake_path(PROJECT_IMPORT)}")
if(DEFINED ESY_SPECS_DIR OR DEFINED GENERATED_INCLUDE_DIR OR
   DEFINED ESY_BUILD_CONFIG_FILE OR CMAKE_BUILD_TYPE)
  message(FATAL_ERROR "Custom consumer acquired unrequested conventions")
endif()
''')
            self.configure(directory)

    def test_project_import_without_early_generates_an_explicit_custom_version(self):
        with tempfile.TemporaryDirectory(prefix="esy explicit project version ") as tmp:
            directory = Path(tmp)
            (directory / "consumer.in").write_text('#define CUSTOM_PROJECT_VERSION "@PROJECT_VERSION@"\n')
            (directory / "version.c").write_text(
                '#include "version.h"\n'
                "int main(void) { return CUSTOM_PROJECT_VERSION[0] == '6' ? 0 : 1; }\n")
            (directory / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(explicit_consumer VERSION 6.5.4 LANGUAGES C)
set(ESY_TARGET_DEVICE PICO)
set(ESY_BOARD_PATH "${{CMAKE_CURRENT_SOURCE_DIR}}/custom board")
set(ESY_VERSION_TEMPLATE "${{CMAKE_CURRENT_SOURCE_DIR}}/consumer.in")
set(GENERATED_INCLUDE_DIR "${{CMAKE_BINARY_DIR}}/custom generated")
add_library(pico_stdlib INTERFACE)
include("{cmake_path(PROJECT_IMPORT)}")
add_executable(explicit_version version.c)
target_link_libraries(explicit_version PRIVATE esy_project_options)
''')
            self.configure(directory)
            self.run_command(["cmake", "--build", str(directory / "out"), "--target",
                              "explicit_version"], directory)
            self.run_command([str(directory / "out/explicit_version")], directory)
            self.assertIn('"6.5.4"', (directory / "out/custom generated/version.h").read_text())

    def test_explicit_missing_feature_and_incomplete_version_inputs_fail(self):
        for setup, error in (
            ('set(ESY_FEATURE_IMPORT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/missing.cmake")',
             "ESY_FEATURE_IMPORT_FILE"),
            ('set(ESY_VERSION_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/version.in")\n'
             'set(GENERATED_INCLUDE_DIR "")', "GENERATED_INCLUDE_DIR"),
            ('set(ESY_VERSION_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/missing-version.in")',
             "ESY_VERSION_TEMPLATE does not exist"),
            ("unset(ESY_BOARD_PATH)", "ESY_BOARD_PATH"),
        ):
            with self.subTest(setup=setup), tempfile.TemporaryDirectory(prefix="esy missing inputs ") as tmp:
                directory = Path(tmp)
                source = self.prepare_pico_consumer(directory,
                    setup + f'\ninclude("{cmake_path(PROJECT_IMPORT)}")\n')
                self.configure(source, error)

    def test_pico_firmware_uses_the_explicit_target_and_slot_paths(self):
        for slot in (None, "a", "B"):
            with self.subTest(slot=slot), tempfile.TemporaryDirectory(prefix="esy firmware slots ") as tmp:
                directory = Path(tmp)
                linkers = directory / "linker scripts outside board"
                linkers.mkdir()
                for name in ("default", "A", "B"):
                    (linkers / f"{name}.ld").write_text("/* Consumer linker fixture. */\n")
                slot_input = f'set(ESY_APP_SLOT "{slot}")' if slot else ""
                source = self.prepare_pico_consumer(directory, f'''
include("{cmake_path(PROJECT_IMPORT)}")
add_executable(actual_image_name main.c)
add_executable(unrelated_target main.c)
add_library(esy_interrupts INTERFACE)
set(ESY_FIRMWARE_TARGET actual_image_name)
set(ESY_LINKER_SCRIPT_DEFAULT "{cmake_path(linkers / 'default.ld')}")
set(ESY_LINKER_SCRIPT_A "{cmake_path(linkers / 'A.ld')}")
set(ESY_LINKER_SCRIPT_B "{cmake_path(linkers / 'B.ld')}")
set(ENABLE_ESY_STDIO_UART ON)
set(ENABLE_ESY_STDIO_RTT OFF)
set(ESY_FIRMWARE_COMPILE_DEFINITIONS FIRMWARE_PRIVATE_SENTINEL=1)
{slot_input}
include("{cmake_path(FIRMWARE)}")
get_target_property(other_linker unrelated_target FIXTURE_LINKER_SCRIPT)
if(other_linker)
  message(FATAL_ERROR "Firmware settings leaked to an unrelated target")
endif()
get_target_property(firmware_definitions actual_image_name COMPILE_DEFINITIONS)
get_target_property(other_definitions unrelated_target COMPILE_DEFINITIONS)
if(NOT firmware_definitions MATCHES "FIRMWARE_PRIVATE_SENTINEL" OR
   other_definitions MATCHES "FIRMWARE_PRIVATE_SENTINEL")
  message(FATAL_ERROR "Firmware definitions were not confined to the requested executable")
endif()
file(GENERATE OUTPUT "${{CMAKE_BINARY_DIR}}/firmware.txt" CONTENT
  "$<TARGET_PROPERTY:actual_image_name,FIXTURE_LINKER_SCRIPT>\n$<TARGET_PROPERTY:actual_image_name,FIXTURE_EXTRA_OUTPUTS>\n$<TARGET_PROPERTY:actual_image_name,FIXTURE_STDIO_uart>\n$<TARGET_PROPERTY:actual_image_name,FIXTURE_STDIO_usb>\n$<TARGET_PROPERTY:actual_image_name,FIXTURE_STDIO_rtt>\n$<TARGET_PROPERTY:actual_image_name,LINK_LIBRARIES>\n")
''')
                self.configure(source)
                actual = (source / "out/firmware.txt").read_text().splitlines()
                expected = (slot.upper() if slot else "default") + ".ld"
                self.assertEqual(cmake_path(linkers / expected), actual[0])
                self.assertEqual(["enabled", "1", "0", "0"], actual[1:5])
                self.assertIn("pico_stdlib", actual[5])
                self.assertIn("esy_interrupts", actual[5])

    def test_firmware_rejects_missing_target_invalid_slot_and_missing_script(self):
        cases = (
            ("", "ESY_FIRMWARE_TARGET"),
            ("set(ESY_FIRMWARE_TARGET nonexistent)", "nonexistent"),
            ("add_executable(image main.c)\nset(ESY_FIRMWARE_TARGET image)\nset(ESY_APP_SLOT C)",
             "ESY_APP_SLOT"),
            ("add_executable(image main.c)\nset(ESY_FIRMWARE_TARGET image)\nset(ESY_APP_SLOT B)",
             "ESY_LINKER_SCRIPT_B"),
            ('add_executable(image main.c)\nset(ESY_FIRMWARE_TARGET image)\n'
             'set(ESY_LINKER_SCRIPT_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}")',
             "ESY_LINKER_SCRIPT_DEFAULT"),
            ("add_library(image STATIC main.c)\nset(ESY_FIRMWARE_TARGET image)",
             "executable"),
        )
        for setup, error in cases:
            with self.subTest(setup=setup), tempfile.TemporaryDirectory(prefix="esy rejected firmware ") as tmp:
                directory = Path(tmp)
                source = self.prepare_pico_consumer(directory, f'''
include("{cmake_path(PROJECT_IMPORT)}")
{setup}
include("{cmake_path(FIRMWARE)}")
''')
                self.configure(source, error)

    def test_stm32_firmware_configures_slot_outputs_on_the_requested_target(self):
        with tempfile.TemporaryDirectory(prefix="esy stm32 firmware ") as tmp:
            directory = Path(tmp)
            (directory / "main.c").write_text("int main(void) { return 0; }\n")
            linker = directory / "slot B supplied by consumer.ld"
            linker.write_text("/* Consumer linker fixture. */\n")
            toolchain = directory / "consumer native fixture toolchain.cmake"
            toolchain.write_text("set(CMAKE_SYSTEM_NAME Generic)\n")
            (directory / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.20)
set(ESY_TARGET_DEVICE STM32F405RG)
set(CMAKE_TOOLCHAIN_FILE "{cmake_path(toolchain)}")
include("{cmake_path(EARLY)}")
project(UnrelatedRootName C)
set(ESY_BOARD_PATH "${{CMAKE_CURRENT_SOURCE_DIR}}/unused consumer board")
include("{cmake_path(PROJECT_IMPORT)}")
add_executable(selected_image main.c)
set(ESY_FIRMWARE_TARGET selected_image)
set(ESY_APP_SLOT B)
set(ESY_LINKER_SCRIPT_B "{cmake_path(linker)}")
include("{cmake_path(FIRMWARE)}")
file(GENERATE OUTPUT "${{CMAKE_BINARY_DIR}}/firmware.txt" CONTENT
  "$<TARGET_PROPERTY:selected_image,LINK_OPTIONS>\n$<TARGET_PROPERTY:selected_image,LINK_DEPENDS>\n$<TARGET_PROPERTY:selected_image,ADDITIONAL_CLEAN_FILES>\n$<TARGET_PROPERTY:selected_image,SUFFIX>\n")
''')
            self.configure(directory)
            properties = (directory / "out/firmware.txt").read_text().splitlines()
            self.assertIn("-T" + cmake_path(linker), properties[0])
            self.assertEqual(cmake_path(linker), properties[1])
            self.assertIn("selected_image_slot_b.hex", properties[2])
            self.assertIn("selected_image_slot_b.bin", properties[2])
            self.assertEqual(".elf", properties[3])
            ninja = (directory / "out/build.ninja").read_text()
            self.assertIn("selected_image_slot_b.hex", ninja)
            self.assertIn("selected_image_slot_b.bin", ninja)
            self.assertIn("-O ihex", ninja)
            self.assertIn("-O binary", ninja)

    def test_stm32_toolchain_initializes_probe_abi_and_preserves_caller_flags(self):
        with tempfile.TemporaryDirectory(prefix="esy toolchain initialization ") as tmp:
            directory = Path(tmp)
            tools = directory / "consumer toolchain root/bin"
            tools.mkdir(parents=True)
            for name in ("gcc", "g++", "objcopy", "size"):
                tool = tools / f"arm-none-eabi-{name}"
                tool.write_text("#!/bin/sh\nexit 0\n")
                tool.chmod(0o755)
            script = directory / "toolchain contract.cmake"
            script.write_text(f'''
set(ESY_TOOLCHAIN_ROOT "{cmake_path(tools.parent)}")
foreach(group C CXX ASM EXE_LINKER)
  set(CMAKE_${{group}}_FLAGS_INIT "-DCONSUMER_SENTINEL=1")
endforeach()
include("{cmake_path(STM32_PRE_PROJECT)}")
if(NOT CMAKE_TOOLCHAIN_FILE STREQUAL "{cmake_path(STM32_PRE_PROJECT)}")
  message(FATAL_ERROR "Pre-project entry did not select the default toolchain")
endif()
foreach(required -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard)
  list(FIND ESY_PLATFORM_COMPILE_OPTIONS "${{required}}" compile_position)
  list(FIND ESY_PLATFORM_LINK_OPTIONS "${{required}}" link_position)
  if(compile_position LESS 0 OR link_position LESS 0)
    message(FATAL_ERROR "Platform targets lost an architecture or ABI option: ${{required}}")
  endif()
endforeach()
include("{cmake_path(STM32_PRE_PROJECT)}")
include("{cmake_path(STM32_PRE_PROJECT)}")
foreach(group C CXX ASM EXE_LINKER)
  set(flags "${{CMAKE_${{group}}_FLAGS_INIT}}")
  foreach(required -DCONSUMER_SENTINEL=1 -mcpu=cortex-m4 -mthumb
                   -mfpu=fpv4-sp-d16 -mfloat-abi=hard)
    string(FIND "${{flags}}" "${{required}}" position)
    if(position EQUAL -1)
      message(FATAL_ERROR "Missing preserved/probe ABI flag: ${{group}} ${{required}}")
    endif()
  endforeach()
  string(REGEX MATCHALL "-mcpu=cortex-m4" architecture_flags "${{flags}}")
  list(LENGTH architecture_flags count)
  if(NOT count EQUAL 1)
    message(FATAL_ERROR "Repeated toolchain inclusion duplicated architecture flags")
  endif()
endforeach()
set(propagated_variables "${{CMAKE_TRY_COMPILE_PLATFORM_VARIABLES}}")
list(FILTER propagated_variables INCLUDE REGEX "^ESY_TOOLCHAIN_ROOT$")
list(LENGTH propagated_variables count)
if(NOT count EQUAL 1)
  message(FATAL_ERROR "Repeated toolchain inclusion duplicated probe inputs")
endif()
''')
            self.run_command(["cmake", "-P", str(script)], directory)

    def test_stm32_pre_project_recognizes_relative_default_toolchain_paths(self):
        for relative_to in ("source", "build"):
            with self.subTest(relative_to=relative_to), \
                    tempfile.TemporaryDirectory(prefix="esy relative stm32 toolchain ") as tmp:
                directory = Path(tmp)
                binary = directory / "out"
                binary.mkdir()
                tools = directory / "toolchain/bin"
                tools.mkdir(parents=True)
                for name in ("gcc", "g++", "objcopy", "size"):
                    tool = tools / f"arm-none-eabi-{name}"
                    tool.write_text("#!/bin/sh\nexit 0\n")
                    tool.chmod(0o755)
                base = directory if relative_to == "source" else binary
                entry = cmake_path(os.path.relpath(STM32_PRE_PROJECT, base))
                script = directory / "relative-toolchain.cmake"
                script.write_text(f'''
set(CMAKE_SOURCE_DIR "{cmake_path(directory)}")
set(CMAKE_BINARY_DIR "{cmake_path(binary)}")
set(CMAKE_TOOLCHAIN_FILE "{entry}")
set(ESY_TOOLCHAIN_ROOT "{cmake_path(tools.parent)}")
include("{cmake_path(STM32_PRE_PROJECT)}")
if(NOT CMAKE_C_COMPILER STREQUAL "{cmake_path(tools / 'arm-none-eabi-gcc')}" OR
   NOT CMAKE_SYSTEM_NAME STREQUAL "Generic")
  message(FATAL_ERROR "Relative default toolchain was mistaken for a custom toolchain")
endif()
''')
                self.run_command(["cmake", "-P", str(script)], directory)

    def test_stm32_pre_project_preserves_an_explicit_toolchain_without_loading_it(self):
        with tempfile.TemporaryDirectory(prefix="esy explicit stm32 toolchain ") as tmp:
            directory = Path(tmp)
            toolchain = directory / "consumer toolchain.cmake"
            toolchain.write_text('message(FATAL_ERROR "Toolchain loaded before project()")\n')
            script = directory / "pre-project contract.cmake"
            script.write_text(f'''
set(CMAKE_TOOLCHAIN_FILE "{cmake_path(toolchain)}")
set(ESY_TOOLCHAIN_ROOT "{cmake_path(directory / 'unavailable compiler')}")
include("{cmake_path(STM32_PRE_PROJECT)}")
if(NOT CMAKE_TOOLCHAIN_FILE STREQUAL "{cmake_path(toolchain)}")
  message(FATAL_ERROR "Pre-project entry replaced the consumer toolchain")
endif()
foreach(required -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard)
  list(FIND ESY_PLATFORM_COMPILE_OPTIONS "${{required}}" compile_position)
  list(FIND ESY_PLATFORM_LINK_OPTIONS "${{required}}" link_position)
  if(compile_position LESS 0 OR link_position LESS 0)
    message(FATAL_ERROR "Custom toolchain lost platform architecture/ABI options")
  endif()
endforeach()
if(DEFINED CMAKE_C_COMPILER OR DEFINED CMAKE_C_FLAGS_INIT)
  message(FATAL_ERROR "Pre-project entry initialized a compiler instead of selecting it")
endif()
''')
            self.run_command(["cmake", "-P", str(script)], directory)

    @unittest.skipUnless(os.environ.get("ESY_TEST_ARM_TOOLCHAIN_ROOT") or
                         os.environ.get("ESY_TEST_PICO_TOOLCHAIN_PATH"),
                         "Set ESY_TEST_ARM_TOOLCHAIN_ROOT for the real STM32 compiler probe")
    def test_real_stm32_toolchain_probes_atomics_before_any_target_options(self):
        toolchain = Path(os.environ.get("ESY_TEST_ARM_TOOLCHAIN_ROOT") or
                         os.environ["ESY_TEST_PICO_TOOLCHAIN_PATH"]).resolve()
        with tempfile.TemporaryDirectory(prefix="esy real compiler probe ") as tmp:
            directory = Path(tmp)
            (directory / "probe.c").write_text("int abi_probe(void) { return 0; }\n")
            (directory / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.20)
set(ESY_TARGET_DEVICE STM32F405RG)
set(ESY_TOOLCHAIN_ROOT "{cmake_path(toolchain)}")
set(CMAKE_C_FLAGS_INIT -DCONSUMER_SENTINEL=1)
include("{cmake_path(EARLY)}")
project(stm32_compiler_probe C)
set(CMAKE_C_STANDARD 11)
include(CheckCSourceCompiles)
check_c_source_compiles([=[
#include <stdatomic.h>
#ifndef CONSUMER_SENTINEL
#error "Consumer initialization flags were lost"
#endif
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "Cortex-M4 native atomics required");
int main(void) {{ return 0; }}
]=] STM32_NATIVE_ATOMICS)
if(NOT STM32_NATIVE_ATOMICS)
  message(FATAL_ERROR "Capability probes did not use the selected Cortex-M4 architecture")
endif()
# No project/optimization target has been created to supply architecture flags.
add_library(abi_probe OBJECT probe.c)
''')
            self.configure(directory)
            self.configure(directory)
            self.run_command(["cmake", "--build", str(directory / "out")], directory)

    def test_esp_idf_owns_its_component_and_image_lifecycle(self):
        with tempfile.TemporaryDirectory(prefix="esy idf lifecycle ") as tmp:
            directory = Path(tmp)
            sdk = directory / "IDF provider outside application"
            (sdk / "tools/cmake").mkdir(parents=True)
            (sdk / "tools/cmake/project.cmake").write_text('''
if(CMAKE_C_COMPILER_LOADED)
  message(FATAL_ERROR "ESP-IDF import ran after project()")
endif()
function(idf_build_process)
endfunction()
''')
            (directory / "component.c").write_text("int component_probe(void) { return 0; }\n")
            (directory / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.20)
set(ESY_TARGET_DEVICE ESP32)
set(IDF_PATH "{cmake_path(sdk)}")
include("{cmake_path(EARLY)}")
project(consumer_with_idf C)
set(ESY_BOARD_PATH "${{CMAKE_CURRENT_SOURCE_DIR}}/consumer board")
include("{cmake_path(PROJECT_IMPORT)}")
add_library(idf_component STATIC component.c)
set(ESY_FIRMWARE_TARGET idf_component)
set(ESY_APP_SLOT B)
include("{cmake_path(FIRMWARE)}")
get_target_property(suffix idf_component SUFFIX)
get_target_property(linker idf_component LINK_DEPENDS)
if(suffix OR linker OR DEFINED ESY_LINK_SCRIPT)
  message(FATAL_ERROR "Generic firmware helper took over ESP-IDF image ownership")
endif()
''')
            self.configure(directory)

    def test_esp_component_conventions_and_overrides_apply_before_idf_import(self):
        for mode in ("default", "custom", "empty", "no_component"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix="esy idf component defaults ") as tmp:
                directory = Path(tmp)
                sdk = directory / "native IDF provider"
                (sdk / "tools/cmake").mkdir(parents=True)
                conventional = directory / "app/esp32"
                if mode != "no_component":
                    conventional.mkdir(parents=True)
                    (conventional / "CMakeLists.txt").write_text("# Consumer component fixture.\n")
                expected_directory = str(conventional) if mode == "default" else ""
                expected_components = "esp32" if mode == "default" else ""
                if mode in ("custom", "empty"):
                    expected_directory = str(directory / "custom component") if mode == "custom" else ""
                    expected_components = "consumer_component" if mode == "custom" else ""
                    (directory / "cmake").mkdir()
                    (directory / "cmake/esy_build_config.cmake").write_text(
                        f'set(EXTRA_COMPONENT_DIRS "{cmake_path(expected_directory)}")\n'
                        f'set(COMPONENTS "{expected_components}")\n')
                (sdk / "tools/cmake/project.cmake").write_text(f'''
if(NOT "${{EXTRA_COMPONENT_DIRS}}" STREQUAL "{cmake_path(expected_directory)}" OR
   NOT "${{COMPONENTS}}" STREQUAL "{expected_components}")
  message(FATAL_ERROR "ESP component defaults/overrides were applied after IDF import")
endif()
function(idf_build_process)
endfunction()
''')
                (directory / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.20)
set(ESY_TARGET_DEVICE ESP32)
set(IDF_PATH "{cmake_path(sdk)}")
include("{cmake_path(EARLY)}")
project(idf_component_defaults C)
include("{cmake_path(PROJECT_IMPORT)}")
''')
                self.configure(directory)

    @unittest.skipUnless(os.environ.get("ESY_TEST_PICO_SDK_PATH"),
                         "Set ESY_TEST_PICO_SDK_PATH for a real Pico SDK firmware build")
    def test_real_pico_sdk_build_uses_defaults_without_a_build_config(self):
        sdk = Path(os.environ["ESY_TEST_PICO_SDK_PATH"]).resolve()
        self.assertTrue((sdk / "external/pico_sdk_import.cmake").is_file())
        toolchain = os.environ.get("ESY_TEST_PICO_TOOLCHAIN_PATH")
        toolchain_input = (f'set(PICO_TOOLCHAIN_PATH "{cmake_path(Path(toolchain).resolve())}")'
                           if toolchain else "")
        with tempfile.TemporaryDirectory(prefix="esy real sdk consumer ") as tmp:
            directory = Path(tmp)
            (directory / "esy_board").mkdir()
            (directory / "esy_board/CMakeLists.txt").write_text(
                "add_library(esy_board INTERFACE)\n")
            (directory / "esy_feature_import.cmake").write_text("set(ESY_RTOS None)\n")
            (directory / "main.c").write_text('''
#include "version.h"
#include "esy_common/utils/integrity/esy_sha256.h"
_Static_assert(APP_VERSION_MAJOR == 1 && APP_VERSION_MINOR == 2 && APP_VERSION_PATCH == 3,
               "Expected application version components from the consumer project");
_Static_assert(APP_VERSION == 10203 && APP_VERSION * 2 == 20406,
               "Expected the library's default application version expression");
int main(void) {
  unsigned char digest[32];
  esy_sha256("", 0, digest);
  return APP_VERSION == 10203 && digest[0] == 0xe3u ? 0 : 1;
}
''')
            (directory / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.20)
set(ESY_TARGET_DEVICE PICO)
set(PICO_SDK_PATH "{cmake_path(sdk)}")
{toolchain_input}
include("{cmake_path(EARLY)}")
project(real_sdk_consumer VERSION 1.2.3 LANGUAGES C CXX ASM)
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT CMAKE_C_STANDARD EQUAL 11)
  message(FATAL_ERROR "Real SDK consumer did not receive build defaults")
endif()
add_subdirectory("{cmake_path(ROOT)}" platform)
if(NOT PICO_SDK OR NOT CMAKE_EXECUTABLE_SUFFIX STREQUAL ".elf")
  message(FATAL_ERROR "Pico SDK outputs did not reach the consumer directory")
endif()
add_executable(actual_image main.c)
target_link_libraries(actual_image PRIVATE esy_opt_default esy_hal esy_common_utils)
set(ESY_FIRMWARE_TARGET actual_image)
set(ESY_LINKER_SCRIPT_DEFAULT "${{PICO_LINKER_SCRIPT_PATH}}/memmap_default.ld")
include("{cmake_path(FIRMWARE)}")
''')
            self.run_command(["cmake", "-S", str(directory), "-B", str(directory / "out"),
                              "-G", "Ninja", "-DPICO_NO_PICOTOOL=1"], directory)
            self.run_command(["cmake", "--build", str(directory / "out")], directory)
            self.assertFalse((directory / "cmake/version.h.in").exists())
            self.assertTrue((directory / "out/generated/version.h").is_file())
            for extension in ("elf", "bin", "hex"):
                self.assertTrue((directory / f"out/actual_image.{extension}").is_file(),
                                f"Missing real SDK firmware output: {extension}")


if __name__ == "__main__":
    unittest.main()
