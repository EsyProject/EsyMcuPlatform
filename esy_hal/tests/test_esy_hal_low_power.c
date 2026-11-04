/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_low_power.h"

#include "esy_hal_backend/esy_hb_low_power.h"
#include "hal_test_fakes.h"
#include "unity.h"

void setUp(void) { platform_test_reset(); }

void tearDown(void) {}

void test_capabilities_are_reported_by_complete_driver_lifecycle(void) {
  esy_power_capabilities_t capabilities;

  platform_test_set_retention_support(false, ESY_OK);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_power_get_capabilities(&capabilities));
  TEST_ASSERT_BITS_HIGH(ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
                            ESY_POWER_STATE_MASK(ESY_POWER_STATE_CPU_IDLE),
                        capabilities.supported_state_mask);
  TEST_ASSERT_BITS_LOW(ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION),
                       capabilities.supported_state_mask);

  platform_test_set_retention_support(true, ESY_OK);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_power_get_capabilities(&capabilities));
  TEST_ASSERT_BITS_HIGH(ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION),
                        capabilities.supported_state_mask);
  TEST_ASSERT_EQUAL_UINT64(
      20u, capabilities.states[ESY_POWER_STATE_RETENTION].exit_latency_us);
  TEST_ASSERT_EQUAL_UINT64(
      30u, capabilities.states[ESY_POWER_STATE_RETENTION].min_residency_us);
}

void test_retention_transaction_restores_clock_inside_platform_port(void) {
  esy_hal_power_transaction_t transaction;
  esy_hal_power_result_t result;

  esy_hal_power_transaction_init(&transaction);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_power_transaction_begin(
                                      &transaction, ESY_POWER_STATE_RETENTION));
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->retention_prepare_calls);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_power_transaction_enter_atomic(&transaction));
  TEST_ASSERT_EQUAL_UINT32(2u, platform_test_state()->last_count);
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->retention_restore_calls);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_power_transaction_complete(&transaction, &result));
  TEST_ASSERT_TRUE(result.entered);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, result.entry_status);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, result.fatal_error);
}

void test_clock_restore_failure_is_fatal_and_cannot_complete(void) {
  esy_hal_power_transaction_t transaction;
  esy_hal_power_result_t result;

  platform_test_set_retention_support(true, ESY_ERR_IO);
  esy_hal_power_transaction_init(&transaction);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_power_transaction_begin(
                                      &transaction, ESY_POWER_STATE_RETENTION));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_IO, esy_hal_power_transaction_enter_atomic(&transaction));
  TEST_ASSERT_TRUE(transaction.result.entered);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, transaction.result.fatal_error);
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE,
      esy_hal_power_transaction_complete(&transaction, &result));
}

void test_driver_entry_failure_is_observable_and_completable(void) {
  esy_hal_power_transaction_t transaction;
  esy_hal_power_result_t result;

  platform_test_set_low_power_result(ESY_ERR_IO);
  esy_hal_power_transaction_init(&transaction);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_power_transaction_begin(
                                      &transaction, ESY_POWER_STATE_CPU_IDLE));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_IO, esy_hal_power_transaction_enter_atomic(&transaction));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_power_transaction_complete(&transaction, &result));
  TEST_ASSERT_FALSE(result.entered);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, result.entry_status);
}

void test_retention_entry_failure_cancels_prepared_snapshot_once(void) {
  esy_hal_power_transaction_t transaction;
  esy_hal_power_result_t result;

  platform_test_set_low_power_result(ESY_ERR_IO);
  esy_hal_power_transaction_init(&transaction);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_power_transaction_begin(
                                      &transaction, ESY_POWER_STATE_RETENTION));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_IO, esy_hal_power_transaction_enter_atomic(&transaction));
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->retention_prepare_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->retention_cancel_calls);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_power_transaction_complete(&transaction, &result));
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->retention_cancel_calls);
}

void test_transaction_rejects_repeated_and_mismatched_lifecycle_calls(void) {
  esy_hal_power_transaction_t transaction;
  esy_hal_power_result_t result;

  esy_hal_power_transaction_init(&transaction);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_power_transaction_begin(
                                      &transaction, ESY_POWER_STATE_CPU_IDLE));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_hal_power_transaction_begin(
                              &transaction, ESY_POWER_STATE_CPU_IDLE));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE,
      esy_hal_power_transaction_complete(&transaction, &result));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_power_transaction_abort(&transaction));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_hal_power_transaction_abort(&transaction));
}

