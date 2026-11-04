/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include "esy_middleware/io_scheduler/esy_pwm_scheduler_core.h"
#include "fake_esy_hal_pwm.h"

#define PWM_EVENT_PROBE_REQUEST_COUNT 2u
#define PWM_EVENT_PROBE_SAMPLE_COUNT 4u

#define CHECK(expression) do { \
  if (!(expression)) { \
    fprintf(stderr, "PWM event assertion failed at line %u: %s\n", \
        (unsigned)__LINE__, #expression); \
    return 1; \
  } \
} while (0)

typedef struct {
  esy_pwm_t pwm;
  esy_pwm_scheduler_core_t core;
  esy_pwm_scheduler_request_t slots[PWM_EVENT_PROBE_REQUEST_COUNT];
  uint16_t samples[PWM_EVENT_PROBE_SAMPLE_COUNT];
  unsigned completed;
  unsigned progress_calls;
  uint32_t progress;
  esy_pwm_scheduler_status_t status;
  esy_err_t error;
  esy_err_t interrupt_result;
  unsigned interrupts;
  bool armed;
  bool inject_error;
  bool valid_callback;
} pwm_event_probe_ctx_t;

/* One deterministic owner; the hook models an IRQ between two owner operations. */
static pwm_event_probe_ctx_t s_ctx;

void pwm_event_probe_before_clear(void) {
  if (!s_ctx.armed) return;
  s_ctx.armed = false;
  ++s_ctx.interrupts;
  s_ctx.interrupt_result =
      fake_esy_hal_pwm_queue_dma_event(&s_ctx.pwm, s_ctx.inject_error);
}

static void complete(const esy_pwm_scheduler_request_t *request,
    esy_pwm_scheduler_status_t status, esy_err_t error, void *arg) {
  ++s_ctx.completed;
  s_ctx.status = status;
  s_ctx.error = error;
  s_ctx.valid_callback = arg == &s_ctx && request->transfer.src == s_ctx.samples &&
      esy_hal_pwm_is_idle(&s_ctx.pwm);
}

static void progress(const esy_pwm_scheduler_request_t *request,
    uint32_t flags, void *arg) {
  (void)request;
  (void)arg;
  ++s_ctx.progress_calls;
  s_ctx.progress |= flags;
}

static int start(bool circular) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_hal_pwm_reset();
  fake_esy_hal_pwm_set_dma_result(ESY_OK);
  esy_pwm_config_t pwm_config = {
      .phys_index = 1u, .freq_hz = 1000u, .duty_cycle = 50u};
  esy_pwm_scheduler_core_config_t config = {.static_resources = {
      .requests = s_ctx.slots, .request_count = PWM_EVENT_PROBE_REQUEST_COUNT}};
  esy_pwm_scheduler_request_t request = {
      .transfer = {.src = s_ctx.samples, .length = sizeof(s_ctx.samples),
          .transfer_size = ESY_DMA_TRANSFER_SIZE_16, .circular = circular},
      .on_complete = complete, .on_progress = progress, .user_data = &s_ctx};
  CHECK(esy_hal_pwm_init(&s_ctx.pwm, &pwm_config) == ESY_OK);
  CHECK(esy_pwm_scheduler_core_init(&s_ctx.core, &s_ctx.pwm, &config) == ESY_OK);
  CHECK(esy_pwm_scheduler_core_submit(&s_ctx.core, &request) == ESY_OK);
  CHECK(esy_pwm_scheduler_core_process(&s_ctx.core) == ESY_OK);
  return 0;
}

static int release(void) {
  CHECK(s_ctx.completed == 1u && s_ctx.valid_callback);
  CHECK(esy_pwm_scheduler_core_process(&s_ctx.core) == ESY_OK);
  CHECK(s_ctx.completed == 1u);
  CHECK(esy_pwm_scheduler_core_is_idle(&s_ctx.core));
  CHECK(!fake_esy_hal_pwm_is_enabled(&s_ctx.pwm));
  CHECK(esy_pwm_scheduler_core_deinit(&s_ctx.core) == ESY_OK);
  CHECK(esy_hal_pwm_deinit(&s_ctx.pwm) == ESY_OK);
  return 0;
}

