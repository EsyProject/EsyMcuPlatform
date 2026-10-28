/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/power_manager/esy_power_manager.h"
#include "esy_middleware/power_manager/esy_power_hal_backend.h"
#include "esy_middleware/power_manager/esy_power_tickless_adapter.h"
#include "esy_middleware/power_manager/esy_power_wakeup_timer_adapter.h"
#include "esy_middleware/power_manager/esy_power_watchdog_constraint.h"
#include "esy_hal/esy_hal_wakeup_timer.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "unity.h"

void vPortSuppressTicksAndSleep(TickType_t expected_idle_time);

typedef struct {
  esy_power_capabilities_t capabilities;
  esy_power_backend_result_t result;
  esy_err_t begin_status;
  esy_err_t enter_status;
  uint32_t begin_calls;
  uint32_t enter_calls;
  uint32_t complete_calls;
  uint32_t abort_calls;
  esy_err_t abort_status;
  esy_err_t complete_status;
} backend_ctx_t;

typedef struct {
  uint32_t supported_mask;
  uint64_t armed_time_us;
  uint32_t prepare_calls;
  uint32_t arm_calls;
  uint32_t cancel_calls;
  uint32_t finish_calls;
  uint32_t abort_calls;
  bool expired;
  esy_err_t prepare_status;
  esy_err_t arm_status;
  esy_err_t cancel_status;
  esy_err_t finish_status;
  esy_err_t abort_status;
} deadline_ctx_t;

typedef struct {
  uint32_t supported_mask;
  uint64_t samples[8];
  uint32_t read_calls;
} clocksource_ctx_t;

typedef struct {
  uint32_t supported_mask;
  uint32_t prepare_calls;
  uint32_t capture_calls;
  uint32_t finish_calls;
  uint32_t abort_calls;
  uint32_t finish_order;
  bool triggered;
  esy_err_t prepare_status;
  esy_err_t capture_status;
  esy_err_t finish_status;
  esy_err_t abort_status;
} wake_ctx_t;

typedef struct {
  uint64_t hard_deadline_us;
  uint64_t revalidated_deadline_us;
  uint64_t prepared_wake_time_us;
  uint64_t resumed_at_us;
  uint32_t prepare_calls;
  uint32_t revalidate_calls;
  uint32_t resume_calls;
  esy_err_t prepare_status;
  esy_err_t resume_status;
} constraint_ctx_t;

typedef struct {
  uint32_t prepare_calls;
  uint32_t park_calls;
  uint32_t release_calls;
  uint32_t release_order;
  uint32_t complete_calls;
  uint32_t abort_calls;
  uint32_t abort_order;
  esy_err_t prepare_status;
  esy_err_t park_status;
  esy_err_t release_status;
  esy_err_t complete_status;
  esy_err_t abort_status;
} coordinator_ctx_t;

typedef struct {
  uint32_t suspend_calls;
  uint32_t atomic_resume_calls;
  uint32_t atomic_resume_order;
  uint32_t resume_calls;
  uint32_t resume_order;
  esy_err_t suspend_status;
  esy_err_t resume_status;
  esy_err_t atomic_resume_status;
} device_ctx_t;

typedef struct {
  uint32_t quiesce_calls;
  uint32_t resume_calls;
  esy_err_t quiesce_status;
  esy_err_t resume_status;
} participant_ctx_t;


typedef struct {
  esy_device_registry_static_resources_t device_resources;
  esy_power_manager_static_resources_t power_resources;
  esy_device_registry_t registry;
  esy_power_manager_t manager;
  esy_device_t device;
  esy_wake_source_t wake_source;
  esy_power_constraint_t constraint;
  esy_power_lock_t lock;
  esy_power_quiesce_participant_t participant;
  backend_ctx_t backend;
  deadline_ctx_t deadline;
  clocksource_ctx_t clocksource;
  wake_ctx_t wake;
  constraint_ctx_t constraint_ctx;
  coordinator_ctx_t coordinator;
  device_ctx_t device_ctx;
  participant_ctx_t participant_ctx;
  uint32_t event_sequence;
  uint64_t watchdog_deadline_us;
  esy_monotonic_clock_t watchdog_clock;
  uint32_t watchdog_deadline_calls;
  uint32_t os_step_ticks;
  uint32_t os_pending_ticks;
  uint32_t os_kernel_ticks;
  uint32_t os_tick_resume_calls;
  uint32_t os_irq_restore_calls;
  bool os_irq_masked;
  uint32_t os_reset_calls;
  esy_hal_power_result_t platform_result;
  esy_power_capabilities_t platform_capabilities;
  uint64_t platform_time_us;
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(device_storage, 2u);
  ESY_POWER_MANAGER_STATIC_STORAGE(power_storage, 4u);
} power_test_ctx_t;

static power_test_ctx_t s_ctx;

void vTaskStartScheduler(void) {}

eSleepModeStatus eTaskConfirmSleepModeStatus(void) {
  return s_ctx.os_pending_ticks != 0u ? eAbortSleep : eStandardSleep;
}

void vTaskStepTick(TickType_t ticks) {
  TEST_ASSERT_TRUE(s_ctx.os_irq_masked);
  s_ctx.os_step_ticks += ticks;
  s_ctx.os_kernel_ticks += ticks;
}

BaseType_t xTaskIncrementTick(void) {
  TEST_ASSERT_TRUE(s_ctx.os_irq_masked);
  ++s_ctx.os_pending_ticks;
  return pdFALSE;
}

static void run_os_tickless(TickType_t expected_idle_time) {
  vPortSuppressTicksAndSleep(expected_idle_time);
  if (!s_ctx.os_irq_masked) {
    /* The real idle owner resumes scheduling after the port hook returns. */
    while (s_ctx.os_pending_ticks != 0u) {
      --s_ctx.os_pending_ticks;
      ++s_ctx.os_kernel_ticks;
    }
  }
}

int xTaskGetSchedulerState(void) { return taskSCHEDULER_NOT_STARTED; }

void os_fixture_disable_interrupts(void) {}

void os_fixture_yield_from_isr(int should_yield) { (void)should_yield; }

esy_err_t esy_hal_system_get_tick_partial_us(
    uint32_t tick_rate_hz, uint32_t *out_partial_tick_us) {
  TEST_ASSERT_EQUAL_UINT32(configTICK_RATE_HZ, tick_rate_hz);
  TEST_ASSERT_NOT_NULL(out_partial_tick_us);
  TEST_ASSERT_FALSE(s_ctx.os_irq_masked);
  *out_partial_tick_us = 500u;
  return ESY_OK;
}

uint32_t esy_hal_system_irq_save(void) {
  TEST_ASSERT_FALSE(s_ctx.os_irq_masked);
  s_ctx.os_irq_masked = true;
  return 0x55u;
}

void esy_hal_system_irq_restore(uint32_t saved_status) {
  TEST_ASSERT_EQUAL_HEX32(0x55u, saved_status);
  TEST_ASSERT_TRUE(s_ctx.os_irq_masked);
  s_ctx.os_irq_masked = false;
  s_ctx.os_irq_restore_calls++;
}

esy_err_t esy_hal_system_suspend_tick(uint32_t tick_rate_hz,
                                           uint32_t *out_partial_tick_us) {
  TEST_ASSERT_EQUAL_UINT32(configTICK_RATE_HZ, tick_rate_hz);
  TEST_ASSERT_NOT_NULL(out_partial_tick_us);
  TEST_ASSERT_TRUE(s_ctx.os_irq_masked);
  *out_partial_tick_us = 500u;
  return ESY_OK;
}

void esy_hal_system_resume_tick(void) { s_ctx.os_tick_resume_calls++; }

bool esy_hal_system_tick_is_pending(void) { return false; }

void esy_hal_system_reset(void) { s_ctx.os_reset_calls++; }

static uint32_t all_states(void) {
  return (1u << (uint32_t)ESY_POWER_STATE_COUNT) - 1u;
}

static esy_err_t device_pm_action(esy_device_t *device,
                                  esy_device_pm_action_t action,
                                  esy_power_state_t state) {
  device_ctx_t *ctx = device->driver_ctx;
  (void)state;
  if (action == ESY_DEVICE_PM_ACTION_SYSTEM_SUSPEND) {
    ctx->suspend_calls++;
    return ctx->suspend_status;
  }
  if (action == ESY_DEVICE_PM_ACTION_SYSTEM_RESUME) {
    ctx->resume_calls++;
    ctx->resume_order = ++s_ctx.event_sequence;
    return ctx->resume_status;
  }
  return ESY_OK;
}

static esy_err_t device_system_resume_atomic(esy_device_t *device,
                                             esy_power_state_t state) {
  device_ctx_t *ctx = device->driver_ctx;
  (void)state;
  ctx->atomic_resume_calls++;
  ctx->atomic_resume_order = ++s_ctx.event_sequence;
  return ctx->atomic_resume_status;
}

static const esy_device_ops_t k_device_ops = {
    .pm_action = device_pm_action,
    .system_resume_atomic = device_system_resume_atomic,
};

