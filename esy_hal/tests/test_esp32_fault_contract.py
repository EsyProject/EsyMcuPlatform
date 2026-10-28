# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Validate native ESP32 fault assembly inputs without emulating its backend."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class Esp32FaultAssemblyContract(unittest.TestCase):
    def configure(self, *, enabled=True, classic=True, iram=True, lto=False,
                  providers=("esp_system", "esp_hw_support", "xtensa"), native=False):
        if not shutil.which("cmake") or not shutil.which("ninja"):
            self.skipTest("CMake/Ninja unavailable")
        temporary = tempfile.TemporaryDirectory(prefix="esp32-fault-contract-")
        self.addCleanup(temporary.cleanup)
        project = Path(temporary.name)
        (project / "dummy.c").write_text("int main(void) { return 0; }\n")
        native_targets = "\n".join(f"add_library(idf::{name} INTERFACE IMPORTED)" for name in providers)
        native_api = """function(idf_build_set_property property value)
    if(NOT property STREQUAL "LINK_OPTIONS")
        message(FATAL_ERROR "Unexpected native build property")
    endif()
    set_property(GLOBAL APPEND PROPERTY native_link_options "${value}")
endfunction()
""" if native else ""
        (project / "CMakeLists.txt").write_text(f"""cmake_minimum_required(VERSION {'3.16' if native else '3.20'})
project(esp32_fault_contract LANGUAGES C)
add_library(esy_board INTERFACE)
add_library(esy_opt_default INTERFACE)
add_library(esy_common INTERFACE)
{native_targets}
{native_api}
add_library(esy_hal_backend STATIC dummy.c)
set(ENABLE_ESY_FAULT {'ON' if enabled else 'OFF'})
set(CONFIG_IDF_TARGET_ESP32 {'ON' if classic else 'OFF'})
set(CONFIG_ESP_PANIC_HANDLER_IRAM {'ON' if iram else 'OFF'})
set(CONFIG_COMPILER_LTO {'ON' if lto else 'OFF'})
add_subdirectory("{ROOT / 'esy_hal_backend/src/esp32'}" backend)
add_library(hal_facade STATIC dummy.c)
target_link_libraries(hal_facade PRIVATE esy_hal_backend)
add_executable(consumer dummy.c)
target_link_libraries(consumer PRIVATE hal_facade)
get_property(native_options GLOBAL PROPERTY native_link_options)
target_link_options(consumer PRIVATE ${{native_options}})
get_target_property(src esy_hal_backend SOURCES)
get_target_property(deps esy_hal_backend LINK_LIBRARIES)
file(WRITE "${{CMAKE_BINARY_DIR}}/assembly.txt" "${{src}}\n${{deps}}\n")
""")
        result = subprocess.run(
            ["cmake", "-S", str(project), "-B", str(project / "build"), "-G", "Ninja"],
            capture_output=True, text=True,
        )
        return result, project / "build"

    def test_enabled_fault_adds_native_provider_deps_and_transitive_panic_wrap(self):
        result, build = self.configure()
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        assembly = (build / "assembly.txt").read_text()
        self.assertIn("esy_hb_fault.c", assembly)
        for name in ("esp_system", "esp_hw_support", "xtensa"):
            self.assertIn(f"idf::{name}", assembly)
        ninja = (build / "build.ninja").read_text()
        consumer = ninja[ninja.index("build consumer:"):]
        self.assertIn("-Wl,--wrap=esp_panic_handler", consumer)
        self.assertIn("-Wl,--undefined=__wrap_esp_panic_handler", consumer)

    def test_disabled_fault_adds_no_provider_or_wrap_requirements(self):
        for native in (False, True):
            with self.subTest(native=native):
                result, build = self.configure(enabled=False, classic=False, iram=False,
                                               providers=(), native=native)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                self.assertNotIn("esy_hb_fault.c", (build / "assembly.txt").read_text())
                self.assertNotIn("--wrap=esp_panic_handler", (build / "build.ninja").read_text())

    def test_native_final_application_gets_wrap_with_older_cmake_policy(self):
        result, build = self.configure(native=True)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        ninja = (build / "build.ninja").read_text()
        consumer = ninja[ninja.index("build consumer:"):]
        self.assertIn("-Wl,--wrap=esp_panic_handler", consumer)
        self.assertIn("-Wl,--undefined=__wrap_esp_panic_handler", consumer)

    def test_nonclassic_target_is_rejected(self):
        result, _ = self.configure(classic=False)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("classic CONFIG_IDF_TARGET_ESP32", result.stdout + result.stderr)

    def test_flash_resident_native_panic_is_rejected(self):
        result, _ = self.configure(iram=False)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("CONFIG_ESP_PANIC_HANDLER_IRAM=y", result.stdout + result.stderr)

    def test_lto_that_can_bypass_link_wrap_is_rejected(self):
        result, _ = self.configure(lto=True)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("CONFIG_COMPILER_LTO=n", result.stdout + result.stderr)

    def test_each_missing_sdk_provider_is_rejected_before_source_assembly(self):
        providers = ("esp_system", "esp_hw_support", "xtensa")
        for missing in providers:
            with self.subTest(provider=missing):
                result, _ = self.configure(providers=tuple(name for name in providers if name != missing))
                self.assertNotEqual(0, result.returncode)
                self.assertIn(f"native idf::{missing}", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
