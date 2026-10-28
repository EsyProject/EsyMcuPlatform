# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Build pure facade, chip and optional device consumers in C11 and C++17."""

from itertools import product
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
CHIPS = ("ADF4351", "ADF4372", "LMX2594", "LMX2592")
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(freq_synth_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(esy_feature_import.cmake)
add_library(esy_opt_default INTERFACE)
target_compile_definitions(esy_opt_default INTERFACE PRIVATE_OPTIONS_SENTINEL)
add_subdirectory("${ROOT}/esy_common/defs" defs)
add_subdirectory("${ROOT}/esy_common/device" device)
if(PROVIDE_HAL)
    add_library(esy_hal STATIC
        "${ROOT}/esy_test/fake/hal/fake_esy_hal_gpio.c"
        "${ROOT}/esy_test/fake/hal/fake_esy_hal_spi.c"
        "${ROOT}/esy_test/fake/hal/fake_esy_hal_system.c")
    target_include_directories(esy_hal PUBLIC "${ROOT}/esy_hal/include")
    target_link_libraries(esy_hal PUBLIC esy_common_defs esy_common_device)
    if(LINK_DEVICE)
        target_sources(esy_hal PRIVATE "${ROOT}/esy_test/fake/hal/fake_esy_hal_gpio_pm.c")
    endif()
endif()
add_subdirectory("${ROOT}/esy_middleware" middleware)
if(ENABLE_ESY_FREQ_SYNTH)
    get_target_property(sources esy_freq_synth SOURCES)
    foreach(chip adf4351 adf4372 lmx2594 lmx2592)
        string(TOUPPER "${chip}" feature)
        if(ENABLE_ESY_${feature})
            if(NOT "${sources}" MATCHES "esy_${chip}\\.c")
                message(FATAL_ERROR "Selected chip source missing")
            endif()
        elseif("${sources}" MATCHES "esy_${chip}\\.c")
            message(FATAL_ERROR "Unselected chip source compiled")
        endif()
    endforeach()
    if("${sources}" MATCHES "_device[.]c")
        message(FATAL_ERROR "Core acquired device registration sources")
    endif()
    if(ENABLE_ESY_GPIO AND NOT TARGET esy_freq_synth_device)
        message(FATAL_ERROR "Device adapter target missing")
    elseif(NOT ENABLE_ESY_GPIO AND TARGET esy_freq_synth_device)
        message(FATAL_ERROR "Disabled device adapter target assembled")
    endif()
    foreach(lang c cpp)
        add_executable(consumer_${lang} consumer.${lang})
        target_link_libraries(consumer_${lang} PRIVATE esy_freq_synth)
        if(LINK_DEVICE)
            target_link_libraries(consumer_${lang} PRIVATE esy_freq_synth_device)
            target_include_directories(consumer_${lang} PRIVATE "${ROOT}/esy_test/fake/hal")
        endif()
    endforeach()
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/core-archive.txt"
        CONTENT "$<TARGET_FILE:esy_freq_synth>")
elseif(TARGET esy_freq_synth OR TARGET esy_freq_synth_device)
    message(FATAL_ERROR "Disabled module assembled a target")
