/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_hal_pwm.h"
#include "esy_common/device/esy_device.h"
#include "unity.h"

void setUp(void) { fake_esy_hal_pwm_reset(); }
void tearDown(void) {}

static void init_pwm(esy_pwm_t *pwm) {
  esy_pwm_config_t config = {.phys_index = 3u, .freq_hz = 1000u,
                             .duty_cycle = 50u};
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_init(pwm, &config));
}

void test_pwm_capabilities_follow_public_bits_and_opt_in_dma_support(void) {
  esy_pwm_t pwm = {0};
  uint32_t caps = UINT32_MAX;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_pwm_get_caps(NULL, &caps));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_pwm_get_caps(&pwm, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_IMPLEMENTED, esy_hal_pwm_get_caps(&pwm, &caps));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, caps);
  init_pwm(&pwm);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_get_caps(&pwm, &caps));
  TEST_ASSERT_EQUAL_UINT32(ESY_PWM_CAP_WRAP_IRQ | ESY_PWM_CAP_POLARITY, caps);
  fake_esy_hal_pwm_set_dma_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_get_caps(&pwm, &caps));
  TEST_ASSERT_EQUAL_UINT32(ESY_PWM_CAP_DMA_OUTPUT | ESY_PWM_CAP_WRAP_IRQ |
                           ESY_PWM_CAP_POLARITY, caps);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_deinit(&pwm));
}

static void count_notification(void *arg) { ++*(unsigned *)arg; }

typedef struct {
  esy_pwm_t *pwm;
  unsigned calls;
} callback_probe_t;

static void inspect_callback(void *arg) {
  callback_probe_t *probe = arg;
  ++probe->calls;
  TEST_ASSERT_TRUE(fake_esy_hal_pwm_is_dma_active(probe->pwm));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_abort_dma(probe->pwm));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_poll(probe->pwm));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_deinit(probe->pwm));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_enable(probe->pwm, false));
}

void test_fake_pwm_lifecycle_matches_status_deinit_and_reinitialization(void) {
  esy_pwm_t pwm = {0};
  esy_pwm_config_t config = {.phys_index = 1u};
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_pwm_init(NULL, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_pwm_init(&pwm, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_pwm_deinit(NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_deinit(&pwm));
  init_pwm(&pwm);
  TEST_ASSERT_TRUE(fake_esy_hal_pwm_is_initialized(&pwm));
  TEST_ASSERT_EQUAL_FLOAT(0.5f, fake_esy_hal_pwm_get_duty(&pwm));
  TEST_ASSERT_NOT_NULL(pwm.ops->abort_dma);
  TEST_ASSERT_NOT_NULL(pwm.ops->poll);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_pwm_init(&pwm, &config));
  uint8_t index = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_get_phys_index(&pwm, &index));
  TEST_ASSERT_EQUAL_UINT8(3u, index);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_deinit(&pwm));
  TEST_ASSERT_FALSE(fake_esy_hal_pwm_is_initialized(&pwm));
  TEST_ASSERT_NULL(pwm.ops);
  TEST_ASSERT_NULL(pwm.ops);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_deinit(&pwm));
  init_pwm(&pwm);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_deinit(&pwm));
}

void test_fake_pwm_registered_handle_cannot_be_destroyed(void) {
  esy_pwm_t pwm = {0};
  esy_device_t device = {0};
  init_pwm(&pwm);
  fake_esy_hal_pwm_set_device(&pwm, &device);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_pwm_deinit(&pwm));
  TEST_ASSERT_TRUE(fake_esy_hal_pwm_is_initialized(&pwm));
  fake_esy_hal_pwm_set_device(&pwm, NULL);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_deinit(&pwm));
}

void test_fake_pwm_idle_poll_abort_are_defined_and_dma_remains_opt_in(void) {
  esy_pwm_t pwm = {0};
  uint16_t sample = 2u;
  esy_dma_config_t dma = {.src = &sample, .length = sizeof(sample),
                          .transfer_size = ESY_DMA_TRANSFER_SIZE_16};
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_pwm_abort_dma(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_pwm_poll(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_pwm_abort_dma(&pwm));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_pwm_poll(&pwm));
  init_pwm(&pwm);
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_pwm_dma_output(&pwm, &dma, false));
  TEST_ASSERT_FALSE(fake_esy_hal_pwm_is_dma_active(&pwm));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_enable(&pwm, true));
  TEST_ASSERT_EQUAL(ESY_OK, pwm.ops->poll(&pwm));
  TEST_ASSERT_EQUAL(ESY_OK, pwm.ops->abort_dma(&pwm));
  TEST_ASSERT_TRUE(fake_esy_hal_pwm_is_enabled(&pwm));
  TEST_ASSERT_TRUE(fake_esy_hal_pwm_is_enabled(&pwm));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_deinit(&pwm));
}

