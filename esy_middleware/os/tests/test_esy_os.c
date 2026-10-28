/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/os/esy_os.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "unity.h"

void vPortSuppressTicksAndSleep(TickType_t expected_idle_time);

typedef struct {
  eSleepModeStatus sleep_status;
  bool tick_pending;
  uint32_t prepare_partial_tick_us;
  uint32_t suspend_partial_tick_us;
  uint32_t confirm_calls;
  uint32_t scheduler_start_calls;
  uint64_t step_ticks;
  uint64_t kernel_ticks;
  uint64_t next_unblock_tick;
  uint64_t safe_step_ticks;
  uint32_t pending_ticks;
  uint32_t catch_up_ticks;
  uint32_t unblocked_tasks;
  TickType_t native_ticks;
  uint32_t core_id;
  uint32_t isr_lock_calls;
  uint32_t isr_unlock_calls;
  bool isr_lock_held;
  uint32_t irq_save_calls;
  uint32_t irq_restore_calls;
  uint32_t tick_suspend_calls;
  uint32_t tick_resume_calls;
  uint32_t prepare_calls;
  uint32_t enter_calls;
  uint32_t recover_calls;
  uint32_t complete_calls;
  uint32_t abort_calls;
  uint32_t reset_calls;
  uint32_t yield_calls;
  uint32_t event_sequence;
  uint32_t step_tick_order;
  uint32_t tick_resume_order;
  uint32_t irq_restore_order;
  uint32_t recover_order;
  uint32_t complete_order;
  uint32_t abort_order;
  uint64_t maximum_sleep_us;
  uint64_t now_us;
  uint64_t recovery_us;
  uint32_t clock_calls;
  uint32_t clock_fail_call;
  bool clock_regresses;
  esy_err_t prepare_recovery_result;
  esy_err_t tick_phase_result;
  esy_err_t prepare_result;
  esy_err_t tick_suspend_result;
  esy_err_t enter_result;
  esy_err_t recover_result;
  esy_err_t complete_result;
  esy_err_t abort_result;
  esy_os_tickless_result_t result;
  int scheduler_state;
  bool irq_masked;
} os_test_ctx_t;

static os_test_ctx_t s_ctx;

void vTaskStartScheduler(void) { s_ctx.scheduler_start_calls++; }

eSleepModeStatus eTaskConfirmSleepModeStatus(void) {
  s_ctx.confirm_calls++;
  return s_ctx.pending_ticks != 0u ? eAbortSleep : s_ctx.sleep_status;
}

void vTaskStepTick(TickType_t ticks) {
  TEST_ASSERT_TRUE(s_ctx.irq_masked);
  TEST_ASSERT_LESS_OR_EQUAL_UINT64(s_ctx.safe_step_ticks, ticks);
  s_ctx.safe_step_ticks -= ticks;
  s_ctx.step_ticks += ticks;
  s_ctx.kernel_ticks += ticks;
  s_ctx.native_ticks += ticks;
  s_ctx.step_tick_order = ++s_ctx.event_sequence;
}

uint32_t os_fixture_tick_core_id(void) { return s_ctx.core_id; }

void os_fixture_tick_isr_lock(void) {
  TEST_ASSERT_TRUE(s_ctx.irq_masked);
  TEST_ASSERT_FALSE(s_ctx.isr_lock_held);
  s_ctx.isr_lock_held = true;
  ++s_ctx.isr_lock_calls;
}

void os_fixture_tick_isr_unlock(void) {
  TEST_ASSERT_TRUE(s_ctx.irq_masked);
  TEST_ASSERT_TRUE(s_ctx.isr_lock_held);
  s_ctx.isr_lock_held = false;
  ++s_ctx.isr_unlock_calls;
}

BaseType_t xTaskIncrementTick(void) {
  TEST_ASSERT_TRUE(s_ctx.irq_masked);
#if configNUMBER_OF_CORES > 1
  TEST_ASSERT_TRUE(s_ctx.isr_lock_held);
#endif
  ++s_ctx.pending_ticks;
  ++s_ctx.catch_up_ticks;
  s_ctx.step_tick_order = ++s_ctx.event_sequence;
  return pdFALSE;
}

