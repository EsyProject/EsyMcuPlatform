/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include <float.h>
#include <math.h>
#include <string.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_middleware/numerics/esy_numerics_quantize.h"

#include <stdint.h>

void setUp(void) {}

void tearDown(void) {}

void test_numerics_quantize_converts_f32_to_q15_and_clamps(void) {
  const float input[] = {-10.0f, 0.5f, 40000.0f};
  const float mean[] = {0.0f, 0.0f, 0.0f};
  const float std[] = {1.0f, 1.0f, 1.0f};
  const float scale[] = {4000.0f, 10.0f, 1.0f};
  int16_t output[3] = {0};

  TEST_ASSERT_EQUAL_INT(
      ESY_OK,
      esy_numerics_quantize_f32_to_q15(input, output, mean, std, scale, 3u));
  TEST_ASSERT_EQUAL_INT16(-32768, output[0]);
  TEST_ASSERT_EQUAL_INT16(5, output[1]);
  TEST_ASSERT_EQUAL_INT16(32767, output[2]);
}

void test_numerics_quantize_rejects_invalid_arguments(void) {
  const float input[] = {1.0f};
  const float mean[] = {0.0f};
  const float zero_std[] = {0.0f};
  const float scale[] = {1.0f};
  int16_t output[1] = {0};

  TEST_ASSERT_EQUAL_INT(
      ESY_ERR_INVALID_ARG,
      esy_numerics_quantize_f32_to_q15(NULL, output, mean, zero_std, scale, 1u));
  TEST_ASSERT_EQUAL_INT(
      ESY_ERR_INVALID_ARG,
      esy_numerics_quantize_f32_to_q15(input, output, mean, zero_std, scale, 1u));
}

void test_numerics_quantize_accepts_zero_length_without_touching_output(void) {
  int16_t output = 123;

  TEST_ASSERT_EQUAL_INT(
      ESY_OK,
      esy_numerics_quantize_f32_to_q15(NULL, &output, NULL, NULL, NULL, 0u));
  TEST_ASSERT_EQUAL_INT16(123, output);
}

void test_numerics_quantize_handles_extreme_finite_values_and_halfway_rounding(void) {
  const float input[] = {1e10f, -1e10f, 0.5f, -0.5f, 32766.5f, -32767.5f,
                         FLT_MAX, -FLT_MAX, FLT_MAX};
  const float mean[] = {0, 0, 0, 0, 0, 0, -FLT_MAX, FLT_MAX, -FLT_MAX};
  const float std[] = {1, 1, 1, 1, 1, 1, FLT_MIN, FLT_MIN, FLT_MIN};
  const float scale[] = {1, 1, 1, 1, 1, 1, FLT_MAX, FLT_MAX, 0};
  const int16_t expected[] = {32767, -32768, 1, -1, 32767, -32768, 32767, -32768, 0};
  int16_t output[9];
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      esy_numerics_quantize_f32_to_q15(input, output, mean, std, scale, 9));
  TEST_ASSERT_EQUAL_INT16_ARRAY(expected, output, 9);
}

void test_numerics_quantize_invalid_arrays_preserve_all_output(void) {
  float input[] = {1, 2}, mean[] = {0, 0}, std[] = {1, 1}, scale[] = {1, 1};
  int16_t output[] = {123, 456};
  const float invalid[] = {NAN, INFINITY, -INFINITY};
  float *arrays[] = {input, mean, std, scale};
  for (int a = 0; a < 4; ++a) {
    float saved = arrays[a][1];
    for (int v = 0; v < 3; ++v) {
      arrays[a][1] = invalid[v];
      TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
          esy_numerics_quantize_f32_to_q15(input, output, mean, std, scale, 2));
      TEST_ASSERT_EQUAL_INT16(123, output[0]);
      TEST_ASSERT_EQUAL_INT16(456, output[1]);
    }
    arrays[a][1] = saved;
  }
  std[1] = -0.0f;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_numerics_quantize_f32_to_q15(input, output, mean, std, scale, 2));
  TEST_ASSERT_EQUAL_INT16(123, output[0]);
  TEST_ASSERT_EQUAL_INT16(456, output[1]);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_numerics_quantize_f32_to_q15(input, NULL, mean, std, scale, 2));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_numerics_quantize_f32_to_q15(input, output, NULL, std, scale, 2));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_numerics_quantize_f32_to_q15(input, output, mean, NULL, scale, 2));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_numerics_quantize_f32_to_q15(input, output, mean, std, NULL, 2));
}
