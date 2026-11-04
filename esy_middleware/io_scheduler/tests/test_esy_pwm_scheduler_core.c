/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/io_scheduler/esy_pwm_scheduler_core.h"
#include "fake_esy_hal_pwm.h"
#include "unity.h"
#include <string.h>

typedef struct {
  esy_pwm_t pwm;
  esy_pwm_scheduler_core_t core;
  esy_pwm_scheduler_request_t slots[4];
  _Alignas(uint32_t) uint16_t samples[4];
  unsigned completed;
  unsigned progress_calls;
  uint32_t progress;
  bool check_reentry;
  esy_pwm_scheduler_status_t status[8];
  esy_err_t error[8];
} pwm_scheduler_test_ctx_t;
static pwm_scheduler_test_ctx_t s_ctx;

static void complete(const esy_pwm_scheduler_request_t *request,
    esy_pwm_scheduler_status_t status, esy_err_t error, void *arg);
static void progress(const esy_pwm_scheduler_request_t *request, uint32_t flags, void *arg);
static esy_pwm_scheduler_request_t request(bool circular) {
  return (esy_pwm_scheduler_request_t){
    .transfer = {.src = s_ctx.samples, .length = sizeof(s_ctx.samples),
        .transfer_size = ESY_DMA_TRANSFER_SIZE_16, .circular = circular},
    .on_complete = complete, .on_progress = progress, .user_data = &s_ctx,
  };
}
static void check_reentry(void) {
  if (!s_ctx.check_reentry) return;
  esy_pwm_scheduler_request_t req = request(false);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_pwm_scheduler_core_submit(&s_ctx.core, &req));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_pwm_scheduler_core_process(&s_ctx.core));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_pwm_scheduler_core_request_stop(&s_ctx.core, true));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_pwm_scheduler_core_request_abort(&s_ctx.core));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_pwm_scheduler_core_deinit(&s_ctx.core));
  TEST_ASSERT_FALSE(esy_pwm_scheduler_core_is_idle(&s_ctx.core));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, esy_pwm_scheduler_core_next_wait_ms(&s_ctx.core));
}
static void complete(const esy_pwm_scheduler_request_t *req,
    esy_pwm_scheduler_status_t status, esy_err_t error, void *arg) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx, arg);
  TEST_ASSERT_EQUAL_PTR(s_ctx.samples, req->transfer.src);
  TEST_ASSERT_TRUE(esy_hal_pwm_is_idle(&s_ctx.pwm));
  s_ctx.status[s_ctx.completed] = status;
  s_ctx.error[s_ctx.completed++] = error;
  check_reentry();
}
static void progress(const esy_pwm_scheduler_request_t *req, uint32_t flags, void *arg) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx, arg);
  TEST_ASSERT_EQUAL_PTR(s_ctx.samples, req->transfer.src);
  TEST_ASSERT_FALSE(esy_hal_pwm_is_idle(&s_ctx.pwm));
  s_ctx.progress |= flags;
  ++s_ctx.progress_calls;
  check_reentry();
}
static esy_pwm_scheduler_core_config_t config(void) {
  return (esy_pwm_scheduler_core_config_t){
    .static_resources = {.requests = s_ctx.slots, .request_count = 4u},
  };
}
static void init(void) {
  esy_pwm_scheduler_core_config_t cfg = config();
  TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_init(&s_ctx.core, &s_ctx.pwm, &cfg));
}
static void submit(bool circular) {
  esy_pwm_scheduler_request_t req = request(circular);
  TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_submit(&s_ctx.core, &req));
}
static void process(void) { TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_process(&s_ctx.core)); }
static void done_event(bool error) {
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_pwm_queue_dma_event(&s_ctx.pwm, error));
}
void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_hal_pwm_reset();
  esy_pwm_config_t cfg = {.phys_index = 1u, .freq_hz = 1000u, .duty_cycle = 50u};
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_init(&s_ctx.pwm, &cfg));
  fake_esy_hal_pwm_set_dma_result(ESY_OK);
}
void tearDown(void) {}

