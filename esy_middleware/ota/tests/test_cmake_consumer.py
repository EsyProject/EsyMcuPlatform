# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Compile and execute public OTA targets with consumer-owned feature choices."""
from pathlib import Path
import json
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
PROJECT = r"""
cmake_minimum_required(VERSION 3.20)
project(ota_consumer C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(esy_feature_import.cmake)
add_library(esy_opt_default INTERFACE)
target_compile_definitions(esy_opt_default INTERFACE PRIVATE_OPTIONS_SENTINEL)
target_compile_options(esy_opt_default INTERFACE -Wall -Wextra -Werror)
add_subdirectory("${ROOT}/esy_common" common)
if(ENABLE_ESY_OTA_FLASH AND NOT OMIT_HAL)
  add_library(esy_hal STATIC
    "${ROOT}/esy_test/fake/hal/fake_esy_hal_boot.c"
    "${ROOT}/esy_test/fake/hal/fake_esy_hal_flash.c"
    "${ROOT}/esy_test/fake/hal/fake_esy_hal_system.c")
  target_include_directories(esy_hal PUBLIC "${ROOT}/esy_hal/include"
    PRIVATE "${ROOT}/esy_test/fake/hal")
  target_link_libraries(esy_hal PUBLIC esy_common_defs)
endif()
if(DIRECT_OTA)
  add_subdirectory("${ROOT}/esy_middleware/ota" ota)
else()
  add_subdirectory("${ROOT}/esy_middleware" middleware)
endif()
if(ENABLE_ESY_OTA)
  foreach(language c cpp)
    add_executable(consumer_${language} consumer.${language})
    target_link_libraries(consumer_${language} PRIVATE esy_ota_core)
  endforeach()
  get_target_property(core_dependencies esy_ota_core INTERFACE_LINK_LIBRARIES)
  foreach(dependency IN LISTS core_dependencies)
    if(dependency MATCHES "esy_(hal|bootloader|protocol|common_protocol|os|ota_flash)")
      message(FATAL_ERROR "Generic core has a concrete dependency: ${dependency}")
    endif()
  endforeach()
  if(ENABLE_ESY_OTA_FLASH)
    foreach(language c cpp)
      add_executable(flash_consumer_${language} flash_consumer.${language})
      target_link_libraries(flash_consumer_${language} PRIVATE esy_ota_flash)
    endforeach()
    add_executable(interrupted_update_probe
      "${ROOT}/esy_middleware/ota/tests/interrupted_update_probe.c")
    target_include_directories(interrupted_update_probe PRIVATE
      "${ROOT}/esy_test/fake/hal")
    target_link_libraries(interrupted_update_probe PRIVATE
      esy_ota_flash esy_bootloader esy_common_utils esy_hal)
  elseif(TARGET esy_ota_flash OR TARGET esy_hal OR TARGET esy_bootloader)
    message(FATAL_ERROR "Pure core assembled hardware providers")
  endif()
elseif(TARGET esy_ota_core OR TARGET esy_ota_flash)
  message(FATAL_ERROR "Disabled OTA assembled targets")
endif()
foreach(dependency esy_proto_bin_session esy_os)
  if(TARGET ${dependency})
    message(FATAL_ERROR "Unexpected transport/OS target: ${dependency}")
  endif()
endforeach()
"""
CONSUMER = r"""
#include "esy_middleware/ota/esy_ota.h"
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private compiler options leaked"
#endif
static esy_err_t begin(void *context, const esy_image_manifest_t *manifest) {
  (void)context; (void)manifest; return ESY_OK;
}
static esy_err_t write(void *context, uint32_t offset, const uint8_t *data, size_t length) {
  (void)context; (void)offset; (void)data; (void)length; return ESY_OK;
}
static esy_err_t operation(void *context) { (void)context; return ESY_OK; }
static esy_err_t new_id(void *context, uint32_t *id) {
  (void)context; *id = 42u; return ESY_OK;
}
int main(void) {
  esy_ota_t ota = {0};
  uint8_t cache[4], bytes[4] = {1u, 2u, 3u, 4u};
  esy_ota_static_resources_t resources = {cache, sizeof(cache)};
  esy_ota_config_t config = {0};
  config.provider.begin = begin; config.provider.write = write;
  config.provider.finish = operation; config.provider.activate = operation;
  config.provider.abort = operation;
  config.static_resources = &resources;
  config.max_image_bytes = 4u; config.max_chunk_bytes = 4u;
  config.new_session_id = new_id;
  esy_image_manifest_t manifest = {0};
  manifest.version = 1u; manifest.image_size = sizeof(bytes);
  esy_image_status_t status;
  if (esy_ota_init(&ota, &config) != ESY_OK ||
      esy_ota_begin(&ota, &manifest, &status) != ESY_OK ||
      esy_ota_write(&ota, status.session_id, 0u, bytes, sizeof(bytes), &status) != ESY_OK ||
      esy_ota_finish(&ota, status.session_id, &status) != ESY_OK ||
      status.phase != ESY_IMAGE_READY ||
      esy_ota_activate(&ota, status.session_id, &status) != ESY_OK ||
      status.phase != ESY_IMAGE_ACTIVATED || esy_ota_poll(&ota) != ESY_OK ||
      esy_ota_status(&ota, &status) != ESY_OK) return 1;
  return esy_ota_deinit(&ota) == ESY_OK ? 0 : 2;
}
"""
FLASH_CONSUMER = r"""
#include "esy_middleware/ota/esy_ota_flash.h"
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private compiler options leaked"
#endif
int main(void) {
  esy_ota_flash_t flash = {0};
  uint8_t cache[16];
  esy_ota_flash_static_resources_t resources = {cache, sizeof(cache)};
  esy_ota_flash_config_t config = {&resources};
  esy_ota_provider_t provider;
  if (esy_ota_flash_init(&flash, &config) != ESY_OK ||
      esy_ota_flash_get_provider(&flash, &provider) != ESY_OK ||
      provider.begin == NULL || provider.write == NULL ||
      provider.finish == NULL || provider.activate == NULL ||
      provider.abort(provider.context) != ESY_OK) return 1;
  return esy_ota_flash_deinit(&flash) == ESY_OK ? 0 : 2;
}
"""


