/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/watchdog_monitor/esy_watchdog_monitor.h"
#include "esy_watchdog_monitor_internal.h"
#include "fake_esy_os_task.h"
#include "fake_esy_hal_watchdog.h"
#include "fake_esy_hal_system.h"
#include "unity.h"
#include <setjmp.h>
#include <string.h>

typedef struct {
  ESY_WATCHDOG_MONITOR_STATIC_STORAGE(storage, 2u, 512u);
  esy_watchdog_monitor_static_resources_t resources;
  esy_watchdog_monitor_core_resources_t core_resources;
  bool core_callback_probe;
  esy_watchdog_report_t first, second;
  esy_watchdog_alert_event_t last_alert;
  char alert_name[ESY_WATCHDOG_MONITOR_TASK_NAME_BYTES];
  uint32_t alert_count;
  uint64_t now_us;
  esy_err_t clock_result;
  bool probe_callback;
  jmp_buf parked;
} watchdog_test_ctx_t;
static watchdog_test_ctx_t s_ctx;

static esy_err_t read_clock(void *ctx, uint64_t *out) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx.now_us, ctx);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_critical_nesting());
  if (s_ctx.clock_result != ESY_OK) return s_ctx.clock_result;
  *out = s_ctx.now_us;
  return ESY_OK;
}
static const esy_monotonic_clock_ops_t k_clock_ops = {read_clock};

static void capture_alert(const esy_watchdog_alert_event_t *event, void *ctx) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx, ctx);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_critical_nesting());
  s_ctx.last_alert = *event;
  strcpy(s_ctx.alert_name, event->task_name);
  ++s_ctx.alert_count;
  if (s_ctx.core_callback_probe) {
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_core_process());
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_stop());
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_deinit());
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_watchdog_monitor_set_alert_cb(NULL, NULL));
    esy_watchdog_monitor_report(&s_ctx.first);
  }
  if (s_ctx.probe_callback) {
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_watchdog_monitor_set_alert_cb(NULL, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_stop());
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_deinit());
    esy_watchdog_monitor_report(&s_ctx.first);
  }
}

static esy_watchdog_monitor_config_t config(void) {
  esy_watchdog_monitor_config_t cfg;
  esy_watchdog_monitor_default_cfg(&cfg);
  cfg.check_period_ms = 10u;
  cfg.hw_timeout_ms = 100u;
  cfg.task_priority = 3u;
  cfg.task_core_affinity = 1u;
  cfg.clock = (esy_monotonic_clock_t){&k_clock_ops, &s_ctx.now_us};
  ESY_WATCHDOG_MONITOR_BIND_STATIC_RESOURCES(&s_ctx.resources, s_ctx.storage);
  cfg.static_resources = &s_ctx.resources;
  return cfg;
}
static esy_watchdog_monitor_ctx_t *monitor_context(void) {
  fake_esy_os_task_entry_t *task = fake_esy_os_task_get_last_created();
  if (task == NULL || task->deleted) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_start());
    task = fake_esy_os_task_get_last_created();
  }
  return task->arg;
}
static void check_once(void) {
  esy_watchdog_monitor_ctx_t *ctx = monitor_context();
  fake_esy_os_task_set_current(fake_esy_os_task_get_last_created()->task_handle);
  esy_watchdog_monitor_check_once(ctx);
  fake_esy_os_task_set_current(NULL);
}
static void init_monitor(void) {
  esy_watchdog_monitor_config_t cfg = config();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_init(&cfg));
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_set_alert_cb(capture_alert, &s_ctx));
}
void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  memset(&s_ctx.storage, 0xa5, sizeof(s_ctx.storage));
  fake_esy_os_task_reset();
  fake_esy_hal_watchdog_reset();
  fake_esy_hal_system_reset();
  s_ctx.now_us = 1000u;
}
void tearDown(void) {
  fake_esy_os_task_set_current(NULL);
  fake_esy_os_task_set_critical_exit_hook(NULL);
  fake_esy_hal_watchdog_get_state()->deinit_result = ESY_OK;
  fake_esy_os_task_entry_t *task = fake_esy_os_task_get_last_created();
  if (task != NULL) task->parked = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_deinit());
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_critical_nesting());
}

