/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_freq_synth_internal.h"

#include "esy_middleware/freq_synth/esy_adf4351.h"
#include "esy_middleware/freq_synth/esy_adf4351_device.h"

#include <stdint.h>
#include <string.h>

#include "fake_esy_hal_gpio.h"
#include "fake_esy_hal_spi.h"
#include "fake_esy_hal_system.h"
#include "unity.h"

#define TEST_ADF_LE_PIN            5u
#define TEST_ADF_CE_PIN            6u
#define TEST_ADF_FREQ_TOLERANCE_HZ 25000ULL

typedef struct {
  esy_spi_t spi;
  esy_gpio_t le_gpio;
} freq_synth_test_ctx_t;

static freq_synth_test_ctx_t s_ctx;

static esy_err_t test_parent_pm_action(esy_device_t *device,
                                       esy_device_pm_action_t action,
                                       esy_power_state_t state) {
  (void)device;
  (void)action;
  (void)state;
  return ESY_OK;
}

static const esy_device_ops_t k_test_parent_ops = {
    .pm_action = test_parent_pm_action,
};

static int32_t test_pm_spi_write(esy_spi_t *spi, const uint8_t *data,
                                 size_t length) {
  (void)spi;
  (void)data;
  return (int32_t)length;
}

static void test_init_gpio(esy_gpio_t *gpio, uint8_t pin, bool value) {
  esy_gpio_config_t config;

  if (esy_hal_gpio_is_initialized(gpio)) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(gpio));
  }

  esy_hal_gpio_default_cfg(&config);
  config.pin = pin;
  config.mode = ESY_GPIO_MODE_OUTPUT;
  config.output_value = value;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_gpio_init(gpio, &config));
}

static uint32_t test_reg_from_spi_write(esy_spi_t *spi, size_t index) {
  uint8_t bytes[4];

  TEST_ASSERT_EQUAL_UINT32(
      sizeof(bytes),
      fake_esy_hal_spi_copy_tx_at(spi, index, bytes, sizeof(bytes)));

  return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
         ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static uint32_t test_find_written_reg(esy_spi_t *spi, uint8_t reg_id) {
  const size_t count = fake_esy_hal_spi_get_tx_count(spi);

  for (size_t i = 0u; i < count; i++) {
    const uint32_t reg = test_reg_from_spi_write(spi, i);
    if ((reg & 0x07u) == reg_id) {
      return reg;
    }
  }

  TEST_FAIL_MESSAGE("register id was not written");
  return 0u;
}

static void test_fill_default_config(esy_adf4351_config_t *cfg) {
  esy_adf4351_default_config(cfg);
  cfg->spi = &s_ctx.spi;
  test_init_gpio(&s_ctx.le_gpio, TEST_ADF_LE_PIN, false);
  cfg->le_gpio = &s_ctx.le_gpio;
  cfg->ce_gpio = NULL;
}

static uint64_t test_get_info_u64(esy_freq_synth_t *synth,
                                  esy_freq_synth_info_id_t id) {
  esy_freq_synth_info_value_t value;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_freq_synth_get_info(synth, id, &value));
  return value.u64;
}

static uint16_t test_get_info_u16(esy_freq_synth_t *synth,
                                  esy_freq_synth_info_id_t id) {
  esy_freq_synth_info_value_t value;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_freq_synth_get_info(synth, id, &value));
  return value.u16;
}

static uint8_t test_get_info_u8(esy_freq_synth_t *synth,
                                esy_freq_synth_info_id_t id) {
  esy_freq_synth_info_value_t value;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_freq_synth_get_info(synth, id, &value));
  return value.u8;
}

static void test_assert_actual_frequency_close(esy_freq_synth_t *synth,
                                               uint64_t requested_hz) {
  const uint64_t actual_hz =
      test_get_info_u64(synth, ESY_FREQ_SYNTH_INFO_ACTUAL_HZ);
  const uint64_t delta_hz = (actual_hz >= requested_hz)
                                ? (actual_hz - requested_hz)
                                : (requested_hz - actual_hz);

  TEST_ASSERT_LESS_OR_EQUAL_UINT64(TEST_ADF_FREQ_TOLERANCE_HZ, delta_hz);
}