class OtaConsumerTests(unittest.TestCase):
    def check_config(self, overrides=None, error=None, direct=False, omit_hal=False):
        features = dict(ENABLE_ESY_OTA="ON", ENABLE_ESY_OTA_FLASH="OFF",
                        ENABLE_ESY_SYSTEM="OFF", ENABLE_ESY_FLASH="OFF",
                        ENABLE_ESY_BOOTLOADER="OFF", ESY_RTOS="None")
        features.update(overrides or {})
        with tempfile.TemporaryDirectory(prefix="ota consumer ") as directory:
            project = Path(directory)
            (project / "CMakeLists.txt").write_text(PROJECT)
            (project / "esy_feature_import.cmake").write_text(
                "".join(f"set({key} {value})\n" for key, value in features.items()))
            for extension in ("c", "cpp"):
                (project / f"consumer.{extension}").write_text(CONSUMER)
                (project / f"flash_consumer.{extension}").write_text(FLASH_CONSUMER)
            configured = subprocess.run(
                ["cmake", "-S", str(project), "-B", str(project / "out"),
                 f"-DROOT={ROOT}", f"-DDIRECT_OTA={'ON' if direct else 'OFF'}",
                 f"-DOMIT_HAL={'ON' if omit_hal else 'OFF'}"],
                capture_output=True, text=True)
            output = configured.stdout + configured.stderr
            if error:
                self.assertNotEqual(0, configured.returncode, output)
                self.assertIn(error, output)
                return
            self.assertEqual(0, configured.returncode, output)
            built = subprocess.run(["cmake", "--build", str(project / "out")],
                                   capture_output=True, text=True)
            self.assertEqual(0, built.returncode, built.stdout + built.stderr)
            probes = []
            if features["ENABLE_ESY_OTA"] == "ON":
                probes += ["consumer_c", "consumer_cpp"]
                if features["ENABLE_ESY_OTA_FLASH"] == "ON":
                    probes += ["flash_consumer_c", "flash_consumer_cpp", "interrupted_update_probe"]
            for probe in probes:
                result = subprocess.run([str(project / "out" / probe)],
                                        capture_output=True, text=True)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            commands = json.loads((project / "out/compile_commands.json").read_text())
            core = [entry for entry in commands if Path(entry["file"]).name == "esy_ota.c"]
            self.assertEqual(1 if probes else 0, len(core))
            for entry in core:
                for forbidden in ("esy_hal/include", "bootloader/include", "protocol/include",
                                  "os/include", "remote_manager/include"):
                    self.assertNotIn(forbidden, entry["command"])

    def test_pure_core_c_and_cpp_without_hal_os_flash_or_transport(self):
        self.check_config()

    def test_native_provider_and_interrupted_boot_flow(self):
        self.check_config(dict(ENABLE_ESY_OTA_FLASH="ON", ENABLE_ESY_SYSTEM="ON",
                               ENABLE_ESY_FLASH="ON", ENABLE_ESY_BOOTLOADER="ON"))

    def test_disabled_module_needs_no_providers(self):
        self.check_config(dict(ENABLE_ESY_OTA="OFF"))

    def test_flash_child_requires_parent(self):
        self.check_config(dict(ENABLE_ESY_OTA="OFF", ENABLE_ESY_OTA_FLASH="ON"),
                          "ENABLE_ESY_OTA_FLASH requires ENABLE_ESY_OTA")

    def test_flash_provider_requires_features(self):
        for missing in ("ENABLE_ESY_SYSTEM", "ENABLE_ESY_FLASH", "ENABLE_ESY_BOOTLOADER"):
            features = dict(ENABLE_ESY_OTA_FLASH="ON", ENABLE_ESY_SYSTEM="ON",
                            ENABLE_ESY_FLASH="ON", ENABLE_ESY_BOOTLOADER="ON")
            features[missing] = "OFF"
            with self.subTest(feature=missing):
                self.check_config(features, f"requires {missing}", direct=True)

    def test_flash_provider_requires_actual_targets(self):
        features = dict(ENABLE_ESY_OTA_FLASH="ON", ENABLE_ESY_SYSTEM="ON",
                        ENABLE_ESY_FLASH="ON", ENABLE_ESY_BOOTLOADER="ON")
        self.check_config(features, "requires target esy_hal", direct=True, omit_hal=True)
        self.check_config(features, "requires target esy_bootloader", direct=True)


if __name__ == "__main__":
    unittest.main()
