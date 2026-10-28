# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Resolve host-tool packages through the Pico SDK initialization boundary."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
POST_PROJECT = ROOT / "cmake/ports/pico/post_project.cmake"
EARLY_IMPORT = ROOT / "cmake/esy_early_import.cmake"
VERSION = "9.8.7"
TOOLS = ("pioasm", "picotool")


class PicoHostToolTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="esy Pico host tools ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "consumer"
        self.source.mkdir()
        self.sdk = self.root / "installer/sdk/renamed-sdk"
        self.sdk.mkdir(parents=True)
        self.packages = {}
        for tool in TOOLS:
            parent = "tools" if tool == "pioasm" else "picotool"
            directory = self.root / "installer" / parent / VERSION / tool
            self.package(directory, tool, "installer")
            self.packages[tool] = directory
        self.environment = dict(os.environ)
        for name in ("pioasm_DIR", "picotool_DIR", "pioasm_ROOT", "picotool_ROOT",
                     "CMAKE_PREFIX_PATH", "CMAKE_SYSTEM_PREFIX_PATH",
                     "PICOTOOL_FORCE_FETCH_FROM_GIT"):
            self.environment.pop(name, None)

    def package(self, directory, tool, identity, version=VERSION):
        directory.mkdir(parents=True, exist_ok=True)
        (directory / f"{tool}Config.cmake").write_text(f'''
if(NOT TARGET {tool})
  add_executable({tool} IMPORTED GLOBAL)
  set_target_properties({tool} PROPERTIES IMPORTED_LOCATION "{directory / tool}"
                        TEST_PROVIDER "{identity}")
endif()
''')
        (directory / f"{tool}ConfigVersion.cmake").write_text(f'''
set(PACKAGE_VERSION "{version}")
if(PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)
  set(PACKAGE_VERSION_EXACT TRUE)
  set(PACKAGE_VERSION_COMPATIBLE TRUE)
else()
  set(PACKAGE_VERSION_COMPATIBLE FALSE)
endif()
''')

    def configure(self, before="", after="", args=(), environment=None,
                  sdk=None, lookup=True):
        sdk = sdk or self.sdk
        (self.source / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(pico_host_tools LANGUAGES NONE)
set(ESY_TARGET_PLATFORM PICO2_W)
set(PICO_PLATFORM rp2350-arm-s)
set(ESY_RTOS None)
set(PICO_SDK_PATH "{sdk}")
set(PICO_SDK_VERSION_STRING "{VERSION}")
set(CMAKE_FIND_USE_PACKAGE_REGISTRY FALSE)
set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY FALSE)
{before}
# This thin SDK fixture leaves package selection to real CMake CONFIG lookup.
# A missing package remains observable as the SDK's source-build fallback.
macro(pico_sdk_init)
  if(TEST_PARENT_SDK)
    message(FATAL_ERROR "The parent-owned SDK must not be initialized again")
  endif()
  if({"TRUE" if lookup else "FALSE"})
    foreach(host_tool IN ITEMS pioasm picotool)
      if(NOT TARGET ${{host_tool}})
        if(host_tool STREQUAL "picotool" AND
           (PICOTOOL_FORCE_FETCH_FROM_GIT OR "$ENV{{PICOTOOL_FORCE_FETCH_FROM_GIT}}"))
          set(picotool_mode forced-source)
        else()
          find_package(${{host_tool}} {VERSION} QUIET CONFIG NO_CMAKE_FIND_ROOT_PATH)
          if(NOT ${{host_tool}}_FOUND)
            set(${{host_tool}}_mode sdk-source-fallback)
          endif()
        endif()
      endif()
    endforeach()
  endif()
  add_library(pico_stdlib INTERFACE)
endmacro()
include("{POST_PROJECT}")
esy_port_pico_initialize_sdk_dependencies()
{after}
file(WRITE "${{CMAKE_BINARY_DIR}}/prefixes.txt" "${{CMAKE_SYSTEM_PREFIX_PATH}}")
foreach(host_tool IN ITEMS pioasm picotool)
  if(TARGET ${{host_tool}})
    get_target_property(selected ${{host_tool}} TEST_PROVIDER)
  else()
    set(selected "${{${{host_tool}}_mode}}")
  endif()
  file(WRITE "${{CMAKE_BINARY_DIR}}/${{host_tool}}.txt" "${{selected}}")
  file(WRITE "${{CMAKE_BINARY_DIR}}/${{host_tool}}-versions.txt"
       "${{${{host_tool}}_CONSIDERED_VERSIONS}}")