endif()
'''
CORE = r'''
#include "esy_middleware/freq_synth/esy_freq_synth.h"
CHIP_INCLUDES
#ifdef PRIVATE_OPTIONS_SENTINEL
#error "Private compiler options leaked"
#endif
static esy_err_t set_frequency(void *context, uint64_t frequency) {
    *(uint64_t *)context = frequency;
    return ESY_OK;
}
static const esy_freq_synth_vtable_t k_ops = {
    set_frequency, NULL, NULL, NULL
};
int main(void) {
    esy_freq_synth_t synth = {0};
    uint64_t frequency = 0;
    synth.ctx = &frequency;
    synth.ops = &k_ops;
    if (esy_freq_synth_set_frequency_hz(&synth, 123u) != ESY_OK || frequency != 123u) return 1;
    esy_freq_synth_detach(&synth);
    CHIP_CALLS
    return esy_freq_synth_is_attached(&synth);
}
'''
DEVICE = r'''
#include "esy_middleware/freq_synth/esy_adfCHIP_device.h"
#include "fake_esy_hal_gpio.h"
#include "fake_esy_hal_spi.h"
static esy_err_t parent_pm(esy_device_t *device, esy_device_pm_action_t action,
                            esy_power_state_t state) {
    (void)device; (void)action; (void)state;
    return ESY_OK;
}
int main(void) {
    esy_freq_synth_t synth = {0}; esy_spi_t spi = {0}; esy_gpio_t control = {0};
    esy_gpio_config_t gpio; esy_adfCHIP_config_t chip;
    esy_freq_synth_device_binding_t binding = {0};
    esy_freq_synth_device_config_t registration = {0};
    esy_device_registry_t registry = {0};
    ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 2u);
    esy_device_registry_static_resources_t resources;
    esy_device_registry_config_t registry_config = {0};
    esy_device_t parent = {0}; esy_device_ops_t parent_ops = {0};
    esy_device_config_t parent_config = {0}; esy_device_t *dependencies[] = {&parent};
    const uint32_t mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
        ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION);
    fake_esy_hal_gpio_reset(); fake_esy_hal_spi_reset();
    esy_hal_gpio_default_cfg(&gpio); gpio.pin = 5u; gpio.mode = ESY_GPIO_MODE_OUTPUT;
    if (esy_hal_gpio_init(&control, &gpio) != ESY_OK) return 1;
    esy_adfCHIP_default_config(&chip); chip.spi = &spi;
    chip.CONTROL_FIELD = &control; chip.initial_output_enabled = true;
    REVISION_SETUP
    if (esy_adfCHIP_init(&synth, &chip) != ESY_OK) return 2;
    ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
    registry_config.static_resources = &resources;
    if (esy_device_registry_init(&registry, &registry_config) != ESY_OK) return 3;
    parent_ops.pm_action = parent_pm; parent_config.name = "spi";
    parent_config.ops = &parent_ops; parent_config.supported_state_mask = mask;
    if (esy_device_register(&registry, &parent, &parent_config) != ESY_OK) return 4;
    fake_esy_hal_spi_set_device(&spi, &parent);
    fake_esy_hal_spi_set_pm_write_hook(&spi, esy_hal_spi_write_blocking);
    registration.registration.name = "synth";
    registration.registration.dependencies = dependencies;
    registration.registration.dependency_count = 1u;
    registration.registration.supported_state_mask = mask;
    if (esy_adfCHIP_register_device(&binding, &synth, &registry, &registration) != ESY_OK) return 5;
    if (esy_device_registry_start(&registry) != ESY_OK) return 6;
    if (esy_device_registry_runtime_suspend_idle(&registry) != ESY_OK) return 7;
    esy_device_state_t state;
    if (esy_device_get_state(&binding.device, &state) != ESY_OK || state != ESY_DEVICE_STATE_ACTIVE) return 8;
    if (esy_device_registry_system_prepare(&registry, ESY_POWER_STATE_RETENTION) != ESY_OK) return 9;
    if (esy_device_registry_system_commit_atomic(&registry) != ESY_OK) return 10;
    if (esy_device_registry_system_resume_atomic(&registry) != ESY_OK) return 11;
    if (esy_device_registry_system_resume(&registry) != ESY_OK) return 12;
    esy_freq_synth_info_value_t info;
    if (esy_freq_synth_get_info(&synth, ESY_FREQ_SYNTH_INFO_OUTPUT_ENABLED, &info) != ESY_OK || info.u8 != 1u) return 13;
    uint32_t refs;
    if (esy_device_get_usage_count(&binding.device, &refs) != ESY_OK || refs != 0u) return 14;
    if (esy_device_registry_deinit(&registry) != ESY_OK) return 15;
    if (esy_freq_synth_unregister_device(&binding) != ESY_OK) return 16;
    return esy_adfCHIP_deinit(&synth) != ESY_OK;
}
'''


class FreqSynthConsumerTests(unittest.TestCase):
    def check(self, chips=(), *, enabled=True, gpio=None, hal=None, device=False,
              missing=None, error=None):
        gpio = (bool(chips) or device) if gpio is None else gpio
        hal = (bool(chips) or gpio) if hal is None else hal
        features = {"ENABLE_ESY_FREQ_SYNTH": enabled, "ENABLE_ESY_GPIO": gpio,
                    "ENABLE_ESY_SPI": bool(chips), "ENABLE_ESY_SYSTEM": bool(chips),
                    "PROVIDE_HAL": hal, "LINK_DEVICE": device}
        features.update({f"ENABLE_ESY_{chip}": chip in chips for chip in CHIPS})
        if missing:
            features[missing] = False
        with tempfile.TemporaryDirectory(prefix="freq synth consumer ") as directory:
            project = Path(directory)
            (project / "CMakeLists.txt").write_text(PROJECT)
            (project / "esy_feature_import.cmake").write_text(''.join(
                f"set({name} {'ON' if value else 'OFF'})\n" for name, value in features.items()))
            if device:
                chip = chips[0].removeprefix("ADF")
                body = DEVICE.replace("CHIP", chip).replace(
                    "CONTROL_FIELD", "le_gpio" if chip == "4351" else "cs_gpio").replace(
                    "REVISION_SETUP", '' if chip == "4351" else
                    'const uint8_t revision = 0x0au; fake_esy_hal_spi_set_rx_data(&spi, &revision, 1u);')
            else:
                body = CORE.replace("CHIP_INCLUDES", '\n'.join(
                    f'#include "esy_middleware/freq_synth/esy_{chip.lower()}.h"' for chip in chips)).replace(
                    "CHIP_CALLS", '\n'.join(
                    f'if (esy_{chip.lower()}_deinit(&synth) != ESY_OK) return 2;' for chip in chips))
            for language in ("c", "cpp"):
                (project / f"consumer.{language}").write_text(body)
            result = subprocess.run(["cmake", "-S", str(project), "-B", str(project / "out"),
                f"-DROOT={ROOT}"], capture_output=True, text=True)
            if error:
                self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
                self.assertIn(error, ' '.join((result.stdout + result.stderr).split()))
                return
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            result = subprocess.run(["cmake", "--build", str(project / "out")], capture_output=True, text=True)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            if not enabled:
                return
            for language in ("c", "cpp"):
                result = subprocess.run([str(project / "out" / f"consumer_{language}")], capture_output=True, text=True)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            archive = (project / "out/core-archive.txt").read_text()
            symbols = subprocess.run(["nm", "-u", archive], capture_output=True, text=True, check=True).stdout
            for forbidden in ("esy_hal_gpio_pm_", "esy_device_register", "esy_device_unregister"):
                self.assertNotIn(forbidden, symbols)
            exported = subprocess.run(["nm", "-g", archive], capture_output=True,
                                      text=True, check=True).stdout
            for retired in ("esy_freq_synth_set_mode", "esy_freq_synth_start_sweep",
                            "esy_freq_synth_stop_sweep"):
                self.assertNotIn(retired, exported)
            if not chips:
                commands = json.loads((project / "out/compile_commands.json").read_text())
                for entry in commands:
                    if Path(entry['file']).name in ('esy_freq_synth.c', 'consumer.c', 'consumer.cpp'):
                        self.assertNotIn('/esy_hal/include', entry['command'])

    def test_all_chip_combinations_keep_device_symbols_out_of_core(self):
        for values in product((False, True), repeat=len(CHIPS)):
            chips = tuple(chip for chip, selected in zip(CHIPS, values) if selected)
            with self.subTest(chips=chips):
                self.check(chips)

    def test_disabled_module_does_not_assemble_core_or_device(self):
        self.check(CHIPS, enabled=False)

    def test_generic_core_runs_without_hal_and_with_unused_device_adapter(self):
        self.check()
        self.check(gpio=True)

    def test_each_typed_adapter_runs_graph_pm_from_its_separate_target(self):
        for chip in ('ADF4351', 'ADF4372'):
            with self.subTest(chip=chip):
                self.check((chip,), device=True)

    def test_missing_driver_hardware_and_adapter_provider_are_diagnosed(self):
        for feature in ('ENABLE_ESY_SPI', 'ENABLE_ESY_GPIO', 'ENABLE_ESY_SYSTEM'):
            with self.subTest(feature=feature):
                self.check(('ADF4351',), missing=feature, error=f'require {feature}')
        self.check(('ADF4372',), hal=False, error='require the esy_hal target')
        self.check(gpio=True, hal=False, error='esy_freq_synth_device requires the esy_hal target')


if __name__ == '__main__':
    unittest.main()