static void test_assert_pll_info_valid(esy_freq_synth_t *synth) {
  const uint64_t pfd_hz = test_get_info_u64(synth, ESY_FREQ_SYNTH_INFO_PFD_HZ);
  const uint64_t spacing_hz =
      test_get_info_u64(synth, ESY_FREQ_SYNTH_INFO_CHANNEL_SPACING_HZ);
  const uint16_t mod = test_get_info_u16(synth, ESY_FREQ_SYNTH_INFO_MOD);
  const uint16_t frac = test_get_info_u16(synth, ESY_FREQ_SYNTH_INFO_FRAC);
  const uint16_t intn = test_get_info_u16(synth, ESY_FREQ_SYNTH_INFO_INT);

  TEST_ASSERT_GREATER_THAN_UINT64(0u, pfd_hz);
  TEST_ASSERT_LESS_OR_EQUAL_UINT64(32000000u, pfd_hz);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT16(2u, mod);
  TEST_ASSERT_LESS_OR_EQUAL_UINT16(4095u, mod);
  TEST_ASSERT_LESS_THAN_UINT16(mod, frac);
  TEST_ASSERT_GREATER_THAN_UINT16(0u, intn);
  TEST_ASSERT_GREATER_THAN_UINT64(0u, spacing_hz);
}

void setUp(void) {
  memset(&s_ctx.spi, 0, sizeof(s_ctx.spi));
  memset(&s_ctx.le_gpio, 0, sizeof(s_ctx.le_gpio));
  fake_esy_hal_gpio_reset();
  fake_esy_hal_spi_reset();
  fake_esy_hal_system_reset();
}

void tearDown(void) {}

void test_adf4351_init_programs_registers_in_r5_to_r0_order(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4351_config_t cfg;

  test_fill_default_config(&cfg);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4351_init(&synth, &cfg));

  TEST_ASSERT_EQUAL_UINT32(6u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  TEST_ASSERT_EQUAL_UINT32(5u, test_reg_from_spi_write(&s_ctx.spi, 0u) & 0x07u);
  TEST_ASSERT_EQUAL_UINT32(4u, test_reg_from_spi_write(&s_ctx.spi, 1u) & 0x07u);
  TEST_ASSERT_EQUAL_UINT32(3u, test_reg_from_spi_write(&s_ctx.spi, 2u) & 0x07u);
  TEST_ASSERT_EQUAL_UINT32(2u, test_reg_from_spi_write(&s_ctx.spi, 3u) & 0x07u);
  TEST_ASSERT_EQUAL_UINT32(1u, test_reg_from_spi_write(&s_ctx.spi, 4u) & 0x07u);
  TEST_ASSERT_EQUAL_UINT32(0u, test_reg_from_spi_write(&s_ctx.spi, 5u) & 0x07u);
}

void test_adf4351_default_init_keeps_main_output_disabled(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4351_config_t cfg;

  test_fill_default_config(&cfg);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4351_init(&synth, &cfg));

  const uint32_t r4 = test_find_written_reg(&s_ctx.spi, 4u);
  TEST_ASSERT_EQUAL_UINT32(0u, (r4 >> 5) & 0x01u);
  TEST_ASSERT_EQUAL_UINT8(
      0u, test_get_info_u8(&synth, ESY_FREQ_SYNTH_INFO_OUTPUT_ENABLED));
  TEST_ASSERT_EQUAL_UINT8(
      0u, test_get_info_u8(&synth, ESY_FREQ_SYNTH_INFO_POWER_LEVEL));
}

