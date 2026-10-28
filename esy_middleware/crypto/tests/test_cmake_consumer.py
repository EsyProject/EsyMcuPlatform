#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Check native provider integration and public C/C++ header propagation."""

import runpy
from pathlib import Path
import subprocess
import tempfile
import unittest


MODULE = Path(__file__).resolve().parents[1]
PLATFORM = MODULE.parents[1]
cached_provider = runpy.run_path(
    PLATFORM / "esy_test/utils/esy_provider_paths.py")["cached_provider"]


class CryptoCmakeConsumerTests(unittest.TestCase):
    def test_missing_provider_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix="esy-crypto-missing-") as directory:
            source = Path(directory)
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(crypto_missing C)\n"
                "add_library(esy_common_defs INTERFACE)\n"
                "add_library(esy_opt_default INTERFACE)\n"
                f'add_subdirectory("{MODULE}" crypto)\n'
            )
            result = subprocess.run(
                ["cmake", "-S", str(source), "-B", str(source / "build")],
                text=True, capture_output=True, check=False,
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("esy_mbedtls_provider", result.stderr)

    def test_real_provider_public_headers_and_c_cpp_consumers(self):
        provider_input = cached_provider("mbedtls")
        if not provider_input:
            self.skipTest("Run ./setup.sh or set ESY_MBEDTLS_PATH for native provider tests")
        provider = Path(provider_input).resolve()
        self.assertTrue((provider / "include/mbedtls/ctr_drbg.h").is_file())
        with tempfile.TemporaryDirectory(prefix="esy-crypto-native-") as directory:
            source = Path(directory)
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(crypto_native C CXX)\n"
                "set(ENABLE_PROGRAMS OFF CACHE BOOL \"\" FORCE)\n"
                "set(ENABLE_TESTING OFF CACHE BOOL \"\" FORCE)\n"
                f'add_subdirectory("{provider}" mbedtls)\n'
                "add_library(esy_common_defs INTERFACE)\n"
                "target_include_directories(esy_common_defs INTERFACE\n"
                f'  "{PLATFORM / "esy_common/defs/include"}")\n'
                "add_library(esy_opt_default INTERFACE)\n"
                "add_library(esy_mbedtls_provider INTERFACE)\n"
                "target_link_libraries(esy_mbedtls_provider INTERFACE mbedcrypto)\n"
                f'add_subdirectory("{MODULE}" crypto)\n'
                "add_executable(consumer_c consumer.c)\n"
                "target_link_libraries(consumer_c PRIVATE Esy::crypto)\n"
                "add_executable(consumer_cpp consumer.cpp)\n"
                "target_link_libraries(consumer_cpp PRIVATE Esy::crypto)\n"
            )
            common = (
                '#include "esy_middleware/crypto/esy_crypto.h"\n'
                '#include "esy_middleware/crypto/esy_crypto_mbedtls.h"\n'
                "int main(void) {\n"
                "  esy_crypto_random_t random = {0};\n"
                "  return esy_crypto_random_deinit(&random);\n"
                "}\n"
            )
            (source / "consumer.c").write_text(common)
            (source / "consumer.cpp").write_text(common)
            for command in (
                ["cmake", "-S", str(source), "-B", str(source / "build")],
                ["cmake", "--build", str(source / "build"), "--parallel", "4"],
                [str(source / "build/consumer_c")],
                [str(source / "build/consumer_cpp")],
            ):
                result = subprocess.run(command, text=True, capture_output=True,
                                        check=False)
                self.assertEqual(0, result.returncode,
                                 result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