static esy_err_t backend_get_capabilities(
    void *arg, esy_power_capabilities_t *out_capabilities) {
  backend_ctx_t *ctx = arg;
  *out_capabilities = ctx->capabilities;
  return ESY_OK;
}

static esy_err_t backend_begin(void *arg, esy_power_state_t state) {
  backend_ctx_t *ctx = arg;
  (void)state;
  ctx->begin_calls++;
  return ctx->begin_status;
}

static esy_err_t backend_enter(void *arg,
                               esy_power_backend_result_t *out_result) {
  backend_ctx_t *ctx = arg;
  ctx->enter_calls++;
  *out_result = ctx->result;
  out_result->entry_status = ctx->enter_status;
  return ctx->enter_status;
}

static esy_err_t backend_complete(void *arg) {
  backend_ctx_t *ctx = arg;
  ctx->complete_calls++;
  return ctx->complete_status;
}

static esy_err_t backend_abort(void *arg) {
  backend_ctx_t *ctx = arg;
  ctx->abort_calls++;
  return ctx->abort_status;
}

static const esy_power_backend_ops_t k_backend_ops = {
    .get_capabilities = backend_get_capabilities,
    .begin = backend_begin,
    .enter_atomic = backend_enter,
    .complete = backend_complete,
    .abort = backend_abort,
};

static esy_err_t deadline_get_mask(void *arg, uint32_t *out_mask) {
  deadline_ctx_t *ctx = arg;
  *out_mask = ctx->supported_mask;
  return ESY_OK;
}

static esy_err_t deadline_prepare(void *arg, esy_power_state_t state) {
  deadline_ctx_t *ctx = arg;
  (void)state;
  ctx->prepare_calls++;
  return ctx->prepare_status;
}

static esy_err_t deadline_arm_atomic(void *arg, esy_power_state_t state,
                                     uint64_t wake_time_us) {
  deadline_ctx_t *ctx = arg;
  (void)state;
  ctx->arm_calls++;
  ctx->armed_time_us = wake_time_us;
  return ctx->arm_status;
}

static esy_err_t deadline_cancel_atomic(void *arg, bool *out_expired) {
  deadline_ctx_t *ctx = arg;
  *out_expired = ctx->expired;
  ctx->cancel_calls++;
  return ctx->cancel_status;
}

static esy_err_t deadline_abort(void *arg) {
  deadline_ctx_t *ctx = arg;
  ctx->abort_calls++;
  return ctx->abort_status;
}

static esy_err_t deadline_finish(void *arg) {
  deadline_ctx_t *ctx = arg;
  ctx->finish_calls++;
  return ctx->finish_status;
}

static const esy_power_deadline_timer_ops_t k_deadline_ops = {
    .get_supported_state_mask = deadline_get_mask,
    .prepare = deadline_prepare,
    .arm_absolute_atomic = deadline_arm_atomic,
    .capture_and_cancel_atomic = deadline_cancel_atomic,
    .finish = deadline_finish,
    .abort = deadline_abort,
};

static esy_err_t clocksource_get_mask(void *arg, uint32_t *out_mask) {
  clocksource_ctx_t *ctx = arg;
  *out_mask = ctx->supported_mask;
  return ESY_OK;
}

static esy_err_t clocksource_read_atomic(void *arg, uint64_t *out_time_us) {
  clocksource_ctx_t *ctx = arg;
  if (ctx->read_calls >= 8u) return ESY_ERR_STATE;
  *out_time_us = ctx->samples[ctx->read_calls++];
  return ESY_OK;
}

static const esy_power_clocksource_ops_t k_clocksource_ops = {
    .get_supported_state_mask = clocksource_get_mask,
};

static const esy_monotonic_clock_ops_t k_clock_ops = {
    .read_atomic = clocksource_read_atomic,
};

static esy_err_t wake_get_mask(void *arg, uint32_t *out_mask) {
  wake_ctx_t *ctx = arg;
  *out_mask = ctx->supported_mask;
  return ESY_OK;
}

static esy_err_t wake_prepare(void *arg, esy_power_state_t state) {
  wake_ctx_t *ctx = arg;
  (void)state;
  ctx->prepare_calls++;
  return ctx->prepare_status;
}

static esy_err_t wake_capture(void *arg, esy_wake_source_result_t *out_result) {
  wake_ctx_t *ctx = arg;
  ctx->capture_calls++;
  out_result->triggered = ctx->triggered;
  return ctx->capture_status;
}

static esy_err_t wake_finish(void *arg) {
  wake_ctx_t *ctx = arg;
  ctx->finish_calls++;
  ctx->finish_order = ++s_ctx.event_sequence;
  return ctx->finish_status;
}

static esy_err_t wake_abort(void *arg) {
  wake_ctx_t *ctx = arg;
  ctx->abort_calls++;
  return ctx->abort_status;
}

static const esy_wake_source_ops_t k_wake_ops = {
    .get_supported_state_mask = wake_get_mask,
    .prepare = wake_prepare,
    .capture_and_mask_atomic = wake_capture,
    .finish_and_unmask = wake_finish,
    .abort = wake_abort,
};

static esy_err_t constraint_get_deadline(void *arg, uint64_t now_us,
                                         esy_power_state_t state,
                                         uint64_t *out_deadline_us) {
  constraint_ctx_t *ctx = arg;
  (void)now_us;
  (void)state;
  *out_deadline_us = ctx->hard_deadline_us;
  return ESY_OK;
}

static esy_err_t constraint_prepare(void *arg, esy_power_state_t state,
                                    uint64_t wake_time_us) {
  constraint_ctx_t *ctx = arg;
  (void)state;
  ctx->prepare_calls++;
  ctx->prepared_wake_time_us = wake_time_us;
  return ctx->prepare_status;
}

static esy_err_t constraint_revalidate(void *arg, uint64_t now_us,
                                       esy_power_state_t state,
                                       uint64_t *out_deadline_us) {
  constraint_ctx_t *ctx = arg;
  (void)now_us;
  (void)state;
  ctx->revalidate_calls++;
  *out_deadline_us = ctx->revalidated_deadline_us;
  return ESY_OK;
}

static esy_err_t constraint_resume(void *arg, esy_power_state_t state,
                                   uint64_t resumed_at_us) {
  constraint_ctx_t *ctx = arg;
  (void)state;
  ctx->resume_calls++;
  ctx->resumed_at_us = resumed_at_us;
  return ctx->resume_status;
}

static const esy_power_constraint_ops_t k_constraint_ops = {
    .get_hard_deadline = constraint_get_deadline,
    .prepare = constraint_prepare,
    .revalidate_atomic = constraint_revalidate,
    .resume = constraint_resume,
};

static esy_err_t participant_quiesce(void *arg, esy_power_state_t state) {
  participant_ctx_t *ctx = arg;

  (void)state;
  ctx->quiesce_calls++;
  return ctx->quiesce_status;
}

static esy_err_t participant_resume(void *arg, esy_power_state_t state) {
  participant_ctx_t *ctx = arg;

  (void)state;
  ctx->resume_calls++;
  return ctx->resume_status;
}

static const esy_power_quiesce_ops_t k_participant_ops = {
    .quiesce = participant_quiesce,
    .resume = participant_resume,
};

static esy_err_t coordinator_prepare(void *arg, esy_power_state_t state) {
  coordinator_ctx_t *ctx = arg;
  (void)state;
  ctx->prepare_calls++;
  return ctx->prepare_status;
}

static esy_err_t coordinator_park(void *arg, esy_power_state_t state) {
  coordinator_ctx_t *ctx = arg;
  (void)state;
  ctx->park_calls++;
  return ctx->park_status;
}

static esy_err_t coordinator_release(void *arg, esy_power_state_t state) {
  coordinator_ctx_t *ctx = arg;
  (void)state;
  ctx->release_calls++;
  ctx->release_order = ++s_ctx.event_sequence;
  return ctx->release_status;
}

static esy_err_t coordinator_complete(void *arg, esy_power_state_t state) {
  coordinator_ctx_t *ctx = arg;
  (void)state;
  ctx->complete_calls++;
  return ctx->complete_status;
}

static esy_err_t coordinator_abort(void *arg, esy_power_state_t state) {
  coordinator_ctx_t *ctx = arg;
  (void)state;
  ctx->abort_calls++;
  ctx->abort_order = ++s_ctx.event_sequence;
  return ctx->abort_status;
}

static const esy_power_core_coordinator_ops_t k_coordinator_ops = {
    .prepare = coordinator_prepare,
    .park_peers_atomic = coordinator_park,
    .release_peers = coordinator_release,
    .complete = coordinator_complete,
    .abort = coordinator_abort,
};

static void initialize_registry(void) {
  esy_device_registry_config_t registry_config = {0};
  esy_device_config_t device_config = {
      .name = "device",
      .ops = &k_device_ops,
      .driver_ctx = &s_ctx.device_ctx,
      .supported_state_mask = all_states(),
  };
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&s_ctx.device_resources,
                                            s_ctx.device_storage);
  registry_config.static_resources = &s_ctx.device_resources;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_init(&s_ctx.registry, &registry_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_register(&s_ctx.registry, &s_ctx.device, &device_config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&s_ctx.registry));
}

