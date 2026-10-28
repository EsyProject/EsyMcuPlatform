/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_pwm.h"
#include "esy_hal/esy_hal_pwm_device.h"

#include "../src/esy_hal_pwm_internal.h"
#include "hal_test_fakes.h"
#include "unity.h"

void setUp(void) {
  platform_test_reset();
  platform_test_set_dma_busy(false);
}

void tearDown(void) {}

static void init_pwm(esy_pwm_t *pwm) {
  esy_pwm_config_t cfg = {
      .phys_index = TEST_PWM_INDEX,
      .freq_hz = 1000u,
      .duty_cycle = 50u,
      .start_low = false,
  };

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_pwm_init(pwm, &cfg));
}

static void pwm_test_handler(void *arg) {
  (void)arg;
}

static void complete_pwm(bool error) {
  esy_dma_config_t submitted = platform_test_state()->submitted_dma;
  esy_dma_callback_t callback = error ? submitted.error_callback : submitted.callback;
  TEST_ASSERT_NOT_NULL(callback);
  callback(error ? submitted.error_callback_arg : submitted.callback_arg);
}

void test_pwm_native_ops_are_shared_and_output_state_is_per_handle(void) {
  esy_pwm_t first = {0};
  esy_pwm_t second = {0};
  init_pwm(&first);
  const esy_pwm_vtable_t *ops = first.ops;
  TEST_ASSERT_NOT_NULL(ops);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_enable(&first, true));
  TEST_ASSERT_TRUE(esy_hal_pwm_runtime(&first)->output_enabled);
  TEST_ASSERT_FALSE(esy_hal_pwm_runtime(&second)->output_enabled);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_deinit(&first));
  TEST_ASSERT_NULL(first.ops);

  init_pwm(&second);
  TEST_ASSERT_EQUAL_PTR(ops, second.ops);
  TEST_ASSERT_FALSE(esy_hal_pwm_runtime(&second)->output_enabled);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_deinit(&second));
}

void test_pwm_custom_immutable_ops_may_omit_optional_hooks(void) {
  static const esy_pwm_vtable_t k_empty_ops = {0};
  esy_pwm_t pwm = {.ops = &k_empty_ops};
  uint32_t caps = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_IMPLEMENTED, esy_hal_pwm_enable(&pwm, true));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_IMPLEMENTED, esy_hal_pwm_get_caps(&pwm, &caps));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_pwm_abort_dma(&pwm));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_pwm_poll(&pwm));
}

void test_esy_hal_pwm_default_cfg_fills_duty_default(void) {
  esy_pwm_config_t cfg = {0};

  esy_hal_pwm_default_cfg(NULL);
  esy_hal_pwm_default_cfg(&cfg);

  TEST_ASSERT_EQUAL_UINT32(50u, cfg.duty_cycle);

  cfg.duty_cycle = 25u;
  esy_hal_pwm_default_cfg(&cfg);
  TEST_ASSERT_EQUAL_UINT32(50u, cfg.duty_cycle);
}

void test_pwm_preflight_rejection_preserves_handle_and_board_resources(void) {
  esy_pwm_t pwm = {0};
  esy_pwm_t before = pwm;
  esy_pwm_config_t cfg = {.phys_index = TEST_PWM_INDEX,
                         .freq_hz = 1000u, .duty_cycle = 50u};
  const esy_err_t results[] = {ESY_ERR_BUSY, ESY_ERR_INVALID_ARG};
  for (size_t i = 0u; i < sizeof(results) / sizeof(results[0]); ++i) {
    platform_test_set_pwm_available_result(results[i]);
    TEST_ASSERT_EQUAL(results[i], esy_hal_pwm_init(&pwm, &cfg));
    TEST_ASSERT_EQUAL_MEMORY(&before, &pwm, sizeof(pwm));
    TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_configure_order);
    TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
    TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_init_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_deinit_order);
  }
  platform_test_set_pwm_available_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_init(&pwm, &cfg));
  TEST_ASSERT_TRUE(platform_test_state()->pwm_available_order <
                   platform_test_state()->board_configure_order);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_deinit(&pwm));
}

void test_pwm_invalid_frequency_and_duty_do_not_configure_board_pins(void) {
  esy_pwm_t pwm = {0};
  esy_pwm_config_t cfg = {.phys_index = TEST_PWM_INDEX,
                         .freq_hz = 0u, .duty_cycle = 50u};
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_pwm_init(&pwm, &cfg));
  cfg.freq_hz = 1000u;
  cfg.duty_cycle = 101u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_pwm_init(&pwm, &cfg));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_configure_order);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->pwm_available_calls);
}