void test_adf4351_enabled_output_prevents_runtime_suspend(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 2u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_config_t registry_config;
  esy_device_registry_t registry = {0};
  esy_device_t spi_device = {0};
  esy_device_config_t spi_device_config = {
      .name = "spi-parent",
      .ops = &k_test_parent_ops,
      .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
                              ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION),
  };
  esy_device_t *dependencies[] = {&spi_device};
  esy_freq_synth_device_config_t synth_device_config = {
      .registration = {
          .name = "adf4351",
          .dependencies = dependencies,
          .dependency_count = 1u,
          .supported_state_mask =
              ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
              ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION),
      },
  };
  esy_freq_synth_device_binding_t binding = {0};
  esy_freq_synth_t synth = {0};
  esy_adf4351_config_t config;
  esy_freq_synth_info_value_t value;
  esy_device_state_t state;

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  registry_config.static_resources = &resources;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_register(&registry, &spi_device, &spi_device_config));
  fake_esy_hal_spi_set_device(&s_ctx.spi, &spi_device);
  fake_esy_hal_spi_set_pm_write_hook(&s_ctx.spi, test_pm_spi_write);
  test_fill_default_config(&config);
  config.initial_output_enabled = true;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4351_init(&synth, &config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_adf4351_register_device(&binding, &synth, &registry,
                                          &synth_device_config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&registry));

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_ACTIVE, state);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_freq_synth_set_frequency_hz(&synth, 2500000000ULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_ACTIVE, state);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_freq_synth_get_info(
                  &synth, ESY_FREQ_SYNTH_INFO_OUTPUT_ENABLED, &value));
  TEST_ASSERT_EQUAL_UINT8(1u, value.u8);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_freq_synth_set_output_enable(
                                      &synth, ESY_FREQ_SYNTH_PORT_MAIN, false));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_RUNTIME_SUSPENDED, state);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_get(&binding.device));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_ACTIVE, state);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_put(&binding.device));
  esy_adf4351_deinit(&synth);
  TEST_ASSERT_TRUE(esy_freq_synth_is_attached(&synth));
}

void test_adf4351_power_change_does_not_reenable_disabled_output(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4351_config_t cfg;

  test_fill_default_config(&cfg);
  cfg.initial_output_enabled = true;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4351_init(&synth, &cfg));

  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_freq_synth_set_output_enable(
                                      &synth, ESY_FREQ_SYNTH_PORT_MAIN, false));
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  TEST_ASSERT_EQUAL_UINT32(0u,
                           (test_reg_from_spi_write(&s_ctx.spi, 0u) >> 5) & 0x01u);

  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_freq_synth_set_power_level(
                                      &synth, ESY_FREQ_SYNTH_PORT_MAIN, 2u));

  const uint32_t r4 = test_find_written_reg(&s_ctx.spi, 4u);
  TEST_ASSERT_EQUAL_UINT32(0u, (r4 >> 5) & 0x01u);
  TEST_ASSERT_EQUAL_UINT32(2u, (r4 >> 3) & 0x03u);
  TEST_ASSERT_EQUAL_UINT8(
      0u, test_get_info_u8(&synth, ESY_FREQ_SYNTH_INFO_OUTPUT_ENABLED));
  TEST_ASSERT_EQUAL_UINT8(
      2u, test_get_info_u8(&synth, ESY_FREQ_SYNTH_INFO_POWER_LEVEL));
}

void test_adf4351_failed_output_disable_keeps_previous_output_state(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4351_config_t cfg;

  test_fill_default_config(&cfg);
  cfg.initial_output_enabled = true;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4351_init(&synth, &cfg));

  fake_esy_hal_spi_set_write_blocking_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO,
                          esy_freq_synth_set_output_enable(
                              &synth, ESY_FREQ_SYNTH_PORT_MAIN, false));

  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_freq_synth_set_power_level(
                                      &synth, ESY_FREQ_SYNTH_PORT_MAIN, 2u));

  const uint32_t r4 = test_find_written_reg(&s_ctx.spi, 4u);
  TEST_ASSERT_EQUAL_UINT32(1u, (r4 >> 5) & 0x01u);
  TEST_ASSERT_EQUAL_UINT32(2u, (r4 >> 3) & 0x03u);
}

