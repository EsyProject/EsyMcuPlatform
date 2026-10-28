# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Exercise coredump C11/C++ consumers and optional dependency isolation."""

import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(coredump_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(esy_feature_import.cmake)
add_library(esy_opt_default INTERFACE)
target_compile_definitions(esy_opt_default INTERFACE PRIVATE_OPTIONS_SENTINEL)
add_subdirectory("${ROOT}/esy_common" common)
if(PROBE_WITH_HAL)
  add_library(esy_hal STATIC "${ROOT}/esy_test/fake/hal/fake_esy_hal_flash.c")
  target_include_directories(esy_hal PUBLIC "${ROOT}/esy_hal/include"
    PRIVATE "${ROOT}/esy_test/fake/hal")
  target_link_libraries(esy_hal PUBLIC esy_common_defs)
endif()
add_subdirectory("${ROOT}/esy_middleware" middleware)
if(TARGET esy_os)
  message(FATAL_ERROR "Coredump unexpectedly assembled an OS dependency")
endif()
if(ENABLE_ESY_COREDUMP)
  if(NOT TARGET esy_coredump_core)
    message(FATAL_ERROR "Enabled coredump has no core target")
  endif()
  get_target_property(private_options esy_coredump_core COMPILE_OPTIONS)
  get_target_property(public_options esy_coredump_core INTERFACE_COMPILE_OPTIONS)
  file(WRITE "${CMAKE_BINARY_DIR}/coredump-options.txt"
    "${CMAKE_C_COMPILER_ID}\n${private_options}\n${public_options}\n")
  add_library(public_headers OBJECT public.c public.cpp)
  target_link_libraries(public_headers PRIVATE esy_coredump_core)
  add_executable(consumer main.c main.cpp)
  target_link_libraries(consumer PRIVATE esy_middleware)
  foreach(adapter IN ITEMS logger file)
    string(TOUPPER "${adapter}" adapter_upper)
    if(ENABLE_ESY_COREDUMP_${adapter_upper})
      if(NOT TARGET esy_coredump_${adapter})
        message(FATAL_ERROR "Enabled coredump adapter is missing")
      endif()
      add_library(public_${adapter} OBJECT "public_${adapter}.c" "public_${adapter}.cpp")
      target_link_libraries(public_${adapter} PRIVATE esy_coredump_${adapter})
    elseif(TARGET esy_coredump_${adapter})
      message(FATAL_ERROR "Disabled coredump adapter was assembled")
    endif()
  endforeach()
  if(ENABLE_ESY_COREDUMP_FILE)
    foreach(language IN ITEMS c cpp)
      add_executable(file_consumer_${language} file_consumer.${language})
      target_link_libraries(file_consumer_${language} PRIVATE esy_coredump_file)
    endforeach()
  endif()
else()
  foreach(component IN ITEMS core logger file)
    if(TARGET esy_coredump_${component})
      message(FATAL_ERROR "Disabled coredump was assembled")
    endif()
  endforeach()
endif()
'''

PUBLIC = r'''
#include "esy_middleware/coredump/esy_coredump.h"
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private optimization options leaked through the public target"
#endif
#ifdef ESY_HAL_FLASH_H
#error "The core header unexpectedly includes the Flash provider"
#endif
#ifdef ESY_LOGGER_H
#error "The core header unexpectedly includes Logger"
#endif
'''

C_CONSUMER = r'''
#include "esy_middleware/coredump/esy_coredump.h"
#include <stdio.h>
#include <string.h>

static esy_err_t new_record_id(void *context, esy_coredump_record_id_t *id) {
  uint8_t *sequence = context;
  ++*sequence;
  for (size_t index = 0u; index < sizeof(id->bytes); ++index)
    id->bytes[index] = (uint8_t)(*sequence + index);
  return ESY_OK;
}

int probe_record(void) {
  esy_coredump_t store = {0};
  _Alignas(uint32_t) uint8_t retained[ESY_COREDUMP_HEADER_BYTES + 4u] = {0};
  uint8_t bytes[sizeof(retained)];
  const uint8_t stack[] = {0, 1, 0x80, 0xff};
  uint8_t sequence = 0u;
  esy_coredump_static_resources_t resources = {retained, sizeof(retained)};
  esy_coredump_config_t config = {
      .static_resources = &resources, .image_base = 0x10030000u,
      .firmware_version = 10001u, .stack_limit = sizeof(stack),
      .new_record_id = new_record_id, .record_id_context = &sequence};
  memset(config.build_id, 0x12, sizeof(config.build_id));
  esy_fault_snapshot_t snapshot = {
      .architecture = ESY_FAULT_ARCH_ARM_M, .reason = ESY_FAULT_REASON_SOFTWARE,
      .flags = ESY_FAULT_FLAG_CORE_REGS_VALID | ESY_FAULT_FLAG_STACK_VALID,
      .stack = stack, .stack_address = 0x20001000u, .stack_length = sizeof(stack)};
  snapshot.r[14] = 0x10031001u;
  snapshot.r[15] = 0x10032003u;
  if (esy_coredump_init(&store, &config) != ESY_OK) return 1;
  if (esy_coredump_capture_fault(&snapshot, &store) != ESY_OK) return 2;
  esy_coredump_info_t info;
  if (esy_coredump_get_info(&store, &info) != ESY_OK) return 3;
  if (esy_coredump_read(&store, &info.record_id, 0, bytes, sizeof(bytes)) != ESY_OK) return 3;
  if (esy_coredump_deinit(&store) != ESY_OK) return 4;
  for (size_t index = 0; index < sizeof(bytes); ++index) printf("%02x", bytes[index]);
  puts("");
  return 0;
}
'''

CPP_CONSUMER = r'''
#include "esy_middleware/coredump/esy_coredump.h"
extern "C" int probe_record(void);
int main() {
  if (esy_coredump_init(nullptr, nullptr) != ESY_ERR_INVALID_ARG) return 5;
  return probe_record();
}
'''

FILE_CONSUMER = r'''
#include "esy_middleware/coredump/esy_coredump_file.h"
#include "esy_middleware/file/esy_file_ram.h"
#ifdef ESY_HAL_FLASH_H
#error "Generic coredump file persistence unexpectedly exposes Flash"
#endif
static esy_err_t identity(void *context, esy_coredump_record_id_t *id) {
  uint8_t *sequence = (uint8_t *)context;
  ++*sequence;
  for (size_t i = 0u; i < sizeof(id->bytes); ++i) id->bytes[i] = (uint8_t)(*sequence + i);
  return ESY_OK;
}
int main(void) {
  esy_coredump_t store = INITIALIZER;
  esy_coredump_file_t adapter = INITIALIZER;
  esy_file_t file = INITIALIZER;
  esy_file_ram_t ram = INITIALIZER;
  union { uint8_t bytes[ESY_COREDUMP_MAX_RECORD_BYTES]; max_align_t alignment; } retained = INITIALIZER;
  uint8_t content[ESY_COREDUMP_MAX_RECORD_BYTES];
  uint8_t workspace[ESY_COREDUMP_MAX_RECORD_BYTES];
  uint8_t sequence = 0u;
  esy_coredump_static_resources_t core_resources = INITIALIZER;
  core_resources.retained_buffer = retained.bytes;
  core_resources.retained_size = sizeof(retained.bytes);
  esy_coredump_config_t core = INITIALIZER;
  core.static_resources = &core_resources;
  core.new_record_id = identity; core.record_id_context = &sequence;
  esy_file_ram_static_resources_t ram_resources = INITIALIZER;
  ram_resources.data_buffer = content; ram_resources.capacity = sizeof(content);
  esy_file_ram_config_t ram_config = INITIALIZER;
  ram_config.static_resources = &ram_resources;
  esy_file_ops_t ops = INITIALIZER;
  esy_coredump_file_static_resources_t resources = INITIALIZER;
  resources.record_buffer = workspace; resources.record_capacity = sizeof(workspace);
  esy_coredump_file_config_t config = INITIALIZER;
  config.store = &store; config.file = &file; config.static_resources = &resources;
  esy_fault_snapshot_t snapshot = INITIALIZER;
  snapshot.architecture = ESY_FAULT_ARCH_ARM_M; snapshot.reason = ESY_FAULT_REASON_SOFTWARE;
  esy_coredump_info_t info = INITIALIZER;
  esy_file_info_t file_info = INITIALIZER;
  if (esy_coredump_init(&store, &core) != ESY_OK) return 1;
  if (esy_file_ram_init(&ram, &ram_config) != ESY_OK) return 2;
  if (esy_file_ram_get_ops(&ram, &ops) != ESY_OK || esy_file_init(&file, &ops) != ESY_OK) return 3;
  if (esy_coredump_file_init(&adapter, &config) != ESY_OK) return 4;
  if (esy_coredump_capture_fault(&snapshot, &store) != ESY_OK) return 5;
  if (esy_coredump_get_info(&store, &info) != ESY_OK) return 6;
  if (esy_coredump_file_persist(&adapter) != ESY_OK || esy_coredump_file_persist(&adapter) != ESY_OK) return 7;
  if (esy_coredump_clear(&store, &info.record_id) != ESY_OK) return 8;
  if (esy_coredump_file_restore(&adapter) != ESY_OK) return 9;
  if (esy_coredump_file_clear(&adapter, &info.record_id) != ESY_OK) return 10;
  if (esy_file_get_info(&file, &file_info) != ESY_OK || file_info.exists) return 11;
  if (esy_coredump_file_deinit(&adapter) != ESY_OK || esy_file_deinit(&file) != ESY_OK) return 12;
  if (esy_file_ram_deinit(&ram) != ESY_OK || esy_coredump_deinit(&store) != ESY_OK) return 13;
  return 0;
}
'''


class CoredumpConsumerTests(unittest.TestCase):
    def command(self, *arguments, error=None):
        result = subprocess.run(arguments, capture_output=True, text=True)
        output = result.stdout + result.stderr
        if error:
            self.assertNotEqual(0, result.returncode, output)
            self.assertIn(error, output)
        else:
            self.assertEqual(0, result.returncode, output)
        return output

    def build(self, enabled=True, logger=False, file=False, flash_backend=False,
              logger_provider=True, file_provider=True, error=None, device=None):
        with tempfile.TemporaryDirectory(prefix="coredump consumer ") as directory:
            path = Path(directory)
            (path / "CMakeLists.txt").write_text(PROJECT)
            boolean = lambda value: "ON" if value else "OFF"
            (path / "esy_feature_import.cmake").write_text(
                f"set(ENABLE_ESY_COREDUMP {boolean(enabled)})\n"
                f"set(ENABLE_ESY_COREDUMP_LOGGER {boolean(logger)})\n"
                f"set(ENABLE_ESY_COREDUMP_FILE {boolean(file)})\n"
                f"set(ENABLE_ESY_FILE {boolean(file and file_provider)})\n"
                f"set(ENABLE_ESY_FILE_FLASH {boolean(flash_backend)})\n"
                f"set(ENABLE_ESY_LOGGER {boolean(logger and logger_provider)})\n"
                f"set(ENABLE_ESY_FLASH {boolean(flash_backend)})\n"
                f"set(PROBE_WITH_HAL {boolean(logger or flash_backend)})\n"
                "set(ESY_RTOS None)\n")
            (path / "public.c").write_text(PUBLIC)
            (path / "public.cpp").write_text(PUBLIC)
            for adapter in ("logger", "file"):
                header = f'#include "esy_middleware/coredump/esy_coredump_{adapter}.h"\n'
                (path / f"public_{adapter}.c").write_text(header)
                (path / f"public_{adapter}.cpp").write_text(header)
            for language in ("c", "cpp"):
                initializer = "{0}" if language == "c" else "{}"
                (path / f"file_consumer.{language}").write_text(
                    FILE_CONSUMER.replace("INITIALIZER", initializer))
            (path / "main.c").write_text(C_CONSUMER)
            (path / "main.cpp").write_text(CPP_CONSUMER)
            self.command("cmake", "-S", str(path), "-B", str(path / "out"),
                         f"-DROOT={ROOT}",
                         *([f"-DESY_TARGET_DEVICE={device}"] if device else []), error=error)
            if not error:
                self.command("cmake", "--build", str(path / "out"), "--parallel", "2")
                if enabled:
                    result = self.command(str(path / "out/consumer"))
                    spec = importlib.util.spec_from_file_location("coredump_decode", ROOT / "tools/coredump/esy_coredump_decode.py")
                    decoder = importlib.util.module_from_spec(spec)
                    spec.loader.exec_module(decoder)
                    info = decoder.decode_record(bytes.fromhex(result.strip()))
                    self.assertEqual("12" * 20, info["build_id"])
                    self.assertEqual(0x10030000, info["image_base"])
                    self.assertEqual(0x10032003, info["registers"]["pc"])
                    self.assertEqual("000180ff", info["stack_hex"])
                    self.assertEqual(bytes(range(1, 17)).hex(), info["record_id"])
                    if file:
                        self.command(str(path / "out/file_consumer_c"))
                        self.command(str(path / "out/file_consumer_cpp"))
                    return (path / "out/coredump-options.txt").read_text().splitlines()
                else:
                    for source in (path / "out").rglob("*"):
                        self.assertNotIn("esy_coredump", source.name)

    def test_core_public_headers_and_real_target_need_no_hal_logger_or_os(self):
        self.build()

    def test_selected_port_architecture_options_stay_private_and_preserve_generic_flags(self):
        for device, architecture_option in (("PICO", True), ("PICO2_W", True),
                                             ("STM32F407ZG", True), ("ESP32", False)):
            with self.subTest(device=device):
                compiler, private, public = self.build(device=device)
                if compiler == "GNU":
                    self.assertIn("-ffreestanding", private)
                    self.assertIn("-fno-tree-loop-distribute-patterns", private)
                    self.assertEqual(architecture_option, "-mgeneral-regs-only" in private)
                else:
                    self.assertNotIn("-mgeneral-regs-only", private)
                self.assertNotIn("-mgeneral-regs-only", public)
                self.assertNotIn("-ffreestanding", public)

    def test_logger_and_file_targets_with_real_flash_backend(self):
        self.build(logger=True, file=True, flash_backend=True)

    def test_ram_file_persistence_requires_no_hal_logger_or_os(self):
        self.build(file=True)

    def test_disabled_parent_assembles_no_coredump_targets(self):
        self.build(enabled=False)

    def test_file_adapter_requires_an_enabled_parent(self):
        self.build(enabled=False, file=True, error="requires ENABLE_ESY_COREDUMP")

    def test_logger_dependency_must_be_enabled(self):
        self.build(logger=True, logger_provider=False, error="requires ENABLE_ESY_LOGGER")

    def test_file_dependency_must_be_enabled(self):
        self.build(file=True, file_provider=False, error="requires ENABLE_ESY_FILE")


if __name__ == "__main__":
    unittest.main()
