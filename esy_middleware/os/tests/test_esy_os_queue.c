/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/os/esy_os_queue.h"

#include <string.h>

#include "queue_test_support.h"
#include "unity.h"

static esy_os_queue_static_storage_t s_storage;
static uint32_t s_queue_buffer[2];
static esy_os_queue_t *s_queue;

void setUp(void) {
  memset(&s_storage, 0, sizeof(s_storage));
  memset(s_queue_buffer, 0, sizeof(s_queue_buffer));
  s_queue = NULL;
  queue_test_support_reset();
}

void tearDown(void) {}

void test_queue_task_operations_map_wait_expiry_to_timeout(void) {
  queue_test_support_ctx_t *ctx = queue_test_support_get_ctx();
  uint32_t item = 0x12345678u;

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_os_queue_create_static(sizeof(item), 2u, s_queue_buffer,
                                         &s_storage, &s_queue));

  ctx->send_result = pdFALSE;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT,
                          esy_os_queue_send(s_queue, &item, 0u));
  TEST_ASSERT_EQUAL_UINT32(0u, ctx->send_ticks_to_wait);

  ctx->receive_result = pdFALSE;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT,
                          esy_os_queue_receive(s_queue, &item, 25u));
  TEST_ASSERT_EQUAL_UINT32(25u, ctx->receive_ticks_to_wait);
}

void test_queue_task_operations_preserve_success_and_invalid_arguments(void) {
  uint32_t item = 0x12345678u;

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_os_queue_create_static(sizeof(item), 2u, s_queue_buffer,
                                         &s_storage, &s_queue));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_os_queue_send(s_queue, &item, 0u));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_os_queue_receive(s_queue, &item, 0u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_os_queue_send(NULL, &item, 0u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_os_queue_receive(s_queue, NULL, 0u));
}

void test_queue_creation_validates_arguments_and_backend_failure(void) {
  queue_test_support_ctx_t *ctx = queue_test_support_get_ctx();

  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_os_queue_create_static(sizeof(uint32_t), 2u, s_queue_buffer, NULL,
                                 &s_queue));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_os_queue_create_static(0u, 2u, s_queue_buffer, &s_storage, &s_queue));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_os_queue_create_static(
                                                   SIZE_MAX, 2u, s_queue_buffer,
                                                   &s_storage, &s_queue));

  ctx->create_should_fail = true;
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_FAIL,
      esy_os_queue_create_static(sizeof(uint32_t), 2u, s_queue_buffer,
                                 &s_storage, &s_queue));
}

void test_queue_isr_failure_semantics_remain_generic(void) {
  queue_test_support_ctx_t *ctx = queue_test_support_get_ctx();
  uint32_t item = 0x12345678u;
  bool need_yield = true;

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_os_queue_create_static(sizeof(item), 2u, s_queue_buffer,
                                         &s_storage, &s_queue));

  ctx->send_from_isr_result = pdFALSE;
  ctx->receive_from_isr_result = pdFALSE;
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_FAIL, esy_os_queue_send_from_isr(s_queue, &item, &need_yield));
  TEST_ASSERT_FALSE(need_yield);
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_FAIL, esy_os_queue_receive_from_isr(s_queue, &item, &need_yield));
  TEST_ASSERT_FALSE(need_yield);
}

void test_queue_delete_and_isr_success_paths(void) {
  queue_test_support_ctx_t *ctx = queue_test_support_get_ctx();
  uint32_t item = 0x12345678u;
  bool need_yield = false;

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_os_queue_create_static(sizeof(item), 2u, s_queue_buffer,
                                         &s_storage, &s_queue));

  ctx->higher_priority_task_woken = pdTRUE;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_os_queue_send_from_isr(s_queue, &item, &need_yield));
  TEST_ASSERT_TRUE(need_yield);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_os_queue_receive_from_isr(s_queue, &item, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_os_queue_send_from_isr(NULL, &item, &need_yield));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_os_queue_receive_from_isr(
                                                   s_queue, NULL, &need_yield));

  esy_os_queue_delete(NULL);
  esy_os_queue_delete(s_queue);
  TEST_ASSERT_EQUAL_UINT32(1u, ctx->delete_calls);
}