void test_defaults_and_invalid_config_do_not_start_hardware(void) {
  esy_watchdog_monitor_config_t cfg;
  esy_watchdog_monitor_default_cfg(NULL);
  esy_watchdog_monitor_default_cfg(&cfg);
  TEST_ASSERT_EQUAL_UINT32(1000u, cfg.check_period_ms);
  TEST_ASSERT_EQUAL_UINT32(5000u, cfg.hw_timeout_ms);
  TEST_ASSERT_NULL(cfg.static_resources);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_init(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_init(&cfg));
  cfg = config(); cfg.check_period_ms = cfg.hw_timeout_ms;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_init(&cfg));
  cfg = config(); cfg.hw_timeout_ms = 0;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_init(&cfg));
  cfg = config(); cfg.static_resources->task_slots = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_init(&cfg));
  cfg = config(); cfg.static_resources->task_stack_buffer = &s_ctx.storage.task_stack_buffer.bytes[1];
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_init(&cfg));
  cfg = config(); fake_esy_os_task_set_park_supported(false);
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_watchdog_monitor_init(&cfg));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_watchdog_get_state()->init_count);
}

void test_first_init_overwrites_backing_storage_and_failure_can_retry(void) {
  esy_watchdog_monitor_config_t cfg = config();
  fake_esy_hal_watchdog_set_init_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_watchdog_monitor_init(&cfg));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_start());
  fake_esy_hal_watchdog_set_init_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_init(&cfg));
  TEST_ASSERT_TRUE(fake_esy_hal_watchdog_get_state()->enabled);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_init(&cfg));
  fake_esy_os_task_set_create_result(ESY_ERR_FAIL);
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_watchdog_monitor_start());
  TEST_ASSERT_TRUE(fake_esy_hal_watchdog_get_state()->enabled);
  fake_esy_os_task_set_create_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_start());
  fake_esy_os_task_entry_t *task = fake_esy_os_task_get_last_created();
  TEST_ASSERT_TRUE(task->created_static);
  TEST_ASSERT_EQUAL_PTR(&s_ctx.storage.task_control_storage, task->static_storage);
  TEST_ASSERT_EQUAL_PTR(s_ctx.storage.task_stack_buffer.bytes, task->stack_buffer);
  TEST_ASSERT_EQUAL_UINT32(512u, task->stack_size_bytes);
  TEST_ASSERT_EQUAL_UINT8(1u, task->core_affinity);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_start());
}

static void park_return(void) { longjmp(s_ctx.parked, 1); }
static void run_requested_stop(void) {
  fake_esy_os_task_entry_t *task = fake_esy_os_task_get_last_created();
  fake_esy_os_task_set_current(task->task_handle);
  fake_esy_os_task_set_park_hook(park_return);
  if (setjmp(s_ctx.parked) == 0) task->entry(task->arg);
  fake_esy_os_task_set_current(NULL);
  TEST_ASSERT_TRUE(task->parked);
}
void test_stop_waits_for_real_entry_to_park_before_reusing_storage(void) {
  init_monitor(); monitor_context();
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_watchdog_monitor_stop());
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_delete_count());
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_start());
  run_requested_stop();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_stop());
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_os_task_get_delete_count());
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_stop());
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_start());
}
void test_deinit_busy_retains_resources_and_forbids_restart_until_parked(void) {
  init_monitor(); monitor_context();
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_watchdog_monitor_deinit());
  TEST_ASSERT_FALSE(fake_esy_hal_watchdog_get_state()->enabled);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_start());
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_init(&(esy_watchdog_monitor_config_t){0}));
  run_requested_stop();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_deinit());
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_deinit());
  init_monitor();
}
void test_irreversible_watchdog_cleanup_preserves_supervision(void) {
  init_monitor(); monitor_context();
  fake_esy_hal_watchdog_get_state()->deinit_result = ESY_ERR_NOT_SUPPORTED;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_watchdog_monitor_deinit());
  TEST_ASSERT_TRUE(fake_esy_hal_watchdog_get_state()->enabled);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_delete_count());
  check_once();
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_hal_watchdog_get_state()->feed_count);
  fake_esy_hal_watchdog_get_state()->deinit_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_watchdog_monitor_deinit());
  check_once();
  TEST_ASSERT_EQUAL_UINT32(2u, fake_esy_hal_watchdog_get_state()->feed_count);
}

