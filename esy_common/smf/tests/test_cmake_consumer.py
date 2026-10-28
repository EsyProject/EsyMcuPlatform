# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Build real common-only SMF consumers without OS or hardware providers."""

from pathlib import Path
import json
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
MODULE = ROOT / "esy_common" / "smf"
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(smf_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include("${CMAKE_CURRENT_LIST_DIR}/esy_feature_import.cmake")
if(WITH_OPTIMIZATION)
    add_library(esy_opt_default INTERFACE)
    target_compile_definitions(esy_opt_default INTERFACE ESY_SMF_PRIVATE_OPTIONS)
endif()
add_subdirectory("${ROOT}/esy_common" common)
foreach(provider esy_os esy_hal esy_hal_backend esy_board)
    if(TARGET ${provider})
        message(FATAL_ERROR "SMF unexpectedly assembled ${provider}")
    endif()
endforeach()
if(EXPECT_SMF)
    if(NOT TARGET esy_common_smf OR NOT TARGET Esy::common_smf)
        message(FATAL_ERROR "Enabled SMF target or alias is missing")
    endif()
elseif(TARGET esy_common_smf OR TARGET Esy::common_smf)
    message(FATAL_ERROR "Disabled SMF leaked a target")
endif()
foreach(language c cpp)
    add_executable(consumer_${language} consumer.${language})
    target_link_libraries(consumer_${language} PRIVATE ${CONSUMER_TARGET})
    target_compile_options(consumer_${language} PRIVATE -Wall -Wextra -Werror)
    target_link_options(consumer_${language} PRIVATE -Wl,--no-gc-sections)
endforeach()
'''
PRIVATE_CHECK = '''
#ifdef ESY_SMF_PRIVATE_OPTIONS
#error "Private optimization settings leaked into the SMF consumer"
#endif
'''


class SmfConsumerTests(unittest.TestCase):
    def run_command(self, command, directory):
        result = subprocess.run(command, cwd=directory, text=True,
                                capture_output=True, check=False)
        self.assertEqual(result.returncode, 0,
                         f"{command}\n{result.stdout}\n{result.stderr}")
        return result

    def build_consumer(self, feature, rtos="None", target="Esy::common",
                       optimization=False, cache_value=None):
        with tempfile.TemporaryDirectory(prefix="esy smf consumer ") as temporary:
            directory = Path(temporary)
            enabled = feature == "ON"
            feature_text = f'set(ESY_RTOS "{rtos}")\n'
            if feature is not None:
                feature_text += f'set(ENABLE_ESY_COMMON_SMF "{feature}")\n'
            (directory / "esy_feature_import.cmake").write_text(feature_text)
            (directory / "CMakeLists.txt").write_text(PROJECT)
            if enabled:
                readme = (MODULE / "README.md").read_text()
                source = readme.split("```c\n", 1)[1].split("```", 1)[0]
            else:
                source = ('#include "esy_common/defs/base/esy_common_defs.h"\n'
                          'int main(void) { return ESY_OK; }\n')
            for suffix in ("c", "cpp"):
                (directory / f"consumer.{suffix}").write_text(PRIVATE_CHECK + source)
            command = ["cmake", "-S", str(directory), "-B", str(directory / "out"),
                       f"-DROOT={ROOT}", f"-DEXPECT_SMF={'ON' if enabled else 'OFF'}",
                       f"-DCONSUMER_TARGET={target}",
                       f"-DWITH_OPTIMIZATION={'ON' if optimization else 'OFF'}"]
            if cache_value is not None:
                command.append(f"-DENABLE_ESY_COMMON_SMF:BOOL={cache_value}")
            self.run_command(command, directory)
            self.run_command(["cmake", "--build", str(directory / "out")], directory)
            commands = json.loads((directory / "out" / "compile_commands.json").read_text())
            smf_sources = [entry for entry in commands
                           if Path(entry["file"]).name == "esy_smf.c"]
            self.assertEqual(len(smf_sources), 1 if enabled else 0)
            if not enabled:
                self.assertFalse(any("smf/include" in entry["command"].replace("\\", "/")
                                     for entry in commands))
            for language in ("c", "cpp"):
                self.run_command([str(directory / "out" / f"consumer_{language}")],
                                 directory)

    def test_feature_on_off_and_unset_without_os_or_hal(self):
        for feature in ("ON", "OFF", None):
            for rtos in ("None", "FreeRTOS"):
                with self.subTest(feature=feature, rtos=rtos):
                    self.build_consumer(feature, rtos)

    def test_narrow_target_keeps_optional_optimization_private(self):
        for optimization in (False, True):
            with self.subTest(optimization=optimization):
                self.build_consumer("ON", target="Esy::common_smf",
                                    optimization=optimization)

    def test_selected_feature_file_overrides_conflicting_cache(self):
        for feature, cache in (("ON", "OFF"), ("OFF", "ON")):
            with self.subTest(feature=feature, cache=cache):
                self.build_consumer(feature, cache_value=cache)


if __name__ == "__main__":
    unittest.main()