void test_adf4351_init_rejects_invalid_configuration_before_spi_write(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4351_config_t cfg;

  test_fill_default_config(&cfg);
  cfg.channel_step_hz = 0u;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_adf4351_init(&synth, &cfg));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));

  test_fill_default_config(&cfg);
  cfg.le_gpio = NULL;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_adf4351_init(&synth, &cfg));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));

  test_fill_default_config(&cfg);
  cfg.muxout = (esy_adf4351_muxout_t)7u;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_adf4351_init(&synth, &cfg));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
}

void test_adf4351_init_failure_retains_handle_until_cleanup_succeeds(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4351_config_t cfg;

  test_fill_default_config(&cfg);
  fake_esy_hal_spi_set_write_blocking_result(ESY_ERR_IO);

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_adf4351_init(&synth, &cfg));
  TEST_ASSERT_TRUE(esy_freq_synth_is_attached(&synth));
  TEST_ASSERT_TRUE(esy_freq_synth_runtime(&synth)->faulted);

  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE, esy_freq_synth_set_frequency_hz(&synth, 3600000000ULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4351_deinit(&synth));
  TEST_ASSERT_FALSE(esy_freq_synth_is_attached(&synth));
}

void test_adf4351_set_frequency_failure_keeps_cached_pll_state(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4351_config_t cfg;

  test_fill_default_config(&cfg);
  cfg.initial_freq_hz = 2200000000ULL;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4351_init(&synth, &cfg));

  const uint64_t old_actual_hz =
      test_get_info_u64(&synth, ESY_FREQ_SYNTH_INFO_ACTUAL_HZ);
  const uint64_t old_pfd_hz =
      test_get_info_u64(&synth, ESY_FREQ_SYNTH_INFO_PFD_HZ);
  const uint64_t old_spacing_hz =
      test_get_info_u64(&synth, ESY_FREQ_SYNTH_INFO_CHANNEL_SPACING_HZ);
  const uint16_t old_mod = test_get_info_u16(&synth, ESY_FREQ_SYNTH_INFO_MOD);
  const uint16_t old_frac = test_get_info_u16(&synth, ESY_FREQ_SYNTH_INFO_FRAC);
  const uint16_t old_intn = test_get_info_u16(&synth, ESY_FREQ_SYNTH_INFO_INT);

  fake_esy_hal_spi_set_write_blocking_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_IO, esy_freq_synth_set_frequency_hz(&synth, 3600000000ULL));

  TEST_ASSERT_EQUAL_UINT64(
      old_actual_hz, test_get_info_u64(&synth, ESY_FREQ_SYNTH_INFO_ACTUAL_HZ));
  TEST_ASSERT_EQUAL_UINT64(
      old_pfd_hz, test_get_info_u64(&synth, ESY_FREQ_SYNTH_INFO_PFD_HZ));
  TEST_ASSERT_EQUAL_UINT64(
      old_spacing_hz,
      test_get_info_u64(&synth, ESY_FREQ_SYNTH_INFO_CHANNEL_SPACING_HZ));
  TEST_ASSERT_EQUAL_UINT16(old_mod,
                           test_get_info_u16(&synth, ESY_FREQ_SYNTH_INFO_MOD));
  TEST_ASSERT_EQUAL_UINT16(old_frac,
                           test_get_info_u16(&synth, ESY_FREQ_SYNTH_INFO_FRAC));
  TEST_ASSERT_EQUAL_UINT16(old_intn,
                           test_get_info_u16(&synth, ESY_FREQ_SYNTH_INFO_INT));
}

void test_adf4351_reports_rf_divider_selector(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4351_config_t cfg;

  test_fill_default_config(&cfg);
  cfg.initial_freq_hz = 35000000ULL;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4351_init(&synth, &cfg));
  TEST_ASSERT_EQUAL_UINT8(
      6u, test_get_info_u8(&synth, ESY_FREQ_SYNTH_INFO_RF_DIV_SEL));

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_freq_synth_set_frequency_hz(&synth, 2200000000ULL));
  TEST_ASSERT_EQUAL_UINT8(
      0u, test_get_info_u8(&synth, ESY_FREQ_SYNTH_INFO_RF_DIV_SEL));
}

