/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_middleware/ccd/esy_ccd_tcd1254.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static esy_ccd_tcd1254_config_t valid_config(void) {
  const esy_ccd_tcd1254_config_t config = {
      .tick_hz = 100000000u,
      .master_period_ticks = 50u,
      .icg_delay_ns = 5000u,
      .icg_to_sh_ns = 500u,
      .sh_high_ns = 1000u,
      .sh_period_ns = 10000u,
      .integration_ns = 100000u,
  };
  return config;
}

static void reject_without_plan_change(const esy_ccd_tcd1254_config_t *config,
                                       esy_err_t expected) {
  esy_ccd_tcd1254_plan_t plan;
  memset(&plan, 0xa5, sizeof(plan));
  const esy_ccd_tcd1254_plan_t original = plan;
  TEST_ASSERT_EQUAL(expected, esy_ccd_tcd1254_plan_compile(config, &plan));
  TEST_ASSERT_EQUAL_MEMORY(&original, &plan, sizeof(plan));
}

void test_tcd1254_geometry_accounts_for_every_output(void) {
  TEST_ASSERT_EQUAL_UINT32(2547u, ESY_CCD_TCD1254_LINE_SAMPLES);
  TEST_ASSERT_EQUAL_UINT32(32u, ESY_CCD_TCD1254_LEADING_SAMPLES);
  TEST_ASSERT_EQUAL_UINT32(2500u, ESY_CCD_TCD1254_ACTIVE_SAMPLES);
  TEST_ASSERT_EQUAL_UINT32(15u, ESY_CCD_TCD1254_TRAILING_SAMPLES);
  TEST_ASSERT_EQUAL_UINT32(ESY_CCD_TCD1254_LINE_SAMPLES,
      ESY_CCD_TCD1254_LEADING_SAMPLES + ESY_CCD_TCD1254_ACTIVE_SAMPLES +
      ESY_CCD_TCD1254_TRAILING_SAMPLES);
}

void test_tcd1254_compiles_complete_electronic_shutter_frame(void) {
  esy_ccd_tcd1254_config_t config = valid_config();
  esy_ccd_tcd1254_plan_t plan;
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_plan_compile(&config, &plan));
  TEST_ASSERT_EQUAL_UINT32(100000000u, plan.tick_hz);
  TEST_ASSERT_EQUAL_UINT32(50u, plan.master_period_ticks);
  TEST_ASSERT_EQUAL_UINT32(50u, plan.sh_rise_ticks);
  TEST_ASSERT_EQUAL_UINT32(100u, plan.sh_high_ticks);
  TEST_ASSERT_EQUAL_UINT32(650u, plan.icg_rise_ticks);
  TEST_ASSERT_EQUAL_UINT32(1000u, plan.sh_period_ticks);
  TEST_ASSERT_EQUAL_UINT32(10000u, plan.integration_ticks);
  TEST_ASSERT_EQUAL_UINT32(254700u, plan.readout_ticks);
  TEST_ASSERT_EQUAL_UINT32(246u, plan.purge_pulses);
  TEST_ASSERT_EQUAL_UINT32(256000u, plan.frame_period_ticks);
  TEST_ASSERT_TRUE(plan.frame_period_ticks >= plan.icg_rise_ticks + plan.readout_ticks);
  TEST_ASSERT_EQUAL_UINT32(plan.frame_period_ticks,
      plan.purge_pulses * plan.sh_period_ticks + plan.integration_ticks);
}

void test_tcd1254_quantization_preserves_pulse_minima_and_repeated_master_phase(void) {
  esy_ccd_tcd1254_config_t config = valid_config();
  config.icg_to_sh_ns = 101u;
  config.sh_high_ns = 1001u;
  config.icg_delay_ns = 1001u;
  config.sh_period_ns = 10001u;
  config.integration_ns = 10001u;
  esy_ccd_tcd1254_plan_t plan;
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_plan_compile(&config, &plan));
  TEST_ASSERT_EQUAL_UINT32(11u, plan.sh_rise_ticks);
  TEST_ASSERT_EQUAL_UINT32(101u, plan.sh_high_ticks);
  TEST_ASSERT_EQUAL_UINT32(250u, plan.icg_rise_ticks);
  TEST_ASSERT_EQUAL_UINT32(1050u, plan.sh_period_ticks);
  TEST_ASSERT_EQUAL_UINT32(1050u, plan.integration_ticks);
  TEST_ASSERT_EQUAL_UINT32(0u, plan.icg_rise_ticks % plan.master_period_ticks);
  TEST_ASSERT_EQUAL_UINT32(0u, plan.sh_period_ticks % plan.master_period_ticks);
  TEST_ASSERT_EQUAL_UINT32(0u, plan.integration_ticks % plan.master_period_ticks);
  TEST_ASSERT_EQUAL_UINT32(0u, plan.frame_period_ticks % plan.master_period_ticks);
  TEST_ASSERT_TRUE(plan.icg_rise_ticks - plan.sh_rise_ticks - plan.sh_high_ticks >= 101u);
}