static esy_power_manager_config_t manager_config(uint8_t core_count, bool use_coordinator) {
  esy_power_manager_config_t config = {
      .device_registry = &s_ctx.registry,
      .backend_ops = &k_backend_ops,
      .backend_ctx = &s_ctx.backend,
      .deadline_timer_ops = &k_deadline_ops,
      .deadline_timer_ctx = &s_ctx.deadline,
      .clocksource_ops = &k_clocksource_ops,
      .clock = {.ops = &k_clock_ops, .ctx = &s_ctx.clocksource},
      .managed_core_count = core_count,
      .static_resources = &s_ctx.power_resources,
  };
  ESY_POWER_MANAGER_BIND_STATIC_RESOURCES(&s_ctx.power_resources, s_ctx.power_storage);
  if (use_coordinator) {
    config.core_coordinator_ops = &k_coordinator_ops;
    config.core_coordinator_ctx = &s_ctx.coordinator;
  }
  return config;
}

static esy_err_t initialize_manager(uint8_t core_count, bool use_coordinator) {
  esy_power_manager_config_t config = manager_config(core_count, use_coordinator);
  return esy_power_manager_init(&s_ctx.manager, &config);
}

static void start_manager(bool register_wake, bool register_constraint) {
  if (register_wake) {
    esy_wake_source_config_t wake_config = {
        .name = "gpio",
        .ops = &k_wake_ops,
        .ctx = &s_ctx.wake,
    };
    TEST_ASSERT_EQUAL_INT32(
        ESY_OK,
        esy_wake_source_register(&s_ctx.manager, &s_ctx.wake_source, &wake_config));
  }
  if (register_constraint) {
    esy_power_constraint_config_t constraint_config = {
        .name = "watchdog",
        .ops = &k_constraint_ops,
        .ctx = &s_ctx.constraint_ctx,
    };
    TEST_ASSERT_EQUAL_INT32(
        ESY_OK, esy_power_constraint_register(&s_ctx.manager, &s_ctx.constraint,
                                              &constraint_config));
  }
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_power_manager_start(&s_ctx.manager));
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.watchdog_deadline_us = 9000u;
  s_ctx.backend.capabilities.supported_state_mask = all_states();
  s_ctx.backend.capabilities.states[ESY_POWER_STATE_CPU_IDLE] =
      (esy_power_state_capability_t){.entry_latency_us = 10u,
                                     .exit_latency_us = 20u,
                                     .min_residency_us = 30u};
  s_ctx.backend.capabilities.states[ESY_POWER_STATE_RETENTION] =
      (esy_power_state_capability_t){.entry_latency_us = 100u,
                                     .exit_latency_us = 200u,
                                     .min_residency_us = 1000u};
  s_ctx.backend.result.entered = true;
  s_ctx.backend.begin_status = ESY_OK;
  s_ctx.backend.enter_status = ESY_OK;
  s_ctx.deadline.supported_mask = all_states();
  s_ctx.clocksource.supported_mask = all_states();
  s_ctx.clocksource.samples[0] = 1000u;
  s_ctx.clocksource.samples[1] = 1050u;
  s_ctx.clocksource.samples[2] = 1450u;
  s_ctx.clocksource.samples[3] = 1500u;
  s_ctx.clocksource.samples[4] = 1550u;
  s_ctx.clocksource.samples[5] = 1600u;
  s_ctx.clocksource.samples[6] = 1650u;
  s_ctx.clocksource.samples[7] = 1700u;
  s_ctx.wake.supported_mask = all_states();
  s_ctx.wake.triggered = true;
  s_ctx.constraint_ctx.hard_deadline_us = ESY_POWER_TIME_INFINITE;
  s_ctx.constraint_ctx.revalidated_deadline_us = ESY_POWER_TIME_INFINITE;
  esy_os_init();
  initialize_registry();
}

void tearDown(void) {}

void test_selection_uses_latency_and_residency_capabilities(void) {
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  esy_power_result_t result;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, initialize_manager(1u, false));
  start_manager(false, false);
  esy_power_request_default(&request);
  request.latest_resume_time_us = 2000u;
  esy_power_transaction_init(&transaction);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_power_transaction_prepare(
                                      &s_ctx.manager, &transaction, &request));
  TEST_ASSERT_EQUAL_INT(ESY_POWER_STATE_CPU_IDLE,
                        transaction.result.selected_state);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_transaction_abort(&transaction, &result));
}

void test_exit_latency_limit_excludes_retention(void) {
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  esy_power_result_t result;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, initialize_manager(1u, false));
  start_manager(false, false);
  esy_power_request_default(&request);
  request.latest_resume_time_us = 6000u;
  request.maximum_exit_latency_us = 100u;
  esy_power_transaction_init(&transaction);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_power_transaction_prepare(
                                      &s_ctx.manager, &transaction, &request));
  TEST_ASSERT_EQUAL_INT(ESY_POWER_STATE_CPU_IDLE,
                        transaction.result.selected_state);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_transaction_recover_atomic(&transaction));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_transaction_abort(&transaction, &result));
}

void test_watchdog_constraint_caps_budget_before_state_selection(void) {
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  esy_power_result_t result;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, initialize_manager(1u, false));
  s_ctx.constraint_ctx.hard_deadline_us = 1700u;
  s_ctx.constraint_ctx.revalidated_deadline_us = 1700u;
  start_manager(false, true);
  esy_power_request_default(&request);
  request.latest_resume_time_us = 6000u;
  esy_power_transaction_init(&transaction);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_power_transaction_prepare(
                                      &s_ctx.manager, &transaction, &request));
  TEST_ASSERT_EQUAL_INT(ESY_POWER_STATE_CPU_IDLE,
                        transaction.result.selected_state);
  TEST_ASSERT_EQUAL_UINT64(1700u, transaction.result.hard_deadline_us);
  TEST_ASSERT_EQUAL_UINT64(1680u, s_ctx.constraint_ctx.prepared_wake_time_us);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_transaction_recover_atomic(&transaction));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_transaction_abort(&transaction, &result));
}

void test_atomic_window_rearms_exact_remaining_budget_and_clocksource(void) {
  esy_power_quiesce_config_t participant_config = {
      .name = "producer",
      .ops = &k_participant_ops,
      .ctx = &s_ctx.participant_ctx,
  };
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  esy_power_result_t result;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, initialize_manager(1u, false));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_power_quiesce_register(&s_ctx.manager, &s_ctx.participant,
                                         &participant_config));
  start_manager(true, true);
  esy_power_request_default(&request);
  request.latest_resume_time_us = 6000u;
  request.late_resume_guard_us = 50u;
  request.required_wake_source_mask = 1u;
  esy_power_transaction_init(&transaction);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_power_transaction_prepare(
                                      &s_ctx.manager, &transaction, &request));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_transaction_enter_atomic(&transaction));
  TEST_ASSERT_EQUAL_UINT64(5750u, s_ctx.deadline.armed_time_us);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.deadline.cancel_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.deadline.finish_calls);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_transaction_recover_atomic(&transaction));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.device_ctx.atomic_resume_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.device_ctx.resume_calls);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_power_transaction_complete(&transaction, &result));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.deadline.finish_calls);
  TEST_ASSERT_TRUE(result.entered);
  TEST_ASSERT_TRUE(result.elapsed_valid);
  TEST_ASSERT_EQUAL_UINT64(400u, result.elapsed_us);
  TEST_ASSERT_EQUAL_UINT32(1u, result.triggered_wake_source_mask);
  TEST_ASSERT_EQUAL_UINT64(1500u, s_ctx.constraint_ctx.resumed_at_us);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.participant_ctx.quiesce_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.participant_ctx.resume_calls);
}

void test_nonfatal_entry_error_is_separate_from_completion_recovery(void) {
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  esy_power_result_t result;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, initialize_manager(1u, false));
  start_manager(false, false);
  s_ctx.backend.enter_status = ESY_ERR_BUSY;
  esy_power_request_default(&request);
  request.latest_resume_time_us = 6000u;
  esy_power_transaction_init(&transaction);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_power_transaction_prepare(
                                      &s_ctx.manager, &transaction, &request));

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY,
                          esy_power_transaction_enter_atomic(&transaction));
  TEST_ASSERT_FALSE(transaction.result.fatal);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_transaction_recover_atomic(&transaction));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_power_transaction_complete(&transaction, &result));

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, result.primary_error);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, result.recovery_error);
}

void test_uncommitted_recovery_masks_source_before_late_graph_resume(void) {
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  esy_power_result_t result;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, initialize_manager(1u, false));
  start_manager(true, false);
  esy_power_request_default(&request);
  request.latest_resume_time_us = 6000u;
  request.required_wake_source_mask = 1u;
  esy_power_transaction_init(&transaction);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_power_transaction_prepare(
                                      &s_ctx.manager, &transaction, &request));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.wake.capture_calls);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_transaction_recover_atomic(&transaction));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.wake.capture_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.device_ctx.atomic_resume_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.device_ctx.resume_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.wake.finish_calls);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_transaction_abort(&transaction, &result));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.device_ctx.resume_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.wake.finish_calls);
  TEST_ASSERT_LESS_THAN_UINT32(s_ctx.wake.finish_order, s_ctx.device_ctx.resume_order);
}

