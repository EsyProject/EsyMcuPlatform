/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esy_middleware/logger/esy_logger.h"

#define TEST_BACKEND_COUNT 3u
#define TEST_BUFFER_SIZE 64u
#define TEST_CAPTURE_SIZE 1200u

typedef struct {
  uint8_t buffer[TEST_BUFFER_SIZE];
  uint8_t capture[TEST_CAPTURE_SIZE];
  size_t capture_len;
  size_t chunk_size;
  size_t acquire_count;
  size_t submit_count;
  esy_err_t acquire_result;
  esy_err_t submit_result;
  char *payload_to_mutate;
  bool reenter;
  esy_err_t detach_result;
  esy_err_t init_result;
} logger_sink_t;

typedef struct {
  logger_sink_t sinks[TEST_BACKEND_COUNT];
  esy_logger_backend_t backends[TEST_BACKEND_COUNT];
  esy_logger_backend_binding_t bindings[TEST_BACKEND_COUNT];
  esy_logger_backend_binding_t replacement[TEST_BACKEND_COUNT];
  esy_logger_static_resources_t resources;
  esy_logger_config_t cfg;
} logger_multi_test_ctx_t;

static logger_multi_test_ctx_t s_ctx;

static esy_err_t sink_acquire(void *self, uint8_t **buf, size_t *buf_len) {
  const esy_logger_backend_t *backend = self;
  if (backend == NULL || backend->ctx == NULL || buf == NULL || buf_len == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  logger_sink_t *sink = backend->ctx;
  ++sink->acquire_count;
  if (sink->acquire_result != ESY_OK) return sink->acquire_result;
  *buf = sink->buffer;
  *buf_len = sink->chunk_size;
  return ESY_OK;
}

static esy_err_t sink_submit(void *self, const uint8_t *buf, size_t len) {
  const esy_logger_backend_t *backend = self;
  if (backend == NULL || backend->ctx == NULL || buf == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  logger_sink_t *sink = backend->ctx;
  ++sink->submit_count;
  if (sink->payload_to_mutate != NULL) {
    sink->payload_to_mutate[0] = 'X';
  }
  if (sink->reenter) {
    sink->reenter = false;
    esy_log("NESTED", ESY_LOG_LEVEL_ERROR, "must be dropped");
    sink->detach_result = esy_logger_detach(backend);
    sink->init_result = esy_logger_init(&s_ctx.cfg);
  }
  if (sink->submit_result != ESY_OK) return sink->submit_result;
  size_t available = sizeof(sink->capture) - sink->capture_len;
  if (len > available) return ESY_ERR_NO_MEM;
  memcpy(sink->capture + sink->capture_len, buf, len);
  sink->capture_len += len;
  return ESY_OK;
}

static esy_err_t bind_single_replacement(esy_log_level_t global_level) {
  s_ctx.replacement[0] = (esy_logger_backend_binding_t){
      .backend = &s_ctx.backends[2], .filter_level = ESY_LOG_LEVEL_VERBOSE};
  const esy_logger_static_resources_t resources = {
      .bindings = s_ctx.replacement, .binding_count = 1u};
  const esy_logger_config_t cfg = {
      .static_resources = &resources, .filter_level = global_level};
  return esy_logger_init(&cfg);
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  for (size_t i = 0u; i < TEST_BACKEND_COUNT; ++i) {
    s_ctx.sinks[i].chunk_size = TEST_BUFFER_SIZE;
    s_ctx.backends[i] = (esy_logger_backend_t){
        .acquire_buffer = sink_acquire,
        .submit_buffer = sink_submit,
        .ctx = &s_ctx.sinks[i],
    };
    s_ctx.bindings[i] = (esy_logger_backend_binding_t){
        .backend = &s_ctx.backends[i],
        .filter_level = ESY_LOG_LEVEL_VERBOSE,
    };
  }
  s_ctx.resources = (esy_logger_static_resources_t){
      .bindings = s_ctx.bindings,
      .binding_count = 2u,
  };
  s_ctx.cfg = (esy_logger_config_t){
      .static_resources = &s_ctx.resources,
      .filter_level = ESY_LOG_LEVEL_VERBOSE,
  };
}

void tearDown(void) {
  for (size_t i = 0u; i < TEST_BACKEND_COUNT; ++i) {
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_detach(&s_ctx.backends[i]));
  }
}

void test_multi_backend_fanout_preserves_line_with_independent_chunks(void) {
  const char expected[] = "[MULTI][I] value=42\n";
  s_ctx.sinks[0].chunk_size = 5u;
  s_ctx.sinks[1].chunk_size = 13u;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_init(&s_ctx.cfg));

  esy_log("MULTI", ESY_LOG_LEVEL_INFO, "value=%d", 42);

  for (size_t i = 0u; i < 2u; ++i) {
    TEST_ASSERT_EQUAL_UINT(sizeof(expected) - 1u, s_ctx.sinks[i].capture_len);
    TEST_ASSERT_EQUAL_MEMORY(expected, s_ctx.sinks[i].capture,
                             sizeof(expected) - 1u);
  }
  TEST_ASSERT_EQUAL_UINT(4u, s_ctx.sinks[0].submit_count);
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.sinks[1].submit_count);
}