void test_registration_pool_errors_and_last_check_status(void) {
  bool alive = true;
  esy_watchdog_report_t third;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_register("task", &third));
  init_monitor();
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_register(NULL, &third));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_register("task", NULL));
  memset(&s_ctx.first, 0xa5, sizeof(s_ctx.first));
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_register("first", &s_ctx.first));
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_register("second", &s_ctx.second));
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_watchdog_monitor_register("third", &third));
  TEST_ASSERT_EQUAL_INT32(2, esy_watchdog_monitor_get_task_count());
  esy_watchdog_monitor_report(&s_ctx.first);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_get_task_status(&s_ctx.first, &alive));
  TEST_ASSERT_FALSE(alive);
  check_once();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_get_task_status(&s_ctx.first, &alive));
  TEST_ASSERT_TRUE(alive);
  check_once();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_get_task_status(&s_ctx.first, &alive));
  TEST_ASSERT_FALSE(alive);
  esy_watchdog_monitor_unregister(&s_ctx.first);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_get_task_status(&s_ctx.first, &alive));
  TEST_ASSERT_EQUAL_INT32(1, esy_watchdog_monitor_get_task_count());
}

void test_stale_reports_rejected_after_slot_reuse_and_reinitialization(void) {
  bool alive;
  init_monitor();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_register("first", &s_ctx.first));
  esy_watchdog_report_t stale = s_ctx.first;
  esy_watchdog_monitor_unregister(&s_ctx.first);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_register("second", &s_ctx.second));
  esy_watchdog_monitor_report(&stale);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_get_task_status(&stale, &alive));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_enable_task_check(&stale));
  esy_watchdog_monitor_disable_task_check(&stale);
  esy_watchdog_monitor_unregister(&stale);
  TEST_ASSERT_EQUAL_INT32(1, esy_watchdog_monitor_get_task_count());
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_deinit());
  init_monitor();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_register("new epoch", &s_ctx.first));
  TEST_ASSERT_NOT_EQUAL_UINT32(stale.generation, s_ctx.first.generation);
  esy_watchdog_monitor_report(&stale);
  esy_watchdog_monitor_unregister(&stale);
  TEST_ASSERT_EQUAL_INT32(1, esy_watchdog_monitor_get_task_count());
  check_once();
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.alert_count);
}