/* Model the idle owner's xTaskResumeAll after the port hook returns. Pending
 * ticks use normal expiry processing; a hardware pending IRQ contributes once. */
static void run_tickless(TickType_t expected_idle_time) {
  s_ctx.safe_step_ticks = expected_idle_time == 0u ? 0u : expected_idle_time - 1u;
  s_ctx.next_unblock_tick = s_ctx.kernel_ticks + expected_idle_time;
  const uint32_t resumes_before = s_ctx.tick_resume_calls;
  vPortSuppressTicksAndSleep(expected_idle_time);
  if (!s_ctx.irq_masked) {
    if (s_ctx.tick_pending && s_ctx.tick_resume_calls != resumes_before) {
      ++s_ctx.pending_ticks;
      s_ctx.tick_pending = false;
    }
    while (s_ctx.pending_ticks != 0u) {
      --s_ctx.pending_ticks;
      ++s_ctx.kernel_ticks;
      ++s_ctx.native_ticks;
      if (s_ctx.kernel_ticks == s_ctx.next_unblock_tick) ++s_ctx.unblocked_tasks;
    }
  }
}

int xTaskGetSchedulerState(void) { return s_ctx.scheduler_state; }

void os_fixture_disable_interrupts(void) {}

void os_fixture_yield_from_isr(int should_yield) {
  if (should_yield != 0) s_ctx.yield_calls++;
}

uint32_t esy_hal_system_irq_save(void) {
  TEST_ASSERT_FALSE(s_ctx.irq_masked);
  s_ctx.irq_save_calls++;
  s_ctx.irq_masked = true;
  return 0x55u;
}

void esy_hal_system_irq_restore(uint32_t saved_status) {
  TEST_ASSERT_EQUAL_HEX32(0x55u, saved_status);
  TEST_ASSERT_TRUE(s_ctx.irq_masked);
  s_ctx.irq_restore_calls++;
  s_ctx.irq_restore_order = ++s_ctx.event_sequence;
  s_ctx.irq_masked = false;
}

esy_err_t esy_hal_system_suspend_tick(uint32_t tick_rate_hz,
                                           uint32_t *out_partial_tick_us) {
  TEST_ASSERT_EQUAL_UINT32(configTICK_RATE_HZ, tick_rate_hz);
  TEST_ASSERT_NOT_NULL(out_partial_tick_us);
  s_ctx.tick_suspend_calls++;
  *out_partial_tick_us = s_ctx.suspend_partial_tick_us;
  return s_ctx.tick_suspend_result;
}

esy_err_t esy_hal_system_get_tick_partial_us(
    uint32_t tick_rate_hz, uint32_t *out_partial_tick_us) {
  TEST_ASSERT_EQUAL_UINT32(configTICK_RATE_HZ, tick_rate_hz);
  TEST_ASSERT_NOT_NULL(out_partial_tick_us);
  TEST_ASSERT_FALSE(s_ctx.irq_masked);
  *out_partial_tick_us = s_ctx.prepare_partial_tick_us;
  return s_ctx.tick_phase_result;
}

void esy_hal_system_resume_tick(void) {
  s_ctx.tick_resume_calls++;
  s_ctx.tick_resume_order = ++s_ctx.event_sequence;
}

bool esy_hal_system_tick_is_pending(void) { return s_ctx.tick_pending; }

void esy_hal_system_reset(void) { s_ctx.reset_calls++; }

static esy_err_t tickless_prepare(void *arg, uint64_t maximum_sleep_us) {
  os_test_ctx_t *ctx = arg;
  TEST_ASSERT_FALSE(ctx->irq_masked);
  ctx->prepare_calls++;
  ctx->maximum_sleep_us = maximum_sleep_us;
  return ctx->prepare_result;
}

static esy_err_t tickless_enter(void *arg,
                                esy_os_tickless_result_t *out_result) {
  os_test_ctx_t *ctx = arg;
  TEST_ASSERT_TRUE(ctx->irq_masked);
  ctx->enter_calls++;
  ctx->now_us += ctx->result.elapsed_us;
  *out_result = ctx->result;
  return ctx->enter_result;
}