void test_clock_restore_fatal_error_requires_fail_stop(void) {
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, initialize_manager(1u, false));
  start_manager(false, false);
  s_ctx.backend.result.fatal_error = ESY_ERR_IO;
  esy_power_request_default(&request);
  request.latest_resume_time_us = 6000u;
  esy_power_transaction_init(&transaction);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_power_transaction_prepare(
                                      &s_ctx.manager, &transaction, &request));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO,
                          esy_power_transaction_enter_atomic(&transaction));
  TEST_ASSERT_TRUE(transaction.result.fatal);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, transaction.result.fatal_error);
}

void test_multicore_manager_fails_closed_without_coordinator(void) {
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, initialize_manager(2u, false));
}

void test_multicore_transaction_uses_rendezvous_contract(void) {
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  esy_power_result_t result;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, initialize_manager(2u, true));
  start_manager(false, false);
  esy_power_request_default(&request);
  request.latest_resume_time_us = 6000u;
  esy_power_transaction_init(&transaction);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_power_transaction_prepare(
                                      &s_ctx.manager, &transaction, &request));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_transaction_enter_atomic(&transaction));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_transaction_recover_atomic(&transaction));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_power_transaction_complete(&transaction, &result));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.coordinator.prepare_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.coordinator.park_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.coordinator.release_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.coordinator.complete_calls);
  TEST_ASSERT_LESS_THAN_UINT32(s_ctx.coordinator.release_order,
                               s_ctx.device_ctx.resume_order);
}

void test_peer_release_waits_for_graph_and_wake_source_finish(void) {
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  esy_power_result_t result;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, initialize_manager(2u, true));
  start_manager(true, false);
  esy_power_request_default(&request);
  request.latest_resume_time_us = 6000u;
  request.required_wake_source_mask = 1u;
  esy_power_transaction_init(&transaction);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_power_transaction_prepare(
                                      &s_ctx.manager, &transaction, &request));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_transaction_enter_atomic(&transaction));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.coordinator.release_calls);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_transaction_recover_atomic(&transaction));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_power_transaction_complete(&transaction, &result));
  TEST_ASSERT_LESS_THAN_UINT32(s_ctx.coordinator.release_order,
                               s_ctx.device_ctx.resume_order);
  TEST_ASSERT_LESS_THAN_UINT32(s_ctx.coordinator.release_order,
                               s_ctx.wake.finish_order);
}

void test_constraint_is_revalidated_after_prepare_time(void) {
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  esy_power_result_t result;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, initialize_manager(1u, false));
  s_ctx.constraint_ctx.hard_deadline_us = 6000u;
  s_ctx.constraint_ctx.revalidated_deadline_us = 3000u;
  s_ctx.clocksource.samples[1] = 2500u;
  start_manager(false, true);
  esy_power_request_default(&request);
  request.latest_resume_time_us = 7000u;
  esy_power_transaction_init(&transaction);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_power_transaction_prepare(
                                      &s_ctx.manager, &transaction, &request));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY,
                          esy_power_transaction_enter_atomic(&transaction));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.constraint_ctx.revalidate_calls);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_transaction_recover_atomic(&transaction));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_transaction_abort(&transaction, &result));
}

void test_deadline_expiry_is_reported_separately_from_wake_sources(void) {
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  esy_power_result_t result;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, initialize_manager(1u, false));
  s_ctx.deadline.expired = true;
  start_manager(false, false);
  esy_power_request_default(&request);
  request.latest_resume_time_us = 6000u;
  esy_power_transaction_init(&transaction);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_power_transaction_prepare(
                                      &s_ctx.manager, &transaction, &request));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_transaction_enter_atomic(&transaction));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_transaction_recover_atomic(&transaction));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_power_transaction_complete(&transaction, &result));
  TEST_ASSERT_TRUE(result.deadline_expired);
  TEST_ASSERT_EQUAL_UINT32(0u, result.triggered_wake_source_mask);
}

void test_manager_borrows_registry_lifecycle(void) {
  TEST_ASSERT_EQUAL_INT32(ESY_OK, initialize_manager(1u, false));
  start_manager(false, false);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_power_manager_deinit(&s_ctx.manager));
  TEST_ASSERT_TRUE(esy_device_registry_is_ready(&s_ctx.registry));
}

void test_manager_registers_and_unregisters_all_participant_types(void) {
  esy_power_lock_config_t lock_config = {
      .name = "latency",
      .deepest_allowed_state = ESY_POWER_STATE_CPU_IDLE,
  };
  esy_power_quiesce_config_t participant_config = {
      .name = "producer",
      .ops = &k_participant_ops,
      .ctx = &s_ctx.participant_ctx,
  };
  esy_wake_source_config_t wake_config = {
      .name = "gpio",
      .ops = &k_wake_ops,
      .ctx = &s_ctx.wake,
  };
  esy_power_constraint_config_t constraint_config = {
      .name = "watchdog",
      .ops = &k_constraint_ops,
      .ctx = &s_ctx.constraint_ctx,
  };

  TEST_ASSERT_EQUAL_INT32(ESY_OK, initialize_manager(1u, false));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_power_lock_register(&s_ctx.manager, &s_ctx.lock, &lock_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_power_quiesce_register(&s_ctx.manager, &s_ctx.participant,
                                         &participant_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK,
      esy_wake_source_register(&s_ctx.manager, &s_ctx.wake_source, &wake_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_power_constraint_register(&s_ctx.manager, &s_ctx.constraint,
                                            &constraint_config));

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_constraint_unregister(&s_ctx.constraint));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_wake_source_unregister(&s_ctx.wake_source));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_quiesce_unregister(&s_ctx.participant));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_power_lock_unregister(&s_ctx.lock));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_power_manager_deinit(&s_ctx.manager));
}

void test_power_lock_activity_guard_tracks_ready_manager_usage(void) {
  esy_power_lock_config_t lock_config = {
      .name = "activity",
      .deepest_allowed_state = ESY_POWER_STATE_CPU_IDLE,
  };
  esy_activity_guard_t guard = {0};

  TEST_ASSERT_EQUAL_INT32(ESY_OK, initialize_manager(1u, false));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_power_lock_register(&s_ctx.manager, &s_ctx.lock, &lock_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_power_lock_bind_activity_guard(&s_ctx.lock, &guard));
  start_manager(false, false);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, guard.acquire(guard.ctx));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_power_manager_deinit(&s_ctx.manager));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, guard.release(guard.ctx));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, guard.release(guard.ctx));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_power_manager_deinit(&s_ctx.manager));
}

void test_manager_queries_and_transaction_entrypoints_validate_state(void) {
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  esy_power_result_t result;
  esy_power_state_t state;
  uint64_t now_us;

  esy_power_request_default(NULL);
  esy_power_transaction_init(NULL);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_power_manager_get_deepest_allowed_state(NULL,
                                                                     &state));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_power_manager_runtime_suspend_idle(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_power_transaction_enter_atomic(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_power_transaction_recover_atomic(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_power_transaction_complete(NULL, &result));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_power_transaction_abort(NULL, &result));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, initialize_manager(1u, false));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE,
      esy_power_manager_get_deepest_allowed_state(&s_ctx.manager, &state));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE, esy_power_manager_runtime_suspend_idle(&s_ctx.manager));
  start_manager(false, false);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_power_manager_get_deepest_allowed_state(&s_ctx.manager, &state));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_power_manager_now_atomic(&s_ctx.manager, &now_us));

  esy_power_request_default(&request);
  esy_power_transaction_init(&transaction);
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_power_transaction_prepare(&s_ctx.manager, &transaction, &request));
  transaction.manager = &s_ctx.manager;
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE, esy_power_transaction_enter_atomic(&transaction));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE, esy_power_transaction_recover_atomic(&transaction));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE, esy_power_transaction_complete(&transaction, &result));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE, esy_power_transaction_abort(&transaction, &result));
}

void test_prepare_failure_rolls_back_and_leaves_manager_ready(void) {
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  esy_power_state_t state;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, initialize_manager(1u, false));
  start_manager(false, false);
  s_ctx.backend.begin_status = ESY_ERR_IO;
  esy_power_request_default(&request);
  request.latest_resume_time_us = 6000u;
  esy_power_transaction_init(&transaction);

  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_IO,
      esy_power_transaction_prepare(&s_ctx.manager, &transaction, &request));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.deadline.abort_calls);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_power_manager_get_deepest_allowed_state(&s_ctx.manager, &state));
}

void test_held_lock_is_revalidated_inside_atomic_window(void) {
  esy_power_lock_config_t lock_config = {
      .name = "latency",
      .deepest_allowed_state = ESY_POWER_STATE_CPU_IDLE,
  };
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  esy_power_result_t result;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, initialize_manager(1u, false));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_power_lock_register(&s_ctx.manager, &s_ctx.lock, &lock_config));
  start_manager(false, false);
  esy_power_request_default(&request);
  request.latest_resume_time_us = 6000u;
  esy_power_transaction_init(&transaction);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_power_transaction_prepare(
                                      &s_ctx.manager, &transaction, &request));
  atomic_store(&s_ctx.lock.usage_count, 1u);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY,
                          esy_power_transaction_enter_atomic(&transaction));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_transaction_recover_atomic(&transaction));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_transaction_abort(&transaction, &result));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_power_lock_release(&s_ctx.lock));
}


