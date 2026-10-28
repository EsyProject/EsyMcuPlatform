# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Build I/O scheduler consumers with HAL fakes and a separate real SPI HAL boundary."""

from pathlib import Path
import json
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
PROJECT = r'''
cmake_minimum_required(VERSION 3.16)
project(io_scheduler_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(esy_feature_import.cmake)
if(PROVIDE_DEPENDENCIES)
    add_library(esy_opt_default INTERFACE)
    target_compile_definitions(esy_opt_default INTERFACE PRIVATE_OPTIONS_SENTINEL)
    add_subdirectory("${PLATFORM_ROOT}/esy_common" common)
    if(PROVIDE_OS)
        add_library(esy_os STATIC
            "${PLATFORM_ROOT}/esy_test/fake/os/fake_esy_os.c"
            "${PLATFORM_ROOT}/esy_test/fake/os/fake_esy_os_task.c")
        target_include_directories(esy_os PUBLIC "${PLATFORM_ROOT}/esy_middleware/os/include")
        target_link_libraries(esy_os PUBLIC esy_common)
    endif()
    add_library(esy_hal STATIC "${PLATFORM_ROOT}/esy_test/fake/hal/fake_esy_hal_system.c")
    target_include_directories(esy_hal PUBLIC "${PLATFORM_ROOT}/esy_hal/include")
    target_link_libraries(esy_hal PUBLIC esy_common)
    foreach(feature IN ITEMS UART SPI DMA)
        if(ENABLE_ESY_${feature})
            string(TOLOWER ${feature} lower)
            if(REAL_SPI_HAL AND feature STREQUAL "SPI")
                target_sources(esy_hal PRIVATE
                    "${PLATFORM_ROOT}/esy_hal/src/esy_hal_spi.c"
                    "${PLATFORM_ROOT}/esy_hal/src/esy_hal_spi_io.c"
                    "${PLATFORM_ROOT}/esy_middleware/io_scheduler/tests/fixtures/spi_hal/native_identity_stub.c")
                target_include_directories(esy_hal PRIVATE
                    "${PLATFORM_ROOT}/esy_hal_backend/include"
                    "${PLATFORM_ROOT}/esy_middleware/io_scheduler/tests/fixtures/spi_hal")
                # Exercise injected ops without native lifecycle assembly. The
                # retained hardware-vtable identity query rejects every handle.
                target_compile_options(esy_hal PRIVATE -ffunction-sections -fdata-sections)
            else()
                target_sources(esy_hal PRIVATE
                    "${PLATFORM_ROOT}/esy_test/fake/hal/fake_esy_hal_${lower}.c")
            endif()
        endif()
    endforeach()
endif()
if(ENABLE_ESY_IO_SCHEDULER)
    add_subdirectory("${PLATFORM_ROOT}/esy_middleware/io_scheduler" io_scheduler)
    if(TARGET esy_io_scheduler)
        message(FATAL_ERROR "Unexpected aggregate target")
    endif()
    foreach(adapter IN ITEMS esy_spi_scheduler esy_uart_tx_scheduler esy_spi_scheduler_core)
        if(TARGET ${adapter})
            get_target_property(dependencies ${adapter} INTERFACE_LINK_LIBRARIES)
            foreach(dependency IN LISTS dependencies)
                if(dependency MATCHES "esy_proto_")
                    message(FATAL_ERROR "A I/O scheduler pulled in a protocol")
                endif()
            endforeach()
        elseif(adapter IN_LIST EXPECTED_TARGETS)
            message(FATAL_ERROR "Missing adapter: ${adapter}")
        endif()
    endforeach()
    foreach(core IN ITEMS esy_spi_scheduler_core)
        if(TARGET ${core})
            get_target_property(dependencies ${core} INTERFACE_LINK_LIBRARIES)
            if("esy_os" IN_LIST dependencies)
                message(FATAL_ERROR "Core target requires OS")
            endif()
        endif()
    endforeach()
    foreach(link_target IN LISTS EXPECTED_TARGETS)
        foreach(language IN ITEMS c cpp)
            add_executable(${link_target}_${language} ${link_target}.${language})
            target_link_libraries(${link_target}_${language} PRIVATE ${link_target})
            if(REAL_SPI_HAL)
                target_link_options(${link_target}_${language} PRIVATE -Wl,--gc-sections)
            endif()
        endforeach()
    endforeach()
    if(REAL_SPI_HAL)
        add_executable(spi_hal_contract
            "${PLATFORM_ROOT}/esy_middleware/io_scheduler/tests/fixtures/spi_hal/consumer.c")
        target_link_libraries(spi_hal_contract PRIVATE esy_spi_scheduler_core)
        target_link_options(spi_hal_contract PRIVATE -Wl,--gc-sections)
    endif()
endif()
if(ESY_RTOS STREQUAL "None" AND TARGET esy_spi_scheduler)
    message(FATAL_ERROR "OS adapter assembled without an OS")
endif()
'''
UMBRELLA_PROJECT = r'''
cmake_minimum_required(VERSION 3.16)
project(io_scheduler_umbrella_consumer LANGUAGES C)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(esy_feature_import.cmake)
add_subdirectory("${PLATFORM_ROOT}/esy_middleware" middleware)
foreach(adapter IN ITEMS esy_spi_scheduler esy_spi_scheduler_core esy_uart_tx_scheduler)
    if(TARGET ${adapter})
        message(FATAL_ERROR "Disabled I/O schedulers assembled target ${adapter}")
    endif()
endforeach()
get_target_property(dependencies esy_middleware INTERFACE_LINK_LIBRARIES)
if(dependencies)
    message(FATAL_ERROR "Disabled I/O schedulers added middleware dependencies")
endif()
'''
CALLS = {
    "esy_spi_scheduler": "esy_spi_scheduler_create(NULL, NULL, NULL, NULL)",
    "esy_spi_scheduler_core": "esy_spi_scheduler_core_create(NULL, NULL, NULL, NULL)",
    "esy_uart_tx_scheduler": "esy_uart_tx_scheduler_init(NULL, NULL, NULL)",
}