void test_esy_hal_pwm_contracts_are_enforced(void) {
  esy_pwm_t pwm = {0};
  uint32_t sample = 0u;
  esy_dma_config_t dma = {
      .src = &sample,
      .length = sizeof(sample),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_32,
  };
  uint32_t caps = 0u;
  uint8_t index = 0u;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_pwm_init(NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_pwm_init(&pwm, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_pwm_set_duty(NULL, 0.5f));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_pwm_set_duty(&pwm, -0.1f));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_pwm_set_duty(&pwm, 1.5f));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED,
                          esy_hal_pwm_set_duty(&pwm, 0.5f));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_pwm_set_wrap(NULL, 1000u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_pwm_set_wrap(&pwm, 0u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED, esy_hal_pwm_set_wrap(&pwm, 1000u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_pwm_enable(NULL, true));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED, esy_hal_pwm_enable(&pwm, true));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_pwm_set_polarity(NULL, true));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED, esy_hal_pwm_set_polarity(&pwm, true));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_pwm_enable_trigger_output(NULL, true));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                          esy_hal_pwm_enable_trigger_output(&pwm, true));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_pwm_dma_output(NULL, &dma, false));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_pwm_dma_output(&pwm, NULL, false));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_hal_pwm_dma_output(&pwm, &dma, false));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_pwm_set_wrap_handler(NULL, pwm_test_handler, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED,
                          esy_hal_pwm_set_wrap_handler(&pwm, pwm_test_handler, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_pwm_suspend(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_hal_pwm_suspend(&pwm));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_pwm_resume(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_hal_pwm_resume(&pwm));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_pwm_set_low_power_mode(NULL, true));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                          esy_hal_pwm_set_low_power_mode(&pwm, true));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_pwm_set_clock_source(NULL, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                          esy_hal_pwm_set_clock_source(&pwm, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_pwm_get_caps(NULL, &caps));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_pwm_get_caps(&pwm, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED, esy_hal_pwm_get_caps(&pwm, &caps));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_pwm_get_phys_index(NULL, &index));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_pwm_get_phys_index(&pwm, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED, esy_hal_pwm_get_phys_index(&pwm, &index));

  init_pwm(&pwm);
  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_TRUE(state->board_configure_order < state->driver_init_order);
  TEST_ASSERT_NOT_NULL(pwm.ops->set_duty);
  TEST_ASSERT_NOT_NULL(pwm.ops->dma_output);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, pwm.ops->dma_output(NULL, &dma, false));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, pwm.ops->dma_output(&pwm, NULL, false));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          pwm.ops->set_wrap_handler(NULL, pwm_test_handler, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_pwm_set_duty(&pwm, 0.5f));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_write_calls);
  TEST_ASSERT_FALSE(esy_hal_pwm_runtime(&pwm)->output_enabled);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_pwm_set_wrap(&pwm, 1000u));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_pwm_enable(&pwm, true));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_pwm_set_duty(&pwm, 0.5f));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_pwm_set_polarity(&pwm, true));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_pwm_enable_trigger_output(&pwm, true));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_hal_pwm_dma_output(&pwm, &dma, false));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_pwm_enable(&pwm, false));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_pwm_dma_output(&pwm, &dma, false));
  TEST_ASSERT_EQUAL_UINT32(TEST_PWM_DREQ, platform_test_state()->last_pwm_inner.dreq);
  TEST_ASSERT_EQUAL_UINT8(TEST_PWM_CHANNEL, platform_test_state()->last_pwm_inner.channel);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_hal_pwm_suspend(&pwm));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_pwm_poll(&pwm));
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->pwm_poll_calls);
  complete_pwm(false);
  TEST_ASSERT_FALSE(esy_hal_pwm_runtime(&pwm)->dma_active);
  TEST_ASSERT_TRUE(esy_hal_pwm_runtime(&pwm)->output_enabled);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_enable(&pwm, false));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_pwm_set_wrap_handler(&pwm, pwm_test_handler, &sample));
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->driver_set_wrap_handler_calls);
  TEST_ASSERT_EQUAL_UINT8(TEST_PWM_IRQ_PRIORITY, platform_test_state()->last_irq_priority);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_pwm_suspend(&pwm));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_pwm_resume(&pwm));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_pwm_set_low_power_mode(&pwm, true));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_pwm_set_clock_source(&pwm, 2u));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_pwm_get_caps(&pwm, &caps));
  TEST_ASSERT_EQUAL_UINT32(ESY_PWM_CAP_DMA_OUTPUT | ESY_PWM_CAP_WRAP_IRQ | ESY_PWM_CAP_POLARITY, caps);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_pwm_get_phys_index(&pwm, &index));
  TEST_ASSERT_EQUAL_UINT8(TEST_PWM_INDEX, index);
  esy_hal_pwm_handle_irq(TEST_PWM_INDEX);

  platform_test_reset_order_counters();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_pwm_deinit(&pwm));
  state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT8(TEST_PWM_INDEX, state->reset_index);
  TEST_ASSERT_TRUE(state->driver_get_index_order < state->board_reset_order);
  TEST_ASSERT_TRUE(state->driver_deinit_order < state->board_reset_order);
}