void esy_hal_power_transaction_init(
    esy_hal_power_transaction_t *transaction) {
  if (transaction != NULL) memset(transaction, 0, sizeof(*transaction));
}

esy_err_t esy_hal_power_get_capabilities(
    esy_power_capabilities_t *out_capabilities) {
  *out_capabilities = s_ctx.platform_capabilities;
  return ESY_OK;
}

esy_err_t esy_hal_power_transaction_begin(
    esy_hal_power_transaction_t *transaction, esy_power_state_t state) {
  if ((transaction->phase != 0u) && (transaction->phase != 3u))
    return ESY_ERR_STATE;
  memset(&transaction->result, 0, sizeof(transaction->result));
  transaction->result.state = state;
  transaction->phase = 1u;
  return ESY_OK;
}

esy_err_t esy_hal_power_transaction_enter_atomic(
    esy_hal_power_transaction_t *transaction) {
  if (transaction->phase != 1u) return ESY_ERR_STATE;
  transaction->result = s_ctx.platform_result;
  transaction->phase = 2u;
  return transaction->result.fatal_error == ESY_OK
             ? transaction->result.entry_status
             : transaction->result.fatal_error;
}

esy_err_t esy_hal_power_transaction_complete(
    esy_hal_power_transaction_t *transaction,
    esy_hal_power_result_t *out_result) {
  if ((transaction->phase != 2u) ||
      (transaction->result.fatal_error != ESY_OK)) return ESY_ERR_STATE;
  *out_result = transaction->result;
  transaction->phase = 3u;
  return ESY_OK;
}

esy_err_t esy_hal_power_transaction_abort(
    esy_hal_power_transaction_t *transaction) {
  if (transaction->phase != 1u) return ESY_ERR_STATE;
  transaction->phase = 3u;
  return ESY_OK;
}

uint32_t esy_hal_system_always_on_state_mask(void) { return all_states(); }

esy_err_t esy_hal_system_always_on_time_us(uint64_t *out_time_us) {
  *out_time_us = s_ctx.platform_time_us;
  return ESY_OK;
}

void test_platform_backend_forwards_capabilities_fatal_and_clocksource(void) {
  esy_power_hal_backend_t backend = {0};
  esy_power_backend_result_t result;
  esy_power_capabilities_t capabilities;
  uint64_t timestamp;
  const esy_power_backend_ops_t *backend_ops;
  const esy_power_clocksource_ops_t *clocksource_ops;
  const esy_monotonic_clock_ops_t *clock_ops;
  memset(&s_ctx.platform_capabilities, 0, sizeof(s_ctx.platform_capabilities));
  s_ctx.platform_capabilities.supported_state_mask = all_states();
  s_ctx.platform_result.entered = true;
  s_ctx.platform_result.entry_status = ESY_OK;
  s_ctx.platform_result.fatal_error = ESY_ERR_IO;
  s_ctx.platform_time_us = 1234u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_power_hal_backend_init(&backend));
  backend_ops = esy_power_hal_backend_ops();
  clocksource_ops = esy_power_platform_clocksource_ops();
  clock_ops = esy_power_platform_monotonic_clock_ops();
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, backend_ops->get_capabilities(&backend, &capabilities));
  TEST_ASSERT_EQUAL_UINT32(all_states(), capabilities.supported_state_mask);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, backend_ops->begin(&backend, ESY_POWER_STATE_RETENTION));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO,
                          backend_ops->enter_atomic(&backend, &result));
  TEST_ASSERT_TRUE(result.entered);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, result.fatal_error);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, backend_ops->complete(&backend));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, backend_ops->abort(&backend));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY,
      backend_ops->begin(&backend, ESY_POWER_STATE_CPU_IDLE));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_power_hal_backend_deinit(&backend));
  TEST_ASSERT_TRUE(backend.active);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, backend_ops->complete(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, backend_ops->abort(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          clocksource_ops->get_supported_state_mask(
                              NULL, &capabilities.supported_state_mask));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, clock_ops->read_atomic(NULL, &timestamp));
  TEST_ASSERT_EQUAL_UINT64(1234u, timestamp);
}

void test_freertos_adapter_passes_revalidated_budget_and_result(void) {
  esy_power_tickless_adapter_t adapter = {0};
  esy_power_tickless_adapter_config_t config = {
      .manager = &s_ctx.manager,
      .maximum_exit_latency_us = 500u,
      .allowed_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_CPU_IDLE) |
                            ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION),
  };
  esy_os_tickless_result_t result = {0};
  const esy_os_tickless_ops_t *ops;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, initialize_manager(1u, false));
  start_manager(false, false);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_tickless_adapter_init(&adapter, &config));
  ops = esy_power_tickless_adapter_ops();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, ops->prepare(&adapter, 5000u));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, ops->enter_atomic(&adapter, &result));
  TEST_ASSERT_EQUAL_UINT64(5800u, s_ctx.deadline.armed_time_us);
  TEST_ASSERT_TRUE(result.committed);
  TEST_ASSERT_TRUE(result.entered);
  TEST_ASSERT_EQUAL_UINT64(50u, result.elapsed_us);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, ops->recover_atomic(&adapter));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, ops->complete(&adapter));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_power_tickless_adapter_deinit(&adapter));
}

void test_freertos_tickless_partial_tick_bounds_real_adapter_deadline(void) {
  esy_power_tickless_adapter_t adapter = {0};
  esy_power_tickless_adapter_config_t config = {
      .manager = &s_ctx.manager,
      .allowed_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_CPU_IDLE),
  };
  const esy_os_tickless_ops_t *ops = esy_power_tickless_adapter_ops();

  s_ctx.backend.capabilities.states[ESY_POWER_STATE_CPU_IDLE] =
      (esy_power_state_capability_t){0};
  TEST_ASSERT_EQUAL_INT32(ESY_OK, initialize_manager(1u, false));
  start_manager(false, false);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_tickless_adapter_init(&adapter, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_os_set_tickless_ops(ops, &adapter));

  run_os_tickless(20u);

  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.os_reset_calls);
  TEST_ASSERT_EQUAL_UINT64(20500u, s_ctx.deadline.armed_time_us);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.os_tick_resume_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.os_irq_restore_calls);
  TEST_ASSERT_FALSE(s_ctx.os_irq_masked);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_os_clear_tickless_ops(ops, &adapter));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_power_tickless_adapter_deinit(&adapter));
}

void test_freertos_tickless_nonfatal_entry_error_does_not_reset(void) {
  esy_power_tickless_adapter_t adapter = {0};
  esy_power_tickless_adapter_config_t config = {
      .manager = &s_ctx.manager,
      .allowed_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_CPU_IDLE),
  };
  const esy_os_tickless_ops_t *ops = esy_power_tickless_adapter_ops();

  s_ctx.backend.capabilities.states[ESY_POWER_STATE_CPU_IDLE] =
      (esy_power_state_capability_t){0};
  s_ctx.backend.enter_status = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, initialize_manager(1u, false));
  start_manager(false, false);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_tickless_adapter_init(&adapter, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_os_set_tickless_ops(ops, &adapter));

  run_os_tickless(20u);

  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.os_reset_calls);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, adapter.result.primary_error);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, adapter.result.recovery_error);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.os_tick_resume_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.os_irq_restore_calls);
  TEST_ASSERT_FALSE(s_ctx.os_irq_masked);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_os_clear_tickless_ops(ops, &adapter));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_power_tickless_adapter_deinit(&adapter));
}

esy_err_t esy_hal_wakeup_timer_get_supported_state_mask(
    esy_hal_wakeup_timer_t *timer, uint32_t *out_state_mask) {
  (void)timer;
  *out_state_mask = s_ctx.deadline.supported_mask;
  return ESY_OK;
}

esy_err_t esy_hal_wakeup_timer_now_atomic(
    esy_hal_wakeup_timer_t *timer, uint64_t *out_time_us) {
  (void)timer;
  return clocksource_read_atomic(&s_ctx.clocksource, out_time_us);
}

esy_err_t esy_hal_wakeup_timer_prepare(esy_hal_wakeup_timer_t *timer,
                                            esy_power_state_t state) {
  (void)timer;
  return deadline_prepare(&s_ctx.deadline, state);
}

esy_err_t esy_hal_wakeup_timer_arm_absolute_atomic(
    esy_hal_wakeup_timer_t *timer, esy_power_state_t state,
    uint64_t wake_time_us) {
  (void)timer;
  return deadline_arm_atomic(&s_ctx.deadline, state, wake_time_us);
}

esy_err_t esy_hal_wakeup_timer_capture_and_cancel_atomic(
    esy_hal_wakeup_timer_t *timer, bool *out_expired) {
  (void)timer;
  return deadline_cancel_atomic(&s_ctx.deadline, out_expired);
}

