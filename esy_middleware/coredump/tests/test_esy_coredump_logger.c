/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"

#include <stdio.h>
#include <string.h>

#include "esy_middleware/coredump/esy_coredump.h"
#include "esy_middleware/coredump/esy_coredump_logger.h"
#include "esy_middleware/logger/esy_logger.h"
#include "coredump_fixture.h"

#define TEST_RECORD_BYTES (FIXTURE_HEADER_BYTES + 3u)
#define TEST_LOG_BYTES 128u

typedef struct {
  esy_coredump_t store;
  esy_coredump_static_resources_t resources;
  esy_coredump_config_t config;
  fixture_id_source_t id_source;
  esy_logger_backend_t backend;
  esy_logger_backend_binding_t binding;
  esy_logger_static_resources_t logger_resources;
  esy_logger_config_t logger_config;
  _Alignas(uint32_t) uint8_t retained[TEST_RECORD_BYTES];
  uint8_t saved[TEST_RECORD_BYTES];
  uint8_t output[TEST_LOG_BYTES];
  size_t output_length;
  uint32_t submissions;
  esy_err_t acquire_result;
} test_ctx_t;

static test_ctx_t s_ctx;

static esy_err_t acquire_output(void *self, uint8_t **buffer, size_t *length) {
  (void)self;
  if (s_ctx.acquire_result != ESY_OK) return s_ctx.acquire_result;
  *buffer = s_ctx.output;
  *length = sizeof(s_ctx.output);
  return ESY_OK;
}

static esy_err_t submit_output(void *self, const uint8_t *buffer, size_t length) {
  (void)self;
  (void)buffer;
  s_ctx.output_length = length;
  ++s_ctx.submissions;
  return ESY_OK;
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  const uint8_t stack[] = {0x00u, 0x80u, 0xffu};
  fixture_record(s_ctx.retained, UINT32_C(0x10030000), 0x12u, stack, sizeof(stack));
  memcpy(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
  s_ctx.resources.retained_buffer = s_ctx.retained;
  s_ctx.resources.retained_size = sizeof(s_ctx.retained);
  s_ctx.config.static_resources = &s_ctx.resources;
  s_ctx.config.new_record_id = fixture_new_record_id;
  s_ctx.config.record_id_context = &s_ctx.id_source;
  s_ctx.config.stack_limit = sizeof(stack);
  memset(s_ctx.config.build_id, 0x34, sizeof(s_ctx.config.build_id));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_init(&s_ctx.store, &s_ctx.config));
  s_ctx.backend.acquire_buffer = acquire_output;
  s_ctx.backend.submit_buffer = submit_output;
  s_ctx.binding.backend = &s_ctx.backend;
  s_ctx.binding.filter_level = ESY_LOG_LEVEL_VERBOSE;
  s_ctx.logger_resources.bindings = &s_ctx.binding;
  s_ctx.logger_resources.binding_count = 1u;
  s_ctx.logger_config.static_resources = &s_ctx.logger_resources;
  s_ctx.logger_config.filter_level = ESY_LOG_LEVEL_VERBOSE;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_init(&s_ctx.logger_config));
}

void tearDown(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_detach(&s_ctx.backend));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_deinit(&s_ctx.store));
}

void test_coredump_logger_offers_one_small_identifiable_chunk_without_clearing_record(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_logger_export_chunk(&s_ctx.store, 0u));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.submissions);
  TEST_ASSERT_LESS_THAN(TEST_LOG_BYTES, s_ctx.output_length);
  s_ctx.output[s_ctx.output_length] = 0u;
  char expected[96];
  snprintf(expected, sizeof(expected), "CD1 12121212121212121212121212121212 %u 0 454344310100000083010000",
           (unsigned)sizeof(s_ctx.retained));
  TEST_ASSERT_NOT_NULL(strstr((const char *)s_ctx.output, expected));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_logger_export_chunk(&s_ctx.store, 0u));
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.submissions);
}

void test_coredump_logger_last_short_chunk_preserves_zero_and_high_bytes(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      esy_coredump_logger_export_chunk(&s_ctx.store, FIXTURE_HEADER_BYTES));
  s_ctx.output[s_ctx.output_length] = 0u;
  TEST_ASSERT_NOT_NULL(strstr((const char *)s_ctx.output, " 387 384 0080ff"));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.submissions);
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
}

void test_coredump_logger_filtered_or_exhausted_output_does_not_claim_collection_or_clear(void) {
  esy_logger_set_filter_level(ESY_LOG_LEVEL_ERROR);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_logger_export_chunk(&s_ctx.store, 0u));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.submissions);
  esy_logger_set_filter_level(ESY_LOG_LEVEL_VERBOSE);
  s_ctx.acquire_result = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_logger_export_chunk(&s_ctx.store, 0u));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.submissions);
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
  s_ctx.acquire_result = ESY_OK;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_logger_export_chunk(&s_ctx.store, 0u));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.submissions);
}

void test_coredump_logger_rejects_invalid_offset_and_corrupted_records_before_output(void) {
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_coredump_logger_export_chunk(&s_ctx.store, sizeof(s_ctx.retained)));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_coredump_logger_export_chunk(&s_ctx.store, SIZE_MAX));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.submissions);
  s_ctx.retained[FIXTURE_HEADER_BYTES] ^= 1u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_CRC_FAIL, esy_coredump_logger_export_chunk(&s_ctx.store, 0u));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.submissions);
}