static int finite_completion_racing_clear_survives_failed_cleanup(void) {
  CHECK(start(false) == 0);
  s_ctx.armed = true;
  CHECK(esy_pwm_scheduler_core_process(&s_ctx.core) == ESY_OK);
  CHECK(s_ctx.interrupts == 1u && s_ctx.interrupt_result == ESY_OK);
  CHECK(!fake_esy_hal_pwm_is_dma_active(&s_ctx.pwm));
  CHECK(fake_esy_hal_pwm_is_enabled(&s_ctx.pwm));
  CHECK(s_ctx.completed == 0u && s_ctx.progress_calls == 0u);
  CHECK(esy_pwm_scheduler_core_request_stop(&s_ctx.core, false) == ESY_OK);
  fake_esy_hal_pwm_set_disable_result(ESY_ERR_IO);
  CHECK(esy_pwm_scheduler_core_process(&s_ctx.core) == ESY_ERR_IO);
  CHECK(s_ctx.completed == 0u);
  CHECK(esy_pwm_scheduler_core_deinit(&s_ctx.core) == ESY_ERR_BUSY);
  fake_esy_hal_pwm_set_disable_result(ESY_OK);
  CHECK(esy_pwm_scheduler_core_process(&s_ctx.core) == ESY_OK);
  CHECK(s_ctx.status == ESY_PWM_SCHEDULER_COMPLETE && s_ctx.error == ESY_OK);
  return release();
}

static int finite_error_racing_clear_remains_terminal(void) {
  CHECK(start(false) == 0);
  s_ctx.inject_error = true;
  s_ctx.armed = true;
  CHECK(esy_pwm_scheduler_core_process(&s_ctx.core) == ESY_OK);
  CHECK(s_ctx.interrupts == 1u && s_ctx.interrupt_result == ESY_OK);
  CHECK(s_ctx.completed == 0u && s_ctx.progress_calls == 0u);
  CHECK(esy_pwm_scheduler_core_process(&s_ctx.core) == ESY_OK);
  CHECK(s_ctx.status == ESY_PWM_SCHEDULER_ERROR && s_ctx.error == ESY_ERR_IO);
  return release();
}

static int circular_completion_racing_clear_is_consumed_once_as_progress(void) {
  CHECK(start(true) == 0);
  CHECK(fake_esy_hal_pwm_queue_half_event(&s_ctx.pwm) == ESY_OK);
  s_ctx.armed = true;
  CHECK(esy_pwm_scheduler_core_process(&s_ctx.core) == ESY_OK);
  CHECK(s_ctx.interrupts == 1u && s_ctx.interrupt_result == ESY_OK);
  CHECK(s_ctx.progress_calls == 1u);
  CHECK(s_ctx.progress == (ESY_PWM_SCHEDULER_PROGRESS_HALF |
      ESY_PWM_SCHEDULER_PROGRESS_CYCLE));
  CHECK(s_ctx.completed == 0u && fake_esy_hal_pwm_is_dma_active(&s_ctx.pwm));
  CHECK(esy_pwm_scheduler_core_process(&s_ctx.core) == ESY_OK);
  CHECK(s_ctx.progress_calls == 1u);
  CHECK(esy_pwm_scheduler_core_request_abort(&s_ctx.core) == ESY_OK);
  CHECK(esy_pwm_scheduler_core_process(&s_ctx.core) == ESY_OK);
  CHECK(s_ctx.status == ESY_PWM_SCHEDULER_CANCELLED && s_ctx.error == ESY_OK);
  return release();
}

int main(void) {
  CHECK(finite_completion_racing_clear_survives_failed_cleanup() == 0);
  CHECK(finite_error_racing_clear_remains_terminal() == 0);
  CHECK(circular_completion_racing_clear_is_consumed_once_as_progress() == 0);
  return 0;
}