endforeach()
''')
        result = subprocess.run(
            ["cmake", "-S", str(self.source), "-B", str(self.source / "out"), *args],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            env=self.environment if environment is None else environment, check=False)
        self.assertEqual(0, result.returncode, result.stdout)
        return {tool: (self.source / f"out/{tool}.txt").read_text() for tool in TOOLS}

    def prefixes(self):
        return (self.source / "out/prefixes.txt").read_text().split(";")

    def test_installer_candidates_use_sdk_metadata_and_canonical_path(self):
        alias = self.root / "sdk-alias"
        alias.symlink_to(self.sdk, target_is_directory=True)
        self.assertEqual(dict.fromkeys(TOOLS, "installer"), self.configure(sdk=alias))
        for directory in self.packages.values():
            self.assertIn(str(directory), self.prefixes())

    def test_explicit_package_directories_keep_precedence(self):
        for source in ("normal", "cache", "environment"):
            with self.subTest(source=source):
                explicit = self.root / source
                for tool in TOOLS:
                    self.package(explicit / tool, tool, source)
                before = ""
                args = ()
                environment = dict(self.environment)
                if source == "normal":
                    before = "\n".join(f'set({tool}_DIR "{explicit / tool}")'
                                       for tool in TOOLS)
                elif source == "cache":
                    args = tuple(f"-D{tool}_DIR:PATH={explicit / tool}" for tool in TOOLS)
                else:
                    # Remove the earlier configure's standard find_package cache.
                    args = tuple(f"-U{tool}_DIR" for tool in TOOLS)
                    environment.update({f"{tool}_DIR": str(explicit / tool)
                                        for tool in TOOLS})
                self.assertEqual(dict.fromkeys(TOOLS, source),
                                 self.configure(before=before, args=args,
                                                environment=environment))

    def test_standard_prefix_inputs_keep_precedence(self):
        for variable, source in (("CMAKE_PREFIX_PATH", "normal"),
                                 ("CMAKE_PREFIX_PATH", "environment"),
                                 ("CMAKE_SYSTEM_PREFIX_PATH", "normal")):
            with self.subTest(variable=variable, source=source):
                prefix = self.root / f"{variable}-{source}"
                for tool in TOOLS:
                    self.package(prefix / "lib/cmake" / tool, tool, "standard-prefix")
                environment = dict(self.environment)
                before = ""
                if source == "environment":
                    environment[variable] = str(prefix)
                else:
                    before = f'list(APPEND {variable} "{prefix}")'
                args = tuple(f"-U{tool}_DIR" for tool in TOOLS)
                self.assertEqual(dict.fromkeys(TOOLS, "standard-prefix"),
                                 self.configure(before=before, args=args,
                                                environment=environment))

    def test_empty_and_notfound_hints_allow_standard_fallback(self):
        for value in ("", "tool-NOTFOUND"):
            with self.subTest(value=value):
                args = tuple(f"-D{tool}_DIR:PATH={value}" for tool in TOOLS)
                self.assertEqual(dict.fromkeys(TOOLS, "installer"),
                                 self.configure(args=args))

    def test_existing_tool_targets_are_preserved(self):
        before = "\n".join(
            f'add_executable({tool} IMPORTED GLOBAL)\n'
            f'set_property(TARGET {tool} PROPERTY TEST_PROVIDER caller)'
            for tool in TOOLS)
        self.assertEqual(dict.fromkeys(TOOLS, "caller"), self.configure(before=before))
        for directory in self.packages.values():
            self.assertNotIn(str(directory), self.prefixes())

    def test_parent_initialized_sdk_is_not_modified(self):
        before = '''