void test_tcd1254_long_integration_needs_no_purge_pulses(void) {
  esy_ccd_tcd1254_config_t config = valid_config();
  config.integration_ns = 10000000u;
  esy_ccd_tcd1254_plan_t plan;
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_plan_compile(&config, &plan));
  TEST_ASSERT_EQUAL_UINT32(0u, plan.purge_pulses);
  TEST_ASSERT_EQUAL_UINT32(plan.integration_ticks, plan.frame_period_ticks);
}

void test_tcd1254_master_frequency_accepts_limits_and_rejects_outside_them(void) {
  esy_ccd_tcd1254_config_t config = valid_config();
  esy_ccd_tcd1254_plan_t plan;
  config.master_period_ticks = 250u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_plan_compile(&config, &plan));
  config.master_period_ticks = 252u;
  reject_without_plan_change(&config, ESY_ERR_INVALID_ARG);
  config.master_period_ticks = 24u;
  reject_without_plan_change(&config, ESY_ERR_INVALID_ARG);
  config.tick_hz = 96000000u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_plan_compile(&config, &plan));
}

void test_tcd1254_invalid_configuration_never_changes_plan(void) {
  esy_ccd_tcd1254_config_t config = valid_config();
  reject_without_plan_change(NULL, ESY_ERR_INVALID_ARG);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_ccd_tcd1254_plan_compile(&config, NULL));
  config.tick_hz = 0u;
  reject_without_plan_change(&config, ESY_ERR_INVALID_ARG);
  config = valid_config();
  config.master_period_ticks = 0u;
  reject_without_plan_change(&config, ESY_ERR_INVALID_ARG);
  config.master_period_ticks = 51u;
  reject_without_plan_change(&config, ESY_ERR_INVALID_ARG);
  config = valid_config();
  config.icg_delay_ns = 999u;
  reject_without_plan_change(&config, ESY_ERR_INVALID_ARG);
  config = valid_config();
  config.icg_to_sh_ns = 99u;
  reject_without_plan_change(&config, ESY_ERR_INVALID_ARG);
  config.icg_to_sh_ns = 1001u;
  reject_without_plan_change(&config, ESY_ERR_INVALID_ARG);
  config = valid_config();
  config.sh_high_ns = 999u;
  reject_without_plan_change(&config, ESY_ERR_INVALID_ARG);
  config = valid_config();
  config.sh_period_ns = 9999u;
  reject_without_plan_change(&config, ESY_ERR_INVALID_ARG);
  config = valid_config();
  config.integration_ns = 10000u;
  reject_without_plan_change(&config, ESY_ERR_INVALID_ARG);
}

void test_tcd1254_checks_actual_t2_after_clock_quantization(void) {
  esy_ccd_tcd1254_config_t config = valid_config();
  config.tick_hz = 800000u;
  config.master_period_ticks = 2u;
  config.icg_to_sh_ns = 100u;
  reject_without_plan_change(&config, ESY_ERR_NOT_SUPPORTED);
  config.tick_hz = 11000000u;
  config.icg_to_sh_ns = 999u;
  esy_ccd_tcd1254_plan_t plan;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_ccd_tcd1254_plan_compile(&config, &plan));
  config.master_period_ticks = 4u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_plan_compile(&config, &plan));
  TEST_ASSERT_EQUAL_UINT32(11u, plan.sh_rise_ticks);
  config.tick_hz = 11500000u;
  reject_without_plan_change(&config, ESY_ERR_NOT_SUPPORTED);
}

void test_tcd1254_rejects_overlapping_sh_pulses_and_purge_during_icg_low(void) {
  esy_ccd_tcd1254_config_t config = valid_config();
  config.sh_high_ns = config.sh_period_ns;
  reject_without_plan_change(&config, ESY_ERR_NOT_SUPPORTED);
  config = valid_config();
  config.integration_ns = 10001u;
  config.sh_high_ns = 10500u;
  config.sh_period_ns = 20000u;
  reject_without_plan_change(&config, ESY_ERR_NOT_SUPPORTED);
  config = valid_config();
  config.icg_delay_ns = 10000u;
  reject_without_plan_change(&config, ESY_ERR_NOT_SUPPORTED);
}

