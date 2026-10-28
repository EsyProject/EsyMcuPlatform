/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/utils/base/esy_bits.h"
#include "esy_common/utils/integrity/esy_crc32.h"
#include "esy_common/utils/base/esy_endian.h"
#include "esy_common/utils/base/esy_seq.h"
#include "esy_common/utils/integrity/esy_sha256.h"
#include "esy_common/utils/base/esy_sort.h"

#include <stdint.h>
#include <string.h>

void setUp(void) {}

void tearDown(void) {}

void test_bits_detects_power_of_two_values(void) {
  TEST_ASSERT_FALSE(esy_is_power_of_two_u32(0u));
  TEST_ASSERT_TRUE(esy_is_power_of_two_u32(1u));
  TEST_ASSERT_TRUE(esy_is_power_of_two_u32(1024u));
  TEST_ASSERT_FALSE(esy_is_power_of_two_u32(1023u));
}

void test_endian_reads_and_writes_little_endian_values(void) {
  uint8_t buf[4] = {0};

  esy_write_u16_le(buf, 0x1234u);
  TEST_ASSERT_EQUAL_HEX8(0x34u, buf[0]);
  TEST_ASSERT_EQUAL_HEX8(0x12u, buf[1]);
  TEST_ASSERT_EQUAL_HEX16(0x1234u, esy_read_u16_le(buf));

  esy_write_u32_le(buf, 0x89ABCDEFu);
  TEST_ASSERT_EQUAL_HEX8(0xEFu, buf[0]);
  TEST_ASSERT_EQUAL_HEX8(0xCDu, buf[1]);
  TEST_ASSERT_EQUAL_HEX8(0xABu, buf[2]);
  TEST_ASSERT_EQUAL_HEX8(0x89u, buf[3]);
  TEST_ASSERT_EQUAL_HEX32(0x89ABCDEFu, esy_read_u32_le(buf));
}

void test_endian_helpers_tolerate_null_buffers(void) {
  TEST_ASSERT_EQUAL_HEX16(0u, esy_read_u16_le(NULL));
  TEST_ASSERT_EQUAL_HEX32(0u, esy_read_u32_le(NULL));

  esy_write_u16_le(NULL, 0x1234u);
  esy_write_u32_le(NULL, 0x89ABCDEFu);
}

void test_seq_helpers_handle_wraparound_ranges(void) {
  TEST_ASSERT_TRUE(esy_seq32_before(0xFFFFFFFEu, 1u));
  TEST_ASSERT_FALSE(esy_seq32_before(1u, 0xFFFFFFFEu));

  TEST_ASSERT_FALSE(esy_seq32_reached(0xFFFFFFFEu, 1u));
  TEST_ASSERT_TRUE(esy_seq32_reached(1u, 0xFFFFFFFEu));
  TEST_ASSERT_TRUE(esy_seq32_reached(10u, 10u));

  TEST_ASSERT_TRUE(esy_seq32_in_closed_range(0xFFFFFFFFu, 0xFFFFFFFEu, 2u));
  TEST_ASSERT_TRUE(esy_seq32_in_closed_range(1u, 0xFFFFFFFEu, 2u));
  TEST_ASSERT_FALSE(esy_seq32_in_closed_range(3u, 0xFFFFFFFEu, 2u));
}

void test_crc32_matches_standard_check_vector(void) {
  const uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  uint32_t crc = ~esy_crc32(0xFFFFFFFFu, data, sizeof(data));

  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc);
}

void test_crc_frame_writes_and_validates_tail_crc(void) {
  uint8_t frame[7] = {'A', 'B', 'C', 0u, 0u, 0u, 0u};

  esy_crc32_fill_tail_le(frame, 3u);
  TEST_ASSERT_TRUE(esy_crc32_validate_tail_le(frame, sizeof(frame)));

  frame[1] ^= 0x01u;
  TEST_ASSERT_FALSE(esy_crc32_validate_tail_le(frame, sizeof(frame)));
}

void test_crc_frame_rejects_invalid_frames(void) {
  TEST_ASSERT_EQUAL_HEX32(0u, esy_crc32_finalize(NULL, 3u));
  TEST_ASSERT_FALSE(esy_crc32_validate_tail_le(NULL, 4u));

  uint8_t too_short[3] = {0u};
  TEST_ASSERT_FALSE(esy_crc32_validate_tail_le(too_short, sizeof(too_short)));
}