esy_err_t esy_hal_wakeup_timer_finish(esy_hal_wakeup_timer_t *timer) {
  (void)timer;
  return deadline_finish(&s_ctx.deadline);
}

esy_err_t esy_hal_wakeup_timer_abort(esy_hal_wakeup_timer_t *timer) {
  (void)timer;
  return deadline_abort(&s_ctx.deadline);
}

void test_wakeup_timer_adapter_uses_one_clock_domain(void) {
  esy_hal_wakeup_timer_t timer = {0};
  const esy_power_deadline_timer_ops_t *deadline_ops =
      esy_power_wakeup_deadline_timer_ops();
  const esy_power_clocksource_ops_t *clocksource_ops =
      esy_power_wakeup_clocksource_ops();
  const esy_monotonic_clock_ops_t *clock_ops =
      esy_power_wakeup_monotonic_clock_ops();
  uint32_t state_mask;
  uint64_t now_us;
  bool expired;

  s_ctx.deadline.expired = true;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, clocksource_ops->get_supported_state_mask(&timer, &state_mask));
  TEST_ASSERT_EQUAL_UINT32(all_states(), state_mask);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, clock_ops->read_atomic(&timer, &now_us));
  TEST_ASSERT_EQUAL_UINT64(1000u, now_us);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, deadline_ops->prepare(&timer, ESY_POWER_STATE_CPU_IDLE));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, deadline_ops->arm_absolute_atomic(
                                      &timer, ESY_POWER_STATE_CPU_IDLE, 5000u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, deadline_ops->capture_and_cancel_atomic(&timer, &expired));
  TEST_ASSERT_TRUE(expired);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, deadline_ops->finish(&timer));
}

esy_err_t esy_watchdog_monitor_get_hard_deadline_atomic(
    const esy_monotonic_clock_t *clock, uint64_t *out_deadline_us) {
  if ((clock == NULL) || (out_deadline_us == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  s_ctx.watchdog_clock = *clock;
  *out_deadline_us = s_ctx.watchdog_deadline_us;
  s_ctx.watchdog_deadline_calls++;
  return ESY_OK;
}

void test_watchdog_constraint_forwards_one_clock_domain_and_lifecycle(void) {
  esy_power_watchdog_constraint_t adapter = {0};
  esy_monotonic_clock_t clock = {
      .ops = &k_clock_ops,
      .ctx = &s_ctx.clocksource,
  };
  const esy_power_constraint_ops_t *ops = esy_power_watchdog_constraint_ops();
  uint64_t deadline_us = 0u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_watchdog_constraint_init(&adapter, &clock));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, ops->get_hard_deadline(&adapter, 1000u, ESY_POWER_STATE_RETENTION,
                                     &deadline_us));
  TEST_ASSERT_EQUAL_UINT64(9000u, deadline_us);
  TEST_ASSERT_EQUAL_PTR(clock.ops, s_ctx.watchdog_clock.ops);
  TEST_ASSERT_EQUAL_PTR(clock.ctx, s_ctx.watchdog_clock.ctx);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.watchdog_deadline_calls);

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, ops->prepare(&adapter, ESY_POWER_STATE_RETENTION, 8000u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY,
                          esy_power_watchdog_constraint_deinit(&adapter));
  s_ctx.watchdog_deadline_us = 7500u;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, ops->revalidate_atomic(&adapter, 2000u, ESY_POWER_STATE_RETENTION,
                                     &deadline_us));
  TEST_ASSERT_EQUAL_UINT64(7500u, deadline_us);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, ops->resume(&adapter, ESY_POWER_STATE_RETENTION, 7000u));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_power_watchdog_constraint_deinit(&adapter));
}

static void prepare_request(esy_power_request_t *request,
                            esy_power_transaction_t *transaction) {
  esy_power_request_default(request);
  request->latest_resume_time_us = 6000u;
  request->required_wake_source_mask = 1u;
  esy_power_transaction_init(transaction);
}

static void register_producer(void) {
  esy_power_quiesce_config_t config = {
      .name = "producer", .ops = &k_participant_ops, .ctx = &s_ctx.participant_ctx};
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_quiesce_register(
      &s_ctx.manager, &s_ctx.participant, &config));
}

void test_new_finite_constraint_cancels_entry_without_unprepared_alarm(void) {
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  esy_power_result_t result;
  TEST_ASSERT_EQUAL(ESY_OK, initialize_manager(2u, true));
  start_manager(true, true);
  prepare_request(&request, &transaction);
  request.latest_resume_time_us = ESY_POWER_TIME_INFINITE;
  s_ctx.constraint_ctx.revalidated_deadline_us = 6000u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_prepare(
      &s_ctx.manager, &transaction, &request));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_power_transaction_enter_atomic(&transaction));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.deadline.arm_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.backend.enter_calls);
  TEST_ASSERT_TRUE(transaction.peers_parked);
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_recover_atomic(&transaction));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.coordinator.abort_calls);
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_abort(&transaction, &result));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.coordinator.abort_calls);
  TEST_ASSERT_LESS_THAN_UINT32(s_ctx.coordinator.abort_order, s_ctx.wake.finish_order);
}

void test_removed_finite_constraint_does_not_arm_infinite_timestamp(void) {
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  esy_power_result_t result;
  TEST_ASSERT_EQUAL(ESY_OK, initialize_manager(1u, false));
  s_ctx.constraint_ctx.hard_deadline_us = 6000u;
  start_manager(true, true);
  prepare_request(&request, &transaction);
  request.latest_resume_time_us = ESY_POWER_TIME_INFINITE;
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_prepare(
      &s_ctx.manager, &transaction, &request));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_enter_atomic(&transaction));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.deadline.arm_calls);
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_recover_atomic(&transaction));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_complete(&transaction, &result));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.deadline.finish_calls);
}

void test_infinite_request_rejects_an_incompatible_installed_clock(void) {
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  TEST_ASSERT_EQUAL(ESY_OK, initialize_manager(1u, false));
  s_ctx.clocksource.supported_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE);
  start_manager(true, false);
  prepare_request(&request, &transaction);
  request.latest_resume_time_us = ESY_POWER_TIME_INFINITE;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_power_transaction_prepare(
      &s_ctx.manager, &transaction, &request));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.backend.begin_calls);
}

void test_graph_prepare_fault_retains_producers_and_reports_recovery_error(void) {
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  TEST_ASSERT_EQUAL(ESY_OK, initialize_manager(1u, false));
  register_producer();
  start_manager(true, true);
  s_ctx.device_ctx.suspend_status = ESY_ERR_STATE;
  prepare_request(&request, &transaction);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_power_transaction_prepare(
      &s_ctx.manager, &transaction, &request));
  TEST_ASSERT_TRUE(esy_device_registry_is_faulted(&s_ctx.registry));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, transaction.result.recovery_error);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.participant_ctx.resume_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.wake.abort_calls);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_power_manager_deinit(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, initialize_manager(1u, false));
}

void test_late_abort_failure_keeps_wake_and_peers_held(void) {
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  esy_power_result_t result;
  TEST_ASSERT_EQUAL(ESY_OK, initialize_manager(2u, true));
  register_producer();
  start_manager(true, false);
  prepare_request(&request, &transaction);
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_prepare(
      &s_ctx.manager, &transaction, &request));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_recover_atomic(&transaction));
  s_ctx.device_ctx.resume_status = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_power_transaction_abort(&transaction, &result));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.participant_ctx.resume_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.wake.finish_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.coordinator.abort_calls);
  TEST_ASSERT_TRUE(transaction.graph_prepared);
}

void test_late_resume_failure_does_not_enable_producers_or_wake(void) {
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  esy_power_result_t result;
  TEST_ASSERT_EQUAL(ESY_OK, initialize_manager(2u, true));
  register_producer();
  start_manager(true, true);
  prepare_request(&request, &transaction);
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_prepare(
      &s_ctx.manager, &transaction, &request));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_enter_atomic(&transaction));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_recover_atomic(&transaction));
  s_ctx.device_ctx.resume_status = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_power_transaction_complete(&transaction, &result));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.participant_ctx.resume_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.wake.finish_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.coordinator.release_calls);
  TEST_ASSERT_TRUE(transaction.device_graph_committed);
}

void test_reinit_cannot_discard_live_lock_and_successful_deinit_allows_reinit(void) {
  esy_power_lock_config_t config = {.name = "active", .deepest_allowed_state = ESY_POWER_STATE_ACTIVE};
  TEST_ASSERT_EQUAL(ESY_OK, initialize_manager(1u, false));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_lock_register(&s_ctx.manager, &s_ctx.lock, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_power_lock_register(&s_ctx.manager, &s_ctx.lock, &config));
  start_manager(false, false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_lock_acquire(&s_ctx.lock));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, initialize_manager(1u, false));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_power_manager_deinit(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_lock_release(&s_ctx.lock));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_manager_deinit(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_power_manager_deinit(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_OK, initialize_manager(1u, false));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_lock_register(&s_ctx.manager, &s_ctx.lock, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_manager_deinit(&s_ctx.manager));
}