void test_tcd1254_large_inputs_are_checked_without_duration_wrap(void) {
  esy_ccd_tcd1254_config_t config = valid_config();
  config.tick_hz = UINT32_MAX;
  config.master_period_ticks = 2000u;
  config.integration_ns = UINT32_MAX;
  reject_without_plan_change(&config, ESY_ERR_NOT_SUPPORTED);
  config.sh_high_ns = UINT32_MAX;
  config.icg_delay_ns = UINT32_MAX;
  config.sh_period_ns = UINT32_MAX;
  reject_without_plan_change(&config, ESY_ERR_NOT_SUPPORTED);
  config.master_period_ticks = UINT32_MAX - 1u;
  reject_without_plan_change(&config, ESY_ERR_INVALID_ARG);
}

void test_tcd1254_only_confirmed_complete_lines_count_toward_startup(void) {
  esy_ccd_tcd1254_t sensor = {0};
  const esy_ccd_tcd1254_config_t config = valid_config();
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_init(&sensor, &config));
  for (uint32_t i = 0u; i < ESY_CCD_TCD1254_STARTUP_READOUTS + 2u; ++i) {
    esy_ccd_tcd1254_readout_t readout;
    bool usable = true;
    TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_begin(&sensor, &readout));
    TEST_ASSERT_EQUAL_UINT64((uint64_t)i + 1u, readout.token);
    TEST_ASSERT_EQUAL(i < ESY_CCD_TCD1254_STARTUP_READOUTS, readout.discard);
    TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_complete(
        &sensor, readout.token, ESY_CCD_TCD1254_LINE_SAMPLES, &usable));
    TEST_ASSERT_EQUAL(i >= ESY_CCD_TCD1254_STARTUP_READOUTS, usable);
  }
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_deinit(&sensor));
}

void test_tcd1254_pending_readout_rejects_reentry_and_deinit_without_mutation(void) {
  esy_ccd_tcd1254_t sensor = {0};
  const esy_ccd_tcd1254_config_t config = valid_config();
  esy_ccd_tcd1254_readout_t readout;
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_init(&sensor, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_begin(&sensor, &readout));
  const esy_ccd_tcd1254_t original = sensor;
  esy_ccd_tcd1254_readout_t unchanged = readout;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_ccd_tcd1254_begin(&sensor, &unchanged));
  TEST_ASSERT_EQUAL_MEMORY(&readout, &unchanged, sizeof(readout));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_ccd_tcd1254_deinit(&sensor));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_ccd_tcd1254_init(&sensor, &config));
  TEST_ASSERT_EQUAL_MEMORY(&original, &sensor, sizeof(sensor));
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_cancel(&sensor, readout.token));
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_deinit(&sensor));
}

void test_tcd1254_completed_token_cannot_complete_again_or_a_new_operation(void) {
  esy_ccd_tcd1254_t sensor = {0};
  const esy_ccd_tcd1254_config_t config = valid_config();
  esy_ccd_tcd1254_readout_t first;
  esy_ccd_tcd1254_readout_t second;
  bool usable = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_init(&sensor, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_begin(&sensor, &first));
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_complete(
      &sensor, first.token, ESY_CCD_TCD1254_LINE_SAMPLES, &usable));
  usable = true;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_ccd_tcd1254_complete(
      &sensor, first.token, ESY_CCD_TCD1254_LINE_SAMPLES, &usable));
  TEST_ASSERT_TRUE(usable);
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_begin(&sensor, &second));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_ccd_tcd1254_complete(
      &sensor, first.token, ESY_CCD_TCD1254_LINE_SAMPLES, &usable));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_ccd_tcd1254_cancel(&sensor, first.token));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_ccd_tcd1254_cancel(&sensor, 0u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_cancel(&sensor, second.token));
}

void test_tcd1254_cancelled_readout_cannot_reactivate_and_restarts_startup(void) {
  esy_ccd_tcd1254_t sensor = {0};
  const esy_ccd_tcd1254_config_t config = valid_config();
  esy_ccd_tcd1254_readout_t readout;
  bool usable = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_init(&sensor, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_begin(&sensor, &readout));
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_complete(
      &sensor, readout.token, ESY_CCD_TCD1254_LINE_SAMPLES, &usable));
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_begin(&sensor, &readout));
  const uint64_t cancelled = readout.token;
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_cancel(&sensor, cancelled));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_ccd_tcd1254_cancel(&sensor, cancelled));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_ccd_tcd1254_complete(
      &sensor, cancelled, ESY_CCD_TCD1254_LINE_SAMPLES, &usable));
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_begin(&sensor, &readout));
  TEST_ASSERT_TRUE(readout.discard);
  TEST_ASSERT_EQUAL_UINT32(0u, sensor.startup_readouts);
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_cancel(&sensor, readout.token));
}