void test_multi_backend_combines_global_and_individual_filters(void) {
  s_ctx.bindings[0].filter_level = ESY_LOG_LEVEL_INFO;
  s_ctx.bindings[1].filter_level = ESY_LOG_LEVEL_DEBUG;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_init(&s_ctx.cfg));

  esy_log("FILTER", ESY_LOG_LEVEL_DEBUG, "debug");
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sinks[0].submit_count);
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sinks[1].submit_count);
  esy_log("FILTER", ESY_LOG_LEVEL_INFO, "info");
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sinks[0].submit_count);
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.sinks[1].submit_count);

  esy_logger_set_filter_level(ESY_LOG_LEVEL_ERROR);
  esy_log("FILTER", ESY_LOG_LEVEL_INFO, "globally filtered");
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sinks[0].submit_count);
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.sinks[1].submit_count);
  esy_log("FILTER", ESY_LOG_LEVEL_ERROR, "error");
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.sinks[0].submit_count);
  TEST_ASSERT_EQUAL_UINT(3u, s_ctx.sinks[1].submit_count);
}

void test_multi_backend_uses_original_formatted_line_after_first_backend_callback(void) {
  char payload[] = "original";
  const char expected[] = "[ONCE][I] original\n";
  s_ctx.sinks[0].payload_to_mutate = payload;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_init(&s_ctx.cfg));

  esy_log("ONCE", ESY_LOG_LEVEL_INFO, "%s", payload);

  TEST_ASSERT_EQUAL_CHAR('X', payload[0]);
  TEST_ASSERT_EQUAL_MEMORY(expected, s_ctx.sinks[0].capture,
                           sizeof(expected) - 1u);
  TEST_ASSERT_EQUAL_MEMORY(expected, s_ctx.sinks[1].capture,
                           sizeof(expected) - 1u);
}

void test_multi_backend_does_not_format_when_every_backend_filters_line(void) {
  s_ctx.bindings[0].filter_level = ESY_LOG_LEVEL_ERROR;
  s_ctx.bindings[1].filter_level = ESY_LOG_LEVEL_WARN;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_init(&s_ctx.cfg));
  int formatted = -1;

  esy_log("FILTER", ESY_LOG_LEVEL_DEBUG, "side effect%n", &formatted);

  TEST_ASSERT_EQUAL_INT(-1, formatted);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sinks[0].acquire_count);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sinks[1].acquire_count);
}