void test_adf4351_rejects_unsupported_ports(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4351_config_t cfg;

  test_fill_default_config(&cfg);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4351_init(&synth, &cfg));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_NOT_SUPPORTED,
      esy_freq_synth_set_output_enable(&synth, ESY_FREQ_SYNTH_PORT_AUX, true));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_NOT_SUPPORTED,
      esy_freq_synth_set_power_level(&synth, ESY_FREQ_SYNTH_PORT_AUX, 1u));
}

void test_adf4351_rejects_invalid_power_level(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4351_config_t cfg;

  test_fill_default_config(&cfg);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4351_init(&synth, &cfg));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_freq_synth_set_power_level(&synth, ESY_FREQ_SYNTH_PORT_MAIN, 4u));
}

void test_adf4351_fast_solver_handles_boundary_frequencies(void) {
  static const uint64_t k_freqs_hz[] = {
      35000000ULL,
      2200000000ULL,
      3600000000ULL,
      4400000000ULL,
  };

  for (size_t i = 0u; i < (sizeof(k_freqs_hz) / sizeof(k_freqs_hz[0])); i++) {
    esy_freq_synth_t synth = {0};
    esy_adf4351_config_t cfg;

    test_fill_default_config(&cfg);
    cfg.solve_mode = ESY_ADF4351_SOLVE_MODE_FAST;
    cfg.initial_freq_hz = k_freqs_hz[i];

    TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4351_init(&synth, &cfg));
    TEST_ASSERT_EQUAL_UINT64(
        k_freqs_hz[i],
        test_get_info_u64(&synth, ESY_FREQ_SYNTH_INFO_REQUESTED_HZ));
    test_assert_actual_frequency_close(&synth, k_freqs_hz[i]);
    test_assert_pll_info_valid(&synth);
  }
}

void test_adf4351_precise_solver_handles_boundary_frequencies(void) {
  static const uint64_t k_freqs_hz[] = {
      35000000ULL,
      2200000000ULL,
      3600000000ULL,
      4400000000ULL,
  };

  for (size_t i = 0u; i < (sizeof(k_freqs_hz) / sizeof(k_freqs_hz[0])); i++) {
    esy_freq_synth_t synth = {0};
    esy_adf4351_config_t cfg;

    test_fill_default_config(&cfg);
    cfg.solve_mode = ESY_ADF4351_SOLVE_MODE_PRECISE;
    cfg.initial_freq_hz = k_freqs_hz[i];

    TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4351_init(&synth, &cfg));
    TEST_ASSERT_EQUAL_UINT64(
        k_freqs_hz[i],
        test_get_info_u64(&synth, ESY_FREQ_SYNTH_INFO_REQUESTED_HZ));
    test_assert_actual_frequency_close(&synth, k_freqs_hz[i]);
    test_assert_pll_info_valid(&synth);
  }
}

void test_adf4351_mod_only_change_commits_with_r0_last(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4351_config_t cfg;
  test_fill_default_config(&cfg);
  cfg.solve_mode = ESY_ADF4351_SOLVE_MODE_PRECISE;
  cfg.initial_freq_hz = 2600022000ULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4351_init(&synth, &cfg));
  const uint32_t old_r0 = test_find_written_reg(&s_ctx.spi, 0u);
  const uint32_t old_r1 = test_find_written_reg(&s_ctx.spi, 1u);
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_frequency_hz(&synth, 2600023000ULL));
  TEST_ASSERT_NOT_EQUAL(old_r1, test_find_written_reg(&s_ctx.spi, 1u));
  TEST_ASSERT_EQUAL(old_r0, test_find_written_reg(&s_ctx.spi, 0u));
  TEST_ASSERT_EQUAL(0u, test_reg_from_spi_write(&s_ctx.spi,
      fake_esy_hal_spi_get_tx_count(&s_ctx.spi) - 1u) & 7u);
}