static void reuse_slot_and_report(void) {
  esy_watchdog_monitor_unregister(&s_ctx.first);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_register("replacement", &s_ctx.second));
  esy_watchdog_monitor_report(&s_ctx.second);
}
void test_reuse_after_scan_cannot_consume_new_owner_heartbeat(void) {
  init_monitor(); monitor_context();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_register("first", &s_ctx.first));
  esy_watchdog_monitor_report(&s_ctx.first);
  fake_esy_os_task_set_critical_exit_hook(reuse_slot_and_report);
  check_once(); check_once();
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.alert_count);
}
void test_report_commit_cannot_overwrite_replacement_heartbeat(void) {
  init_monitor(); monitor_context();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_register("first", &s_ctx.first));
  esy_watchdog_report_t copy = s_ctx.first;
  fake_esy_os_task_set_critical_exit_hook(reuse_slot_and_report);
  esy_watchdog_monitor_report(&copy);
  check_once();
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.alert_count);
}
static void unregister_first(void) { esy_watchdog_monitor_unregister(&s_ctx.first); }
void test_enable_and_unregister_are_serialized_without_lost_control(void) {
  init_monitor();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_register("first", &s_ctx.first));
  fake_esy_os_task_set_critical_exit_hook(unregister_first);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_enable_task_check(&s_ctx.first));
  TEST_ASSERT_EQUAL_INT32(0, esy_watchdog_monitor_get_task_count());
}
void test_name_is_copied_at_registration_and_again_before_callback(void) {
  init_monitor(); monitor_context();
  char name[] = "original";
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_register(name, &s_ctx.first));
  memset(name, 'X', sizeof(name) - 1u);
  fake_esy_os_task_set_critical_exit_hook(reuse_slot_and_report);
  check_once();
  TEST_ASSERT_EQUAL_STRING("original", s_ctx.alert_name);
  check_once();
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.alert_count);
}
void test_callback_pair_is_retained_until_invocation_finishes(void) {
  init_monitor();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_register("first", &s_ctx.first));
  s_ctx.probe_callback = true;
  check_once();
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.alert_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_set_alert_cb(NULL, NULL));
  check_once(); check_once();
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.alert_count);
}
void test_boolean_window_disabled_task_and_feed_controls(void) {
  init_monitor();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_register("first", &s_ctx.first));
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_register("second", &s_ctx.second));
  check_once();
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.alert_count);
  TEST_ASSERT_EQUAL_STRING("first", s_ctx.alert_name);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.last_alert.registered_task_count);
  esy_watchdog_monitor_disable_task_check(&s_ctx.second);
  esy_watchdog_monitor_report(&s_ctx.first); check_once();
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_hal_watchdog_get_state()->feed_count);
  esy_watchdog_monitor_disable_hw_feed();
  esy_watchdog_monitor_report(&s_ctx.first); check_once();
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_hal_watchdog_get_state()->feed_count);
}
static void stop_during_wait(void) {
  fake_esy_os_task_set_current(NULL);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_watchdog_monitor_stop());
}
static void fake_advance_check_period(void) {
  fake_esy_hal_system_advance_tick(10u);
}
void test_iteration_checks_and_stop_wakes_without_another_feed(void) {
  init_monitor(); esy_watchdog_monitor_ctx_t *ctx = monitor_context();
  fake_esy_os_task_set_current(fake_esy_os_task_get_last_created()->task_handle);
  fake_esy_os_task_set_notify_take_hook(fake_advance_check_period);
  TEST_ASSERT_TRUE(esy_watchdog_monitor_iteration(ctx));
  TEST_ASSERT_EQUAL_UINT32(10u, fake_esy_os_task_get_last_created()->notify_take_timeout_ms);
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_hal_watchdog_get_state()->feed_count);
  fake_esy_os_task_set_notify_take_hook(stop_during_wait);
  TEST_ASSERT_FALSE(esy_watchdog_monitor_iteration(ctx));
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_hal_watchdog_get_state()->feed_count);
  TEST_ASSERT_FALSE(esy_watchdog_monitor_iteration(NULL));
  esy_watchdog_monitor_check_once(NULL);
}

static void delayed_feed(void) {
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_critical_nesting());
  s_ctx.now_us += 50000u;
}
void test_deadline_uses_pre_feed_time_and_certified_bound(void) {
  esy_watchdog_monitor_config_t cfg = config();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_init(&cfg));
  uint64_t deadline;
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_get_hard_deadline_atomic(&cfg.clock, &deadline));
  TEST_ASSERT_EQUAL_UINT64(101000u, deadline);
  fake_esy_hal_watchdog_get_state()->feed_hook = delayed_feed;
  check_once();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_get_hard_deadline_atomic(&cfg.clock, &deadline));
  TEST_ASSERT_EQUAL_UINT64(101000u, deadline);
  TEST_ASSERT_EQUAL_UINT64(51000u, s_ctx.now_us);
}
void test_deadline_fails_closed_for_unproven_clock_failure_and_inflight_publish(void) {
  esy_watchdog_monitor_config_t cfg = config(); uint64_t deadline = 7u;
  fake_esy_hal_watchdog_get_state()->bound_result = ESY_ERR_NOT_SUPPORTED;
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_init(&cfg));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_watchdog_monitor_get_hard_deadline_atomic(&cfg.clock, &deadline));
  TEST_ASSERT_EQUAL_UINT64(7u, deadline);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_deinit());
  fake_esy_hal_watchdog_get_state()->bound_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_init(&cfg));
  esy_watchdog_monitor_ctx_t *ctx = monitor_context();
  atomic_fetch_add(&ctx->deadline_sequence, 1u);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_watchdog_monitor_get_hard_deadline_atomic(&cfg.clock, &deadline));
  atomic_fetch_add(&ctx->deadline_sequence, 1u);
  s_ctx.clock_result = ESY_ERR_IO; check_once();
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_get_hard_deadline_atomic(&cfg.clock, &deadline));
  TEST_ASSERT_EQUAL_UINT64(7u, deadline);
}
void test_deadline_validates_domain_and_saturates_without_wrapping(void) {
  esy_watchdog_monitor_config_t cfg = config(); uint64_t deadline;
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_init(&cfg));
  esy_monotonic_clock_t wrong = cfg.clock; wrong.ctx = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_get_hard_deadline_atomic(&wrong, &deadline));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_get_hard_deadline_atomic(NULL, &deadline));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_get_hard_deadline_atomic(&cfg.clock, NULL));
  s_ctx.now_us = UINT64_MAX - 1u; check_once();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_get_hard_deadline_atomic(&cfg.clock, &deadline));
  TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, deadline);
}
void test_uninitialized_and_invalid_handle_paths_leave_outputs_unchanged(void) {
  bool alive = true;
  esy_watchdog_report_t invalid = {0};
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_set_alert_cb(NULL, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_get_task_count());
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_enable_task_check(&invalid));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_get_task_status(&invalid, &alive));
  esy_watchdog_monitor_unregister(&invalid); esy_watchdog_monitor_report(&invalid);
  esy_watchdog_monitor_disable_task_check(&invalid); esy_watchdog_monitor_disable_hw_feed();
  init_monitor();
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_enable_task_check(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_get_task_status(NULL, &alive));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_get_task_status(&invalid, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_get_task_status(&invalid, &alive));
  TEST_ASSERT_TRUE(alive);
  esy_watchdog_monitor_unregister(NULL); esy_watchdog_monitor_report(NULL);
  esy_watchdog_monitor_disable_task_check(NULL);
}
void test_generation_exhaustion_never_wraps_to_an_old_token(void) {
  init_monitor(); esy_watchdog_monitor_ctx_t *ctx = monitor_context();
  uint32_t previous = ctx->generation_seed;
  ctx->generation_seed = UINT32_MAX;
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_watchdog_monitor_register("exhausted", &s_ctx.first));
  ctx->generation_seed = previous;
}

