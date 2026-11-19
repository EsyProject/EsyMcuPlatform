/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_middleware/adc/esy_adc_ad7276.h"

void setUp(void) {
}

void tearDown(void) {
}

void test_14_clock_frames_extract_all_codes_and_ignore_other_captures(void) {
  for (uint32_t code = 0u; code <= ESY_ADC_AD7276_MAX_CODE; ++code) {
    const uint16_t raw = (uint16_t)(code | UINT16_C(0xf000));
    uint16_t sample = UINT16_MAX;
    TEST_ASSERT_EQUAL(ESY_OK, esy_adc_ad7276_decode(&raw, 1u,
        ESY_ADC_AD7276_FRAME_14_CLOCKS, &sample, 1u));
    TEST_ASSERT_EQUAL_UINT16(code, sample);
  }
}

void test_16_clock_frames_extract_all_codes_and_ignore_leading_and_trailing_bits(void) {
  for (uint32_t code = 0u; code <= ESY_ADC_AD7276_MAX_CODE; ++code) {
    const uint16_t raw = (uint16_t)((code << 2u) | UINT16_C(0xc003));
    uint16_t sample = UINT16_MAX;
    TEST_ASSERT_EQUAL(ESY_OK, esy_adc_ad7276_decode(&raw, 1u,
        ESY_ADC_AD7276_FRAME_16_CLOCKS, &sample, 1u));
    TEST_ASSERT_EQUAL_UINT16(code, sample);
  }
}

void test_block_decode_preserves_order_input_and_unused_output_capacity(void) {
  const uint16_t raw[] = {0xc003u, 0xc007u, 0xd56bu, 0xe003u, 0xffffu};
  const uint16_t expected[] = {0u, 1u, 0x55au, 0x800u, 0xfffu};
  uint16_t samples[] = {0xa55au, 0xa55au, 0xa55au, 0xa55au, 0xa55au, 0xa55au};
  TEST_ASSERT_EQUAL(ESY_OK, esy_adc_ad7276_decode(raw, 5u,
      ESY_ADC_AD7276_FRAME_16_CLOCKS, samples, 6u));
  TEST_ASSERT_EQUAL_UINT16_ARRAY(expected, samples, 5u);
  TEST_ASSERT_EQUAL_HEX16(0xa55au, samples[5]);
  TEST_ASSERT_EQUAL_HEX16(0xd56bu, raw[2]);
}

void test_exact_in_place_decode_accepts_both_formats(void) {
  uint16_t words14[] = {0xf000u, 0xf123u, 0xffffu, 0xa55au};
  const uint16_t expected14[] = {0u, 0x123u, 0xfffu, 0xa55au};
  uint16_t words16[] = {0xc003u, 0xc48fu, 0xffffu, 0xa55au};
  const uint16_t expected16[] = {0u, 0x123u, 0xfffu, 0xa55au};
  TEST_ASSERT_EQUAL(ESY_OK, esy_adc_ad7276_decode(words14, 3u,
      ESY_ADC_AD7276_FRAME_14_CLOCKS, words14, 4u));
  TEST_ASSERT_EQUAL_UINT16_ARRAY(expected14, words14, 4u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_adc_ad7276_decode(words16, 3u,
      ESY_ADC_AD7276_FRAME_16_CLOCKS, words16, 4u));
  TEST_ASSERT_EQUAL_UINT16_ARRAY(expected16, words16, 4u);
}

