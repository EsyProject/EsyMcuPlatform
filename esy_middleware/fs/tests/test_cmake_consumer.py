# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0
"""Build public C11/C++17 filesystem consumers without an OS or vendor SDK."""
from pathlib import Path
import json
import os
import runpy
import shlex
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
cached_provider = runpy.run_path(
    ROOT / "esy_test/utils/esy_provider_paths.py")["cached_provider"]
PROVIDER_NAMES = {"ESY_LITTLEFS_PATH": "littlefs", "ESY_FATFS_PATH": "fatfs"}
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(filesystem_consumer C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra -Werror -pedantic)
include(features.cmake)
add_library(esy_opt_default INTERFACE)
target_compile_options(esy_opt_default INTERFACE -DESY_FS_PLATFORM_OPTION=1)
add_subdirectory("${ROOT}/esy_common" common)
if(ENABLE_ESY_FLASH)
    add_library(esy_hal STATIC "${ROOT}/esy_test/fake/hal/fake_esy_hal_flash.c")
    target_include_directories(esy_hal PUBLIC "${ROOT}/esy_hal/include" "${ROOT}/esy_test/fake/hal")
    target_link_libraries(esy_hal PUBLIC esy_common_defs)
endif()
add_subdirectory("${ROOT}/esy_middleware" middleware)
foreach(forbidden IN ITEMS esy_os esy_hal_backend esy_board esy_settings esy_coredump_core)
    if(TARGET ${forbidden})
        message(FATAL_ERROR "Unexpected dependency: ${forbidden}")
    endif()
endforeach()
set(consumer_targets)
foreach(suffix IN ITEMS "" _littlefs _fatfs _flash)
    if(TARGET esy_fs${suffix})
        list(APPEND consumer_targets esy_fs${suffix})
    endif()
endforeach()
if(NOT ENABLE_ESY_FS)
    foreach(forbidden IN ITEMS esy_fs esy_fs_littlefs esy_fs_littlefs_provider esy_fs_fatfs esy_fs_fatfs_provider esy_fs_flash)
        if(TARGET ${forbidden})
            message(FATAL_ERROR "Disabled filesystem unexpectedly created: ${forbidden}")
        endif()
    endforeach()
    return()
endif()
if(NOT TARGET esy_fs)
    message(FATAL_ERROR "Core filesystem target missing")
endif()
foreach(suffix IN ITEMS LITTLEFS FATFS FLASH)
    if(ENABLE_ESY_FS_${suffix})
        add_compile_definitions(TEST_${suffix})
    endif()
endforeach()
if(ENABLE_ESY_FILE_FS)
    list(APPEND consumer_targets esy_file_fs)
    add_compile_definitions(TEST_FILE_FS)
endif()
foreach(language IN ITEMS c cpp)
    add_executable(consumer_${language} consumer.${language})
    target_link_libraries(consumer_${language} PRIVATE ${consumer_targets})