void test_sha256_matches_standard_check_vectors(void) {
  static const uint8_t expected_empty[ESY_SHA256_DIGEST_BYTES] = {
      0xE3u, 0xB0u, 0xC4u, 0x42u, 0x98u, 0xFCu, 0x1Cu, 0x14u,
      0x9Au, 0xFBu, 0xF4u, 0xC8u, 0x99u, 0x6Fu, 0xB9u, 0x24u,
      0x27u, 0xAEu, 0x41u, 0xE4u, 0x64u, 0x9Bu, 0x93u, 0x4Cu,
      0xA4u, 0x95u, 0x99u, 0x1Bu, 0x78u, 0x52u, 0xB8u, 0x55u,
  };
  static const uint8_t expected_abc[ESY_SHA256_DIGEST_BYTES] = {
      0xBAu, 0x78u, 0x16u, 0xBFu, 0x8Fu, 0x01u, 0xCFu, 0xEAu,
      0x41u, 0x41u, 0x40u, 0xDEu, 0x5Du, 0xAEu, 0x22u, 0x23u,
      0xB0u, 0x03u, 0x61u, 0xA3u, 0x96u, 0x17u, 0x7Au, 0x9Cu,
      0xB4u, 0x10u, 0xFFu, 0x61u, 0xF2u, 0x00u, 0x15u, 0xADu,
  };
  uint8_t digest[ESY_SHA256_DIGEST_BYTES];
  esy_sha256_ctx_t ctx;

  esy_sha256(NULL, 0u, digest);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_empty, digest, sizeof(digest));

  esy_sha256_init(&ctx);
  esy_sha256_update(&ctx, "a", 1u);
  esy_sha256_update(&ctx, "bc", 2u);
  esy_sha256_final(&ctx, digest);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_abc, digest, sizeof(digest));
}