static void try_cleanup_during_feed(void) {
  fake_esy_os_task_set_current(NULL);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_watchdog_monitor_deinit());
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_watchdog_get_state()->deinit_count);
}
void test_cleanup_cannot_stop_hardware_during_an_admitted_feed(void) {
  init_monitor();
  fake_esy_hal_watchdog_get_state()->feed_hook = try_cleanup_during_feed;
  check_once();
  TEST_ASSERT_TRUE(fake_esy_hal_watchdog_get_state()->enabled);
}
void test_long_names_are_terminated_and_disabled_checks_preserve_last_result(void) {
  bool alive;
  init_monitor();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_register(
      "abcdefghijklmnopqrstuvwxyz0123456789", &s_ctx.first));
  check_once();
  TEST_ASSERT_EQUAL_UINT32(31u, strlen(s_ctx.alert_name));
  esy_watchdog_monitor_report(&s_ctx.first); check_once();
  esy_watchdog_monitor_disable_task_check(&s_ctx.first); check_once();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_get_task_status(&s_ctx.first, &alive));
  TEST_ASSERT_TRUE(alive);
}

static esy_watchdog_monitor_core_config_t core_config(void) {
  esy_watchdog_monitor_core_config_t cfg;
  esy_watchdog_monitor_core_default_cfg(&cfg);
  ESY_WATCHDOG_MONITOR_CORE_BIND_STATIC_RESOURCES(&s_ctx.core_resources, s_ctx.storage);
  cfg.check_period_ms = 10u;
  cfg.hw_timeout_ms = 100u;
  cfg.clock = (esy_monotonic_clock_t){&k_clock_ops, &s_ctx.now_us};
  cfg.static_resources = &s_ctx.core_resources;
  return cfg;
}

static void init_core(void) {
  esy_watchdog_monitor_core_config_t cfg = core_config();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_core_init(&cfg));
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_set_alert_cb(capture_alert, &s_ctx));
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_core_start());
}