void test_wake_slots_remain_stable_across_unregister_and_reuse(void) {
  esy_wake_source_t sources[3] = {0};
  esy_wake_source_config_t config = {.name = "wake", .ops = &k_wake_ops, .ctx = &s_ctx.wake};
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  esy_power_result_t result;
  TEST_ASSERT_EQUAL(ESY_OK, initialize_manager(1u, false));
  for (uint32_t i = 0; i < 3u; i++)
    TEST_ASSERT_EQUAL(ESY_OK, esy_wake_source_register(&s_ctx.manager, &sources[i], &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_wake_source_unregister(&sources[0]));
  TEST_ASSERT_EQUAL_UINT32(1u, sources[1].manager_index);
  TEST_ASSERT_EQUAL_UINT32(2u, sources[2].manager_index);
  TEST_ASSERT_EQUAL(ESY_OK, esy_wake_source_register(&s_ctx.manager, &sources[0], &config));
  TEST_ASSERT_EQUAL_UINT32(0u, sources[0].manager_index);
  TEST_ASSERT_EQUAL(ESY_OK, esy_wake_source_unregister(&sources[1]));
  start_manager(false, false);
  prepare_request(&request, &transaction);
  request.required_wake_source_mask = 2u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_power_transaction_prepare(&s_ctx.manager, &transaction, &request));
  request.required_wake_source_mask = 4u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_prepare(&s_ctx.manager, &transaction, &request));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_enter_atomic(&transaction));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_recover_atomic(&transaction));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_complete(&transaction, &result));
  TEST_ASSERT_EQUAL_UINT32(4u, result.triggered_wake_source_mask);
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_manager_deinit(&s_ctx.manager));
}

void test_tickless_failed_prepare_cleanup_resets_without_releasing_resources(void) {
  esy_power_tickless_adapter_t adapter = {0};
  esy_power_tickless_adapter_config_t config = {
      .manager = &s_ctx.manager, .maximum_exit_latency_us = 500u,
      .allowed_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION)};
  const esy_os_tickless_ops_t *ops = esy_power_tickless_adapter_ops();
  TEST_ASSERT_EQUAL(ESY_OK, initialize_manager(1u, false));
  register_producer();
  start_manager(false, false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_tickless_adapter_init(&adapter, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_set_tickless_ops(ops, &adapter));
  s_ctx.backend.begin_status = ESY_ERR_IO;
  s_ctx.participant_ctx.resume_status = ESY_ERR_IO;
  run_os_tickless(20u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_clear_tickless_ops(ops, &adapter));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.os_reset_calls);
  TEST_ASSERT_TRUE(s_ctx.os_irq_masked);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.os_irq_restore_calls);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_power_tickless_adapter_deinit(&adapter));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_power_tickless_adapter_init(&adapter, &config));
}

void test_tickless_adapter_repeated_lifecycle_preserves_prepared_transaction(void) {
  esy_power_tickless_adapter_t adapter = {0};
  esy_power_tickless_adapter_config_t config = {
      .manager = &s_ctx.manager, .maximum_exit_latency_us = 500u,
      .allowed_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION)};
  const esy_os_tickless_ops_t *ops = esy_power_tickless_adapter_ops();
  TEST_ASSERT_EQUAL(ESY_OK, initialize_manager(1u, false));
  start_manager(false, false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_tickless_adapter_init(&adapter, &config));
  TEST_ASSERT_EQUAL(ESY_OK, ops->prepare(&adapter, 5000u));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_power_tickless_adapter_init(&adapter, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_power_tickless_adapter_deinit(&adapter));
  TEST_ASSERT_EQUAL(ESY_OK, ops->abort(&adapter));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_tickless_adapter_deinit(&adapter));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_power_tickless_adapter_deinit(&adapter));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_tickless_adapter_init(&adapter, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_tickless_adapter_deinit(&adapter));
}

void test_watchdog_adapter_repeated_lifecycle_does_not_erase_prepared_state(void) {
  esy_power_watchdog_constraint_t adapter = {0};
  esy_monotonic_clock_t clock = {.ops = &k_clock_ops, .ctx = &s_ctx.clocksource};
  const esy_power_constraint_ops_t *ops = esy_power_watchdog_constraint_ops();
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_watchdog_constraint_init(&adapter, &clock));
  TEST_ASSERT_EQUAL(ESY_OK, ops->prepare(&adapter, ESY_POWER_STATE_CPU_IDLE, 5000u));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_power_watchdog_constraint_init(&adapter, &clock));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_power_watchdog_constraint_deinit(&adapter));
  TEST_ASSERT_EQUAL(ESY_OK, ops->resume(&adapter, ESY_POWER_STATE_CPU_IDLE, 5000u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_watchdog_constraint_deinit(&adapter));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_power_watchdog_constraint_deinit(&adapter));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_watchdog_constraint_init(&adapter, &clock));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_watchdog_constraint_deinit(&adapter));
}

void test_platform_backend_repeated_init_preserves_active_transaction(void) {
  esy_power_hal_backend_t backend = {0};
  const esy_power_backend_ops_t *ops = esy_power_hal_backend_ops();
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_hal_backend_init(&backend));
  TEST_ASSERT_EQUAL(ESY_OK, ops->begin(&backend, ESY_POWER_STATE_CPU_IDLE));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_power_hal_backend_init(&backend));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, ops->begin(&backend, ESY_POWER_STATE_CPU_IDLE));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_power_hal_backend_deinit(&backend));
  TEST_ASSERT_EQUAL(ESY_OK, ops->abort(&backend));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_hal_backend_deinit(&backend));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_power_hal_backend_deinit(&backend));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_hal_backend_init(&backend));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_hal_backend_deinit(&backend));
}

void test_init_failure_preserves_storage_and_valid_retry_initializes_it(void) {
  esy_power_manager_config_t config = manager_config(1u, false);
  esy_power_backend_ops_t backend = k_backend_ops;
  unsigned char saved[sizeof(s_ctx.power_storage)];
  memset(&s_ctx.power_storage, 0xa5, sizeof(s_ctx.power_storage));
  memcpy(saved, &s_ctx.power_storage, sizeof(saved));
  backend.enter_atomic = NULL;
  config.backend_ops = &backend;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_power_manager_init(&s_ctx.manager, &config));
  TEST_ASSERT_EQUAL_MEMORY(saved, &s_ctx.power_storage, sizeof(saved));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.manager.magic);
  config.backend_ops = &k_backend_ops;
  s_ctx.power_resources.wake_source_capacity = 33u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_power_manager_init(&s_ctx.manager, &config));
  TEST_ASSERT_EQUAL_MEMORY(saved, &s_ctx.power_storage, sizeof(saved));
  s_ctx.power_resources.wake_source_capacity = 4u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_manager_init(&s_ctx.manager, &config));
  TEST_ASSERT_NULL(s_ctx.power_storage.wake_sources[0]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_manager_deinit(&s_ctx.manager));
}

void test_optional_timer_and_clock_contracts_fail_closed(void) {
  esy_power_manager_config_t config = manager_config(1u, false);
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  esy_power_result_t result;
  config.clock.ops = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_power_manager_init(&s_ctx.manager, &config));
  config.clocksource_ops = NULL;
  config.deadline_timer_ops = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_power_manager_init(&s_ctx.manager, &config));
  config.deadline_timer_ctx = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_manager_init(&s_ctx.manager, &config));
  start_manager(true, false);
  prepare_request(&request, &transaction);
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_power_transaction_prepare(&s_ctx.manager, &transaction, &request));
  request.latest_resume_time_us = ESY_POWER_TIME_INFINITE;
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_prepare(&s_ctx.manager, &transaction, &request));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_enter_atomic(&transaction));
  TEST_ASSERT_FALSE(transaction.result.elapsed_valid);
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_recover_atomic(&transaction));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_complete(&transaction, &result));
}

void test_prepare_failure_matrix_balances_previously_prepared_resources(void) {
  for (uint32_t scenario = 0u; scenario < 6u; scenario++) {
    esy_power_request_t request;
    esy_power_transaction_t transaction;
    setUp();
    TEST_ASSERT_EQUAL(ESY_OK, initialize_manager(2u, true));
    register_producer();
    start_manager(true, true);
    prepare_request(&request, &transaction);
    switch (scenario) {
      case 0: s_ctx.participant_ctx.quiesce_status = ESY_ERR_IO; break;
      case 1: s_ctx.constraint_ctx.prepare_status = ESY_ERR_IO; break;
      case 2: s_ctx.wake.prepare_status = ESY_ERR_IO; break;
      case 3: s_ctx.deadline.prepare_status = ESY_ERR_IO; break;
      case 4: s_ctx.coordinator.prepare_status = ESY_ERR_IO; break;
      default: s_ctx.backend.begin_status = ESY_ERR_IO; break;
    }
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_power_transaction_prepare(&s_ctx.manager, &transaction, &request));
    TEST_ASSERT_EQUAL(ESY_OK, transaction.result.recovery_error);
    TEST_ASSERT_FALSE(s_ctx.participant.quiesced);
    TEST_ASSERT_FALSE(s_ctx.constraint.prepared);
    TEST_ASSERT_FALSE(s_ctx.wake_source.prepared);
    TEST_ASSERT_FALSE(transaction.deadline_prepared);
    TEST_ASSERT_FALSE(transaction.cores_prepared);
    TEST_ASSERT_EQUAL(ESY_OK, esy_power_manager_deinit(&s_ctx.manager));
  }
}