static esy_err_t tickless_complete(void *arg) {
  os_test_ctx_t *ctx = arg;
  TEST_ASSERT_FALSE(ctx->irq_masked);
  TEST_ASSERT_EQUAL_UINT32(ctx->complete_calls + 1u, ctx->tick_resume_calls);
  ctx->complete_calls++;
  ctx->complete_order = ++ctx->event_sequence;
  return ctx->complete_result;
}

static esy_err_t tickless_abort(void *arg) {
  os_test_ctx_t *ctx = arg;
  TEST_ASSERT_FALSE(ctx->irq_masked);
  ctx->abort_calls++;
  ctx->abort_order = ++ctx->event_sequence;
  return ctx->abort_result;
}

static esy_err_t tickless_recover_atomic(void *arg) {
  os_test_ctx_t *ctx = arg;
  TEST_ASSERT_TRUE(ctx->irq_masked);
  ctx->recover_calls++;
  ctx->now_us += ctx->recovery_us;
  ctx->recover_order = ++ctx->event_sequence;
  return ctx->recover_result;
}

static esy_err_t tickless_check_prepare_recovery(void *arg) {
  os_test_ctx_t *ctx = arg;
  TEST_ASSERT_FALSE(ctx->irq_masked);
  return ctx->prepare_recovery_result;
}

static esy_err_t tickless_read_clock_atomic(void *arg, uint64_t *out_time_us) {
  os_test_ctx_t *ctx = arg;
  TEST_ASSERT_TRUE(ctx->irq_masked);
  ctx->clock_calls++;
  if (ctx->clock_calls == ctx->clock_fail_call) return ESY_ERR_IO;
  *out_time_us = ctx->clock_regresses && ctx->clock_calls > 1u ? 0u : ctx->now_us;
  return ESY_OK;
}

static const esy_os_tickless_ops_t k_tickless_ops = {
    .prepare = tickless_prepare,
    .enter_atomic = tickless_enter,
    .recover_atomic = tickless_recover_atomic,
    .complete = tickless_complete,
    .abort = tickless_abort,
    .check_prepare_recovery = tickless_check_prepare_recovery,
    .read_clock_atomic = tickless_read_clock_atomic,
};

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.sleep_status = eStandardSleep;
  s_ctx.now_us = 1000u;
  s_ctx.result.committed = true;
  s_ctx.result.entered = true;
  s_ctx.result.elapsed_valid = true;
  esy_os_init();
}

void tearDown(void) {
  (void)esy_os_clear_tickless_ops(&k_tickless_ops, &s_ctx);
}

void test_os_core_lifecycle_and_yield_helpers(void) {
  esy_os_start_scheduler();
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.scheduler_start_calls);
  esy_os_yield_from_isr(false);
  esy_os_yield_from_isr(true);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.yield_calls);
  TEST_ASSERT_EQUAL_UINT8(configNUMBER_OF_CORES, esy_os_core_count());
  s_ctx.scheduler_state = taskSCHEDULER_NOT_STARTED;
  TEST_ASSERT_FALSE(esy_os_started());
  s_ctx.scheduler_state = 1;
  TEST_ASSERT_TRUE(esy_os_started());
  esy_os_systick();
}

void test_tickless_registration_contract(void) {
  esy_os_tickless_ops_t invalid_ops = {0};
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_os_set_tickless_ops(NULL, &s_ctx));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_os_set_tickless_ops(&invalid_ops, &s_ctx));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_os_clear_tickless_ops(&k_tickless_ops, NULL));
}

void test_tickless_settles_partial_elapsed_before_restoring_interrupts(void) {
  s_ctx.prepare_partial_tick_us = 500u;
  s_ctx.suspend_partial_tick_us = 500u;
  s_ctx.result.elapsed_us = 19500u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));

  run_tickless(20u);

  TEST_ASSERT_EQUAL_UINT64(19500u, s_ctx.maximum_sleep_us);
  TEST_ASSERT_EQUAL_UINT32(19u, s_ctx.step_ticks);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.catch_up_ticks);
  TEST_ASSERT_EQUAL_UINT64(20u, s_ctx.kernel_ticks);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.unblocked_tasks);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.recover_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.complete_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.tick_suspend_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.tick_resume_calls);
  TEST_ASSERT_LESS_THAN_UINT32(s_ctx.irq_restore_order,
                               s_ctx.step_tick_order);
  TEST_ASSERT_LESS_THAN_UINT32(s_ctx.tick_resume_order, s_ctx.recover_order);
  TEST_ASSERT_LESS_THAN_UINT32(s_ctx.irq_restore_order, s_ctx.recover_order);
  TEST_ASSERT_LESS_THAN_UINT32(s_ctx.complete_order, s_ctx.irq_restore_order);
}