void test_tcd1254_wrong_length_consumes_token_and_invalidates_startup(void) {
  esy_ccd_tcd1254_t sensor = {0};
  const esy_ccd_tcd1254_config_t config = valid_config();
  esy_ccd_tcd1254_readout_t readout;
  bool usable = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_init(&sensor, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_begin(&sensor, &readout));
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_ccd_tcd1254_complete(
      &sensor, readout.token, ESY_CCD_TCD1254_ACTIVE_SAMPLES, &usable));
  TEST_ASSERT_TRUE(usable);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_ccd_tcd1254_complete(
      &sensor, readout.token, ESY_CCD_TCD1254_LINE_SAMPLES, &usable));
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_begin(&sensor, &readout));
  TEST_ASSERT_TRUE(readout.discard);
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_cancel(&sensor, readout.token));
}

void test_tcd1254_deinit_reinit_preserves_token_identity_and_restarts_startup(void) {
  esy_ccd_tcd1254_t sensor = {0};
  const esy_ccd_tcd1254_config_t config = valid_config();
  esy_ccd_tcd1254_readout_t readout;
  bool usable = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_init(&sensor, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_begin(&sensor, &readout));
  const uint64_t old = readout.token;
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_complete(
      &sensor, old, ESY_CCD_TCD1254_LINE_SAMPLES, &usable));
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_deinit(&sensor));
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_deinit(&sensor));
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_init(&sensor, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_begin(&sensor, &readout));
  TEST_ASSERT_TRUE(readout.discard);
  TEST_ASSERT_EQUAL_UINT64(old + 1u, readout.token);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_ccd_tcd1254_complete(
      &sensor, old, ESY_CCD_TCD1254_LINE_SAMPLES, &usable));
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_cancel(&sensor, readout.token));
}

void test_tcd1254_token_counter_exhaustion_never_wraps(void) {
  esy_ccd_tcd1254_t sensor = {0};
  const esy_ccd_tcd1254_config_t config = valid_config();
  esy_ccd_tcd1254_readout_t readout;
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_init(&sensor, &config));
  /* Model the boundary after UINT64_MAX-1 previously issued operations. */
  sensor.sequence = UINT64_MAX - 1u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_begin(&sensor, &readout));
  TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, readout.token);
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_cancel(&sensor, readout.token));
  const esy_ccd_tcd1254_readout_t original = readout;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_ccd_tcd1254_begin(&sensor, &readout));
  TEST_ASSERT_EQUAL_MEMORY(&original, &readout, sizeof(readout));
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_deinit(&sensor));
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_init(&sensor, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_ccd_tcd1254_begin(&sensor, &readout));
}

void test_tcd1254_invalid_lifecycle_arguments_and_failed_init_are_reusable(void) {
  esy_ccd_tcd1254_t sensor = {0};
  esy_ccd_tcd1254_config_t config = valid_config();
  esy_ccd_tcd1254_readout_t readout;
  bool usable = true;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_ccd_tcd1254_init(NULL, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_ccd_tcd1254_init(&sensor, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_ccd_tcd1254_begin(NULL, &readout));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_ccd_tcd1254_begin(&sensor, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_ccd_tcd1254_complete(NULL, 1u, 2547u, &usable));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_ccd_tcd1254_complete(&sensor, 1u, 2547u, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_ccd_tcd1254_cancel(NULL, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_ccd_tcd1254_deinit(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_ccd_tcd1254_begin(&sensor, &readout));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_ccd_tcd1254_complete(&sensor, 1u, 2547u, &usable));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_ccd_tcd1254_cancel(&sensor, 1u));
  config.icg_to_sh_ns = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_ccd_tcd1254_init(&sensor, &config));
  const esy_ccd_tcd1254_t zero = {0};
  TEST_ASSERT_EQUAL_MEMORY(&zero, &sensor, sizeof(sensor));
  config = valid_config();
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_init(&sensor, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_begin(&sensor, &readout));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_ccd_tcd1254_complete(&sensor, readout.token, 2547u, NULL));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_ccd_tcd1254_complete(&sensor, readout.token, 2547u, &usable));
  TEST_ASSERT_EQUAL(ESY_OK, esy_ccd_tcd1254_deinit(&sensor));
}