void test_zero_length_accepts_null_buffers_without_writes(void) {
  uint16_t sentinel = 0x1234u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_adc_ad7276_decode(NULL, 0u,
      ESY_ADC_AD7276_FRAME_14_CLOCKS, NULL, 0u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_adc_ad7276_decode(NULL, 0u,
      ESY_ADC_AD7276_FRAME_16_CLOCKS, &sentinel, 1u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_adc_ad7276_decode(&sentinel, 0u,
      ESY_ADC_AD7276_FRAME_16_CLOCKS, NULL, 0u));
  TEST_ASSERT_EQUAL_HEX16(0x1234u, sentinel);
}

void test_invalid_pointers_capacity_and_formats_preserve_output(void) {
  const uint16_t raw[] = {0x1234u, 0x5678u};
  uint16_t samples[] = {0xa55au, 0x5aa5u};
  const uint16_t expected[] = {0xa55au, 0x5aa5u};
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_adc_ad7276_decode(NULL, 1u,
      ESY_ADC_AD7276_FRAME_16_CLOCKS, samples, 2u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_adc_ad7276_decode(raw, 1u,
      ESY_ADC_AD7276_FRAME_16_CLOCKS, NULL, 2u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_adc_ad7276_decode(raw, 2u,
      ESY_ADC_AD7276_FRAME_16_CLOCKS, samples, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_adc_ad7276_decode(raw, 1u,
      (esy_adc_ad7276_frame_t)-1, samples, 2u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_adc_ad7276_decode(raw, 1u,
      (esy_adc_ad7276_frame_t)15, samples, 2u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_adc_ad7276_decode(NULL, 0u,
      (esy_adc_ad7276_frame_t)0, NULL, 0u));
  TEST_ASSERT_EQUAL_UINT16_ARRAY(expected, samples, 2u);
}

void test_partial_overlap_is_rejected_in_both_directions_before_writing(void) {
  uint16_t words[] = {0x1234u, 0x5678u, 0x9abcu, 0xdef0u};
  const uint16_t expected[] = {0x1234u, 0x5678u, 0x9abcu, 0xdef0u};
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_adc_ad7276_decode(words, 3u,
      ESY_ADC_AD7276_FRAME_16_CLOCKS, &words[1], 3u));
  TEST_ASSERT_EQUAL_UINT16_ARRAY(expected, words, 4u);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_adc_ad7276_decode(&words[1], 3u,
      ESY_ADC_AD7276_FRAME_16_CLOCKS, words, 3u));
  TEST_ASSERT_EQUAL_UINT16_ARRAY(expected, words, 4u);
}

void test_adjacent_used_ranges_are_allowed_even_when_unused_capacity_overlaps(void) {
  uint16_t words[] = {0xc48fu, 0xeaafu, 0xa55au, 0x5aa5u};
  TEST_ASSERT_EQUAL(ESY_OK, esy_adc_ad7276_decode(words, 2u,
      ESY_ADC_AD7276_FRAME_16_CLOCKS, &words[2], 2u));
  TEST_ASSERT_EQUAL_HEX16(0xc48fu, words[0]);
  TEST_ASSERT_EQUAL_HEX16(0xeaafu, words[1]);
  TEST_ASSERT_EQUAL_HEX16(0x123u, words[2]);
  TEST_ASSERT_EQUAL_HEX16(0xaabu, words[3]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_adc_ad7276_decode(&words[2], 2u,
      ESY_ADC_AD7276_FRAME_14_CLOCKS, words, 4u));
  TEST_ASSERT_EQUAL_HEX16(0x123u, words[0]);
  TEST_ASSERT_EQUAL_HEX16(0xaabu, words[1]);
}

void test_size_and_address_overflow_is_rejected_without_accessing_buffers(void) {
  const uint16_t raw = 0x1234u;
  uint16_t sample = 0xa55au;
  const size_t excessive_count = SIZE_MAX / sizeof(uint16_t) + 1u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_adc_ad7276_decode(&raw,
      excessive_count, ESY_ADC_AD7276_FRAME_16_CLOCKS, &sample, excessive_count));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_adc_ad7276_decode(&raw, 1u,
      ESY_ADC_AD7276_FRAME_16_CLOCKS, &sample, excessive_count));
  const uintptr_t wrapping_address = UINTPTR_MAX - (UINTPTR_MAX % _Alignof(uint16_t));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_adc_ad7276_decode(
      (const uint16_t *)wrapping_address, 1u,
      ESY_ADC_AD7276_FRAME_16_CLOCKS, &sample, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_adc_ad7276_decode(&raw, 1u,
      ESY_ADC_AD7276_FRAME_16_CLOCKS, (uint16_t *)wrapping_address, 1u));
  TEST_ASSERT_EQUAL_HEX16(0xa55au, sample);
}

void test_misaligned_buffers_are_rejected_before_access(void) {
  _Alignas(uint16_t) unsigned char bytes[4] = {0x12u, 0x34u, 0x56u, 0x78u};
  const unsigned char expected[] = {0x12u, 0x34u, 0x56u, 0x78u};
  const uint16_t raw = 0x1234u;
  uint16_t sample = 0xa55au;
  if (_Alignof(uint16_t) > 1u) {
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_adc_ad7276_decode(
        (const uint16_t *)&bytes[1], 1u, ESY_ADC_AD7276_FRAME_16_CLOCKS,
        &sample, 1u));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_adc_ad7276_decode(&raw, 1u,
        ESY_ADC_AD7276_FRAME_16_CLOCKS, (uint16_t *)&bytes[1], 1u));
  }
  TEST_ASSERT_EQUAL_MEMORY(expected, bytes, sizeof(bytes));
  TEST_ASSERT_EQUAL_HEX16(0xa55au, sample);
}