typedef struct {
  esy_pwm_t *pwm;
  uint32_t calls;
  bool saw_borrow;
} pwm_callback_probe_t;

static void inspect_pwm_callback(void *arg) {
  pwm_callback_probe_t *probe = arg;
  ++probe->calls;
  probe->saw_borrow = esy_hal_pwm_runtime(probe->pwm)->dma_active;
  TEST_ASSERT_TRUE(esy_hal_pwm_runtime(probe->pwm)->callback_running);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_abort_dma(probe->pwm));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_poll(probe->pwm));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_deinit(probe->pwm));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_set_duty(probe->pwm, 0.5f));
}

void test_pwm_finite_completion_is_published_before_submission_and_retires_after_callback(void) {
  esy_pwm_t pwm = {0};
  uint16_t values[2] = {1u, 2u};
  pwm_callback_probe_t probe = {.pwm = &pwm};
  esy_dma_config_t dma = {.src = values, .length = sizeof(values),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_16,
      .callback = inspect_pwm_callback, .callback_arg = &probe};
  init_pwm(&pwm);
  platform_test_set_pwm_dma_complete_inline(true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_dma_output(&pwm, &dma, false));
  TEST_ASSERT_TRUE(probe.saw_borrow);
  TEST_ASSERT_EQUAL_UINT32(1u, probe.calls);
  TEST_ASSERT_FALSE(esy_hal_pwm_runtime(&pwm)->dma_active);
  TEST_ASSERT_TRUE(esy_hal_pwm_runtime(&pwm)->output_enabled);
  TEST_ASSERT_FALSE(esy_hal_pwm_runtime(&pwm)->callback_running);
  TEST_ASSERT_EQUAL_PTR(inspect_pwm_callback, dma.callback);
  TEST_ASSERT_NULL(dma.dst);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_deinit(&pwm));
}

static void count_pwm_callback(void *arg) { ++*(uint32_t *)arg; }

void test_pwm_circular_notifications_keep_borrow_until_successful_abort(void) {
  esy_pwm_t pwm = {0};
  uint16_t values[2] = {1u, 2u};
  uint32_t complete = 0u, half = 0u;
  esy_dma_config_t dma = {.src = values, .length = sizeof(values),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_16, .circular = true,
      .callback = count_pwm_callback, .callback_arg = &complete,
      .half_callback = count_pwm_callback, .half_callback_arg = &half};
  init_pwm(&pwm);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_dma_output(&pwm, &dma, false));
  esy_dma_config_t submitted = platform_test_state()->submitted_dma;
  TEST_ASSERT_TRUE(submitted.circular);
  submitted.half_callback(submitted.half_callback_arg);
  submitted.callback(submitted.callback_arg);
  submitted.callback(submitted.callback_arg);
  TEST_ASSERT_EQUAL_UINT32(1u, half);
  TEST_ASSERT_EQUAL_UINT32(2u, complete);
  TEST_ASSERT_TRUE(esy_hal_pwm_runtime(&pwm)->dma_active);
  TEST_ASSERT_TRUE(esy_hal_pwm_runtime(&pwm)->output_enabled);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_dma_output(&pwm, &dma, false));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_enable(&pwm, false));
  platform_test_set_pwm_results(ESY_OK, ESY_ERR_BUSY, ESY_OK, ESY_ERR_BUSY);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_poll(&pwm));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_abort_dma(&pwm));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_deinit(&pwm));
  TEST_ASSERT_TRUE(esy_hal_pwm_runtime(&pwm)->dma_active);
  TEST_ASSERT_NOT_NULL(pwm.ctx);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  platform_test_set_pwm_results(ESY_OK, ESY_OK, ESY_OK, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_abort_dma(&pwm));
  TEST_ASSERT_FALSE(esy_hal_pwm_runtime(&pwm)->dma_active);
  TEST_ASSERT_FALSE(esy_hal_pwm_runtime(&pwm)->output_enabled);
  submitted.callback(submitted.callback_arg);
  TEST_ASSERT_EQUAL_UINT32(2u, complete);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_deinit(&pwm));
}

