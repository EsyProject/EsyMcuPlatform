/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"

#include <string.h>

#include "fake_esy_watchdog_monitor.h"

static void test_alert_callback(
    const esy_watchdog_alert_event_t *event, void *user_ctx) {
  (void)event;
  (void)user_ctx;
}

void setUp(void) {
  fake_esy_watchdog_monitor_reset();
}

void tearDown(void) {}

void test_fake_watchdog_monitor_records_lifecycle_controls(void) {
  fake_esy_watchdog_monitor_ctx_t *ctx =
      fake_esy_watchdog_monitor_get_ctx();
  esy_watchdog_monitor_config_t config;

  memset(&config, 0x5a, sizeof(config));
  esy_watchdog_monitor_default_cfg(&config);
  TEST_ASSERT_EACH_EQUAL_UINT8(0u, &config, sizeof(config));

  config.check_period_ms = 123u;
  ctx->init_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO,
                          esy_watchdog_monitor_init(&config));
  TEST_ASSERT_TRUE(ctx->has_last_init_config);
  TEST_ASSERT_EQUAL_UINT32(123u, ctx->last_init_config.check_period_ms);

  ctx->start_result = ESY_ERR_TIMEOUT;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT, esy_watchdog_monitor_start());
  esy_watchdog_monitor_stop();
  esy_watchdog_monitor_deinit();
  TEST_ASSERT_EQUAL_UINT32(1u, ctx->init_count);
  TEST_ASSERT_EQUAL_UINT32(1u, ctx->start_count);
  TEST_ASSERT_EQUAL_UINT32(1u, ctx->stop_count);
  TEST_ASSERT_EQUAL_UINT32(1u, ctx->deinit_count);
}

void test_fake_watchdog_monitor_returns_deterministic_report_tokens(void) {
  fake_esy_watchdog_monitor_ctx_t *ctx =
      fake_esy_watchdog_monitor_get_ctx();
  esy_watchdog_report_t report = {0};

  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG, esy_watchdog_monitor_register(NULL, &report));
  ctx->register_result = ESY_ERR_NO_MEM;
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_NO_MEM, esy_watchdog_monitor_register("worker", &report));

  ctx->register_result = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_watchdog_monitor_register("worker", &report));
  TEST_ASSERT_EQUAL_STRING("worker", ctx->last_task_name);
  TEST_ASSERT_EQUAL_UINT32(1u, report.generation);
  TEST_ASSERT_NOT_EQUAL(0u, report.magic);
  TEST_ASSERT_EQUAL_INT32(1, esy_watchdog_monitor_get_task_count());

  esy_watchdog_monitor_report(&report);
  esy_watchdog_monitor_disable_task_check(&report);
  TEST_ASSERT_EQUAL_UINT32(1u, ctx->report_count);
  TEST_ASSERT_EQUAL_UINT32(1u, ctx->disable_count);

  esy_watchdog_monitor_unregister(&report);
  TEST_ASSERT_EACH_EQUAL_UINT8(0u, &report, sizeof(report));
  TEST_ASSERT_EQUAL_INT32(0, esy_watchdog_monitor_get_task_count());
}

void test_fake_watchdog_monitor_exposes_status_and_error_results(void) {
  fake_esy_watchdog_monitor_ctx_t *ctx =
      fake_esy_watchdog_monitor_get_ctx();
  esy_watchdog_report_t report = {
      .slot_index = 2u,
      .generation = 3u,
      .magic = 4u,
  };
  bool alive = false;

  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_watchdog_monitor_enable_task_check(NULL));
  ctx->enable_result = ESY_ERR_STATE;
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE, esy_watchdog_monitor_enable_task_check(&report));

  ctx->task_alive = true;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_watchdog_monitor_get_task_status(&report, &alive));
  TEST_ASSERT_TRUE(alive);
  ctx->get_status_result = ESY_ERR_STATE;
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE,
      esy_watchdog_monitor_get_task_status(&report, &alive));

  ctx->get_task_count_result = ESY_ERR_STATE;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_watchdog_monitor_get_task_count());
}

void test_fake_watchdog_monitor_records_alert_and_hardware_feed_controls(void) {
  fake_esy_watchdog_monitor_ctx_t *ctx =
      fake_esy_watchdog_monitor_get_ctx();
  uint32_t user_ctx = 7u;

  esy_watchdog_monitor_set_alert_cb(test_alert_callback, &user_ctx);
  esy_watchdog_monitor_disable_hw_feed();

  TEST_ASSERT_EQUAL_PTR(test_alert_callback, ctx->alert_cb);
  TEST_ASSERT_EQUAL_PTR(&user_ctx, ctx->alert_user_ctx);
  TEST_ASSERT_EQUAL_UINT32(1u, ctx->set_alert_count);
  TEST_ASSERT_EQUAL_UINT32(1u, ctx->disable_hw_feed_count);
}

void test_fake_watchdog_deadline_has_configurable_fail_closed_result(void) {
  fake_esy_watchdog_monitor_ctx_t *ctx = fake_esy_watchdog_monitor_get_ctx();
  esy_monotonic_clock_t clock = {0};
  uint64_t deadline = 9u;
  ctx->deadline_result = ESY_ERR_NOT_SUPPORTED;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED,
      esy_watchdog_monitor_get_hard_deadline_atomic(&clock, &deadline));
  TEST_ASSERT_EQUAL_UINT64(9u, deadline);
  ctx->deadline_result = ESY_OK;
  ctx->deadline_us = 123u;
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_watchdog_monitor_get_hard_deadline_atomic(&clock, &deadline));
  TEST_ASSERT_EQUAL_UINT64(123u, deadline);
  TEST_ASSERT_EQUAL_UINT32(2u, ctx->deadline_query_count);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_watchdog_monitor_get_hard_deadline_atomic(NULL, &deadline));
}

void test_fake_cleanup_and_callback_busy_results_are_observable(void) {
  fake_esy_watchdog_monitor_ctx_t *ctx = fake_esy_watchdog_monitor_get_ctx();
  ctx->stop_result = ESY_ERR_BUSY;
  ctx->deinit_result = ESY_ERR_NOT_SUPPORTED;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_watchdog_monitor_stop());
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_watchdog_monitor_deinit());
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_set_alert_cb(test_alert_callback, ctx));
  ctx->set_alert_result = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_watchdog_monitor_set_alert_cb(NULL, NULL));
  TEST_ASSERT_EQUAL_PTR(test_alert_callback, ctx->alert_cb);
  TEST_ASSERT_EQUAL_PTR(ctx, ctx->alert_user_ctx);
}
