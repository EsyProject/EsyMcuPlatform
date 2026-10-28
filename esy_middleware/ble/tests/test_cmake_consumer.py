# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Check BLE features, core requirements and optional real BTstack compilation."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
BTSTACK_PATH = None
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(ble_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(esy_feature_import.cmake)
add_library(esy_opt_default INTERFACE)
target_compile_options(esy_opt_default INTERFACE -Wall -Wextra -Werror)
target_compile_definitions(esy_opt_default INTERFACE BLE_PRIVATE_OPTIONS_SENTINEL)
add_subdirectory("${PLATFORM_ROOT}/esy_common" common)
if(PROVIDE_BTSTACK)
    add_library(esy_ble_btstack_provider INTERFACE)
    target_compile_definitions(esy_ble_btstack_provider INTERFACE
        BLE_PROVIDER_PRIVATE_SENTINEL)
    if(ESY_BTSTACK_PATH)
        target_include_directories(esy_ble_btstack_provider SYSTEM INTERFACE
            "${CMAKE_CURRENT_SOURCE_DIR}/btstack_config"
            "${ESY_BTSTACK_PATH}/src"
            "${ESY_BTSTACK_PATH}/src/ble"
            "${ESY_BTSTACK_PATH}/platform/embedded"
            "${PLATFORM_ROOT}/esy_middleware/ble/tests/btstack")
    endif()
endif()
add_subdirectory("${PLATFORM_ROOT}/esy_middleware" middleware)
if(TARGET esy_hal OR TARGET esy_os)
    message(FATAL_ERROR "BLE core introduced HAL or OS target dependencies")
endif()
if(ENABLE_ESY_BLE)
    if(NOT TARGET esy_ble OR NOT TARGET Esy::ble)
        message(FATAL_ERROR "Enabled BLE core target or public alias is missing")
    endif()
    get_target_property(core_links esy_ble LINK_LIBRARIES)
    if(core_links MATCHES "esy_os|esy_hal|esy_ble_btstack_provider")
        message(FATAL_ERROR "BLE core has an unexpected runtime dependency")
    endif()
    get_target_property(aggregate_links esy_middleware INTERFACE_LINK_LIBRARIES)
    if(NOT "esy_ble" IN_LIST aggregate_links)
        message(FATAL_ERROR "Middleware aggregate omitted the enabled BLE core")
    endif()
    foreach(language c cpp)
        add_library(header_${language} OBJECT header.${language})
        target_link_libraries(header_${language} PRIVATE Esy::ble)
        target_compile_options(header_${language} PRIVATE -Wall -Wextra -Werror)
        add_executable(consumer_${language} consumer.${language})
        target_link_libraries(consumer_${language} PRIVATE Esy::ble)
    endforeach()
    if(ENABLE_ESY_BLE_BTSTACK)
        if(NOT TARGET esy_ble_btstack OR NOT TARGET Esy::ble_btstack)
            message(FATAL_ERROR "Enabled BTstack adapter target or alias is missing")
        endif()
        get_target_property(adapter_links esy_ble_btstack INTERFACE_LINK_LIBRARIES)
        if(NOT "esy_ble" IN_LIST adapter_links OR
           "esy_ble_btstack_provider" IN_LIST adapter_links OR
           NOT "$<LINK_ONLY:esy_ble_btstack_provider>" IN_LIST adapter_links)
            message(FATAL_ERROR "BTstack public core/private provider linkage is invalid")
        endif()
        if(NOT "esy_ble_btstack" IN_LIST aggregate_links)
            message(FATAL_ERROR "Middleware aggregate omitted the enabled BTstack adapter")
        endif()
        foreach(language c cpp)
            add_library(adapter_header_${language} OBJECT adapter_header.${language})
            target_link_libraries(adapter_header_${language} PRIVATE Esy::ble_btstack)
            target_compile_options(adapter_header_${language} PRIVATE -Wall -Wextra -Werror)
        endforeach()
    elseif(TARGET esy_ble_btstack OR TARGET Esy::ble_btstack)
        message(FATAL_ERROR "Disabled BTstack adapter leaked a target")
    endif()
elseif(TARGET esy_ble OR TARGET Esy::ble OR
       TARGET esy_ble_btstack OR TARGET Esy::ble_btstack)
    message(FATAL_ERROR "Disabled BLE module leaked a target")
endif()
'''
HEADER = r'''
#include "esy_middleware/ble/esy_ble.h"
#ifdef BLE_PRIVATE_OPTIONS_SENTINEL
#error "Private BLE build options leaked to the consumer"
#endif
#ifdef BLE_PROVIDER_PRIVATE_SENTINEL
#error "Private BTstack provider options leaked to the consumer"
#endif
'''
CONSUMER = HEADER + r'''
int main(void) {
    if (esy_ble_init(NULL, NULL, NULL, NULL) != ESY_ERR_INVALID_ARG) return 1;
    return esy_ble_deinit(NULL) == ESY_ERR_INVALID_ARG ? 0 : 2;
}
'''


class BleConsumerTests(unittest.TestCase):
    def check_configuration(self, enabled, adapter=False, provider=False,
                            stack_path=None, error=None, native_config=None,
                            build_error=None):
        with tempfile.TemporaryDirectory(prefix="ble consumer ") as tmp:
            directory = Path(tmp)
            (directory / "CMakeLists.txt").write_text(PROJECT)
            if stack_path:
                configuration = directory / "btstack_config"
                configuration.mkdir()
                if native_config is None:
                    native_config = (
                        ROOT / "esy_middleware/ble/tests/btstack/btstack_config.h"
                    ).read_text()
                (configuration / "btstack_config.h").write_text(native_config)
            (directory / "esy_feature_import.cmake").write_text(
                "set(ESY_RTOS None)\n"
                f"set(ENABLE_ESY_BLE {'ON' if enabled else 'OFF'})\n"
                f"set(ENABLE_ESY_BLE_BTSTACK {'ON' if adapter else 'OFF'})\n")
            for language in ("c", "cpp"):
                (directory / f"header.{language}").write_text(HEADER)
                (directory / f"consumer.{language}").write_text(CONSUMER)
                (directory / f"adapter_header.{language}").write_text(
                    HEADER + '#include "esy_middleware/ble/esy_ble_btstack.h"\n')
            command = ["cmake", "-S", str(directory), "-B", str(directory / "out"),
                       "-G", "Ninja", f"-DPLATFORM_ROOT={ROOT}",
                       f"-DPROVIDE_BTSTACK={'ON' if provider else 'OFF'}"]
            if stack_path:
                command.append(f"-DESY_BTSTACK_PATH={stack_path}")
            result = subprocess.run(command, capture_output=True, text=True)
            output = result.stdout + result.stderr
            if error:
                self.assertNotEqual(result.returncode, 0, output)
                self.assertIn(error, output)
                return
            self.assertEqual(result.returncode, 0, output)
            if adapter and stack_path is None:
                # No native implementation is compiled for this target fixture.
                return
            commands = [["cmake", "--build", str(directory / "out")]]
            if build_error:
                result = subprocess.run(commands[0], capture_output=True, text=True)
                output = result.stdout + result.stderr
                self.assertNotEqual(result.returncode, 0, output)
                self.assertIn(build_error, output)
                return
            if enabled:
                commands.extend([str(directory / f"out/consumer_{language}")]
                                for language in ("c", "cpp"))
            for command in commands:
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_disabled_module_omits_ble_target(self):
        for provider in (False, True):
            with self.subTest(provider=provider):
                self.check_configuration(enabled=False, provider=provider)

    def test_core_without_os_hal_or_native_stack_requirements(self):
        for provider in (False, True):
            with self.subTest(provider=provider):
                self.check_configuration(enabled=True, provider=provider)

    def test_btstack_adapter_requires_parent_module(self):
        self.check_configuration(enabled=False, adapter=True,
                                 error="ENABLE_ESY_BLE_BTSTACK requires ENABLE_ESY_BLE")

    def test_btstack_adapter_requires_consumer_provider(self):
        self.check_configuration(enabled=True, adapter=True,
                                 error="requires consumer target esy_ble_btstack_provider")

    def test_btstack_adapter_target_contract(self):
        self.check_configuration(enabled=True, adapter=True, provider=True)

    def test_btstack_adapter_compiles_against_explicit_real_headers(self):
        if BTSTACK_PATH is None:
            self.skipTest("set ESY_BTSTACK_PATH or --btstack-path for real BTstack headers")
        self.assertTrue((BTSTACK_PATH / "src/btstack_version.h").is_file(),
                        "Explicit BTstack path must contain real source headers")
        self.check_configuration(enabled=True, adapter=True, provider=True,
                                 stack_path=BTSTACK_PATH)

    def test_btstack_adapter_rejects_native_ccc_persistence(self):
        if BTSTACK_PATH is None:
            self.skipTest("set ESY_BTSTACK_PATH or --btstack-path for real BTstack headers")
        configuration = (
            ROOT / "esy_middleware/ble/tests/btstack/btstack_config.h"
        ).read_text()
        for setting in ("", "#define NVN_NUM_GATT_SERVER_CCC 20"):
            with self.subTest(setting=setting):
                self.check_configuration(
                    enabled=True, adapter=True, provider=True, stack_path=BTSTACK_PATH,
                    native_config=configuration.replace("#define NVN_NUM_GATT_SERVER_CCC 0", setting),
                    build_error="NVN_NUM_GATT_SERVER_CCC=0")

    def test_btstack_adapter_rejects_eatt_bearer_configuration(self):
        if BTSTACK_PATH is None:
            self.skipTest("set ESY_BTSTACK_PATH or --btstack-path for real BTstack headers")
        configuration = (
            ROOT / "esy_middleware/ble/tests/btstack/btstack_config.h"
        ).read_text()
        self.check_configuration(
            enabled=True, adapter=True, provider=True, stack_path=BTSTACK_PATH,
            native_config=configuration + "\n#define ENABLE_GATT_OVER_EATT\n",
            build_error="does not support EATT bearers")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--btstack-path", default=os.environ.get("ESY_BTSTACK_PATH"))
    arguments, remaining = parser.parse_known_args()
    if arguments.btstack_path:
        BTSTACK_PATH = Path(arguments.btstack_path).resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