void test_pwm_failed_submission_and_cleanup_preserve_buffers_until_retry(void) {
  esy_pwm_t pwm = {0};
  uint16_t values[2] = {1u, 2u};
  uint32_t errors = 0u;
  esy_dma_config_t dma = {.src = values, .length = sizeof(values),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_16,
      .error_callback = count_pwm_callback, .error_callback_arg = &errors};
  init_pwm(&pwm);
  platform_test_set_pwm_results(ESY_OK, ESY_ERR_BUSY, ESY_ERR_FAIL, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_dma_output(&pwm, &dma, false));
  TEST_ASSERT_TRUE(esy_hal_pwm_runtime(&pwm)->dma_active);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_set_wrap(&pwm, 2000u));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_deinit(&pwm));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  platform_test_set_pwm_results(ESY_OK, ESY_OK, ESY_OK, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_abort_dma(&pwm));
  TEST_ASSERT_EQUAL_UINT32(0u, errors);
  TEST_ASSERT_FALSE(esy_hal_pwm_runtime(&pwm)->dma_active);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_dma_output(&pwm, &dma, false));
  complete_pwm(true);
  TEST_ASSERT_EQUAL_UINT32(1u, errors);
  TEST_ASSERT_FALSE(esy_hal_pwm_runtime(&pwm)->dma_active);
  TEST_ASSERT_FALSE(esy_hal_pwm_runtime(&pwm)->output_enabled);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_deinit(&pwm));
}

void test_pwm_deinit_failure_keeps_handle_and_pin_until_retry(void) {
  esy_pwm_t pwm = {0};
  init_pwm(&pwm);
  platform_test_set_pwm_results(ESY_ERR_FAIL, ESY_OK, ESY_OK, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_hal_pwm_deinit(&pwm));
  TEST_ASSERT_NOT_NULL(pwm.ctx);
  TEST_ASSERT_NOT_NULL(pwm.ops->set_duty);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_pwm_init(&pwm,
      &(esy_pwm_config_t){.phys_index = TEST_PWM_INDEX}));
  platform_test_set_pwm_results(ESY_OK, ESY_OK, ESY_OK, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_deinit(&pwm));
  TEST_ASSERT_NULL(pwm.ctx);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_deinit(&pwm));
  init_pwm(&pwm);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_deinit(&pwm));
}

void test_pwm_graph_teardown_unbinds_only_after_dma_retirement_then_reinitializes(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = {
      .name = "pwm", .supported_state_mask =
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE)};
  esy_pwm_device_binding_t binding = {0};
  esy_pwm_t pwm = {0};
  uint16_t value = 1u;
  esy_dma_config_t dma = {.src = &value, .length = sizeof(value),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_16};
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  esy_device_registry_config_t config = {.static_resources = &resources};
  for (unsigned cycle = 0u; cycle < 2u; ++cycle) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_init(&registry, &config));
    init_pwm(&pwm);
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_register_device(&binding, &pwm,
                                                       &registry, &registration));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
    uint32_t usage = 0u;
    if (cycle == 0u) {
      platform_test_set_pwm_results(ESY_OK, ESY_ERR_BUSY, ESY_ERR_FAIL, ESY_OK);
      TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_dma_output(&pwm, &dma, true));
      TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&binding.device, &usage));
      TEST_ASSERT_EQUAL_UINT32(1u, usage);
      TEST_ASSERT_TRUE(binding.dma_ref_held);
      TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_device_registry_deinit(&registry));
      TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_abort_dma(&pwm));
      TEST_ASSERT_TRUE(binding.dma_ref_held);
      platform_test_set_pwm_results(ESY_OK, ESY_OK, ESY_OK, ESY_OK);
      TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_abort_dma(&pwm));
      TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&binding.device, &usage));
      TEST_ASSERT_EQUAL_UINT32(0u, usage);
    }
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_dma_output(&pwm, &dma, true));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&binding.device, &usage));
    TEST_ASSERT_EQUAL_UINT32(1u, usage);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_device_registry_deinit(&registry));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_unregister_device(&binding));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_abort_dma(&pwm));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&binding.device, &usage));
    TEST_ASSERT_EQUAL_UINT32(0u, usage);
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
    TEST_ASSERT_NULL(binding.device.registry);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_pwm_deinit(&pwm));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_unregister_device(&binding));
    TEST_ASSERT_NULL(esy_hal_pwm_runtime(&pwm)->device);
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_deinit(&pwm));
  }
}

void test_disabled_pwm_duty_never_reaches_the_hardware_backend(void) {
  esy_pwm_t pwm = {0};
  init_pwm(&pwm);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_pwm_set_duty(&pwm, 0.5f));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_write_calls);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_enable(&pwm, true));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_set_duty(&pwm, 0.5f));
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->driver_write_calls);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_enable(&pwm, false));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_pwm_set_duty(&pwm, 0.75f));
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->driver_write_calls);
  TEST_ASSERT_FALSE(esy_hal_pwm_runtime(&pwm)->output_enabled);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_enable(&pwm, false));
  TEST_ASSERT_FALSE(esy_hal_pwm_runtime(&pwm)->output_enabled);
}