endforeach()
'''
SOURCE = r'''
#include "esy_middleware/fs/esy_fs.h"
#ifdef TEST_LITTLEFS
#include "esy_middleware/fs/esy_fs_littlefs.h"
#endif
#ifdef TEST_FATFS
#include "esy_middleware/fs/esy_fs_fatfs.h"
#include "esy_middleware/fs/esy_fs_block.h"
#endif
#ifdef TEST_FLASH
#include "esy_middleware/fs/esy_fs_flash.h"
#endif
#ifdef TEST_FILE_FS
#include "esy_middleware/file/esy_file_fs.h"
#endif
int main(void) {
    esy_fs_t volume = INITIALIZER;
    if (esy_fs_deinit(&volume) != ESY_OK) return 1;
#ifdef TEST_LITTLEFS
    esy_fs_littlefs_t littlefs = INITIALIZER;
    if (esy_fs_littlefs_deinit(&littlefs) != ESY_OK) return 2;
#endif
#ifdef TEST_FATFS
    esy_fs_fatfs_t fatfs = INITIALIZER;
    if (esy_fs_fatfs_deinit(&fatfs) != ESY_OK) return 3;
#endif
#ifdef TEST_FLASH
    if (esy_fs_flash_init(NULL, NULL) != ESY_ERR_INVALID_ARG) return 4;
#endif
#ifdef TEST_FILE_FS
    esy_file_fs_t binding = INITIALIZER;
    if (esy_file_fs_deinit(&binding) != ESY_OK) return 5;
#endif
    return 0;
}
'''


class FsConsumerTests(unittest.TestCase):
    def command(self, *args, error=None, environment=None):
        result = subprocess.run(args, capture_output=True, text=True, env=environment)
        output = result.stdout + result.stderr
        if error is None:
            self.assertEqual(0, result.returncode, output)
        else:
            self.assertNotEqual(0, result.returncode, output)
            self.assertIn(error, output)

    def prepare(self, path, *, enabled=True, littlefs=False, fatfs=False,
                flash=False, file_fs=False, flash_provider=True):
        (path / "CMakeLists.txt").write_text(PROJECT)
        flags = {"ENABLE_ESY_FS": enabled,
                 "ENABLE_ESY_FS_LITTLEFS": littlefs,
                 "ENABLE_ESY_FS_FATFS": fatfs,
                 "ENABLE_ESY_FS_FLASH": flash,
                 "ENABLE_ESY_FILE": file_fs,
                 "ENABLE_ESY_FILE_FS": file_fs,
                 "ENABLE_ESY_FLASH": flash and flash_provider}
        (path / "features.cmake").write_text("set(ESY_RTOS None)\n" + "".join(
            f"set({name} {'ON' if value else 'OFF'})\n" for name, value in flags.items()))
        for suffix in ("c", "cpp"):
            initializer = "{}" if suffix == "cpp" else "{0}"
            (path / f"consumer.{suffix}").write_text(SOURCE.replace("INITIALIZER", initializer))

    def configure(self, path, *, error=None, providers=True):
        args = ["cmake", "-S", str(path), "-B", str(path / "out"), f"-DROOT={ROOT}"]
        if providers:
            for variable, provider_name in PROVIDER_NAMES.items():
                provider = cached_provider(provider_name)
                if provider is not None:
                    args.append(f"-D{variable}={provider}")
        environment = dict(os.environ, ESY_PROVIDER_CACHE_DIR=str(path / "empty provider cache"))
        for variable in PROVIDER_NAMES:
            environment.pop(variable, None)
        self.command(*args, error=error, environment=environment)

    def require_providers(self, *names):
        for variable in names:
            if not cached_provider(PROVIDER_NAMES[variable]):
                self.skipTest(f"Run ./setup.sh or set {variable} for the upstream provider")

    def run_case(self, **options):
        with tempfile.TemporaryDirectory(prefix="filesystem consumer ") as directory:
            path = Path(directory)
            self.prepare(path, **options)
            self.configure(path)
            self.command("cmake", "--build", str(path / "out"), "--parallel", "2")
            if options.get("enabled", True):
                for suffix in ("c", "cpp"):
                    self.command(str(path / "out" / f"consumer_{suffix}"))

    def test_core_headers_and_linkage_have_no_os_or_hal_dependency(self):
        self.run_case()

    def test_disabled_filesystem_does_not_read_provider_inputs_or_create_targets(self):
        with mock.patch.dict(os.environ, {"ESY_LITTLEFS_PATH": "/missing littlefs provider",
                                         "ESY_FATFS_PATH": "/missing fatfs provider"}):
            self.run_case(enabled=False)

    def test_bound_file_adapter_requires_no_filesystem_backend(self):
        self.run_case(file_fs=True)

    def test_littlefs_public_headers_and_linkage(self):
        self.require_providers("ESY_LITTLEFS_PATH")
        self.run_case(littlefs=True)

    def test_fatfs_public_headers_and_linkage(self):
        self.require_providers("ESY_FATFS_PATH")
        self.run_case(fatfs=True)

    def test_both_backends_and_file_binding_can_be_linked_together(self):
        self.require_providers("ESY_LITTLEFS_PATH", "ESY_FATFS_PATH")
        self.run_case(littlefs=True, fatfs=True, flash=True, file_fs=True)

    def test_native_providers_inherit_consumer_compile_options_privately(self):
        self.require_providers("ESY_LITTLEFS_PATH", "ESY_FATFS_PATH")
        with tempfile.TemporaryDirectory(prefix="filesystem provider options ") as directory:
            path = Path(directory)
            self.prepare(path, littlefs=True, fatfs=True)
            self.configure(path)
            commands = json.loads((path / "out" / "compile_commands.json").read_text())
            provider_sources = {"lfs.c", "lfs_util.c", "ff.c", "ffunicode.c"}
            providers = [entry for entry in commands if Path(entry["file"]).name in provider_sources]
            self.assertEqual(provider_sources, {Path(entry["file"]).name for entry in providers})
            for entry in providers:
                with self.subTest(source=entry["file"]):
                    self.assertIn("-DESY_FS_PLATFORM_OPTION=1", shlex.split(entry["command"]))
            consumers = [entry for entry in commands
                         if Path(entry["file"]).name in {"consumer.c", "consumer.cpp"}]
            self.assertEqual(2, len(consumers))
            for entry in consumers:
                with self.subTest(consumer=entry["file"]):
                    self.assertNotIn("-DESY_FS_PLATFORM_OPTION=1", shlex.split(entry["command"]))

    def test_backend_children_require_the_filesystem_parent(self):
        for child in ("ENABLE_ESY_FS_LITTLEFS", "ENABLE_ESY_FS_FATFS", "ENABLE_ESY_FS_FLASH"):
            with self.subTest(child=child), tempfile.TemporaryDirectory(prefix="filesystem missing parent ") as directory:
                path = Path(directory)
                self.prepare(path, enabled=False)
                with (path / "features.cmake").open("a") as feature_file:
                    feature_file.write(f"set({child} ON)\n")
                self.configure(path, error=f"{child} requires ENABLE_ESY_FS")

    def test_backend_features_require_explicit_provider_paths(self):
        for name, options in (("LITTLEFS", {"littlefs": True}), ("FATFS", {"fatfs": True})):
            with self.subTest(backend=name), tempfile.TemporaryDirectory(prefix="filesystem missing provider ") as directory:
                path = Path(directory)
                self.prepare(path, **options)
                self.configure(path, providers=False,
                               error=f"ENABLE_ESY_FS_{name} requires ESY_{name}_PATH")

    def test_flash_adapter_requires_littlefs(self):
        with tempfile.TemporaryDirectory(prefix="filesystem flash missing backend ") as directory:
            path = Path(directory)
            self.prepare(path, flash=True)
            self.configure(path, error="ENABLE_ESY_FS_FLASH requires ENABLE_ESY_FS_LITTLEFS")

    def test_flash_adapter_requires_public_hal(self):
        self.require_providers("ESY_LITTLEFS_PATH")
        with tempfile.TemporaryDirectory(prefix="filesystem flash missing hal ") as directory:
            path = Path(directory)
            self.prepare(path, littlefs=True, flash=True, flash_provider=False)
            self.configure(path, error="ENABLE_ESY_FS_FLASH requires")

    def test_file_binding_requires_the_file_parent_and_filesystem(self):
        for disabled in ("ENABLE_ESY_FILE", "ENABLE_ESY_FS"):
            with self.subTest(disabled=disabled), tempfile.TemporaryDirectory(prefix="filesystem invalid binding ") as directory:
                path = Path(directory)
                self.prepare(path, file_fs=True)
                with (path / "features.cmake").open("a") as feature_file:
                    feature_file.write(f"set({disabled} OFF)\n")
                self.configure(path, error="ENABLE_ESY_FILE_FS requires")


if __name__ == "__main__":
    unittest.main()