void test_pwm_core_invalid_and_repeated_init_preserve_existing_output_and_borrows(void) {
  esy_pwm_scheduler_core_config_t cfg = config();
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_init(NULL, &s_ctx.pwm, &cfg));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_init(&s_ctx.core, NULL, &cfg));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_init(&s_ctx.core, &s_ctx.pwm, NULL));
  cfg.static_resources.requests = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_init(&s_ctx.core, &s_ctx.pwm, &cfg));
  cfg = config(); cfg.static_resources.request_count = 3u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_init(&s_ctx.core, &s_ctx.pwm, &cfg));
  cfg.static_resources.request_count = 1u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_init(&s_ctx.core, &s_ctx.pwm, &cfg));
  cfg.static_resources.request_count = UINT32_C(1) << 31;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_init(&s_ctx.core, &s_ctx.pwm, &cfg));
  cfg = config();
  cfg.static_resources.requests = (esy_pwm_scheduler_request_t *)((uint8_t *)s_ctx.slots + 1u);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_init(&s_ctx.core, &s_ctx.pwm, &cfg));
  cfg = config();
  esy_pwm_t invalid = {0};
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_pwm_scheduler_core_init(&s_ctx.core, &invalid, &cfg));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_enable(&s_ctx.pwm, true));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_pwm_scheduler_core_init(&s_ctx.core, &s_ctx.pwm, &cfg));
  TEST_ASSERT_TRUE(fake_esy_hal_pwm_is_enabled(&s_ctx.pwm));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_enable(&s_ctx.pwm, false));
  esy_dma_config_t dma = request(true).transfer;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_dma_output(&s_ctx.pwm, &dma, true));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_pwm_scheduler_core_init(&s_ctx.core, &s_ctx.pwm, &cfg));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_abort_dma(&s_ctx.pwm));
  init();
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_pwm_scheduler_core_init(&s_ctx.core, &s_ctx.pwm, &cfg));
  TEST_ASSERT_TRUE(esy_pwm_scheduler_core_is_idle(&s_ctx.core));
  TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_deinit(&s_ctx.core));
  TEST_ASSERT_TRUE(fake_esy_hal_pwm_is_initialized(&s_ctx.pwm));
  TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_deinit(&s_ctx.core));
  init();
}

static void forbidden_dma_callback(void *arg) { (void)arg; TEST_FAIL_MESSAGE("Unexpected original DMA callback"); }
void test_pwm_core_submit_validation_and_fifo_full_are_nonmutating(void) {
  esy_pwm_scheduler_request_t req = request(false);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_submit(NULL, &req));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_submit(&s_ctx.core, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_pwm_scheduler_core_submit(&s_ctx.core, &req));
  init();
  req.on_complete = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_submit(&s_ctx.core, &req));
  req = request(false); req.transfer.src = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_submit(&s_ctx.core, &req));
  req = request(false); req.transfer.length = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_submit(&s_ctx.core, &req));
  req = request(false); req.transfer.transfer_size = (esy_dma_transfer_size_t)3u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_submit(&s_ctx.core, &req));
  req = request(false); req.transfer.length = 3u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_submit(&s_ctx.core, &req));
  req = request(false); req.transfer.src = (uint8_t *)s_ctx.samples + 1u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_submit(&s_ctx.core, &req));
  req = request(false); req.transfer.callback = forbidden_dma_callback;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_submit(&s_ctx.core, &req));
  req = request(false); req.transfer.half_callback = forbidden_dma_callback;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_submit(&s_ctx.core, &req));
  req = request(false); req.transfer.error_callback = forbidden_dma_callback;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_submit(&s_ctx.core, &req));
  req = request(false); req.transfer.callback_arg = &s_ctx;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_submit(&s_ctx.core, &req));
  req = request(false); req.transfer.half_callback_arg = &s_ctx;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_submit(&s_ctx.core, &req));
  req = request(false); req.transfer.error_callback_arg = &s_ctx;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_submit(&s_ctx.core, &req));
  submit(false); submit(false); submit(false);
  req = request(false);
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_pwm_scheduler_core_submit(&s_ctx.core, &req));
  TEST_ASSERT_TRUE(esy_hal_pwm_is_idle(&s_ctx.pwm));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_pwm_scheduler_core_deinit(&s_ctx.core));
  TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_request_stop(&s_ctx.core, true));
  process(); process(); process();
  TEST_ASSERT_EQUAL_UINT(3u, s_ctx.completed);
  TEST_ASSERT_EQUAL(ESY_PWM_SCHEDULER_CANCELLED, s_ctx.status[0]);
}

void test_pwm_core_finite_fifo_completes_only_in_owner_and_disables_between_items(void) {
  init(); s_ctx.check_reentry = true;
  submit(false); submit(false);
  TEST_ASSERT_EQUAL_UINT32(0u, esy_pwm_scheduler_core_next_wait_ms(&s_ctx.core));
  process();
  TEST_ASSERT_EQUAL_UINT32(1u, esy_pwm_scheduler_core_next_wait_ms(&s_ctx.core));
  process(); process();
  done_event(false);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.completed);
  TEST_ASSERT_EQUAL_UINT32(1u, esy_pwm_scheduler_core_next_wait_ms(&s_ctx.core));
  process();
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.completed);
  TEST_ASSERT_EQUAL(ESY_PWM_SCHEDULER_COMPLETE, s_ctx.status[0]);
  TEST_ASSERT_EQUAL_UINT32(0u, esy_pwm_scheduler_core_next_wait_ms(&s_ctx.core));
  process(); done_event(false); process();
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.completed);
  TEST_ASSERT_TRUE(esy_pwm_scheduler_core_is_idle(&s_ctx.core));
  TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_deinit(&s_ctx.core));
}