void test_adf4351_failed_requests_do_not_change_frequency_or_power_intent(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4351_config_t cfg;
  test_fill_default_config(&cfg);
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4351_init(&synth, &cfg));
  fake_esy_hal_spi_fail_write_after(0u, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_freq_synth_set_frequency_hz(&synth, 3500000000ULL));
  TEST_ASSERT_EQUAL_UINT64(cfg.initial_freq_hz,
      test_get_info_u64(&synth, ESY_FREQ_SYNTH_INFO_REQUESTED_HZ));
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_power_level(&synth, ESY_FREQ_SYNTH_PORT_MAIN, 2u));
  TEST_ASSERT_EQUAL_UINT64(cfg.initial_freq_hz,
      test_get_info_u64(&synth, ESY_FREQ_SYNTH_INFO_ACTUAL_HZ));
  fake_esy_hal_spi_fail_write_after(0u, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_freq_synth_set_power_level(&synth, ESY_FREQ_SYNTH_PORT_MAIN, 3u));
  TEST_ASSERT_EQUAL(2u, test_get_info_u8(&synth, ESY_FREQ_SYNTH_INFO_POWER_LEVEL));
}

void test_adf4351_short_writes_never_latch_or_commit(void) {
  for (int32_t count = 0; count < 4; count++) {
    esy_freq_synth_t synth = {0};
    esy_adf4351_config_t cfg;
    setUp();
    test_fill_default_config(&cfg);
    TEST_ASSERT_EQUAL(ESY_OK, esy_adf4351_init(&synth, &cfg));
    fake_esy_hal_spi_fail_write_after(0u, count);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_freq_synth_set_output_enable(&synth, ESY_FREQ_SYNTH_PORT_MAIN, true));
    TEST_ASSERT_FALSE(fake_esy_hal_gpio_get_written_value(TEST_ADF_LE_PIN));
    TEST_ASSERT_EQUAL(0u, test_get_info_u8(&synth, ESY_FREQ_SYNTH_INFO_OUTPUT_ENABLED));
    TEST_ASSERT_EQUAL(ESY_OK, esy_adf4351_deinit(&synth));
  }
}

void test_adf4351_each_le_edge_failure_is_reported_and_retry_restores_state(void) {
  for (size_t edge = 0u; edge < 3u; edge++) {
    esy_freq_synth_t synth = {0};
    esy_adf4351_config_t cfg;
    setUp();
    test_fill_default_config(&cfg);
    TEST_ASSERT_EQUAL(ESY_OK, esy_adf4351_init(&synth, &cfg));
    fake_esy_hal_gpio_fail_write_after(edge, ESY_ERR_IO);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_freq_synth_set_output_enable(&synth, ESY_FREQ_SYNTH_PORT_MAIN, true));
    TEST_ASSERT_EQUAL(0u, test_get_info_u8(&synth, ESY_FREQ_SYNTH_INFO_OUTPUT_ENABLED));
    TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_output_enable(&synth, ESY_FREQ_SYNTH_PORT_MAIN, false));
    TEST_ASSERT_FALSE(fake_esy_hal_gpio_get_written_value(TEST_ADF_LE_PIN));
  }
}

void test_adf4351_deinit_failure_preserves_context_and_reinit_is_rejected(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4351_config_t cfg;
  test_fill_default_config(&cfg);
  cfg.initial_output_enabled = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4351_init(&synth, &cfg));
  const esy_freq_synth_t before = synth;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_adf4351_init(&synth, &cfg));
  TEST_ASSERT_EQUAL_MEMORY(&before, &synth, sizeof(synth));
  fake_esy_hal_spi_fail_write_after(0u, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_adf4351_deinit(&synth));
  TEST_ASSERT_TRUE(esy_freq_synth_runtime(&synth)->faulted);
  esy_freq_synth_detach(&synth);
  TEST_ASSERT_TRUE(esy_freq_synth_is_attached(&synth));
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4351_deinit(&synth));
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4351_deinit(&synth));
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4351_init(&synth, &cfg));
}