void test_tick_phase_query_failure_skips_tickless_transaction(void) {
  s_ctx.tick_phase_result = ESY_ERR_NOT_SUPPORTED;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));

  run_tickless(20u);

  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.prepare_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.irq_save_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.tick_suspend_calls);
}

void test_pending_tick_is_left_for_isr_without_double_counting(void) {
  s_ctx.tick_pending = true;
  s_ctx.result.elapsed_us = 10000u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));

  run_tickless(10u);

  TEST_ASSERT_EQUAL_UINT32(9u, s_ctx.step_ticks);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.catch_up_ticks);
  TEST_ASSERT_EQUAL_UINT64(10u, s_ctx.kernel_ticks);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.unblocked_tasks);
}

void test_fractional_elapsed_has_no_long_term_tick_drift(void) {
  s_ctx.result.elapsed_us = 333u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));

  for (uint32_t i = 0u; i < 3000u; i++) {
    run_tickless(2u);
  }

  TEST_ASSERT_EQUAL_UINT32(999u, s_ctx.step_ticks);
}

void test_final_kernel_check_aborts_prepared_transaction(void) {
  s_ctx.sleep_status = eAbortSleep;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));

  run_tickless(10u);

  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.prepare_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.recover_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.abort_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.tick_suspend_calls);
  TEST_ASSERT_LESS_THAN_UINT32(s_ctx.abort_order, s_ctx.irq_restore_order);
}

void test_preexisting_kernel_pending_ticks_abort_sleep_and_remain_accounted(void) {
  s_ctx.pending_ticks = 2u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));
  run_tickless(10u);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.abort_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.enter_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.tick_suspend_calls);
  TEST_ASSERT_EQUAL_UINT64(0u, s_ctx.step_ticks);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.catch_up_ticks);
  TEST_ASSERT_EQUAL_UINT64(2u, s_ctx.kernel_ticks);
}

void test_uncommitted_atomic_failure_uses_abort_path(void) {
  s_ctx.enter_result = ESY_ERR_BUSY;
  s_ctx.result.committed = false;
  s_ctx.result.entered = false;
  s_ctx.result.elapsed_valid = false;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));

  run_tickless(10u);

  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.abort_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.recover_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.complete_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.step_ticks);
  TEST_ASSERT_LESS_THAN_UINT32(s_ctx.abort_order, s_ctx.tick_resume_order);
  TEST_ASSERT_LESS_THAN_UINT32(s_ctx.abort_order, s_ctx.irq_restore_order);
}

void test_tick_suspend_failure_recovers_masked_then_aborts_after_irq_restore(
    void) {
  s_ctx.tick_suspend_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));

  run_tickless(10u);

  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.abort_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.recover_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.enter_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.tick_resume_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.irq_restore_calls);
  TEST_ASSERT_LESS_THAN_UINT32(s_ctx.abort_order, s_ctx.irq_restore_order);
}

void test_expired_partial_tick_recovers_before_tick_and_irq_restore(void) {
  s_ctx.prepare_partial_tick_us = 500u;
  s_ctx.suspend_partial_tick_us = 900u;
  s_ctx.recovery_us = 2000u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));

  run_tickless(1u);

  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.abort_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.recover_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.enter_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.step_ticks);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.catch_up_ticks);
  TEST_ASSERT_EQUAL_UINT64(2u, s_ctx.kernel_ticks);
  TEST_ASSERT_LESS_THAN_UINT32(s_ctx.abort_order, s_ctx.tick_resume_order);
  TEST_ASSERT_LESS_THAN_UINT32(s_ctx.abort_order, s_ctx.irq_restore_order);
}

void test_late_abort_failure_resets_after_irq_restore(void) {
  s_ctx.sleep_status = eAbortSleep;
  s_ctx.abort_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));

  run_tickless(10u);

  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.abort_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.recover_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.reset_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.tick_resume_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.irq_restore_calls);
}