void test_pwm_core_circular_progress_retains_request_until_explicit_abort(void) {
  init(); s_ctx.check_reentry = true;
  submit(true); submit(false); process();
  TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_request_stop(&s_ctx.core, true));
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_pwm_queue_half_event(&s_ctx.pwm));
  done_event(false); process();
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.progress_calls);
  TEST_ASSERT_EQUAL_UINT32(ESY_PWM_SCHEDULER_PROGRESS_HALF | ESY_PWM_SCHEDULER_PROGRESS_CYCLE,
      s_ctx.progress);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.completed);
  TEST_ASSERT_FALSE(esy_pwm_scheduler_core_is_idle(&s_ctx.core));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_pwm_scheduler_core_deinit(&s_ctx.core));
  TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_request_abort(&s_ctx.core));
  process(); process();
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.completed);
  TEST_ASSERT_EQUAL(ESY_PWM_SCHEDULER_CANCELLED, s_ctx.status[0]);
  TEST_ASSERT_EQUAL(ESY_PWM_SCHEDULER_CANCELLED, s_ctx.status[1]);
  TEST_ASSERT_TRUE(esy_pwm_scheduler_core_is_idle(&s_ctx.core));
  esy_pwm_scheduler_request_t req = request(false);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_pwm_scheduler_core_submit(&s_ctx.core, &req));
}

void test_pwm_core_native_error_stop_failure_retains_waveform_until_abort(void) {
  init(); submit(true); process();
  fake_esy_hal_pwm_set_cleanup_results(ESY_ERR_BUSY, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, fake_esy_hal_pwm_queue_dma_event(&s_ctx.pwm, true));
  process();
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.completed);
  TEST_ASSERT_FALSE(esy_pwm_scheduler_core_is_idle(&s_ctx.core));
  TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_request_abort(&s_ctx.core));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_pwm_scheduler_core_process(&s_ctx.core));
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.completed);
  fake_esy_hal_pwm_set_cleanup_results(ESY_OK, ESY_OK);
  process();
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.completed);
  TEST_ASSERT_EQUAL(ESY_PWM_SCHEDULER_CANCELLED, s_ctx.status[0]);
  TEST_ASSERT_TRUE(esy_pwm_scheduler_core_is_idle(&s_ctx.core));
}

void test_pwm_core_partial_submission_cleanup_and_disable_failures_hold_fifo_head(void) {
  init(); submit(false);
  fake_esy_hal_pwm_set_dma_result(ESY_ERR_IO);
  fake_esy_hal_pwm_set_cleanup_results(ESY_ERR_TIMEOUT, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_pwm_scheduler_core_process(&s_ctx.core));
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.completed);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_pwm_scheduler_core_process(&s_ctx.core));
  TEST_ASSERT_EQUAL_UINT32(1u, esy_pwm_scheduler_core_next_wait_ms(&s_ctx.core));
  fake_esy_hal_pwm_set_cleanup_results(ESY_OK, ESY_OK);
  process();
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.completed);
  TEST_ASSERT_EQUAL(ESY_PWM_SCHEDULER_ERROR, s_ctx.status[0]);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, s_ctx.error[0]);
  fake_esy_hal_pwm_set_dma_result(ESY_OK);
  submit(false); process(); done_event(false);
  fake_esy_hal_pwm_set_disable_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_pwm_scheduler_core_process(&s_ctx.core));
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.completed);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_pwm_scheduler_core_deinit(&s_ctx.core));
  fake_esy_hal_pwm_set_disable_result(ESY_OK);
  process();
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.completed);
}

void test_pwm_core_stop_drains_finite_and_cancels_only_unsent_work(void) {
  init(); submit(false); submit(false); process();
  TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_request_stop(&s_ctx.core, false));
  TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_request_stop(&s_ctx.core, true));
  TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_request_stop(&s_ctx.core, false));
  process(); TEST_ASSERT_EQUAL_UINT(0u, s_ctx.completed);
  done_event(false); process(); process();
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.completed);
  TEST_ASSERT_EQUAL(ESY_PWM_SCHEDULER_COMPLETE, s_ctx.status[0]);
  TEST_ASSERT_EQUAL(ESY_PWM_SCHEDULER_CANCELLED, s_ctx.status[1]);
}

