/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_fault.h"
#include "esy_hal_backend/esy_hb_fault.h"
#include "hal_test_fakes.h"
#include "unity.h"

#include <setjmp.h>
#include <string.h>

typedef struct {
  jmp_buf escape;
  const esy_fault_config_t *config;
  esy_err_t init_result;
  uint32_t init_calls;
  uint32_t raise_calls;
  esy_fault_reason_t reason;
  uint32_t detail0, detail1;
} fault_test_ctx_t;

static fault_test_ctx_t s_ctx;

/* Narrow backend adapter exercises only the public HAL wrapper boundary. */
esy_err_t esy_hb_fault_init(const esy_fault_config_t *config) {
  ++s_ctx.init_calls;
  s_ctx.config = config;
  return s_ctx.init_result;
}

void esy_hb_fault_raise(esy_fault_reason_t reason, uint32_t detail0,
                        uint32_t detail1) {
  ++s_ctx.raise_calls;
  s_ctx.reason = reason;
  s_ctx.detail0 = detail0;
  s_ctx.detail1 = detail1;
  longjmp(s_ctx.escape, 1);
}

static esy_err_t capture(const esy_fault_snapshot_t *snapshot, void *context) {
  (void)snapshot;
  (void)context;
  return ESY_OK;
}

void setUp(void) { memset(&s_ctx, 0, sizeof(s_ctx)); }
void tearDown(void) {}

void test_fault_init_rejects_invalid_configuration_without_backend_calls(void) {
  esy_fault_config_t config = {.capture = capture};
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_hal_fault_init(NULL));
  config.capture = NULL;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_hal_fault_init(&config));
  config.capture = capture;
  config.stack_snapshot_bytes = ESY_FAULT_STACK_MAX_BYTES + 1u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_hal_fault_init(&config));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.init_calls);
}

void test_fault_init_forwards_binding_and_all_backend_statuses(void) {
  const esy_err_t statuses[] = {ESY_OK, ESY_ERR_STATE, ESY_ERR_BUSY, ESY_ERR_NOT_SUPPORTED, ESY_ERR_IO};
  esy_fault_config_t config = {.capture = capture, .ctx = &s_ctx,
                              .stack_snapshot_bytes = ESY_FAULT_STACK_MAX_BYTES};
  for (size_t index = 0u; index < sizeof(statuses) / sizeof(statuses[0]); ++index) {
    s_ctx.init_result = statuses[index];
    TEST_ASSERT_EQUAL_INT(statuses[index], esy_hal_fault_init(&config));
    TEST_ASSERT_EQUAL_PTR(&config, s_ctx.config);
    TEST_ASSERT_EQUAL_PTR(&s_ctx, s_ctx.config->ctx);
    TEST_ASSERT_EQUAL_UINT32(ESY_FAULT_STACK_MAX_BYTES, s_ctx.config->stack_snapshot_bytes);
  }
  config.ctx = NULL;
  config.stack_snapshot_bytes = 0u;
  s_ctx.init_result = ESY_OK;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_hal_fault_init(&config));
}

void test_fault_raise_forwards_opaque_values_without_returning(void) {
  if (setjmp(s_ctx.escape) == 0) {
    esy_hal_fault_raise(ESY_FAULT_REASON_ASSERT, 0xdeadbeefu, UINT32_MAX);
  }
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.raise_calls);
  TEST_ASSERT_EQUAL(ESY_FAULT_REASON_ASSERT, s_ctx.reason);
  TEST_ASSERT_EQUAL_HEX32(0xdeadbeefu, s_ctx.detail0);
  TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, s_ctx.detail1);
}