void test_multi_backend_acquire_and_submit_failures_preserve_other_outputs(void) {
  s_ctx.sinks[0].chunk_size = 3u;
  s_ctx.sinks[0].acquire_result = ESY_ERR_NO_MEM;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_init(&s_ctx.cfg));

  esy_log("FAIL", ESY_LOG_LEVEL_INFO, "acquire failure");
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sinks[0].acquire_count);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sinks[0].submit_count);
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sinks[1].submit_count);

  s_ctx.sinks[0].acquire_result = ESY_OK;
  s_ctx.sinks[0].submit_result = ESY_ERR_IO;
  esy_log("FAIL", ESY_LOG_LEVEL_INFO, "submit failure");
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.sinks[0].acquire_count);
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sinks[0].submit_count);
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.sinks[1].submit_count);
}

void test_multi_backend_empty_acquired_buffer_preserves_other_outputs(void) {
  s_ctx.sinks[0].chunk_size = 0u;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_init(&s_ctx.cfg));

  esy_log("EMPTY", ESY_LOG_LEVEL_INFO, "continue");

  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sinks[0].acquire_count);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sinks[0].submit_count);
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sinks[1].submit_count);
}

void test_multi_backend_rejects_invalid_configuration_without_replacing_binding(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, bind_single_replacement(ESY_LOG_LEVEL_VERBOSE));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_logger_init(NULL));
  s_ctx.cfg.static_resources = NULL;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_logger_init(&s_ctx.cfg));
  s_ctx.cfg.static_resources = &s_ctx.resources;
  s_ctx.resources.bindings = NULL;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_logger_init(&s_ctx.cfg));
  s_ctx.resources.bindings = s_ctx.bindings;
  s_ctx.resources.binding_count = 0u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_logger_init(&s_ctx.cfg));
  s_ctx.resources.binding_count = SIZE_MAX;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_logger_init(&s_ctx.cfg));
  s_ctx.resources.binding_count = 2u;
  s_ctx.cfg.filter_level = (esy_log_level_t)-1;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_logger_init(&s_ctx.cfg));

  esy_log("OLD", ESY_LOG_LEVEL_VERBOSE, "still bound");
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sinks[0].submit_count);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sinks[1].submit_count);
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sinks[2].submit_count);
}

void test_multi_backend_rejects_invalid_entries_duplicates_and_no_active_backend(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, bind_single_replacement(ESY_LOG_LEVEL_INFO));
  s_ctx.bindings[1].backend = &s_ctx.backends[0];
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_logger_init(&s_ctx.cfg));
  s_ctx.bindings[1].backend = &s_ctx.backends[1];
  s_ctx.bindings[1].filter_level = (esy_log_level_t)99;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_logger_init(&s_ctx.cfg));
  s_ctx.bindings[1].filter_level = ESY_LOG_LEVEL_INFO;
  s_ctx.backends[1].acquire_buffer = NULL;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_logger_init(&s_ctx.cfg));
  s_ctx.backends[1].acquire_buffer = sink_acquire;
  s_ctx.backends[1].submit_buffer = NULL;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_logger_init(&s_ctx.cfg));
  s_ctx.backends[1].submit_buffer = sink_submit;
  s_ctx.bindings[0].backend = NULL;
  s_ctx.bindings[1].backend = NULL;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_logger_init(&s_ctx.cfg));

  esy_log("OLD", ESY_LOG_LEVEL_INFO, "still bound");
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sinks[2].submit_count);
}

void test_multi_backend_accepts_inactive_entries_and_detaches_only_matching_backend(void) {
  s_ctx.resources.binding_count = TEST_BACKEND_COUNT;
  s_ctx.bindings[0].backend = NULL;
  s_ctx.bindings[2].filter_level = ESY_LOG_LEVEL_ERROR;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_init(&s_ctx.cfg));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_logger_detach(NULL));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_detach(&s_ctx.backends[0]));

  esy_log("DETACH", ESY_LOG_LEVEL_INFO, "first");
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sinks[0].submit_count);
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sinks[1].submit_count);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sinks[2].submit_count);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_detach(&s_ctx.backends[1]));
  esy_log("DETACH", ESY_LOG_LEVEL_INFO, "filtered");
  esy_log("DETACH", ESY_LOG_LEVEL_ERROR, "remaining");
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sinks[1].submit_count);
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sinks[2].submit_count);
}