void test_core_early_polls_preserve_heartbeats_and_only_due_scan_feeds(void) {
  init_core();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_register("polling participant", &s_ctx.first));
  esy_watchdog_monitor_report(&s_ctx.first);
  for (unsigned i = 0u; i < 100u; ++i)
    TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_core_process());
  fake_esy_hal_system_set_tick(9u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_core_process());
  TEST_ASSERT_EQUAL(0u, fake_esy_hal_watchdog_get_state()->feed_count);
  fake_esy_hal_system_set_tick(10u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_core_process());
  bool alive = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_get_task_status(&s_ctx.first, &alive));
  TEST_ASSERT_TRUE(alive);
  TEST_ASSERT_EQUAL(1u, fake_esy_hal_watchdog_get_state()->feed_count);
  TEST_ASSERT_EQUAL(0u, s_ctx.alert_count);
  fake_esy_hal_system_set_tick(20u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_core_process());
  TEST_ASSERT_EQUAL(1u, s_ctx.alert_count);
  TEST_ASSERT_EQUAL(1u, fake_esy_hal_watchdog_get_state()->feed_count);
  TEST_ASSERT_EQUAL(0u, fake_esy_os_task_get_create_static_count());
}

void test_core_late_polls_scan_once_without_replaying_missed_windows(void) {
  init_core();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_register("loop", &s_ctx.first));
  esy_watchdog_monitor_report(&s_ctx.first);
  fake_esy_hal_system_set_tick(35u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_core_process());
  TEST_ASSERT_EQUAL(1u, fake_esy_hal_watchdog_get_state()->feed_count);
  TEST_ASSERT_EQUAL(0u, s_ctx.alert_count);
  fake_esy_hal_system_set_tick(44u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_core_process());
  TEST_ASSERT_EQUAL(0u, s_ctx.alert_count);
  fake_esy_hal_system_set_tick(45u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_core_process());
  TEST_ASSERT_EQUAL(1u, s_ctx.alert_count);
}

void test_core_period_handles_unsigned_tick_wrap_and_restart_anchor(void) {
  fake_esy_hal_system_set_tick(UINT32_MAX - 4u);
  init_core();
  fake_esy_hal_system_set_tick(4u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_core_process());
  TEST_ASSERT_EQUAL(0u, fake_esy_hal_watchdog_get_state()->feed_count);
  fake_esy_hal_system_set_tick(5u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_core_process());
  TEST_ASSERT_EQUAL(1u, fake_esy_hal_watchdog_get_state()->feed_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_stop());
  TEST_ASSERT_TRUE(fake_esy_hal_watchdog_get_state()->enabled);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_core_process());
  fake_esy_hal_system_set_tick(50u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_core_start());
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_core_process());
  TEST_ASSERT_EQUAL(1u, fake_esy_hal_watchdog_get_state()->feed_count);
  fake_esy_hal_system_set_tick(60u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_core_process());
  TEST_ASSERT_EQUAL(2u, fake_esy_hal_watchdog_get_state()->feed_count);
}

void test_core_callbacks_keep_context_alive_and_reject_recursive_lifecycle(void) {
  init_core();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_register("loop", &s_ctx.first));
  s_ctx.core_callback_probe = true;
  fake_esy_hal_system_set_tick(10u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_core_process());
  TEST_ASSERT_EQUAL(1u, s_ctx.alert_count);
  fake_esy_hal_system_set_tick(20u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_core_process());
  TEST_ASSERT_EQUAL(1u, fake_esy_hal_watchdog_get_state()->feed_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_set_alert_cb(NULL, NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_deinit());
}

void test_core_failed_hardware_stop_retains_supervision_and_deadline(void) {
  init_core();
  fake_esy_hal_watchdog_get_state()->deinit_result = ESY_ERR_NOT_SUPPORTED;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_watchdog_monitor_deinit());
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_core_init(&(esy_watchdog_monitor_core_config_t){0}));
  fake_esy_hal_system_set_tick(10u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_core_process());
  uint64_t deadline;
  esy_watchdog_monitor_core_config_t cfg = core_config();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_get_hard_deadline_atomic(&cfg.clock, &deadline));
  TEST_ASSERT_EQUAL_UINT64(101000u, deadline);
  TEST_ASSERT_EQUAL(1u, fake_esy_hal_watchdog_get_state()->feed_count);
  TEST_ASSERT_EQUAL(0u, fake_esy_os_task_get_delete_count());
  fake_esy_hal_watchdog_get_state()->deinit_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_deinit());
  init_core();
}

void test_core_and_os_lifecycle_modes_are_exclusive(void) {
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_core_start());
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_core_process());
  init_monitor();
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_core_start());
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_core_process());
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_deinit());
  esy_watchdog_monitor_core_config_t cfg = core_config();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_core_init(&cfg));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_start());
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_core_process());
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_core_start());
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_watchdog_monitor_core_start());
}