void test_early_recovery_failure_resets_before_tick_or_irq_restore(void) {
  s_ctx.recover_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));

  run_tickless(10u);

  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.recover_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.complete_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.abort_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.reset_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.tick_resume_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.irq_restore_calls);
}

void test_kernel_abort_recovers_prepared_transaction(void) {
  s_ctx.sleep_status = eAbortSleep;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));
  run_tickless(10u);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.prepare_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.irq_save_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.irq_restore_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.abort_calls);
}

void test_no_tasks_waiting_timeout_is_a_valid_sleep_decision(void) {
  s_ctx.sleep_status = eNoTasksWaitingTimeout;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));

  run_tickless(10u);

  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.enter_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.complete_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.abort_calls);
}

void test_late_resume_failure_resets_after_tick_and_irq_restore(void) {
  s_ctx.complete_result = ESY_ERR_STATE;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));

  run_tickless(10u);

  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.reset_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.recover_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.tick_resume_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.irq_restore_calls);
}

void test_fatal_atomic_result_resets_before_tick_or_irq_restore(void) {
  s_ctx.result.fatal = true;
  s_ctx.result.fatal_error = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));

  run_tickless(10u);

  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.reset_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.tick_resume_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.irq_restore_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.complete_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.abort_calls);
}

void test_prepare_cleanup_failure_resets_with_interrupts_masked(void) {
  s_ctx.prepare_result = ESY_ERR_IO;
  s_ctx.prepare_recovery_result = ESY_ERR_STATE;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));
  run_tickless(10u);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.reset_calls);
  TEST_ASSERT_TRUE(s_ctx.irq_masked);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.enter_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.irq_restore_calls);
}

void test_clean_prepare_failure_returns_without_reset(void) {
  s_ctx.prepare_result = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));
  run_tickless(10u);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.reset_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.irq_save_calls);
}

void test_stopped_tick_interval_includes_atomic_recovery(void) {
  s_ctx.suspend_partial_tick_us = 500u;
  s_ctx.result.elapsed_us = 400u;
  s_ctx.recovery_us = 2000u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));
  run_tickless(20u);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.step_ticks);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.clock_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.reset_calls);
}

void test_recovery_time_is_accounted_when_entry_is_cancelled(void) {
  s_ctx.prepare_partial_tick_us = 500u;
  s_ctx.suspend_partial_tick_us = 500u;
  s_ctx.result.committed = false;
  s_ctx.result.entered = false;
  s_ctx.result.elapsed_valid = false;
  s_ctx.result.elapsed_us = 9500u;
  s_ctx.enter_result = ESY_ERR_BUSY;
  s_ctx.recovery_us = 2000u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));
  run_tickless(10u);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.enter_calls);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.clock_calls);
  TEST_ASSERT_EQUAL_UINT32(9u, s_ctx.step_ticks);
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.catch_up_ticks);
  TEST_ASSERT_EQUAL_UINT64(12u, s_ctx.kernel_ticks);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.unblocked_tasks);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.abort_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.complete_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.reset_calls);
}

void test_committed_recovery_overrun_replays_expired_ticks(void) {
  s_ctx.prepare_partial_tick_us = 500u;
  s_ctx.suspend_partial_tick_us = 500u;
  s_ctx.result.elapsed_us = 9500u;
  s_ctx.recovery_us = 2000u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));
  run_tickless(10u);
  TEST_ASSERT_EQUAL_UINT64(12u, s_ctx.kernel_ticks);
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.catch_up_ticks);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.unblocked_tasks);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.complete_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.abort_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.reset_calls);
}

void test_overrun_pending_tick_and_fraction_are_each_accounted_once(void) {
  s_ctx.prepare_partial_tick_us = 500u;
  s_ctx.suspend_partial_tick_us = 500u;
  s_ctx.result.elapsed_us = 9500u;
  s_ctx.recovery_us = 2250u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));
  for (unsigned interval = 0u; interval < 4u; ++interval) {
    s_ctx.tick_pending = true;
    run_tickless(10u);
  }
  TEST_ASSERT_EQUAL_UINT64(49u, s_ctx.kernel_ticks);
  TEST_ASSERT_EQUAL_UINT64(36u, s_ctx.step_ticks);
  TEST_ASSERT_EQUAL_UINT32(9u, s_ctx.catch_up_ticks);
  TEST_ASSERT_EQUAL_UINT32(4u, s_ctx.unblocked_tasks);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.pending_ticks);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.reset_calls);
}