void test_prepare_rollback_failure_matrix_retains_failed_ownership(void) {
  for (uint32_t scenario = 0u; scenario < 6u; scenario++) {
    esy_power_request_t request;
    esy_power_transaction_t transaction;
    esy_power_result_t result;
    setUp();
    TEST_ASSERT_EQUAL(ESY_OK, initialize_manager(2u, true));
    register_producer();
    start_manager(true, true);
    prepare_request(&request, &transaction);
    TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_prepare(&s_ctx.manager, &transaction, &request));
    switch (scenario) {
      case 0: s_ctx.backend.abort_status = ESY_ERR_IO; break;
      case 1: s_ctx.deadline.abort_status = ESY_ERR_IO; break;
      case 2: s_ctx.constraint_ctx.resume_status = ESY_ERR_IO; break;
      case 3: s_ctx.participant_ctx.resume_status = ESY_ERR_IO; break;
      case 4: s_ctx.wake.abort_status = ESY_ERR_IO; break;
      default: s_ctx.coordinator.abort_status = ESY_ERR_IO; break;
    }
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_power_transaction_abort(&transaction, &result));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, result.recovery_error);
    TEST_ASSERT_TRUE(transaction.cores_prepared);
    if (scenario < 4u) TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.wake.abort_calls);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_power_manager_deinit(&s_ctx.manager));
  }
}

void test_atomic_failure_matrix_never_releases_peers_before_late_recovery(void) {
  for (uint32_t scenario = 0u; scenario < 3u; scenario++) {
    esy_power_request_t request;
    esy_power_transaction_t transaction;
    esy_power_result_t result;
    setUp();
    TEST_ASSERT_EQUAL(ESY_OK, initialize_manager(2u, true));
    start_manager(true, true);
    prepare_request(&request, &transaction);
    TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_prepare(&s_ctx.manager, &transaction, &request));
    if (scenario == 0u) s_ctx.coordinator.park_status = ESY_ERR_IO;
    if (scenario == 1u) s_ctx.constraint_ctx.revalidated_deadline_us = 1050u;
    if (scenario == 2u) s_ctx.deadline.arm_status = ESY_ERR_IO;
    TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_power_transaction_enter_atomic(&transaction));
    TEST_ASSERT_FALSE(transaction.result.fatal);
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.coordinator.abort_calls);
    TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_recover_atomic(&transaction));
    TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_abort(&transaction, &result));
    TEST_ASSERT_LESS_THAN_UINT32(s_ctx.coordinator.abort_order, s_ctx.wake.finish_order);
  }
}

void test_atomic_capture_failures_are_fatal_and_keep_resources_owned(void) {
  for (uint32_t scenario = 0u; scenario < 3u; scenario++) {
    esy_power_request_t request;
    esy_power_transaction_t transaction;
    setUp();
    TEST_ASSERT_EQUAL(ESY_OK, initialize_manager(2u, true));
    start_manager(true, false);
    prepare_request(&request, &transaction);
    TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_prepare(&s_ctx.manager, &transaction, &request));
    if (scenario == 0u) s_ctx.wake.capture_status = ESY_ERR_IO;
    if (scenario == 1u) s_ctx.deadline.cancel_status = ESY_ERR_IO;
    if (scenario == 2u) s_ctx.clocksource.samples[2] = 0u;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_power_transaction_enter_atomic(&transaction));
    TEST_ASSERT_TRUE(transaction.result.fatal);
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.wake.finish_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.coordinator.release_calls);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_power_transaction_recover_atomic(&transaction));
  }
}

void test_early_recovery_errors_fault_manager_and_retain_backend_on_failure(void) {
  for (uint32_t scenario = 0u; scenario < 3u; scenario++) {
    esy_power_request_t request;
    esy_power_transaction_t transaction;
    setUp();
    TEST_ASSERT_EQUAL(ESY_OK, initialize_manager(2u, true));
    start_manager(true, false);
    prepare_request(&request, &transaction);
    TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_prepare(&s_ctx.manager, &transaction, &request));
    if (scenario == 0u) s_ctx.wake.capture_status = ESY_ERR_IO;
    if (scenario == 1u) s_ctx.backend.abort_status = ESY_ERR_IO;
    if (scenario == 2u) s_ctx.device_ctx.atomic_resume_status = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_power_transaction_recover_atomic(&transaction));
    TEST_ASSERT_TRUE(transaction.result.fatal);
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.wake.finish_calls);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_power_manager_deinit(&s_ctx.manager));
    if (scenario == 1u) TEST_ASSERT_TRUE(transaction.backend_begun);
  }
}

void test_late_completion_failure_matrix_holds_dependent_resources(void) {
  for (uint32_t scenario = 0u; scenario < 6u; scenario++) {
    esy_power_request_t request;
    esy_power_transaction_t transaction;
    esy_power_result_t result;
    setUp();
    TEST_ASSERT_EQUAL(ESY_OK, initialize_manager(2u, true));
    register_producer();
    start_manager(true, true);
    prepare_request(&request, &transaction);
    TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_prepare(&s_ctx.manager, &transaction, &request));
    TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_enter_atomic(&transaction));
    TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_recover_atomic(&transaction));
    switch (scenario) {
      case 0: s_ctx.deadline.finish_status = ESY_ERR_IO; break;
      case 1: s_ctx.constraint_ctx.resume_status = ESY_ERR_IO; break;
      case 2: s_ctx.participant_ctx.resume_status = ESY_ERR_IO; break;
      case 3: s_ctx.wake.finish_status = ESY_ERR_IO; break;
      case 4: s_ctx.coordinator.release_status = ESY_ERR_IO; break;
      default: s_ctx.coordinator.complete_status = ESY_ERR_IO; break;
    }
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_power_transaction_complete(&transaction, &result));
    TEST_ASSERT_TRUE(transaction.cores_prepared);
    if (scenario < 3u) TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.wake.finish_calls);
    if (scenario < 4u) TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.coordinator.release_calls);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_power_manager_deinit(&s_ctx.manager));
  }
}

void test_full_registration_capacity_and_frozen_registration_leave_inputs_unchanged(void) {
  esy_power_lock_t locks[5] = {0};
  esy_power_lock_config_t config = {.name = "lock", .deepest_allowed_state = ESY_POWER_STATE_CPU_IDLE};
  TEST_ASSERT_EQUAL(ESY_OK, initialize_manager(1u, false));
  for (uint32_t i = 0u; i < 4u; i++)
    TEST_ASSERT_EQUAL(ESY_OK, esy_power_lock_register(&s_ctx.manager, &locks[i], &config));
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_power_lock_register(&s_ctx.manager, &locks[4], &config));
  TEST_ASSERT_EQUAL_UINT32(0u, locks[4].magic);
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_lock_unregister(&locks[1]));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_lock_register(&s_ctx.manager, &locks[4], &config));
  start_manager(false, false);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_power_lock_unregister(&locks[4]));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_power_lock_register(&s_ctx.manager, &locks[1], &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_manager_deinit(&s_ctx.manager));
}

void test_transition_rejects_new_lock_and_second_transaction(void) {
  esy_power_request_t request;
  esy_power_transaction_t transaction;
  esy_power_transaction_t other = {0};
  esy_power_result_t result;
  esy_power_lock_config_t config = {.name = "lock", .deepest_allowed_state = ESY_POWER_STATE_CPU_IDLE};
  TEST_ASSERT_EQUAL(ESY_OK, initialize_manager(1u, false));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_lock_register(&s_ctx.manager, &s_ctx.lock, &config));
  start_manager(true, false);
  prepare_request(&request, &transaction);
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_prepare(&s_ctx.manager, &transaction, &request));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_power_lock_acquire(&s_ctx.lock));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_power_transaction_prepare(&s_ctx.manager, &other, &request));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_power_manager_deinit(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_transaction_abort(&transaction, &result));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_lock_acquire(&s_ctx.lock));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_lock_release(&s_ctx.lock));
}

void test_platform_backend_successful_completion_allows_next_transaction(void) {
  esy_power_hal_backend_t backend = {0};
  esy_power_backend_result_t result;
  const esy_power_backend_ops_t *ops = esy_power_hal_backend_ops();
  s_ctx.platform_result.entered = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_hal_backend_init(&backend));
  TEST_ASSERT_EQUAL(ESY_OK, ops->begin(&backend, ESY_POWER_STATE_CPU_IDLE));
  TEST_ASSERT_EQUAL(ESY_OK, ops->enter_atomic(&backend, &result));
  TEST_ASSERT_EQUAL(ESY_OK, ops->complete(&backend));
  TEST_ASSERT_FALSE(backend.active);
  TEST_ASSERT_EQUAL(ESY_OK, ops->begin(&backend, ESY_POWER_STATE_CPU_IDLE));
  TEST_ASSERT_EQUAL(ESY_OK, ops->abort(&backend));
  TEST_ASSERT_EQUAL(ESY_OK, esy_power_hal_backend_deinit(&backend));
}