void test_pwm_core_null_inactive_periodic_and_shutdown_edges(void) {
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_process(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_pwm_scheduler_core_process(&s_ctx.core));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_request_stop(NULL, true));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_pwm_scheduler_core_request_stop(&s_ctx.core, true));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_request_abort(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_pwm_scheduler_core_request_abort(&s_ctx.core));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_deinit(NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_deinit(&s_ctx.core));
  TEST_ASSERT_FALSE(esy_pwm_scheduler_core_is_idle(NULL));
  TEST_ASSERT_FALSE(esy_pwm_scheduler_core_is_idle(&s_ctx.core));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, esy_pwm_scheduler_core_next_wait_ms(NULL));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, esy_pwm_scheduler_core_next_wait_ms(&s_ctx.core));
  init(); submit(false); process();
  TEST_ASSERT_EQUAL_UINT32(1u, esy_pwm_scheduler_core_next_wait_ms(&s_ctx.core));
  process();
  TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_request_abort(&s_ctx.core));
  fake_esy_hal_pwm_set_cleanup_results(ESY_ERR_BUSY, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_pwm_scheduler_core_process(&s_ctx.core));
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.completed);
  fake_esy_hal_pwm_set_cleanup_results(ESY_OK, ESY_OK); process();
  TEST_ASSERT_EQUAL(ESY_PWM_SCHEDULER_CANCELLED, s_ctx.status[0]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_deinit(&s_ctx.core));
  init(); submit(false); process();
  TEST_ASSERT_EQUAL_UINT32(1u, esy_pwm_scheduler_core_next_wait_ms(&s_ctx.core));
  done_event(false); process();
}

void test_pwm_core_unsupported_submit_retires_once_without_borrow(void) {
  init(); submit(false);
  fake_esy_hal_pwm_set_dma_result(ESY_ERR_NOT_SUPPORTED);
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_pwm_scheduler_core_process(&s_ctx.core));
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.completed);
  TEST_ASSERT_EQUAL(ESY_PWM_SCHEDULER_ERROR, s_ctx.status[0]);
  TEST_ASSERT_TRUE(esy_pwm_scheduler_core_is_idle(&s_ctx.core));
}

void test_pwm_core_waits_for_native_irq_and_reports_error_once_from_owner(void) {
  init(); submit(false); process();
  process();
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.completed);
  done_event(true);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.completed);
  process();
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.completed);
  TEST_ASSERT_EQUAL(ESY_PWM_SCHEDULER_ERROR, s_ctx.status[0]);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, s_ctx.error[0]);
  process();
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.completed);
}

void test_pwm_core_accepts_byte_and_word_element_sizes_without_enum_arithmetic(void) {
  init();
  esy_pwm_scheduler_request_t req = request(false);
  req.transfer.transfer_size = ESY_DMA_TRANSFER_SIZE_8;
  TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_submit(&s_ctx.core, &req));
  req.transfer.transfer_size = ESY_DMA_TRANSFER_SIZE_32;
  req.transfer.src = s_ctx.samples;
  req.transfer.length = 3u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_pwm_scheduler_core_submit(&s_ctx.core, &req));
  req.transfer.length = 4u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_submit(&s_ctx.core, &req));
  TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_request_stop(&s_ctx.core, true));
  process(); process();
}

void test_pwm_core_queued_control_keeps_circular_output_processing_until_abort(void) {
  init(); submit(true); process();
  submit(false);
  TEST_ASSERT_EQUAL_UINT32(1u, esy_pwm_scheduler_core_next_wait_ms(&s_ctx.core));
  process();
  TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_request_stop(&s_ctx.core, true));
  process();
  TEST_ASSERT_EQUAL_UINT32(1u, esy_pwm_scheduler_core_next_wait_ms(&s_ctx.core));
  TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_request_abort(&s_ctx.core));
  process(); process();
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.completed);
}

void test_pwm_core_without_half_irq_support_preserves_terminal_only_waveforms(void) {
  init();
  fake_esy_hal_pwm_set_half_supported(false);
  esy_pwm_scheduler_request_t req = request(false);
  req.on_progress = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_submit(&s_ctx.core, &req));
  process();
  done_event(false);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.completed);
  process();
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.completed);
  TEST_ASSERT_EQUAL(ESY_PWM_SCHEDULER_COMPLETE, s_ctx.status[0]);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.progress_calls);

  req.on_progress = progress;
  TEST_ASSERT_EQUAL(ESY_OK, esy_pwm_scheduler_core_submit(&s_ctx.core, &req));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_pwm_scheduler_core_process(&s_ctx.core));
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.completed);
  TEST_ASSERT_EQUAL(ESY_PWM_SCHEDULER_ERROR, s_ctx.status[1]);
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, s_ctx.error[1]);
  TEST_ASSERT_TRUE(esy_pwm_scheduler_core_is_idle(&s_ctx.core));
}
