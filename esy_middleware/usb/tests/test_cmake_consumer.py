# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Verify USB target propagation, feature combinations and registry bounds."""

from pathlib import Path
import itertools
import subprocess
import tempfile
import unittest

PLATFORM_ROOT = Path(__file__).resolve().parents[3]
CMAKE_PROJECT = """\
cmake_minimum_required(VERSION 3.20)
project(usb_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
include(esy_feature_import.cmake)
add_library(esy_opt_default INTERFACE)
target_compile_definitions(esy_opt_default INTERFACE PRIVATE_OPTIONS_SENTINEL)
add_subdirectory("${PLATFORM_ROOT}/esy_common" common)
add_library(esy_hal INTERFACE)
target_include_directories(esy_hal INTERFACE "${PLATFORM_ROOT}/esy_hal/include")
target_link_libraries(esy_hal INTERFACE esy_common_defs esy_common_device)
if(PROVIDE_TINYUSB)
    add_library(tinyusb_device STATIC
        "${PLATFORM_ROOT}/esy_test/fake/third_party/tinyusb/fake_tinyusb.c")
    target_include_directories(tinyusb_device PUBLIC
        "${PLATFORM_ROOT}/esy_test/fake/third_party/tinyusb")
endif()
add_subdirectory("${PLATFORM_ROOT}/esy_middleware" middleware)
if(EXPECT_USB)
    if(NOT TARGET esy_usb)
        message(FATAL_ERROR "Enabled USB target missing")
    endif()
    if(DEFINED REGISTRY_CAPACITY)
        target_compile_definitions(esy_usb PUBLIC
            ESY_USB_CDC_MAX_IFACES=${REGISTRY_CAPACITY})
    endif()
else()
    if(TARGET esy_usb OR TARGET esy_proto_bin_usb_channel)
        message(FATAL_ERROR "Disabled USB target or channel was assembled")
    endif()
endif()
if(TARGET esy_proto_bin_usb_channel)
    message(FATAL_ERROR "USB availability must not implicitly enable a protocol channel")
endif()
foreach(language IN ITEMS c cpp)
    add_executable(consumer_${language} consumer.${language})
    target_link_libraries(consumer_${language} PRIVATE ${CONSUMER_TARGET})
endforeach()
"""
CONSUMER = """\
#include "esy_middleware/usb/esy_usb_cdc.h"
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private build options leaked"
#endif
int main(void) {
    esy_usb_cdc_t cdc;
    esy_usb_cdc_config_t cfg;
    esy_usb_cdc_default_config(&cfg);
    if (esy_usb_cdc_init(&cdc, &cfg) != ESY_OK) return 1;
    if (esy_usb_cdc_init(&cdc, &cfg) != ESY_ERR_STATE) return 2;
    esy_usb_cdc_deinit(&cdc);
    cfg.itf = 1;
    esy_err_t expected = ESY_USB_CDC_MAX_IFACES == 1 ? ESY_ERR_INVALID_ARG : ESY_OK;
    esy_err_t result = esy_usb_cdc_init(&cdc, &cfg);
    esy_usb_cdc_deinit(&cdc);
    return result == expected ? 0 : 3;
}
"""


class UsbConsumerTests(unittest.TestCase):
    def check_configuration(self, use=True, cdc=True, backend=True, provider=True,
                            protocol=False, capacity=None, target="esy_middleware",
                            configure_error=None, build_error=None):
        enabled = use and cdc and backend
        with tempfile.TemporaryDirectory(prefix="usb consumer ") as directory:
            project = Path(directory)
            (project / "CMakeLists.txt").write_text(CMAKE_PROJECT)
            values = {
                "ENABLE_ESY_USB": use, "ENABLE_ESY_USB_CDC": cdc,
                "ENABLE_ESY_USB_BACKEND_TINYUSB": backend,
                "PROVIDE_TINYUSB": provider, "EXPECT_USB": enabled,
                "ENABLE_ESY_PROTO_BIN": protocol, "ENABLE_ESY_PROTO_MODBUS": False,
                "ENABLE_ESY_PROTO_BIN_USB_CHANNEL": False, "ENABLE_ESY_COMMON_DS": protocol,
                "ENABLE_ESY_COMMON_PROTOCOL": protocol,
                "ENABLE_ESY_SYSTEM": protocol,
            }
            features = "".join(f"set({key} {'ON' if value else 'OFF'})\n"
                               for key, value in values.items())
            features += f"set(CONSUMER_TARGET {target})\n"
            if capacity is not None:
                features += f"set(REGISTRY_CAPACITY {capacity})\n"
            (project / "esy_feature_import.cmake").write_text(features)
            for suffix in ("c", "cpp"):
                (project / f"consumer.{suffix}").write_text(
                    CONSUMER if enabled else "int main(void) { return 0; }\n")
            result = subprocess.run(
                ["cmake", "-S", str(project), "-B", str(project / "out"),
                 f"-DPLATFORM_ROOT={PLATFORM_ROOT}"],
                capture_output=True, text=True, timeout=60)
            output = result.stdout + result.stderr
            if configure_error:
                self.assertNotEqual(0, result.returncode, output)
                self.assertIn(configure_error, " ".join(output.split()))
                return
            self.assertEqual(0, result.returncode, output)
            result = subprocess.run(
                ["cmake", "--build", str(project / "out")],
                capture_output=True, text=True, timeout=60)
            output = result.stdout + result.stderr
            if build_error:
                self.assertNotEqual(0, result.returncode, output)
                self.assertIn(build_error, output)
                return
            self.assertEqual(0, result.returncode, output)
            for suffix in ("c", "cpp"):
                result = subprocess.run([str(project / "out" / f"consumer_{suffix}")],
                                        capture_output=True, text=True, timeout=5)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_feature_matrix(self):
        for use, cdc, backend in itertools.product((False, True), repeat=3):
            error = None
            if use and cdc != backend:
                error = "requires ENABLE_ESY_USB_CDC" if backend else "requires a CDC backend"
            with self.subTest(use=use, cdc=cdc, backend=backend):
                self.check_configuration(use, cdc, backend, provider=use and backend,
                                         configure_error=error)

    def test_direct_public_target(self):
        self.check_configuration(target="esy_usb")

    def test_disabled_cdc_does_not_assemble_protocol_usb_channel(self):
        self.check_configuration(cdc=False, backend=False, provider=False, protocol=True)

    def test_enabled_usb_does_not_implicitly_assemble_protocol_channel(self):
        self.check_configuration(protocol=True)

    def test_missing_provider_has_configure_diagnostic(self):
        self.check_configuration(provider=False, configure_error="consumer-provided tinyusb_device")

    def test_registry_capacity_boundaries(self):
        for capacity in (1, 255):
            with self.subTest(capacity=capacity):
                self.check_configuration(capacity=capacity)
        for capacity in (0, 256):
            with self.subTest(capacity=capacity):
                self.check_configuration(capacity=capacity,
                                         build_error="ESY_USB_CDC_MAX_IFACES must be in 1..255")


if __name__ == "__main__":
    unittest.main()
