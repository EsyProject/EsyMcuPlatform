# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Check backend selection, cached inputs and provider dependency closure."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PROJECT = r'''cmake_minimum_required(VERSION 3.20)
project(atomic_consumer C)
set(CMAKE_C_STANDARD 11)
if(FORCE_NON_NATIVE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D__GCC_ATOMIC_INT_LOCK_FREE=1")
endif()
# Stale normal and cache values must not override the actual compiler probe.
set(ESY_NATIVE_ATOMIC_U32_LOCK_FREE ON)
set(ESY_NATIVE_ATOMIC_U32_LOCK_FREE ON CACHE INTERNAL "stale fixture")
foreach(target esy_opt_default esy_board pico_time pico_stdio)
    add_library(${target} INTERFACE)
endforeach()
if(PROVIDER)
    add_library(pico_atomic INTERFACE)
endif()
include("${ROOT}/cmake/esy_atomic_backend.cmake")
add_subdirectory("${ROOT}/esy_common" common)
add_subdirectory("${ROOT}/esy_hal_backend" driver)
get_target_property(definitions esy_common_defs INTERFACE_COMPILE_DEFINITIONS)
get_target_property(dependencies esy_hal_backend LINK_LIBRARIES)
get_target_property(sources esy_hal_backend SOURCES)
file(WRITE "${CMAKE_BINARY_DIR}/backend.txt"
    "${ESY_ATOMIC_IRQ_SPINLOCK_BACKEND}\n${definitions}\n${dependencies}\n${sources}\n")
'''


class AtomicBackendTests(unittest.TestCase):
    def configure(self, root, *, non_native=False, provider=False, platform="PICO"):
        return subprocess.run([
            "cmake", "-S", str(root), "-B", str(root / "out"),
            f"-DROOT={ROOT}", f"-DESY_TARGET_DEVICE={'STM32F407ZG' if platform == 'STM32F4' else platform}",
            f"-DFORCE_NON_NATIVE={'ON' if non_native else 'OFF'}",
            f"-DPROVIDER={'ON' if provider else 'OFF'}",
        ], capture_output=True, text=True, timeout=60)

    def project(self, path):
        (path / "CMakeLists.txt").write_text(PROJECT)

    def test_native_compiler_needs_no_sdk_provider(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.project(root)
            result = self.configure(root)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            metadata = (root / "out/backend.txt").read_text()
            self.assertTrue(metadata.startswith("OFF\n"))
            self.assertNotIn("ESY_ATOMIC_IRQ_SPINLOCK_BACKEND=1", metadata)
            self.assertNotIn("esy_hb_atomic.c", metadata)

    def test_non_native_requires_provider_even_with_stale_probe_results(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.project(root)
            result = self.configure(root, non_native=True)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("pico_atomic", result.stdout + result.stderr)

    def test_other_non_native_platform_cannot_use_pico_provider(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.project(root)
            result = self.configure(root, non_native=True, provider=True, platform="STM32F4")
            self.assertNotEqual(0, result.returncode)
            self.assertIn("pico_atomic", result.stdout + result.stderr)

    def test_rp2040_provider_reaches_driver_and_common_capability(self):
        for platform in ("PICO", "PICO_W"):
            with self.subTest(platform=platform), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                self.project(root)
                result = self.configure(root, non_native=True, provider=True, platform=platform)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                metadata = (root / "out/backend.txt").read_text()
                self.assertTrue(metadata.startswith("ON\n"))
                self.assertIn("ESY_ATOMIC_IRQ_SPINLOCK_BACKEND=1", metadata)
                self.assertIn("pico_atomic", metadata)
                self.assertIn("esy_hb_atomic.c", metadata)

    def test_cached_switch_back_to_native_removes_spinlock_capability(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.project(root)
            result = self.configure(root, non_native=True, provider=True)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            result = self.configure(root, platform="PICO2")
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            metadata = (root / "out/backend.txt").read_text()
            self.assertTrue(metadata.startswith("OFF\n"))
            self.assertNotIn("ESY_ATOMIC_IRQ_SPINLOCK_BACKEND=1", metadata)
            self.assertNotIn("esy_hb_atomic.c", metadata)


if __name__ == "__main__":
    unittest.main()