void test_sha256_padding_and_multiblock_vectors_match_one_shot_and_chunked(void) {
  /* Expected bytes were generated independently with Python hashlib SHA-256. */
  static const struct {
    size_t length;
    uint8_t digest[ESY_SHA256_DIGEST_BYTES];
  } k_vectors[] = {
      {55u, {
          0x46u, 0x3Eu, 0xB2u, 0x8Eu, 0x72u, 0xF8u, 0x2Eu, 0x0Au,
          0x96u, 0xC0u, 0xA4u, 0xCCu, 0x53u, 0x69u, 0x0Cu, 0x57u,
          0x12u, 0x81u, 0x13u, 0x1Fu, 0x67u, 0x2Au, 0xA2u, 0x29u,
          0xE0u, 0xD4u, 0x5Au, 0xE5u, 0x9Bu, 0x59u, 0x8Bu, 0x59u,
      }},
      {56u, {
          0xDAu, 0x2Au, 0xE4u, 0xD6u, 0xB3u, 0x67u, 0x48u, 0xF2u,
          0xA3u, 0x18u, 0xF2u, 0x3Eu, 0x7Au, 0xB1u, 0xDFu, 0xDFu,
          0x45u, 0xACu, 0xDCu, 0x9Du, 0x04u, 0x9Bu, 0xD8u, 0x0Eu,
          0x59u, 0xDEu, 0x82u, 0xA6u, 0x08u, 0x95u, 0xF5u, 0x62u,
      }},
      {63u, {
          0x29u, 0xAFu, 0x26u, 0x86u, 0xFDu, 0x53u, 0x37u, 0x4Au,
          0x36u, 0xB0u, 0x84u, 0x66u, 0x94u, 0xCCu, 0x34u, 0x21u,
          0x77u, 0xE4u, 0x28u, 0xD1u, 0x64u, 0x75u, 0x15u, 0xF0u,
          0x78u, 0x78u, 0x4Du, 0x69u, 0xCDu, 0xB9u, 0xE4u, 0x88u,
      }},
      {64u, {
          0xFDu, 0xEAu, 0xB9u, 0xACu, 0xF3u, 0x71u, 0x03u, 0x62u,
          0xBDu, 0x26u, 0x58u, 0xCDu, 0xC9u, 0xA2u, 0x9Eu, 0x8Fu,
          0x9Cu, 0x75u, 0x7Fu, 0xCFu, 0x98u, 0x11u, 0x60u, 0x3Au,
          0x8Cu, 0x44u, 0x7Cu, 0xD1u, 0xD9u, 0x15u, 0x11u, 0x08u,
      }},
      {65u, {
          0x4Bu, 0xFDu, 0x2Cu, 0x8Bu, 0x6Fu, 0x1Eu, 0xECu, 0x7Au,
          0x2Au, 0xFEu, 0xB4u, 0x8Bu, 0x93u, 0x4Eu, 0xE4u, 0xB2u,
          0x69u, 0x41u, 0x82u, 0x02u, 0x7Eu, 0x6Du, 0x0Fu, 0xC0u,
          0x75u, 0x07u, 0x4Fu, 0x2Fu, 0xABu, 0xB3u, 0x17u, 0x81u,
      }},
      {119u, {
          0xDAu, 0x18u, 0x79u, 0x7Eu, 0xD7u, 0xC3u, 0xA7u, 0x77u,
          0xF0u, 0x84u, 0x7Fu, 0x42u, 0x97u, 0x24u, 0xA2u, 0xD8u,
          0xCDu, 0x51u, 0x38u, 0xE6u, 0xEDu, 0x28u, 0x95u, 0xC3u,
          0xFAu, 0x1Au, 0x6Du, 0x39u, 0xD1u, 0x8Fu, 0x7Eu, 0xC6u,
      }},
      {120u, {
          0xF5u, 0x2Bu, 0x23u, 0xDBu, 0x1Fu, 0xBBu, 0x6Du, 0xEDu,
          0x89u, 0xEFu, 0x42u, 0xA2u, 0x3Cu, 0xE0u, 0xC8u, 0x92u,
          0x2Cu, 0x45u, 0xF2u, 0x5Cu, 0x50u, 0xB5u, 0x68u, 0xA9u,
          0x3Bu, 0xF1u, 0xC0u, 0x75u, 0x42u, 0x0Bu, 0xBBu, 0x7Cu,
      }},
      {127u, {
          0x92u, 0xCAu, 0x0Fu, 0xA6u, 0x65u, 0x1Eu, 0xE2u, 0xF9u,
          0x7Bu, 0x88u, 0x4Bu, 0x72u, 0x46u, 0xA5u, 0x62u, 0xFAu,
          0x71u, 0x25u, 0x0Fu, 0xEDu, 0xEFu, 0xE5u, 0xEBu, 0xF2u,
          0x70u, 0xD3u, 0x1Cu, 0x54u, 0x6Bu, 0xFEu, 0xA9u, 0x76u,
      }},
      {128u, {
          0x47u, 0x1Fu, 0xB9u, 0x43u, 0xAAu, 0x23u, 0xC5u, 0x11u,
          0xF6u, 0xF7u, 0x2Fu, 0x8Du, 0x16u, 0x52u, 0xD9u, 0xC8u,
          0x80u, 0xCFu, 0xA3u, 0x92u, 0xADu, 0x80u, 0x50u, 0x31u,
          0x20u, 0x54u, 0x77u, 0x03u, 0xE5u, 0x6Au, 0x2Bu, 0xE5u,
      }},
      {129u, {
          0x50u, 0x99u, 0xC6u, 0xA5u, 0x62u, 0x03u, 0xF9u, 0x68u,
          0x7Fu, 0x7Du, 0x33u, 0xF4u, 0xBFu, 0xDFu, 0x57u, 0x6Du,
          0x31u, 0xDCu, 0x91u, 0xF6u, 0xB6u, 0x95u, 0xECu, 0xEAu,
          0x38u, 0xB2u, 0x77u, 0x0Cu, 0x87u, 0x63u, 0x11u, 0x35u,
      }},
      {1024u, {
          0x2Bu, 0xCEu, 0x1Bu, 0xA6u, 0x28u, 0x72u, 0x06u, 0x64u,
          0xBEu, 0x4Bu, 0x9Fu, 0xDDu, 0x77u, 0xAAu, 0xE0u, 0x67u,
          0x8Eu, 0x5Fu, 0x0Fu, 0x3Fu, 0x02u, 0xFCu, 0x6Fu, 0xF6u,
          0x41u, 0xECu, 0x87u, 0x90u, 0x94u, 0xF6u, 0xA4u, 0x04u,
      }},
  };
  uint8_t input[1024u];
  uint8_t digest[ESY_SHA256_DIGEST_BYTES];
  esy_sha256_ctx_t ctx;

  for (size_t i = 0u; i < sizeof(input); ++i) input[i] = (uint8_t)(i % 251u);
  for (size_t i = 0u; i < sizeof(k_vectors) / sizeof(k_vectors[0]); ++i) {
    esy_sha256(input, k_vectors[i].length, digest);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(k_vectors[i].digest, digest, sizeof(digest));
    memset(&ctx, 0xA5, sizeof(ctx));
    esy_sha256_init(&ctx);
    for (size_t offset = 0u; offset < k_vectors[i].length;) {
      size_t chunk = 1u + offset % 67u;
      if (chunk > k_vectors[i].length - offset) chunk = k_vectors[i].length - offset;
      esy_sha256_update(&ctx, NULL, 0u);
      esy_sha256_update(&ctx, input + offset, chunk);
      offset += chunk;
    }
    esy_sha256_final(&ctx, digest);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(k_vectors[i].digest, digest, sizeof(digest));
  }
}

