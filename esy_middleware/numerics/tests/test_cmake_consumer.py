# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Exercise numerics target usage, feature isolation and consistent model layouts."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
PROJECT = """\
cmake_minimum_required(VERSION 3.20)
project(numerics_consumer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
include(esy_feature_import.cmake)
add_library(esy_opt_speed INTERFACE)
target_compile_definitions(esy_opt_speed INTERFACE NUMERICS_PRIVATE_SENTINEL)
add_subdirectory("${PLATFORM_ROOT}/esy_common/defs" common_defs)
add_subdirectory("${PLATFORM_ROOT}/esy_middleware" middleware)
if(ENABLE_ESY_NUMERICS)
  get_target_property(sources esy_numerics SOURCES)
  if(ENABLE_ESY_NUMERICS_QUANTIZE AND NOT "${sources}" MATCHES "esy_numerics_quantize.c")
    message(FATAL_ERROR "Quantize source missing")
  elseif(NOT ENABLE_ESY_NUMERICS_QUANTIZE AND "${sources}" MATCHES "esy_numerics_quantize.c")
    message(FATAL_ERROR "Disabled quantize source leaked")
  endif()
  foreach(language c cpp)
    add_executable(consumer_${language} consumer.${language})
    target_link_libraries(consumer_${language} PRIVATE esy_numerics)
    if(ENABLE_ESY_NUMERICS_QUANTIZE)
      target_compile_definitions(consumer_${language} PRIVATE TEST_QUANTIZE)
    endif()
  endforeach()
elseif(TARGET esy_numerics)
  message(FATAL_ERROR "Disabled numerics target leaked")
endif()
"""
CONSUMER = """\
#include "esy_middleware/numerics/esy_linalg.h"
#include "esy_middleware/numerics/esy_poly_fit.h"
#include "esy_middleware/numerics/esy_numerics_quantize.h"
#include <math.h>
#ifdef NUMERICS_PRIVATE_SENTINEL
#error "Private options leaked"
#endif
int main(void) {
  const float X[] = {0, 1}, Y[] = {1, 3};
  float A[2];
  struct { esy_poly_model_t model; unsigned guard; } state;
  state.guard = 0x12345678u;
  if (esy_poly_fit(X, Y, 2, 1, 0, A, &state.model, 0) != ESY_OK) return 1;
  if (state.guard != 0x12345678u || fabsf(A[1] - 2) > 0.001f) return 2;
#ifdef TEST_QUANTIZE
  float zero = 0, one = 1, huge = 1e10f; int16_t q;
  if (esy_numerics_quantize_f32_to_q15(&huge, &q, &zero, &one, &one, 1) || q != 32767) return 3;
#endif
  return 0;
}
"""


class ConsumerTests(unittest.TestCase):
    def run_case(self, enabled=True, quantize=True, config="", prefix="", error=None):
        with tempfile.TemporaryDirectory(prefix="numerics consumer ") as directory:
            p = Path(directory)
            (p / "CMakeLists.txt").write_text(PROJECT)
            (p / "esy_feature_import.cmake").write_text(
                f"set(ENABLE_ESY_NUMERICS {'ON' if enabled else 'OFF'})\n"
                f"set(ENABLE_ESY_NUMERICS_QUANTIZE {'ON' if quantize else 'OFF'})\n" + config)
            for language in ("c", "cpp"):
                (p / f"consumer.{language}").write_text(prefix + CONSUMER)
            result = subprocess.run(["cmake", "-S", str(p), "-B", str(p / "out"),
                                     f"-DPLATFORM_ROOT={ROOT}"], capture_output=True, text=True)
            if result.returncode == 0:
                result = subprocess.run(["cmake", "--build", str(p / "out")],
                                        capture_output=True, text=True)
            if error:
                self.assertNotEqual(0, result.returncode)
                self.assertIn(error, result.stdout + result.stderr)
                return
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            if enabled:
                for language in ("c", "cpp"):
                    subprocess.run([str(p / "out" / f"consumer_{language}")], check=True)

    def test_feature_combinations_without_emlearn(self):
        for enabled in (False, True):
            for quantize in (False, True):
                with self.subTest(enabled=enabled, quantize=quantize):
                    self.run_case(enabled, quantize)

    def test_shared_nondefault_limits(self):
        self.run_case(config="set(ESY_LINALG_MAX_M 4)\nset(ESY_FIT_MAX_M 3)\n"
                             "set(ESY_FIT_MAX_DEG 2)\nset(ESY_FIT_MAX_N 8)\n")

    def test_local_layout_overrides_are_compile_errors(self):
        for macro, value in (("ESY_FIT_MAX_M", 2), ("ESY_FIT_MAX_DEG", 1),
                             ("ESY_LINALG_MAX_M", 4), ("ESY_FIT_MAX_N", 16)):
            with self.subTest(macro=macro):
                self.run_case(prefix=f"#define {macro} {value}\n",
                              error=f"{macro} differs from the esy_numerics build configuration")

    def test_invalid_limits_fail_configuration(self):
        for config in ("set(ESY_FIT_MAX_M 0)", "set(ESY_LINALG_MAX_M 4)",
                       "set(ESY_FIT_MAX_N 0)", "set(ESY_FIT_MAX_DEG 8)"):
            with self.subTest(config=config):
                self.run_case(config=config, error="Inconsistent esy_numerics numerical limits")
        self.run_case(config="set(ESY_FIT_MAX_M invalid)", error="nonnegative decimal integer")

    def test_direct_header_rejects_local_override(self):
        for language, compiler, standard in (("c", "cc", "c11"), ("c++", "c++", "c++17")):
            result = subprocess.run([compiler, f"-std={standard}", "-x", language,
                                     "-fsyntax-only", "-", "-DESY_FIT_MAX_M=2",
                                     f"-I{ROOT}/esy_middleware/numerics/include",
                                     f"-I{ROOT}/esy_common/defs/include"],
                                    input='#include "esy_middleware/numerics/esy_fit.h"\n',
                                    text=True, capture_output=True)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("differs from the esy_numerics build configuration", result.stderr)


if __name__ == "__main__":
    unittest.main()