void test_native_tick_wrap_uses_pending_expiry_processing(void) {
  s_ctx.native_ticks = portMAX_DELAY - 2u;
  s_ctx.result.elapsed_us = 4000u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));
  run_tickless(3u);
  TEST_ASSERT_EQUAL_UINT64(4u, s_ctx.kernel_ticks);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.catch_up_ticks);
  TEST_ASSERT_EQUAL_UINT64(1u, s_ctx.native_ticks);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.unblocked_tasks);
}

void test_excessive_tick_replay_fails_closed_before_partial_settlement(void) {
  s_ctx.result.elapsed_us = 2000000u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));
  run_tickless(1u);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.reset_calls);
  TEST_ASSERT_TRUE(s_ctx.irq_masked);
  TEST_ASSERT_EQUAL_UINT64(0u, s_ctx.step_ticks);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.catch_up_ticks);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.tick_resume_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.irq_restore_calls);
}

void test_tick_replay_accepts_its_exact_documented_budget(void) {
  s_ctx.result.elapsed_us = 1024000u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));
  run_tickless(1u);
  TEST_ASSERT_EQUAL_UINT64(1024u, s_ctx.kernel_ticks);
  TEST_ASSERT_EQUAL_UINT32(1024u, s_ctx.catch_up_ticks);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.reset_calls);
}

void test_tickless_64_bit_expected_window_is_not_narrowed(void) {
  if (sizeof(TickType_t) <= sizeof(uint32_t)) return;
  const uint64_t ticks = (uint64_t)UINT32_MAX + 2u;
  s_ctx.result.elapsed_us = ticks * 1000u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));
  run_tickless((TickType_t)(ticks + 1u));
  TEST_ASSERT_EQUAL_UINT64((ticks + 1u) * 1000u, s_ctx.maximum_sleep_us);
  TEST_ASSERT_EQUAL_UINT64(ticks, s_ctx.step_ticks);
  TEST_ASSERT_EQUAL_UINT64(ticks, s_ctx.kernel_ticks);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.catch_up_ticks);
}

#if configNUMBER_OF_CORES > 1
void test_only_native_tick_owner_runs_tickless_and_replay_uses_isr_lock(void) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));
  s_ctx.core_id = configTICK_CORE + 1u;
  run_tickless(10u);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.prepare_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.irq_save_calls);
  s_ctx.core_id = configTICK_CORE;
  s_ctx.result.elapsed_us = 12000u;
  run_tickless(10u);
  TEST_ASSERT_EQUAL_UINT64(12u, s_ctx.kernel_ticks);
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.isr_lock_calls);
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.isr_unlock_calls);
  TEST_ASSERT_FALSE(s_ctx.isr_lock_held);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.irq_restore_calls);
}
#endif

void test_clock_failure_after_tick_stop_keeps_interrupts_masked(void) {
  s_ctx.clock_fail_call = 1u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));
  run_tickless(10u);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.reset_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.tick_resume_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.irq_restore_calls);
}

void test_final_clock_failure_keeps_interrupts_masked(void) {
  s_ctx.clock_fail_call = 2u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));
  run_tickless(10u);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.recover_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.reset_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.tick_resume_calls);
}

void test_clock_regression_is_fatal_before_tick_restart(void) {
  s_ctx.clock_regresses = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_set_tickless_ops(&k_tickless_ops, &s_ctx));
  run_tickless(10u);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.reset_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.irq_restore_calls);
}

void test_old_tickless_adapters_without_new_callbacks_are_rejected(void) {
  esy_os_tickless_ops_t ops = k_tickless_ops;
  ops.read_clock_atomic = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_set_tickless_ops(&ops, &s_ctx));
  ops = k_tickless_ops;
  ops.check_prepare_recovery = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_set_tickless_ops(&ops, &s_ctx));
}