void test_core_config_failure_and_hardware_failure_leave_resources_retryable(void) {
  esy_watchdog_monitor_core_config_t cfg;
  esy_watchdog_monitor_core_default_cfg(NULL);
  esy_watchdog_monitor_core_default_cfg(&cfg);
  TEST_ASSERT_NULL(cfg.static_resources);
  TEST_ASSERT_EQUAL(1000u, cfg.check_period_ms);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_core_init(&cfg));
  cfg = core_config(); cfg.check_period_ms = UINT32_MAX / 2u + 1u; cfg.hw_timeout_ms = UINT32_MAX;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_core_init(&cfg));
  cfg = core_config(); cfg.static_resources->task_slot_count = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_core_init(&cfg));
  cfg = core_config(); cfg.static_resources->task_slot_count = UINT32_MAX;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_core_init(&cfg));
  cfg = core_config(); cfg.static_resources->task_slots = (void *)&s_ctx.storage.task_slots[0].bytes[1];
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_core_init(&cfg));
  TEST_ASSERT_EQUAL(0u, fake_esy_hal_watchdog_get_state()->init_count);
  cfg = core_config();
  fake_esy_hal_watchdog_set_init_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_watchdog_monitor_core_init(&cfg));
  fake_esy_hal_watchdog_set_init_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_core_init(&cfg));
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_deinit());
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_core_init(&cfg));
}

void test_os_early_notifications_do_not_consume_heartbeats_or_advance_check_period(void) {
  init_monitor();
  esy_watchdog_monitor_ctx_t *ctx = monitor_context();
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_register("task", &s_ctx.first));
  esy_watchdog_monitor_report(&s_ctx.first);
  fake_esy_os_task_set_current(fake_esy_os_task_get_last_created()->task_handle);
  fake_esy_hal_system_set_tick(3u);
  TEST_ASSERT_TRUE(esy_watchdog_monitor_iteration(ctx));
  TEST_ASSERT_EQUAL(7u, fake_esy_os_task_get_last_created()->notify_take_timeout_ms);
  TEST_ASSERT_EQUAL(0u, fake_esy_hal_watchdog_get_state()->feed_count);
  fake_esy_hal_system_set_tick(10u);
  TEST_ASSERT_TRUE(esy_watchdog_monitor_iteration(ctx));
  TEST_ASSERT_EQUAL(0u, fake_esy_os_task_get_last_created()->notify_take_timeout_ms);
  TEST_ASSERT_EQUAL(1u, fake_esy_hal_watchdog_get_state()->feed_count);
  fake_esy_os_task_set_current(NULL);
}

void test_os_resource_validation_rejects_missing_or_misaligned_storage_before_hardware(void) {
  esy_watchdog_monitor_config_t cfg = config();
  cfg.static_resources->task_stack_buffer = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_init(&cfg));
  cfg = config(); cfg.static_resources->task_stack_buffer_size = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_init(&cfg));
  cfg = config(); cfg.static_resources->task_control_storage = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_init(&cfg));
  cfg = config(); cfg.static_resources->task_control_storage =
      (void *)((uint8_t *)&s_ctx.storage.task_control_storage + 1u);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_watchdog_monitor_init(&cfg));
  TEST_ASSERT_EQUAL(0u, fake_esy_hal_watchdog_get_state()->init_count);
}

static void advance_or_request_stop(void) {
  if (fake_esy_hal_watchdog_get_state()->feed_count == 0u) {
    fake_esy_hal_system_advance_tick(10u);
  } else {
    esy_os_task_t *current = fake_esy_os_task_get_current();
    fake_esy_os_task_set_current(NULL);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_watchdog_monitor_stop());
    fake_esy_os_task_set_current(current);
  }
}

void test_real_os_entry_checks_due_window_then_stops_without_an_extra_feed(void) {
  init_monitor(); monitor_context();
  fake_esy_os_task_set_notify_take_hook(advance_or_request_stop);
  run_requested_stop();
  fake_esy_os_task_set_notify_take_hook(NULL);
  TEST_ASSERT_EQUAL(1u, fake_esy_hal_watchdog_get_state()->feed_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_watchdog_monitor_stop());
  TEST_ASSERT_EQUAL(1u, fake_esy_os_task_get_delete_count());
}