set(TEST_PARENT_SDK TRUE)
add_library(pico_stdlib INTERFACE)
'''
        self.configure(before=before)
        for directory in self.packages.values():
            self.assertNotIn(str(directory), self.prefixes())

    def test_custom_sdk_layout_keeps_source_build_fallback(self):
        sdk = self.root / "custom/sdk"
        sdk.mkdir(parents=True)
        self.assertEqual(dict.fromkeys(TOOLS, "sdk-source-fallback"),
                         self.configure(sdk=sdk))
        for directory in self.packages.values():
            self.assertNotIn(str(directory), self.prefixes())

    def test_incompatible_candidates_still_undergo_sdk_version_check(self):
        for tool, directory in self.packages.items():
            self.package(directory, tool, "wrong-version", version="1.0.0")
        self.assertEqual(dict.fromkeys(TOOLS, "sdk-source-fallback"), self.configure())
        for tool in TOOLS:
            versions = (self.source / f"out/{tool}-versions.txt").read_text()
            self.assertIn("1.0.0", versions)

    def test_force_fetch_does_not_add_picotool_candidate(self):
        for source in ("normal", "environment"):
            with self.subTest(source=source):
                environment = dict(self.environment)
                before = ""
                if source == "normal":
                    before = "set(PICOTOOL_FORCE_FETCH_FROM_GIT ON)"
                else:
                    environment["PICOTOOL_FORCE_FETCH_FROM_GIT"] = "1"
                self.assertEqual({"pioasm": "installer", "picotool": "forced-source"},
                                 self.configure(before=before, environment=environment))
                self.assertNotIn(str(self.packages["picotool"]), self.prefixes())

    def test_false_force_fetch_environment_keeps_installer_candidate(self):
        for value in ("", "OFF", "0"):
            with self.subTest(value=value):
                environment = dict(self.environment, PICOTOOL_FORCE_FETCH_FROM_GIT=value)
                self.assertEqual(dict.fromkeys(TOOLS, "installer"),
                                 self.configure(environment=environment))

    def test_sdk_without_version_metadata_keeps_sdk_lookup_unchanged(self):
        self.assertEqual(dict.fromkeys(TOOLS, "sdk-source-fallback"), self.configure(
            before="unset(PICO_SDK_VERSION_STRING)"))
        for directory in self.packages.values():
            self.assertNotIn(str(directory), self.prefixes())

    def test_missing_package_config_is_not_a_candidate(self):
        for tool, directory in self.packages.items():
            (directory / f"{tool}Config.cmake").unlink()
        self.assertEqual(dict.fromkeys(TOOLS, "sdk-source-fallback"), self.configure())
        for directory in self.packages.values():
            self.assertNotIn(str(directory), self.prefixes())

    def test_standard_system_search_opt_out_is_preserved(self):
        self.assertEqual(dict.fromkeys(TOOLS, "sdk-source-fallback"), self.configure(
            before="set(CMAKE_FIND_USE_CMAKE_SYSTEM_PATH FALSE)"))

    def test_candidates_do_not_expose_installer_parent_to_target_packages(self):
        target_prefix = self.root / "installer/tools" / VERSION
        self.package(target_prefix / "lib/cmake/unrelated", "unrelated", "unexpected")
        self.configure(after='''
find_package(unrelated QUIET CONFIG NO_CMAKE_FIND_ROOT_PATH)
if(unrelated_FOUND OR TARGET unrelated)
  message(FATAL_ERROR "Host-tool discovery exposed the installer parent")
endif()
''')
        self.assertNotIn(str(target_prefix), self.prefixes())

    def test_sdk_switch_recomputes_optional_prefixes_without_private_cache(self):
        self.configure(lookup=False)
        cache = (self.source / "out/CMakeCache.txt").read_text()
        self.assertNotIn("CMAKE_SYSTEM_PREFIX_PATH:", cache)
        for tool in TOOLS:
            self.assertNotIn(f"{tool}_DIR:", cache)
        sdk = self.root / "second-installer/sdk/other-name"
        sdk.mkdir(parents=True)
        for tool in TOOLS:
            parent = "tools" if tool == "pioasm" else "picotool"
            directory = self.root / "second-installer" / parent / VERSION / tool
            self.package(directory, tool, "second-installer")
        self.assertEqual(dict.fromkeys(TOOLS, "second-installer"), self.configure(sdk=sdk))
        for directory in self.packages.values():
            self.assertNotIn(str(directory), self.prefixes())

    def test_existing_package_cache_is_a_standard_explicit_input(self):
        explicit = self.root / "explicit"
        args = []
        for tool in TOOLS:
            self.package(explicit / tool, tool, "explicit-cache")
            args.append(f"-D{tool}_DIR:PATH={explicit / tool}")
        self.assertEqual(dict.fromkeys(TOOLS, "explicit-cache"), self.configure(args=args))
        sdk = self.root / "custom/sdk"
        sdk.mkdir(parents=True)
        self.assertEqual(dict.fromkeys(TOOLS, "explicit-cache"), self.configure(sdk=sdk))

    def test_script_discovery_for_pico_and_other_targets_does_not_import_tools(self):
        for device in ("PICO2_W", "STM32F407ZG"):
            with self.subTest(device=device):
                script = self.root / "discover.cmake"
                script.write_text(f'''
set(ESY_TARGET_DEVICE {device})
set(ESY_PROJECT_ROOT "{self.source}")
set(CMAKE_SYSTEM_PREFIX_PATH sentinel)
include("{EARLY_IMPORT}")
if(NOT CMAKE_SYSTEM_PREFIX_PATH STREQUAL "sentinel")
  message(FATAL_ERROR "Dependency discovery modified host-tool search paths")
endif()
''')
                result = subprocess.run(["cmake", "-P", str(script)], text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        env=self.environment, check=False)
                self.assertEqual(0, result.returncode, result.stdout)


if __name__ == "__main__":
    unittest.main()
