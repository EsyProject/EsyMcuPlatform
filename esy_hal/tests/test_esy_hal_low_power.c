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