void test_adf4351_nonintegral_pfd_uses_reference_ratio_in_registers(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4351_config_t cfg;
  test_fill_default_config(&cfg);
  cfg.r_counter = 3u;
  cfg.initial_freq_hz = 2600000000ULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4351_init(&synth, &cfg));
  const uint32_t r0 = test_find_written_reg(&s_ctx.spi, 0u);
  TEST_ASSERT_EQUAL_UINT32(390u, r0 >> 15u);
  TEST_ASSERT_EQUAL_UINT32(0u, (r0 >> 3u) & 4095u);
  TEST_ASSERT_EQUAL_UINT64(cfg.initial_freq_hz,
      test_get_info_u64(&synth, ESY_FREQ_SYNTH_INFO_ACTUAL_HZ));
}

void test_adf4351_invalid_frequency_and_band_clock_are_rejected(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4351_config_t cfg;
  test_fill_default_config(&cfg);
  cfg.band_sel_div = 1u;
  TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_adf4351_init(&synth, &cfg));
  cfg.band_sel_div = 200u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4351_init(&synth, &cfg));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_freq_synth_set_frequency_hz(&synth, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_freq_synth_set_frequency_hz(&synth, 4400000001ULL));
}

void test_adf4351_each_initial_register_failure_cleans_up_and_allows_retry(void) {
  for (size_t write = 0u; write < 6u; write++) {
    esy_freq_synth_t synth = {0};
    esy_adf4351_config_t cfg;
    setUp();
    test_fill_default_config(&cfg);
    fake_esy_hal_spi_fail_write_after(write, ESY_ERR_IO);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_adf4351_init(&synth, &cfg));
    TEST_ASSERT_FALSE(esy_freq_synth_is_attached(&synth));
    TEST_ASSERT_FALSE(esy_freq_synth_runtime(&synth)->faulted);
    TEST_ASSERT_EQUAL(ESY_OK, esy_adf4351_init(&synth, &cfg));
    TEST_ASSERT_EQUAL(ESY_OK, esy_adf4351_deinit(&synth));
  }
}

void test_adf4351_zero_band_divider_is_invalid_before_any_spi_write(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4351_config_t cfg;
  test_fill_default_config(&cfg);
  cfg.band_sel_div = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_adf4351_init(&synth, &cfg));
  TEST_ASSERT_FALSE(esy_freq_synth_is_attached(&synth));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
}

void test_adf4351_band_divider_upper_limits_follow_slow_and_fast_mode_contract(void) {
  static const struct {
    uint32_t reference_hz;
    uint8_t divider;
    esy_err_t expected;
    bool fast;
  } k_cases[] = {
      {20000000u, 255u, ESY_OK, false},
      {32000000u, 254u, ESY_OK, true},
      {32000000u, 255u, ESY_ERR_INVALID_ARG, true}};
  for (size_t i = 0u; i < sizeof(k_cases) / sizeof(k_cases[0]); ++i) {
    esy_freq_synth_t synth = {0};
    esy_adf4351_config_t cfg;
    setUp();
    test_fill_default_config(&cfg);
    cfg.ref_clk_hz = k_cases[i].reference_hz;
    cfg.band_sel_div = k_cases[i].divider;
    TEST_ASSERT_EQUAL(k_cases[i].expected, esy_adf4351_init(&synth, &cfg));
    if (k_cases[i].expected == ESY_OK) {
      const uint32_t r3 = test_find_written_reg(&s_ctx.spi, 3u);
      TEST_ASSERT_EQUAL(k_cases[i].fast ? 1u : 0u, (r3 >> 23u) & 1u);
      TEST_ASSERT_EQUAL(ESY_OK, esy_adf4351_deinit(&synth));
    } else {
      TEST_ASSERT_FALSE(esy_freq_synth_is_attached(&synth));
    }
  }
}