void test_sha256_invalid_inputs_preserve_state_and_finalized_storage_can_restart(void) {
  esy_sha256_ctx_t ctx;
  esy_sha256_ctx_t before;
  const uint8_t cleared[sizeof(esy_sha256_ctx_t)] = {0};
  uint8_t digest[ESY_SHA256_DIGEST_BYTES];
  uint8_t expected[ESY_SHA256_DIGEST_BYTES];

  memset(&ctx, 0xA5, sizeof(ctx));
  esy_sha256_init(&ctx);
  esy_sha256_update(&ctx, "partial", 7u);
  memcpy(&before, &ctx, sizeof(before));
  esy_sha256_update(&ctx, NULL, 1u);
  esy_sha256_final(&ctx, NULL);
  TEST_ASSERT_EQUAL_MEMORY(&before, &ctx, sizeof(ctx));
  esy_sha256_init(NULL);
  esy_sha256_update(NULL, "abc", 3u);
  memset(digest, 0xA5, sizeof(digest));
  memcpy(expected, digest, sizeof(expected));
  esy_sha256_final(NULL, digest);
  esy_sha256(NULL, 1u, digest);
  esy_sha256("abc", 3u, NULL);
  TEST_ASSERT_EQUAL_MEMORY(expected, digest, sizeof(digest));

  /* Restart discards partial input; final clears storage and permits reinit. */
  esy_sha256_init(&ctx);
  esy_sha256_update(&ctx, "abc", 3u);
  esy_sha256_final(&ctx, digest);
  TEST_ASSERT_EQUAL_MEMORY(cleared, &ctx, sizeof(ctx));
  esy_sha256("abc", 3u, expected);
  TEST_ASSERT_EQUAL_MEMORY(expected, digest, sizeof(digest));
  esy_sha256_init(&ctx);
  esy_sha256_update(&ctx, "abc", 3u);
  esy_sha256_final(&ctx, digest);
  TEST_ASSERT_EQUAL_MEMORY(expected, digest, sizeof(digest));
}

void test_sort_orders_uint16_array(void) {
  uint16_t values[] = {9u, 1u, 5u, 1u, 3u};
  const uint16_t expected[] = {1u, 1u, 3u, 5u, 9u};

  esy_sort_u16(values, sizeof(values) / sizeof(values[0]));
  TEST_ASSERT_EQUAL_UINT16_ARRAY(expected, values, 5u);
}

void test_sort_handles_empty_single_sorted_and_reverse_arrays(void) {
  uint16_t single[] = {7u};
  uint16_t sorted[] = {1u, 2u, 3u};
  uint16_t reverse[] = {5u, 4u, 3u, 2u, 1u};
  const uint16_t expected_reverse[] = {1u, 2u, 3u, 4u, 5u};

  esy_sort_u16(NULL, 0u);
  esy_sort_u16(single, 1u);
  TEST_ASSERT_EQUAL_UINT16(7u, single[0]);

  esy_sort_u16(sorted, sizeof(sorted) / sizeof(sorted[0]));
  TEST_ASSERT_EQUAL_UINT16_ARRAY(((uint16_t[]){1u, 2u, 3u}), sorted, 3u);

  esy_sort_u16(reverse, sizeof(reverse) / sizeof(reverse[0]));
  TEST_ASSERT_EQUAL_UINT16_ARRAY(expected_reverse, reverse, 5u);
}