void test_fake_pwm_finite_poll_retires_dma_after_callback_and_holds_output(void) {
  esy_pwm_t pwm = {0};
  uint16_t samples[2] = {1u, 2u};
  callback_probe_t probe = {.pwm = &pwm};
  esy_dma_config_t dma = {.src = samples, .length = sizeof(samples),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_16,
      .callback = inspect_callback, .callback_arg = &probe};
  init_pwm(&pwm);
  fake_esy_hal_pwm_set_dma_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_dma_output(&pwm, &dma, false));
  TEST_ASSERT_TRUE(fake_esy_hal_pwm_is_dma_active(&pwm));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_set_duty(&pwm, 0.25f));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_poll(&pwm));
  TEST_ASSERT_EQUAL_UINT(0u, probe.calls);
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_pwm_queue_dma_event(&pwm, false));
  TEST_ASSERT_EQUAL_UINT(0u, probe.calls);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_poll(&pwm));
  TEST_ASSERT_EQUAL_UINT(1u, probe.calls);
  TEST_ASSERT_FALSE(fake_esy_hal_pwm_is_dma_active(&pwm));
  TEST_ASSERT_TRUE(fake_esy_hal_pwm_is_enabled(&pwm));
  TEST_ASSERT_TRUE(fake_esy_hal_pwm_is_enabled(&pwm));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, fake_esy_hal_pwm_queue_dma_event(&pwm, false));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_poll(&pwm));
  TEST_ASSERT_EQUAL_UINT(1u, probe.calls);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_dma_output(&pwm, &dma, false));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_enable(&pwm, false));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_deinit(&pwm));
}

void test_fake_pwm_circular_notifications_repeat_and_error_wins(void) {
  esy_pwm_t pwm = {0};
  uint16_t sample = 2u;
  unsigned cycles = 0u, errors = 0u;
  esy_dma_config_t dma = {.src = &sample, .length = sizeof(sample),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_16, .circular = true,
      .callback = count_notification, .callback_arg = &cycles,
      .error_callback = count_notification, .error_callback_arg = &errors};
  init_pwm(&pwm);
  fake_esy_hal_pwm_set_dma_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_dma_output(&pwm, &dma, false));
  for (unsigned i = 0u; i < 2u; ++i) {
    TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_pwm_queue_dma_event(&pwm, false));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_poll(&pwm));
    TEST_ASSERT_TRUE(fake_esy_hal_pwm_is_dma_active(&pwm));
  }
  TEST_ASSERT_EQUAL_UINT(2u, cycles);
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_pwm_queue_dma_event(&pwm, true));
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_pwm_queue_dma_event(&pwm, false));
  fake_esy_hal_pwm_set_cleanup_results(ESY_ERR_BUSY, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_poll(&pwm));
  TEST_ASSERT_TRUE(fake_esy_hal_pwm_is_dma_active(&pwm));
  TEST_ASSERT_EQUAL_UINT(0u, errors);
  fake_esy_hal_pwm_set_cleanup_results(ESY_OK, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_poll(&pwm));
  TEST_ASSERT_EQUAL_UINT(1u, errors);
  TEST_ASSERT_EQUAL_UINT(2u, cycles);
  TEST_ASSERT_FALSE(fake_esy_hal_pwm_is_dma_active(&pwm));
  TEST_ASSERT_FALSE(fake_esy_hal_pwm_is_enabled(&pwm));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_deinit(&pwm));
}

void test_fake_pwm_failed_cleanup_retains_handle_and_abort_suppresses_callbacks(void) {
  esy_pwm_t pwm = {0};
  uint16_t sample = 2u;
  unsigned notifications = 0u;
  esy_dma_config_t dma = {.src = &sample, .length = sizeof(sample),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_16,
      .callback = count_notification, .callback_arg = &notifications};
  init_pwm(&pwm);
  fake_esy_hal_pwm_set_dma_result(ESY_ERR_IO);
  fake_esy_hal_pwm_set_cleanup_results(ESY_ERR_BUSY, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_dma_output(&pwm, &dma, false));
  TEST_ASSERT_TRUE(fake_esy_hal_pwm_is_dma_active(&pwm));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_pwm_deinit(&pwm));
  TEST_ASSERT_TRUE(fake_esy_hal_pwm_is_initialized(&pwm));
  TEST_ASSERT_NOT_NULL(pwm.ops);
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_pwm_queue_dma_event(&pwm, false));
  fake_esy_hal_pwm_set_cleanup_results(ESY_OK, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_abort_dma(&pwm));
  TEST_ASSERT_FALSE(fake_esy_hal_pwm_is_dma_active(&pwm));
  TEST_ASSERT_FALSE(fake_esy_hal_pwm_is_enabled(&pwm));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_poll(&pwm));
  TEST_ASSERT_EQUAL_UINT(0u, notifications);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_pwm_deinit(&pwm));
  TEST_ASSERT_TRUE(fake_esy_hal_pwm_is_initialized(&pwm));
  TEST_ASSERT_NOT_NULL(pwm.ops);
  fake_esy_hal_pwm_set_cleanup_results(ESY_OK, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_deinit(&pwm));
  TEST_ASSERT_NULL(pwm.ops);
}
