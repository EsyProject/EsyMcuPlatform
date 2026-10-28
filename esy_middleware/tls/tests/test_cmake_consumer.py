#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Check disabled-feature isolation and explicit native provider assembly."""

from pathlib import Path
import os
import runpy
import subprocess
import tempfile
import unittest


PLATFORM = Path(__file__).resolve().parents[3]
cached_provider = runpy.run_path(
    PLATFORM / "esy_test/utils/esy_provider_paths.py")["cached_provider"]
PRELUDE = """
cmake_minimum_required(VERSION 3.20)
project(tls_consumer LANGUAGES C CXX)
include(esy_feature_import.cmake)
include("${PLATFORM_ROOT}/cmake/esy_mbedtls_provider.cmake")
"""


class TLSConsumerTests(unittest.TestCase):
    def configure(self, body, *, features="", expected=None, prepare=None):
        with tempfile.TemporaryDirectory(prefix="esy tls consumer ") as directory:
            source = Path(directory)
            (source / "esy_feature_import.cmake").write_text(features, encoding="utf-8")
            (source / "CMakeLists.txt").write_text(PRELUDE + body, encoding="utf-8")
            if prepare:
                prepare(source)
            environment = dict(
                os.environ, ESY_PROVIDER_CACHE_DIR=str(source / "empty provider cache"))
            environment.pop("ESY_MBEDTLS_PATH", None)
            result = subprocess.run(
                ["cmake", "-S", str(source), "-B", str(source / "out"),
                 f"-DPLATFORM_ROOT={PLATFORM}"], capture_output=True, text=True, env=environment)
            if expected:
                self.assertNotEqual(0, result.returncode)
                self.assertIn(expected, result.stdout + result.stderr)
            else:
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                result = subprocess.run(["cmake", "--build", str(source / "out"),
                                         "--parallel", "4"], capture_output=True, text=True)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_disabled_security_modules_ignore_invalid_provider_path(self):
        self.configure(r"""
set(ESY_MBEDTLS_PATH "nonexistent provider must remain unused")
add_subdirectory("${PLATFORM_ROOT}/esy_middleware" middleware)
foreach(disabled esy_tls esy_crypto esy_credentials esy_mbedtls_provider)
  if(TARGET ${disabled})
    message(FATAL_ERROR "Disabled security dependency was assembled")
  endif()
endforeach()
""", features="set(ENABLE_ESY_TLS OFF)\nset(ENABLE_ESY_CRYPTO OFF)\n"
                      "set(ENABLE_ESY_CREDENTIALS OFF)\n")

    def test_tls_requires_its_feature_dependencies(self):
        self.configure('add_subdirectory("${PLATFORM_ROOT}/esy_middleware" middleware)\n',
                       features="set(ENABLE_ESY_TLS ON)\nset(ENABLE_ESY_NETWORK OFF)\n",
                       expected="ENABLE_ESY_TLS requires ENABLE_ESY_NETWORK")

    def test_tls_module_reports_missing_native_provider(self):
        self.configure(r"""
foreach(dependency esy_common_defs esy_common_utils esy_network esy_credentials
                   esy_crypto esy_opt_default)
  add_library(${dependency} INTERFACE)
endforeach()
add_subdirectory("${PLATFORM_ROOT}/esy_middleware/tls" tls)
""", expected="esy_tls requires target esy_mbedtls_provider")

    def test_helper_reports_missing_explicit_path(self):
        self.configure("esy_configure_mbedtls_provider()\n", expected="ESY_MBEDTLS_PATH")

    def test_helper_rejects_incomplete_checkout(self):
        self.configure('set(ESY_MBEDTLS_PATH "${CMAKE_CURRENT_SOURCE_DIR}")\n'
                       "esy_configure_mbedtls_provider()\n", expected="complete Mbed TLS 3.6.x")

    def test_helper_rejects_old_and_newer_incompatible_versions(self):
        for version in ("2.28.10", "3.7.0"):
            with self.subTest(version=version):
                def prepare(source):
                    include = source / "provider/include/mbedtls"
                    include.mkdir(parents=True)
                    (include / "version.h").write_text(
                        f'#define MBEDTLS_VERSION_STRING "{version}"\n', encoding="utf-8")
                    (source / "provider/CMakeLists.txt").write_text(
                        'message(FATAL_ERROR "Incompatible provider was executed")\n', encoding="utf-8")
                self.configure('set(ESY_MBEDTLS_PATH "${CMAKE_CURRENT_SOURCE_DIR}/provider")\n'
                               "esy_configure_mbedtls_provider()\n", prepare=prepare,
                               expected="must provide Mbed TLS 3.6.x")

    def test_helper_preserves_existing_consumer_provider(self):
        self.configure(r"""
add_library(esy_mbedtls_provider INTERFACE)
set_property(TARGET esy_mbedtls_provider PROPERTY ESY_TEST_OWNER consumer)
set(ESY_MBEDTLS_PATH "invalid path is intentionally ignored")
esy_configure_mbedtls_provider()
esy_configure_mbedtls_provider()
get_target_property(owner esy_mbedtls_provider ESY_TEST_OWNER)
if(NOT owner STREQUAL "consumer")
  message(FATAL_ERROR "Consumer provider was replaced")
endif()
""")

    def test_generic_public_header_needs_no_native_provider_or_os(self):
        self.configure(r"""
add_subdirectory("${PLATFORM_ROOT}/esy_common/defs" defs)
add_subdirectory("${PLATFORM_ROOT}/esy_middleware/network" network)
foreach(language c cpp)
  add_library(header_${language} OBJECT "consumer.${language}")
  target_include_directories(header_${language} PRIVATE
    "${PLATFORM_ROOT}/esy_middleware/tls/include"
    "${PLATFORM_ROOT}/esy_middleware/credentials/include"
    "${PLATFORM_ROOT}/esy_middleware/crypto/include")
  target_link_libraries(header_${language} PRIVATE esy_network)
endforeach()
""", prepare=self.write_generic_consumers)

    @staticmethod
    def write_generic_consumers(source):
        contents = ('#include "esy_middleware/tls/esy_tls.h"\n'
                    '#if defined(MBEDTLS_VERSION_NUMBER) || defined(INC_FREERTOS_H)\n'
                    '#error Generic TLS header leaked native dependencies\n#endif\n'
                    'int main(void) { esy_tls_t *tls = 0; return tls != 0; }\n')
        for suffix in ("c", "cpp"):
            (source / f"consumer.{suffix}").write_text(contents, encoding="utf-8")

    def test_real_helper_provider_builds_public_c_and_cpp_consumers(self):
        provider = cached_provider("mbedtls")
        if not provider:
            self.skipTest("Run ./setup.sh or set ESY_MBEDTLS_PATH for native provider verification")
        def prepare(source):
            native = ('#include "esy_middleware/tls/esy_tls_mbedtls.h"\n'
                      'int main(void) { esy_tls_t tls = {0}; return esy_tls_deinit(&tls); }\n')
            for suffix in ("c", "cpp"):
                (source / f"consumer.{suffix}").write_text(native, encoding="utf-8")
        self.configure(r"""
esy_configure_mbedtls_provider()
add_library(esy_opt_default INTERFACE)
add_subdirectory("${PLATFORM_ROOT}/esy_common/defs" defs)
add_subdirectory("${PLATFORM_ROOT}/esy_common/utils" utils)
add_subdirectory("${PLATFORM_ROOT}/esy_middleware" middleware)
foreach(language c cpp)
  add_executable(consumer_${language} "consumer.${language}")
  target_link_libraries(consumer_${language} PRIVATE Esy::tls)
endforeach()
""", prepare=prepare, features=f'set(ESY_MBEDTLS_PATH "{Path(provider).resolve()}")\n'
                      "set(ENABLE_ESY_TLS ON)\nset(ENABLE_ESY_NETWORK ON)\n"
                      "set(ENABLE_ESY_CREDENTIALS ON)\nset(ENABLE_ESY_CRYPTO ON)\n")


if __name__ == "__main__":
    unittest.main()