void test_multi_backend_final_detach_releases_storage_for_rebinding(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_init(&s_ctx.cfg));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_detach(&s_ctx.backends[0]));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_detach(&s_ctx.backends[1]));
  esy_log("EMPTY", ESY_LOG_LEVEL_ERROR, "dropped");
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sinks[0].submit_count);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sinks[1].submit_count);

  s_ctx.bindings[0].backend = &s_ctx.backends[0];
  TEST_ASSERT_NULL(s_ctx.bindings[1].backend);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_init(&s_ctx.cfg));
  esy_log("REUSE", ESY_LOG_LEVEL_INFO, "rebound");
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sinks[0].submit_count);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sinks[1].submit_count);
}

void test_multi_backend_replacement_releases_old_array_and_preserves_new_filter(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_init(&s_ctx.cfg));
  s_ctx.replacement[0] = (esy_logger_backend_binding_t){
      .backend = &s_ctx.backends[2], .filter_level = ESY_LOG_LEVEL_DEBUG};
  s_ctx.resources.bindings = s_ctx.replacement;
  s_ctx.resources.binding_count = 1u;
  s_ctx.cfg.filter_level = ESY_LOG_LEVEL_DEBUG;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_init(&s_ctx.cfg));
  memset(s_ctx.bindings, 0, sizeof(s_ctx.bindings));

  esy_log("REPLACE", ESY_LOG_LEVEL_DEBUG, "new backend");
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sinks[0].submit_count);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sinks[1].submit_count);
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sinks[2].submit_count);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_detach(&s_ctx.backends[0]));
  esy_log("REPLACE", ESY_LOG_LEVEL_DEBUG, "still bound");
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.sinks[2].submit_count);
}

void test_single_entry_configuration_replaces_all_backends_and_sets_global_filter(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_init(&s_ctx.cfg));
  TEST_ASSERT_EQUAL_INT(ESY_OK, bind_single_replacement(ESY_LOG_LEVEL_DEBUG));
  memset(s_ctx.bindings, 0, sizeof(s_ctx.bindings));

  esy_log("SINGLE", ESY_LOG_LEVEL_VERBOSE, "filtered by configuration");
  esy_log("SINGLE", ESY_LOG_LEVEL_DEBUG, "configured threshold");
  esy_log("SINGLE", ESY_LOG_LEVEL_INFO, "new backend");

  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sinks[0].submit_count);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sinks[1].submit_count);
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.sinks[2].submit_count);
}

void test_multi_backend_rejects_overlapping_borrowed_storage(void) {
  s_ctx.resources.bindings = &s_ctx.bindings[1];
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_init(&s_ctx.cfg));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_logger_init(&s_ctx.cfg));
  s_ctx.resources.bindings = s_ctx.bindings;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_logger_init(&s_ctx.cfg));
  s_ctx.resources.bindings = &s_ctx.bindings[2];
  s_ctx.resources.binding_count = 1u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_logger_init(&s_ctx.cfg));

  esy_log("OVERLAP", ESY_LOG_LEVEL_INFO, "old binding");
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sinks[0].submit_count);
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sinks[1].submit_count);
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sinks[2].submit_count);
}

void test_multi_backend_reentrancy_drops_nested_line_and_reports_busy_lifecycle(void) {
  s_ctx.sinks[0].reenter = true;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_init(&s_ctx.cfg));

  esy_log("OUTER", ESY_LOG_LEVEL_INFO, "one line");

  TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, s_ctx.sinks[0].detach_result);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, s_ctx.sinks[0].init_result);
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sinks[0].submit_count);
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sinks[1].submit_count);
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.sinks[0].capture, s_ctx.sinks[1].capture,
                           s_ctx.sinks[0].capture_len);
  esy_log("NEXT", ESY_LOG_LEVEL_INFO, "guard released");
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.sinks[0].submit_count);
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.sinks[1].submit_count);
}
