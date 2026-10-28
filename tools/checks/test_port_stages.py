# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Build an independent port without editing shared platform dispatchers."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]


class PortStageTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="esy independent port ")
        self.addCleanup(temporary.cleanup)
        self.project = Path(temporary.name)
        self.library = self.project / "library"
        self.library.mkdir()
        for directory in ("cmake", "esy_common", "esy_hal", "esy_hal_backend", "esy_middleware"):
            shutil.copytree(ROOT / directory, self.library / directory,
                            ignore=shutil.ignore_patterns("tests", "build"))
        shutil.copyfile(ROOT / "CMakeLists.txt", self.library / "CMakeLists.txt")
        self.port = self.library / "cmake/ports/fixture"
        self.write("library/cmake/ports/fixture/device_map.cmake", r'''
            set(ESY_PORT_EXPORT_FIELDS ESY_FIXTURE_REVISION)
            function(esy_port_fixture_match device output)
                if(device STREQUAL "FIXTURE_MCU")
                    set(${output} TRUE PARENT_SCOPE)
                else()
                    set(${output} FALSE PARENT_SCOPE)
                endif()
            endfunction()
            function(esy_port_fixture_resolve device result)
                foreach(pair IN ITEMS
                        "ESY_TARGET_DEVICE|${device}" "ESY_TARGET_PLATFORM|FIXTURE"
                        "ESY_BUILD_PORT|fixture" "ESY_BOARD_PLATFORM_PATH|fixture_board"
                        "ESY_SPECS_PLATFORM_PATH|fixture_specs" "ESY_FIXTURE_REVISION|r1")
                    string(REPLACE "|" ";" fields "${pair}")
                    list(GET fields 0 field)
                    list(GET fields 1 value)
                    set(${result}_${field} "${value}" PARENT_SCOPE)
                endforeach()
            endfunction()
        ''')
        self.write("library/cmake/ports/fixture/pre_project.cmake", r'''
            if(CMAKE_C_COMPILER_LOADED)
                message(FATAL_ERROR "Fixture pre-project entry ran after project()")
            endif()
            file(APPEND "${ESY_PROJECT_ROOT}/events.txt" "early\n")
        ''')
        self.write("library/cmake/ports/fixture/post_project.cmake", r'''
            macro(esy_port_fixture_configure_build_defaults)
                set(ESY_FIXTURE_DEFAULT "from port")
            endmacro()

            function(esy_port_fixture_configure_project_options)
                if(NOT TARGET esy_project_options OR NOT FIXTURE_FEATURES_LOADED)
                    message(FATAL_ERROR "Fixture project options ran before features/options")
                endif()
                target_compile_definitions(esy_project_options INTERFACE FIXTURE_PROJECT_OPTION=1)
                file(APPEND "${ESY_PROJECT_ROOT}/events.txt" "project\n")
            endfunction()

            function(esy_port_fixture_initialize_sdk_dependencies)
                if(NOT CMAKE_C_COMPILER_LOADED OR NOT FIXTURE_FEATURES_LOADED)
                    message(FATAL_ERROR "Fixture providers ran before final features")
                endif()
                if(NOT TARGET fixture_sdk)
                    add_library(fixture_sdk INTERFACE)
                endif()
                file(APPEND "${ESY_PROJECT_ROOT}/events.txt" "providers\n")
            endfunction()

            function(esy_port_fixture_configure_network_providers)
                if(NOT TARGET fixture_network)
                    message(FATAL_ERROR "Network defaults ran before consumer target selection")
                endif()
                get_target_property(identity fixture_network FIXTURE_IDENTITY)
                if(NOT identity STREQUAL "consumer")
                    message(FATAL_ERROR "Consumer provider was replaced")
                endif()
                file(APPEND "${ESY_PROJECT_ROOT}/events.txt" "network\n")
            endfunction()

            function(esy_port_fixture_configure_runtime_targets)
                if(NOT TARGET esy_common OR NOT TARGET esy_board OR TARGET esy_hal_backend)
                    message(FATAL_ERROR "Fixture runtime hook has incorrect ordering")
                endif()
                add_library(fixture_runtime STATIC
                    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../esy_hal_backend/src/fixture/fixture_runtime.c")
                file(APPEND "${ESY_PROJECT_ROOT}/events.txt" "runtime\n")
            endfunction()

            macro(esy_port_fixture_export_sdk_variables)
                set(FIXTURE_PROMOTED "from library directory" PARENT_SCOPE)
            endmacro()

            function(esy_port_fixture_configure_firmware target)
                if(NOT TARGET ${target} OR NOT TARGET fixture_runtime)
                    message(FATAL_ERROR "Fixture firmware entry ran before assembly")
                endif()
                get_target_property(calls ${target} FIXTURE_FIRMWARE_CALLS)
                if(NOT calls)
                    set(calls 0)
                endif()
                math(EXPR calls "${calls} + 1")
                set_property(TARGET ${target} PROPERTY FIXTURE_FIRMWARE_CALLS ${calls})
                target_compile_definitions(${target} PRIVATE FIXTURE_FIRMWARE=1)
                file(APPEND "${ESY_PROJECT_ROOT}/events.txt" "firmware\n")
            endfunction()
        ''')
        self.write("library/esy_hal_backend/src/fixture/fixture_runtime.c", '''
            int fixture_runtime_value(void) { return 11; }
        ''')
        self.write("library/esy_hal_backend/src/fixture/CMakeLists.txt", r'''
            if(NOT TARGET fixture_sdk OR NOT TARGET fixture_runtime)
                message(FATAL_ERROR "Fixture backend lost its platform prerequisites")
            endif()
            target_sources(esy_hal_backend PRIVATE fixture_backend.c)
            target_link_libraries(esy_hal_backend PRIVATE fixture_sdk fixture_runtime)
            if(ENABLE_ESY_GPIO)
                add_library(fixture_vectors OBJECT fixture_vectors.c)
                esy_register_interrupt_target(esy_hal_backend fixture_vectors)
                esy_register_interrupt_target(esy_hal_backend fixture_vectors)
            endif()
        ''')
        self.write("library/esy_hal_backend/src/fixture/fixture_backend.c", '''
            int fixture_backend_value(void) { return 7; }
        ''')
        self.write("library/esy_hal_backend/src/fixture/fixture_vectors.c", '''
            int fixture_vector_value(void) { return 23; }
        ''')
        self.write("board/CMakeLists.txt", r'''
            add_library(esy_board INTERFACE)
            target_include_directories(esy_board INTERFACE "${CMAKE_CURRENT_LIST_DIR}/include")
        ''')
        self.write("board/include/esy_board/esy_board.h", '''
            #include "esy_common/defs/board/esy_board_defs.h"
        ''')
        self.write("esy_feature_import.cmake", r'''
            set(ESY_RTOS None)
            set(FIXTURE_FEATURES_LOADED TRUE)
            set(ENABLE_ESY_GPIO "${FIXTURE_GPIO}")
        ''')
        self.write("main.c", '''
            #ifndef FIXTURE_FIRMWARE
            #error "Firmware configuration did not reach its target"
            #endif
            extern int fixture_backend_value(void);
            extern int fixture_runtime_value(void);
            __attribute__((weak)) int fixture_vector_value(void) { return -1; }
            int main(void) {
                return fixture_backend_value() != 7 || fixture_runtime_value() != 11 ||
                       fixture_vector_value() != EXPECTED_VECTOR;
            }
        ''')
        self.write("CMakeLists.txt", r'''
            cmake_minimum_required(VERSION 3.20)
            set(ESY_TARGET_DEVICE fixture_mcu)
            set(ESY_BOARD_PATH "${CMAKE_CURRENT_SOURCE_DIR}/board")
            set(ESY_PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
            file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/events.txt" "")
            if(FIXTURE_DIRECT)
                project(independent_port C)
                include("${CMAKE_CURRENT_SOURCE_DIR}/library/cmake/esy_target_device.cmake")
                esy_configure_target_device()
                include(esy_feature_import.cmake)
                foreach(target esy_opt_default esy_opt_speed fixture_sdk)
                    add_library(${target} INTERFACE)
                endforeach()
            else()
                include("${CMAKE_CURRENT_SOURCE_DIR}/library/cmake/esy_early_import.cmake")
                project(independent_port C)
                if(FIXTURE_EXPLICIT_IMPORT)
                    include("${CMAKE_CURRENT_SOURCE_DIR}/library/cmake/esy_project_import.cmake")
                    include("${CMAKE_CURRENT_SOURCE_DIR}/library/cmake/esy_project_import.cmake")
                endif()
            endif()
            add_library(fixture_network INTERFACE)
            set_property(TARGET fixture_network PROPERTY FIXTURE_IDENTITY consumer)
            add_subdirectory(library)
            if(NOT FIXTURE_DIRECT AND NOT FIXTURE_EXPLICIT_IMPORT AND
               NOT FIXTURE_PROMOTED STREQUAL "from library directory")
                message(FATAL_ERROR "Port promotion changed scope")
            endif()
            add_executable(fixture_firmware main.c)
            target_link_libraries(fixture_firmware PRIVATE esy_hal)
            if(ENABLE_ESY_GPIO)
                target_compile_definitions(fixture_firmware PRIVATE EXPECTED_VECTOR=23)
            else()
                target_compile_definitions(fixture_firmware PRIVATE EXPECTED_VECTOR=-1)
            endif()
            set(ESY_FIRMWARE_TARGET fixture_firmware)
            include("${CMAKE_CURRENT_SOURCE_DIR}/library/cmake/esy_firmware.cmake")
            include("${CMAKE_CURRENT_SOURCE_DIR}/library/cmake/esy_firmware.cmake")
            if(ENABLE_ESY_GPIO)
                get_target_property(vectors esy_interrupts INTERFACE_SOURCES)
                if(NOT vectors STREQUAL "$<TARGET_OBJECTS:fixture_vectors>")
                    message(FATAL_ERROR "Interrupt objects were missing or duplicated: ${vectors}")
                endif()
            elseif(TARGET esy_interrupts)
                message(FATAL_ERROR "Disabled interrupts retained stale registrations")
            endif()
        ''')

    def write(self, relative, content):
        path = self.project / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(textwrap.dedent(content).lstrip(), encoding="utf-8")

    def command(self, arguments, expected=0):
        result = subprocess.run(arguments, cwd=self.project, capture_output=True,
                                text=True, timeout=120)
        self.assertEqual(expected, result.returncode, result.stdout + result.stderr)
        return result

    def configure_and_run(self, *, gpio=True, direct=False, explicit=False):
        self.command(["cmake", "-S", str(self.project), "-B", str(self.project / "out"),
                      f"-DFIXTURE_GPIO={'ON' if gpio else 'OFF'}",
                      f"-DFIXTURE_DIRECT={'ON' if direct else 'OFF'}",
                      f"-DFIXTURE_EXPLICIT_IMPORT={'ON' if explicit else 'OFF'}"])
        self.command(["cmake", "--build", str(self.project / "out"), "-j2"])
        self.command([str(self.project / "out/fixture_firmware")])
        return (self.project / "events.txt").read_text().splitlines()

    def test_new_port_builds_through_shared_stages_and_retains_strong_vectors(self):
        self.assertEqual(["early", "project", "providers", "network", "runtime", "firmware"],
                         self.configure_and_run())

    def test_reconfigure_removes_disabled_vector_registration(self):
        self.configure_and_run()
        self.assertEqual(["early", "project", "providers", "network", "runtime", "firmware"],
                         self.configure_and_run(gpio=False))

    def test_direct_consumer_keeps_its_sdk_and_skips_optional_project_integration(self):
        self.assertEqual(["runtime", "firmware"], self.configure_and_run(direct=True))

    def test_explicit_project_import_is_idempotent_before_library_assembly(self):
        self.assertEqual(["early", "project", "providers", "network", "runtime", "firmware"],
                         self.configure_and_run(explicit=True))

    def test_all_ports_use_the_three_entry_files(self):
        expected = {"device_map.cmake", "pre_project.cmake", "post_project.cmake"}
        for port in sorted((self.library / "cmake/ports").iterdir()):
            if port.is_dir():
                with self.subTest(port=port.name):
                    self.assertEqual(expected, {path.name for path in port.glob("*.cmake")})

    def test_firmware_configuration_is_per_target_and_idempotent(self):
        with (self.project / "CMakeLists.txt").open("a", encoding="utf-8") as output:
            output.write(textwrap.dedent(r'''
                add_executable(second_firmware main.c)
                target_link_libraries(second_firmware PRIVATE esy_hal)
                target_compile_definitions(second_firmware PRIVATE EXPECTED_VECTOR=23)
                set(ESY_FIRMWARE_TARGET second_firmware)
                include("${CMAKE_CURRENT_SOURCE_DIR}/library/cmake/esy_firmware.cmake")
                include("${CMAKE_CURRENT_SOURCE_DIR}/library/cmake/esy_firmware.cmake")
                foreach(target fixture_firmware second_firmware)
                    get_target_property(calls ${target} FIXTURE_FIRMWARE_CALLS)
                    if(NOT calls EQUAL 1)
                        message(FATAL_ERROR "Firmware configuration ran ${calls} times for ${target}")
                    endif()
                endforeach()
            '''))
        self.assertEqual(["early", "project", "providers", "network", "runtime",
                          "firmware", "firmware"], self.configure_and_run())
        self.command([str(self.project / "out/second_firmware")])

    def test_firmware_rejects_a_missing_post_project_entry_or_firmware_hook(self):
        post = self.port / "post_project.cmake"
        definitions = post.read_text()
        self.write("stub.c", "int main(void) { return 0; }\n")
        self.write("CMakeLists.txt", r'''
            cmake_minimum_required(VERSION 3.20)
            project(missing_firmware_hook C)
            set(ESY_TARGET_DEVICE FIXTURE_MCU)
            add_library(esy_opt_default INTERFACE)
            add_executable(image stub.c)
            set(ESY_FIRMWARE_TARGET image)
            include("${CMAKE_CURRENT_SOURCE_DIR}/library/cmake/esy_firmware.cmake")
        ''')
        for missing, diagnostic in (("entry", "Unsupported firmware"),
                                    ("hook", "esy_port_fixture_configure_firmware(target)")):
            with self.subTest(missing=missing):
                if missing == "entry":
                    post.unlink()
                else:
                    post.write_text(definitions.replace(
                        "esy_port_fixture_configure_firmware", "unused_firmware_hook"))
                result = self.command(["cmake", "-S", str(self.project), "-B",
                                       str(self.project / f"out-{missing}")], expected=1)
                self.assertIn(diagnostic, result.stdout + result.stderr)

    def test_loading_post_project_definitions_has_no_build_or_export_side_effects(self):
        definitions = sorted((self.library / "cmake/ports").glob("*/post_project.cmake"))
        paths = " ".join(f'"{path}"' for path in definitions)
        self.write("load_definitions.cmake", f'''
            set(ESY_PROJECT_ROOT "{self.project}")
            function(load_definitions_only)
                foreach(definitions IN ITEMS {paths})
                    include("${{definitions}}")
                endforeach()
                foreach(output ESY_FIXTURE_DEFAULT ESY_STM32_SUPPORT_ROOT
                               ESY_ATOMIC_IRQ_SPINLOCK_BACKEND FREE_RTOS_KERNEL_LIB
                               esy_port_lvgl_libraries esy_port_segger_rtt_root)
                    if(DEFINED ${{output}})
                        message(FATAL_ERROR "Loading post_project executed a hook: ${{output}}")
                    endif()
                endforeach()
            endfunction()
            load_definitions_only()
            if(DEFINED FIXTURE_PROMOTED OR EXISTS "{self.project}/events.txt")
                message(FATAL_ERROR "Loading post_project imported SDK state or exported variables")
            endif()
        ''')
        self.command(["cmake", "-P", str(self.project / "load_definitions.cmake")])

    def test_script_mode_preserves_defaults_without_importing_the_sdk(self):
        self.write("script_defaults.cmake", f'''
            set(ESY_TARGET_DEVICE FIXTURE_MCU)
            set(ESY_PROJECT_ROOT "{self.project}")
            include("{self.library}/cmake/esy_early_import.cmake")
            if(NOT ESY_FIXTURE_DEFAULT STREQUAL "from port" OR
               NOT ESY_FIXTURE_REVISION STREQUAL "r1")
                message(FATAL_ERROR "Script mode lost device outputs or port defaults")
            endif()
            if(EXISTS "{self.project}/events.txt")
                message(FATAL_ERROR "Script mode imported the SDK/toolchain")
            endif()
        ''')
        self.command(["cmake", "-P", str(self.project / "script_defaults.cmake")])

    def test_hook_dispatch_preserves_macro_outputs_and_parent_scope(self):
        self.write("hook_scope.cmake", f'''
            include("{self.library}/cmake/esy_port.cmake")
            set(FIXTURE_PROMOTED "old parent value")
            function(invoke_port_hooks)
                set(ESY_BUILD_PORT fixture)
                set(FIXTURE_PROMOTED "caller local value")
                esy_call_port_hook(configure_build_defaults)
                if(NOT ESY_FIXTURE_DEFAULT STREQUAL "from port")
                    message(FATAL_ERROR "Hook dispatch added a scope around macro outputs")
                endif()
                esy_call_port_hook(export_sdk_variables)
                if(NOT FIXTURE_PROMOTED STREQUAL "caller local value")
                    message(FATAL_ERROR "The export hook wrote to the wrong parent scope")
                endif()
                esy_call_port_hook(select_atomic_backend)
            endfunction()
            invoke_port_hooks()
            if(NOT FIXTURE_PROMOTED STREQUAL "from library directory" OR
               DEFINED ESY_FIXTURE_DEFAULT)
                message(FATAL_ERROR "Hook dispatch changed output ownership")
            endif()
        ''')
        self.command(["cmake", "-P", str(self.project / "hook_scope.cmake")])

    def test_stage_lookup_rejects_traversal_and_clears_a_missing_result(self):
        for port, stage in (("../pico", "pre_project"), ("fixture", "../pre_project")):
            with self.subTest(port=port, stage=stage):
                self.write("lookup.cmake", f'''
                    include("{self.library}/cmake/esy_port.cmake")
                    set(ESY_BUILD_PORT "{port}")
                    esy_get_port_file("{stage}" result)
                ''')
                result = subprocess.run(["cmake", "-P", str(self.project / "lookup.cmake")],
                                        capture_output=True, text=True)
                self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
        self.write("lookup.cmake", f'''
            include("{self.library}/cmake/esy_port.cmake")
            set(ESY_BUILD_PORT fixture)
            set(result "stale stage")
            esy_get_port_file("unused_optional_entry" result)
            if(NOT result STREQUAL "")
                message(FATAL_ERROR "Missing optional stage retained an old result")
            endif()
        ''')
        self.command(["cmake", "-P", str(self.project / "lookup.cmake")])

    def test_interrupt_registration_rejects_a_static_library(self):
        with (self.project / "CMakeLists.txt").open("a", encoding="utf-8") as output:
            output.write("\nesy_register_interrupt_target(esy_hal_backend esy_hal_backend)\n")
        result = subprocess.run(["cmake", "-S", str(self.project), "-B", str(self.project / "out")],
                                capture_output=True, text=True)
        self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("Interrupt target must be an OBJECT library", result.stdout + result.stderr)

    def test_standalone_provider_import_recomputes_stale_cached_port(self):
        self.write("CMakeLists.txt", r'''
            cmake_minimum_required(VERSION 3.20)
            project(stale_provider_port C)
            set(ESY_TARGET_DEVICE FIXTURE_MCU)
            set(ESY_TARGET_PLATFORM ESP32 CACHE STRING "Stale platform")
            set(ESY_BUILD_PORT esp32 CACHE STRING "Stale port")
            set(ESY_RTOS None)
            set(FIXTURE_FEATURES_LOADED TRUE)
            set(ESY_PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
            include("${CMAKE_CURRENT_SOURCE_DIR}/library/cmake/esy_third_party_import.cmake")
            if(NOT ESY_TARGET_PLATFORM STREQUAL "FIXTURE" OR
               NOT ESY_BUILD_PORT STREQUAL "fixture" OR NOT TARGET fixture_sdk)
                message(FATAL_ERROR "Provider import trusted a stale port")
            endif()
            if(DEFINED CACHE{ESY_BUILD_PORT} OR DEFINED CACHE{ESY_TARGET_PLATFORM})
                message(FATAL_ERROR "Provider import retained derived cache entries")
            endif()
        ''')
        self.command(["cmake", "-S", str(self.project), "-B", str(self.project / "out")])

    def test_build_overrides_cannot_replace_or_cache_any_derived_target_field(self):
        fields = {
            "ESY_BUILD_PORT": "fixture",
            "ESY_BOARD_PLATFORM_PATH": "fixture_board",
            "ESY_SPECS_PLATFORM_PATH": "fixture_specs",
            "ESY_FIXTURE_REVISION": "r1",
        }
        changes = []
        for field, value in fields.items():
            changes.extend((
                f"set({field} incorrect)",
                f"unset({field})",
                f'set({field} "{value}" CACHE STRING "Unexpected derived cache" FORCE)',
                f'set({field} incorrect CACHE STRING "Unexpected derived cache" FORCE)',
            ))
        changes.extend((
            "set(ESY_TARGET_EXPORT_FIELDS ESY_TARGET_DEVICE)",
            "unset(ESY_TARGET_EXPORT_FIELDS)",
            'set(ESY_TARGET_EXPORT_FIELDS "${ESY_TARGET_EXPORT_FIELDS}" CACHE STRING "Unexpected registry cache")',
        ))
        for change in changes:
            with self.subTest(change=change):
                self.write("cmake/esy_build_config.cmake", change + "\n")
                result = subprocess.run(
                    ["cmake", "-DESY_TARGET_DEVICE=FIXTURE_MCU",
                     f"-DESY_PROJECT_ROOT={self.project}",
                     "-P", str(self.library / "cmake/esy_early_import.cmake")],
                    capture_output=True, text=True)
                self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
                self.assertIn("must not change platform/device selection", result.stdout + result.stderr)

    def test_standalone_firmware_cannot_skip_device_checks_through_stale_port(self):
        self.write("stub.c", "int image_symbol(void) { return 0; }\n")
        self.write("CMakeLists.txt", r'''
            cmake_minimum_required(VERSION 3.20)
            project(stale_firmware_port C)
            set(ESY_TARGET_DEVICE PICO2_W)
            set(ESY_TARGET_PLATFORM ESP32 CACHE STRING "Stale platform")
            set(ESY_BUILD_PORT esp32 CACHE STRING "Stale port")
            add_library(esy_opt_default INTERFACE)
            add_library(image STATIC stub.c)
            set(ESY_FIRMWARE_TARGET image)
            include("${CMAKE_CURRENT_SOURCE_DIR}/library/cmake/esy_firmware.cmake")
        ''')
        result = subprocess.run(["cmake", "-S", str(self.project), "-B", str(self.project / "out")],
                                capture_output=True, text=True)
        self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("must be an executable", result.stdout + result.stderr)

    def test_explicit_invalid_device_cannot_select_an_inherited_port(self):
        self.write("stub.c", "int image_symbol(void) { return 0; }\n")
        for entry in ("providers", "firmware", "coredump"):
            for device in ("", "OFF"):
                with self.subTest(entry=entry, device=device):
                    if entry == "coredump":
                        invoke = 'add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/library/esy_middleware/coredump" coredump)'
                    else:
                        filename = "esy_third_party_import" if entry == "providers" else "esy_firmware"
                        invoke = f'include("${{CMAKE_CURRENT_SOURCE_DIR}}/library/cmake/{filename}.cmake")'
                    self.write("CMakeLists.txt", r'''
                        cmake_minimum_required(VERSION 3.20)
                        project(invalid_device C)
                        set(ESY_TARGET_DEVICE "@DEVICE@")
                        set(ESY_BUILD_PORT esp32 CACHE STRING "Stale port")
                        add_library(esy_opt_default INTERFACE)
                        add_library(image STATIC stub.c)
                        set(ESY_FIRMWARE_TARGET image)
                        @INVOKE@
                    '''.replace("@DEVICE@", device).replace("@INVOKE@", invoke))
                    result = subprocess.run(
                        ["cmake", "-S", str(self.project), "-B", str(self.project / "out")],
                        capture_output=True, text=True)
                    self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
                    self.assertIn("ESY_TARGET_DEVICE", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