def consumer(adapters):
    includes = "".join(f'#include "esy_middleware/io_scheduler/{a}.h"\n' for a in adapters)
    checks = "".join(f"if ({CALLS[a]} != ESY_ERR_INVALID_ARG) return 1;\n" for a in adapters)
    if "esy_spi_scheduler" in adapters or "esy_spi_scheduler_core" in adapters:
        checks += r'''
        uint8_t *tx = NULL, *rx = NULL;
        if (esy_spi_scheduler_get_read_write_buffer_with_arg(&tx, &rx, 3u, NULL)
            != ESY_ERR_INVALID_ARG) return 2;
'''
    return includes + '#ifdef PRIVATE_OPTIONS_SENTINEL\n#error "Private options leaked"\n#endif\nint main(void) {\n' + checks + 'return 0;\n}\n'


class IoSchedulerConsumerTests(unittest.TestCase):
    def check_umbrella(self, overrides):
        flags = dict(ESY_RTOS="None")
        flags.update(overrides)
        with tempfile.TemporaryDirectory(prefix="io scheduler umbrella consumer ") as d:
            project = Path(d)
            (project / "CMakeLists.txt").write_text(UMBRELLA_PROJECT)
            (project / "esy_feature_import.cmake").write_text(
                "".join(f'set({key} "{value}")\n' for key, value in flags.items()))
            configured = subprocess.run(["cmake", "-S", str(project), "-B", str(project / "out"),
                                         f"-DPLATFORM_ROOT={ROOT}"], capture_output=True, text=True)
            self.assertEqual(0, configured.returncode, configured.stdout + configured.stderr)
            built = subprocess.run(["cmake", "--build", str(project / "out")],
                                   capture_output=True, text=True)
            self.assertEqual(0, built.returncode, built.stdout + built.stderr)
            commands = project / "out/compile_commands.json"
            self.assertNotIn("esy_middleware/io_scheduler/src",
                             commands.read_text() if commands.exists() else "")

    def check(self, adapters=(), overrides=None, error=None):
        flags = dict(ENABLE_ESY_IO_SCHEDULER="ON", ENABLE_ESY_UART="OFF", ENABLE_ESY_SPI="OFF",
                     ENABLE_ESY_DMA="ON", ENABLE_ESY_SYSTEM="ON", ESY_RTOS="FreeRTOS",
                     PROVIDE_DEPENDENCIES="ON", PROVIDE_OS="ON",
                     REAL_SPI_HAL="OFF",
                     ENABLE_ESY_COMMON_DS="ON", ENABLE_ESY_COMMON_PROTOCOL="OFF")
        flags.update(overrides or {})
        with tempfile.TemporaryDirectory(prefix="io scheduler consumer ") as d:
            project = Path(d)
            (project / "CMakeLists.txt").write_text(PROJECT)
            targets = list(adapters)
            for adapter in adapters:
                if adapter == "esy_spi_scheduler":
                    targets.append(adapter + "_core")
            for target in targets:
                for extension in ("c", "cpp"):
                    (project / f"{target}.{extension}").write_text(consumer((target,)))
            cores = tuple(target for target in targets if target.endswith("_core"))
            features = "".join(f'set({key} "{value}")\n' for key, value in flags.items())
            features += 'set(EXPECTED_TARGETS "' + ";".join(targets) + '")\n'
            (project / "esy_feature_import.cmake").write_text(features)
            configured = subprocess.run(["cmake", "-S", str(project), "-B", str(project / "out"),
                                         f"-DPLATFORM_ROOT={ROOT}"], capture_output=True, text=True)
            if error:
                self.assertNotEqual(0, configured.returncode)
                self.assertIn(error, " ".join((configured.stdout + configured.stderr).split()))
                return
            self.assertEqual(0, configured.returncode, configured.stdout + configured.stderr)
            built = subprocess.run(["cmake", "--build", str(project / "out"), "-j2"], capture_output=True, text=True)
            self.assertEqual(0, built.returncode, built.stdout + built.stderr)
            commands = project / "out/compile_commands.json"
            command_text = commands.read_text() if commands.exists() else "[]"
            source_dir = ROOT / "esy_middleware/io_scheduler/src"
            sources = {str(Path(item["file"]).relative_to(source_dir))
                       for item in json.loads(command_text)
                       if Path(item["file"]).is_relative_to(source_dir)}
            if flags["ESY_RTOS"] == "None" and flags["PROVIDE_OS"] == "OFF":
                self.assertNotIn("esy_middleware/os", command_text)
            self.assertEqual({f"{target}.c" for target in targets}, sources)
            self.assertNotIn("esy_middleware/protocol/src", command_text)
            if flags["ENABLE_ESY_IO_SCHEDULER"] == "ON":
                for item in json.loads(command_text):
                    if Path(item["file"]).stem in cores:
                        self.assertNotIn("esy_middleware/os/include", item["command"])
                for target in targets:
                    for language in ("c", "cpp"):
                        self.assertEqual(0, subprocess.run([str(project / f"out/{target}_{language}")]).returncode)
                if flags["REAL_SPI_HAL"] == "ON":
                    self.assertIn("esy_hal/src/esy_hal_spi.c", command_text)
                    self.assertIn("esy_hal/src/esy_hal_spi_io.c", command_text)
                    self.assertNotIn("fake_esy_hal_spi.c", command_text)
                    exercised = subprocess.run([str(project / "out/spi_hal_contract")],
                                               capture_output=True, text=True)
                    self.assertEqual(0, exercised.returncode,
                                     exercised.stdout + exercised.stderr)

    def test_uart_platform_support_adds_no_scheduler(self):
        self.check(overrides=dict(ENABLE_ESY_UART="ON", PROVIDE_DEPENDENCIES="OFF", ESY_RTOS="None"))

    def test_uart_scheduler_without_rtos_or_protocol(self):
        self.check(("esy_uart_tx_scheduler",), dict(ENABLE_ESY_UART="ON",
            ENABLE_ESY_UART_TX_SCHEDULER="ON", ESY_RTOS="None", PROVIDE_OS="OFF",
            ENABLE_ESY_SYSTEM="OFF"))

    def test_explicit_spi_has_no_protocol_dependency(self):
        self.check(("esy_spi_scheduler",), {"ENABLE_ESY_SPI": "ON", "ENABLE_ESY_SPI_SCHEDULER": "ON"})

    def test_spi_core_without_any_os_target(self):
        self.check(("esy_spi_scheduler_core",), dict(ENABLE_ESY_SPI="ON", ENABLE_ESY_SPI_SCHEDULER="ON", ESY_RTOS="None", PROVIDE_OS="OFF"))

    def test_spi_core_uses_real_hal_for_single_direction_dma_and_retained_errors(self):
        self.check(("esy_spi_scheduler_core",), dict(ENABLE_ESY_SPI="ON",
            ENABLE_ESY_SPI_SCHEDULER="ON", ESY_RTOS="None", PROVIDE_OS="OFF",
            REAL_SPI_HAL="ON"))

    def test_all_concrete_adapters_link(self):
        self.check(("esy_spi_scheduler", "esy_uart_tx_scheduler"), dict(ENABLE_ESY_UART="ON",
            ENABLE_ESY_SPI="ON", ENABLE_ESY_SPI_SCHEDULER="ON", ENABLE_ESY_UART_TX_SCHEDULER="ON"))

    def test_explicitly_disabled_adapters_need_no_dependencies(self):
        self.check(overrides=dict(ENABLE_ESY_UART="ON", ENABLE_ESY_SPI="ON",
            ENABLE_ESY_SPI_SCHEDULER="OFF", PROVIDE_DEPENDENCIES="OFF", ESY_RTOS="None"))

    def test_spi_hal_without_explicit_scheduler_gate_adds_no_scheduler(self):
        self.check(overrides=dict(ENABLE_ESY_SPI="ON", PROVIDE_DEPENDENCIES="OFF", ESY_RTOS="None"))

    def test_empty_selection_is_valid(self):
        self.check(overrides=dict(PROVIDE_DEPENDENCIES="OFF", ESY_RTOS="None"))

    def test_disabled_parent_adds_no_targets(self):
        self.check(overrides=dict(ENABLE_ESY_IO_SCHEDULER="OFF", PROVIDE_DEPENDENCIES="OFF", ESY_RTOS="None"))
        self.check_umbrella(dict(ENABLE_ESY_IO_SCHEDULER="OFF"))

    def test_missing_dependencies_fail_at_configure(self):
        for name, value, message in (("ENABLE_ESY_DMA", "OFF", "requires ENABLE_ESY_DMA"),
            ("ENABLE_ESY_SYSTEM", "OFF", "requires ENABLE_ESY_SYSTEM"),
            ("PROVIDE_OS", "OFF", "requires the esy_os target"),
            ("ENABLE_ESY_COMMON_DS", "OFF", "requires the esy_common_ds target"),
            ):
            with self.subTest(feature=name):
                self.check(overrides={"ENABLE_ESY_SPI": "ON", "ENABLE_ESY_SPI_SCHEDULER": "ON", name: value}, error=message)

    def test_missing_spi_feature_is_rejected(self):
        self.check(overrides=dict(ENABLE_ESY_SPI_SCHEDULER="ON"), error="requires ENABLE_ESY_SPI")


if __name__ == "__main__":
    unittest.main()