void test_explicit_shutdown_is_not_an_idle_state(void) {
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_shutdown(ESY_HAL_SHUTDOWN_MODE_STANDBY));
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->shutdown_calls);
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_hal_shutdown(ESY_HAL_SHUTDOWN_MODE_COUNT));
}

void test_explicit_sleep_preserves_provider_wake_and_elapsed_evidence(void) {
  const esy_power_sleep_config_t config = {
      .state = ESY_POWER_STATE_RETENTION, .duration_us = UINT32_MAX};
  esy_power_sleep_result_t result = {0};
  platform_test_set_explicit_sleep_result((esy_power_sleep_result_t){
      .elapsed_us = (uint64_t)UINT32_MAX + 100u,
      .entered = true,
      .gpio_wakeup = true,
  });
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_power_sleep(&config, &result));
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->explicit_sleep_calls);
  TEST_ASSERT_EQUAL(ESY_POWER_STATE_RETENTION,
      platform_test_state()->explicit_sleep_config.state);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX,
      platform_test_state()->explicit_sleep_config.duration_us);
  TEST_ASSERT_TRUE(result.entered);
  TEST_ASSERT_TRUE(result.gpio_wakeup);
  TEST_ASSERT_FALSE(result.timer_wakeup);
  TEST_ASSERT_EQUAL_UINT64((uint64_t)UINT32_MAX + 100u, result.elapsed_us);
  /* Explicit provider entry does not claim a generic atomic transaction. */
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_prepare_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_start_calls);
}

void test_explicit_sleep_failure_keeps_actual_entry_evidence_for_recovery(void) {
  const esy_power_sleep_config_t config = {
      .state = ESY_POWER_STATE_RETENTION, .duration_us = 10000u};
  esy_power_sleep_result_t result = {0};
  platform_test_set_explicit_sleep_result((esy_power_sleep_result_t){
      .elapsed_us = 12000u, .entered = true, .timer_wakeup = true});
  platform_test_set_low_power_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_power_sleep(&config, &result));
  TEST_ASSERT_TRUE(result.entered);
  TEST_ASSERT_TRUE(result.timer_wakeup);
  TEST_ASSERT_EQUAL_UINT64(12000u, result.elapsed_us);
}

void test_explicit_sleep_rejects_invalid_requests_before_native_side_effects(void) {
  esy_power_sleep_config_t config = {
      .state = ESY_POWER_STATE_RETENTION, .duration_us = 1u};
  esy_power_sleep_result_t result = {.elapsed_us = 42u, .entered = true};
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_power_sleep(NULL, &result));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_power_sleep(&config, NULL));
  config.duration_us = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_power_sleep(&config, &result));
  config.duration_us = 1u;
  config.state = ESY_POWER_STATE_CPU_IDLE;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_power_sleep(&config, &result));
  config.state = ESY_POWER_STATE_ACTIVE;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_power_sleep(&config, &result));
  config.state = ESY_POWER_STATE_COUNT;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_power_sleep(&config, &result));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->explicit_sleep_calls);
  TEST_ASSERT_EQUAL_UINT64(42u, result.elapsed_us);
  TEST_ASSERT_TRUE(result.entered);
}

void test_explicit_sleep_unsupported_provider_and_cleanup_only_result_are_visible(void) {
  const esy_power_sleep_config_t config = {
      .state = ESY_POWER_STATE_RETENTION, .duration_us = 1u};
  esy_power_sleep_result_t result = {.entered = true};
  platform_test_set_low_power_result(ESY_ERR_NOT_SUPPORTED);
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED,
      esy_hal_power_sleep(&config, &result));
  TEST_ASSERT_FALSE(result.entered);
  platform_test_set_low_power_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_power_sleep(&config, &result));
  TEST_ASSERT_FALSE(result.entered);
  TEST_ASSERT_EQUAL_UINT64(0u, result.elapsed_us);
}

void test_transaction_rejects_unsupported_state_and_invalid_arguments(void) {
  esy_hal_power_transaction_t transaction;

  esy_hal_power_transaction_init(&transaction);
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_hal_power_transaction_begin(NULL, ESY_POWER_STATE_CPU_IDLE));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_power_transaction_begin(
                              &transaction, ESY_POWER_STATE_ACTIVE));
  platform_test_set_retention_support(false, ESY_OK);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                          esy_hal_power_transaction_begin(
                              &transaction, ESY_POWER_STATE_RETENTION));
}
