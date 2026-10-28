/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_freq_synth_internal.h"

#include "esy_middleware/freq_synth/esy_adf4372.h"
#include "esy_middleware/freq_synth/esy_adf4372_device.h"

#include <string.h>

#include "fake_esy_hal_gpio.h"
#include "fake_esy_hal_spi.h"
#include "fake_esy_hal_system.h"
#include "unity.h"

#define TEST_ADF4372_CS_PIN 6u

typedef struct {
  esy_spi_t spi;
  esy_gpio_t cs_gpio;
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
  esy_gpio_config_t gpio_config;

  if (esy_hal_gpio_is_initialized(gpio)) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(gpio));
  }

  esy_hal_gpio_default_cfg(&gpio_config);
  gpio_config.pin = pin;
  gpio_config.mode = ESY_GPIO_MODE_OUTPUT;
  gpio_config.output_value = value;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_gpio_init(gpio, &gpio_config));
}

static void test_config(esy_adf4372_config_t *config) {
  const uint8_t revision = 0x0Au;
  esy_adf4372_default_config(config);
  config->spi = &s_ctx.spi;
  test_init_gpio(&s_ctx.cs_gpio, TEST_ADF4372_CS_PIN, true);
  config->cs_gpio = &s_ctx.cs_gpio;
  fake_esy_hal_spi_set_rx_data(&s_ctx.spi, &revision, sizeof(revision));
}

static uint16_t test_address_at(size_t index) {
  uint8_t bytes[3];
  const size_t count =
      fake_esy_hal_spi_copy_tx_at(&s_ctx.spi, index, bytes, sizeof(bytes));
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(2u, count);
  return (uint16_t)(((uint16_t)(bytes[0] & 0x7Fu) << 8u) | bytes[1]);
}

static size_t test_last_address(uint16_t address) {
  const size_t count = fake_esy_hal_spi_get_tx_count(&s_ctx.spi);
  for (size_t index = count; index > 0u; index--) {
    if (test_address_at(index - 1u) == address) return index - 1u;
  }
  TEST_FAIL_MESSAGE("register was not written");
  return 0u;
}

static size_t test_address_count(uint16_t address) {
  const size_t count = fake_esy_hal_spi_get_tx_count(&s_ctx.spi);
  size_t matches = 0u;
  for (size_t index = 0u; index < count; index++) {
    if (test_address_at(index) == address) matches++;
  }
  return matches;
}

static uint8_t test_value_at(size_t index) {
  uint8_t bytes[3];
  TEST_ASSERT_EQUAL_UINT32(
      sizeof(bytes),
      fake_esy_hal_spi_copy_tx_at(&s_ctx.spi, index, bytes, sizeof(bytes)));
  return bytes[2];
}

static void test_assert_alc_profile(uint8_t revision,
                                    esy_adf4372_vco_supply_t supply,
                                    uint8_t reg2c, uint8_t reg2d, uint8_t reg2e,
                                    uint8_t reg2f) {
  esy_freq_synth_t synth = {0};
  esy_adf4372_config_t config;
  memset(&s_ctx.spi, 0, sizeof(s_ctx.spi));
  memset(&s_ctx.cs_gpio, 0, sizeof(s_ctx.cs_gpio));
  fake_esy_hal_gpio_reset();
  fake_esy_hal_spi_reset();
  fake_esy_hal_system_reset();
  test_config(&config);
  config.vco_supply = supply;
  fake_esy_hal_spi_set_rx_data(&s_ctx.spi, &revision, sizeof(revision));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4372_init(&synth, &config));
  TEST_ASSERT_EQUAL_HEX8(reg2c, test_value_at(test_last_address(0x2Cu)));
  TEST_ASSERT_EQUAL_HEX8(reg2d, test_value_at(test_last_address(0x2Du)));
  TEST_ASSERT_EQUAL_HEX8(reg2e, test_value_at(test_last_address(0x2Eu)));
  TEST_ASSERT_EQUAL_HEX8(reg2f, test_value_at(test_last_address(0x2Fu)));
  esy_adf4372_deinit(&synth);
}

void setUp(void) {
  memset(&s_ctx.spi, 0, sizeof(s_ctx.spi));
  memset(&s_ctx.cs_gpio, 0, sizeof(s_ctx.cs_gpio));
  fake_esy_hal_gpio_reset();
  fake_esy_hal_spi_reset();
  fake_esy_hal_system_reset();
}

void tearDown(void) {}

void test_adf4372_init_triggers_frequency_update_and_keeps_output_disabled(
    void) {
  esy_freq_synth_t synth = {0};
  esy_adf4372_config_t config;
  esy_freq_synth_info_value_t value;

  test_config(&config);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4372_init(&synth, &config));
  TEST_ASSERT_EQUAL_UINT32(0u, test_address_at(0u));
  TEST_ASSERT_EQUAL_UINT32(
      0x10u, test_address_at(fake_esy_hal_spi_get_tx_count(&s_ctx.spi) - 1u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_freq_synth_get_info(
                  &synth, ESY_FREQ_SYNTH_INFO_OUTPUT_ENABLED, &value));
  TEST_ASSERT_EQUAL_UINT8(0u, value.u8);
}

void test_adf4372_three_wire_init_writes_consistent_reg0_and_reads_revision(
    void) {
  uint8_t bytes[3];
  esy_freq_synth_t synth = {0};
  esy_adf4372_config_t config;

  test_config(&config);
  config.spi_three_wire = true;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4372_init(&synth, &config));
  TEST_ASSERT_EQUAL_UINT32(0u, test_address_at(0u));
  TEST_ASSERT_EQUAL_HEX8(0x81u, test_value_at(0u));
  TEST_ASSERT_EQUAL_UINT32(0u, test_address_at(1u));
  TEST_ASSERT_EQUAL_HEX8(0x00u, test_value_at(1u));
  TEST_ASSERT_EQUAL_UINT32(
      2u, fake_esy_hal_spi_copy_tx_at(&s_ctx.spi, 3u, bytes, sizeof(bytes)));
  TEST_ASSERT_EQUAL_HEX8(0x80u, bytes[0]);
  TEST_ASSERT_EQUAL_HEX8(0x06u, bytes[1]);
}

void test_adf4372_init_rejects_zero_and_ff_revision_readback(void) {
  const uint8_t invalid_revisions[] = {0x00u, 0xFFu};
  esy_freq_synth_t synth = {0};
  esy_adf4372_config_t config;

  for (size_t i = 0u; i < sizeof(invalid_revisions); i++) {
    memset(&s_ctx.spi, 0, sizeof(s_ctx.spi));
    fake_esy_hal_gpio_reset();
    fake_esy_hal_spi_reset();
    fake_esy_hal_system_reset();
    test_config(&config);
    fake_esy_hal_spi_set_rx_data(&s_ctx.spi, &invalid_revisions[i], 1u);
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_adf4372_init(&synth, &config));
    TEST_ASSERT_FALSE(esy_freq_synth_is_attached(&synth));
  }
}

void test_adf4372_revision_9_selects_supply_specific_alc_thresholds(void) {
  test_assert_alc_profile(0x09u, ESY_ADF4372_VCO_SUPPLY_3V3, 0x24u, 0x09u,
                          0x0Au, 0x0Au);
  test_assert_alc_profile(0x09u, ESY_ADF4372_VCO_SUPPLY_5V, 0x44u, 0x11u, 0x12u,
                          0x94u);
}

void test_adf4372_revision_a_selects_supply_specific_alc_thresholds(void) {
  test_assert_alc_profile(0x0Au, ESY_ADF4372_VCO_SUPPLY_3V3, 0x24u, 0x0Au,
                          0x0Au, 0x0Au);
  test_assert_alc_profile(0x0Au, ESY_ADF4372_VCO_SUPPLY_5V, 0x44u, 0x11u, 0x10u,
                          0x92u);
}

void test_adf4372_50mhz_pfd_programs_reference_calibration_timing(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4372_config_t config;

  test_config(&config);
  config.ref_clk_hz = 50000000u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4372_init(&synth, &config));
  TEST_ASSERT_EQUAL_HEX8(0x02u, test_value_at(test_last_address(0x31u)));
  TEST_ASSERT_EQUAL_HEX8(0x03u, test_value_at(test_last_address(0x34u)));
  TEST_ASSERT_EQUAL_HEX8(0x7Du, test_value_at(test_last_address(0x35u)));
  esy_adf4372_deinit(&synth);
}

void test_adf4372_fast_lock_uses_manual_calibration_values(void) {
  const uint8_t rx[] = {0u, 4u, 0x7Au, 5u};
  esy_freq_synth_t synth = {0};
  esy_adf4372_config_t config;
  esy_adf4372_vco_calibration_t calibration;
  size_t index;

  test_config(&config);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4372_init(&synth, &config));
  fake_esy_hal_spi_reset();
  fake_esy_hal_spi_set_rx_data(&s_ctx.spi, rx, sizeof(rx));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_adf4372_read_vco_calibration(&synth, &calibration));
  TEST_ASSERT_EQUAL_UINT8(4u, calibration.vco_core);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4372_set_frequency_fast(
                                      &synth, 5001000000ULL, &calibration));
  index = test_last_address(0x12u);
  TEST_ASSERT_EQUAL_UINT8(0u, test_value_at(index));
  index = test_last_address(0x34u);
  TEST_ASSERT_EQUAL_UINT8(0x25u, test_value_at(index));
  index = test_last_address(0x38u);
  TEST_ASSERT_EQUAL_UINT8(0x45u, test_value_at(index));
}

void test_adf4372_fast_tune_writes_only_changed_registers_and_triggers_last(
    void) {
  esy_freq_synth_t synth = {0};
  esy_adf4372_config_t config;
  const esy_adf4372_vco_calibration_t calibration = {
      .frequency_hz = 5000000000ULL,
      .vco_core = 4u,
      .vco_band = 0x7Au,
      .vco_bias = 5u,
      .channel = ESY_ADF4372_CHANNEL_RF8,
  };

  test_config(&config);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4372_init(&synth, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4372_set_frequency_fast(
                                      &synth, 5001000000ULL, &calibration));

  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4372_set_frequency_fast(
                                      &synth, 5002000000ULL, &calibration));
  TEST_ASSERT_GREATER_THAN_UINT32(1u,
                                  fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  TEST_ASSERT_LESS_THAN_UINT32(18u,
                               fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  TEST_ASSERT_EQUAL_UINT32(0u, test_address_count(0x1Fu));
  TEST_ASSERT_EQUAL_UINT32(0u, test_address_count(0x24u));
  TEST_ASSERT_EQUAL_UINT32(0u, test_address_count(0x26u));
  TEST_ASSERT_EQUAL_UINT32(0u, test_address_count(0x2Bu));
  TEST_ASSERT_EQUAL_UINT32(0u, test_address_count(0x34u));
  TEST_ASSERT_EQUAL_UINT32(0u, test_address_count(0x37u));
  TEST_ASSERT_EQUAL_UINT32(0u, test_address_count(0x38u));
  TEST_ASSERT_EQUAL_UINT32(
      0x10u, test_address_at(fake_esy_hal_spi_get_tx_count(&s_ctx.spi) - 1u));

  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4372_set_frequency_fast(
                                      &synth, 5002000000ULL, &calibration));
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  TEST_ASSERT_EQUAL_UINT32(0x10u, test_address_at(0u));
}

void test_adf4372_fast_tune_write_failure_invalidates_register_shadow(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4372_config_t config;
  const esy_adf4372_vco_calibration_t calibration = {
      .frequency_hz = 5000000000ULL,
      .vco_core = 4u,
      .vco_band = 0x7Au,
      .vco_bias = 5u,
      .channel = ESY_ADF4372_CHANNEL_RF8,
  };

  test_config(&config);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4372_init(&synth, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4372_set_frequency_fast(
                                      &synth, 5001000000ULL, &calibration));

  fake_esy_hal_spi_reset();
  fake_esy_hal_spi_set_write_blocking_result(-1);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_adf4372_set_frequency_fast(
                                          &synth, 5001000000ULL, &calibration));
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  TEST_ASSERT_EQUAL_UINT32(0x10u, test_address_at(0u));

  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4372_set_frequency_fast(
                                      &synth, 5001000000ULL, &calibration));
  TEST_ASSERT_EQUAL_UINT32(18u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  TEST_ASSERT_EQUAL_UINT32(
      0x10u, test_address_at(fake_esy_hal_spi_get_tx_count(&s_ctx.spi) - 1u));
}

void test_adf4372_automatic_tune_clears_manual_vco_override(void) {
  const uint8_t rx[] = {0u, 4u, 0x7Au, 5u};
  esy_freq_synth_t synth = {0};
  esy_adf4372_config_t config;
  esy_adf4372_vco_calibration_t calibration;
  size_t test_mode_index;
  size_t trigger_index;

  test_config(&config);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4372_init(&synth, &config));
  fake_esy_hal_spi_reset();
  fake_esy_hal_spi_set_rx_data(&s_ctx.spi, rx, sizeof(rx));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_adf4372_read_vco_calibration(&synth, &calibration));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4372_set_frequency_fast(
                                      &synth, 5001000000ULL, &calibration));

  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_freq_synth_set_frequency_hz(&synth, 5002000000ULL));
  TEST_ASSERT_EQUAL_HEX8(0x40u, test_value_at(test_last_address(0x12u)));
  test_mode_index = test_last_address(0x34u);
  trigger_index = test_last_address(0x10u);
  TEST_ASSERT_EQUAL_HEX8(0x05u, test_value_at(test_mode_index));
  TEST_ASSERT_LESS_THAN_UINT32(trigger_index, test_mode_index);
  TEST_ASSERT_EQUAL_UINT32(16u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
}

void test_adf4372_rejects_invalid_initial_frequency_before_spi_write(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4372_config_t config;

  test_config(&config);
  config.initial_freq_hz = 50000000ULL;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_adf4372_init(&synth, &config));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
}

void test_adf4372_generic_controls_and_diagnostics_cover_supported_ports(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4372_config_t config;
  esy_freq_synth_info_value_t value;

  test_config(&config);
  config.cs_high_delay_us = 1u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4372_init(&synth, &config));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_freq_synth_set_output_enable(
                                      &synth, ESY_FREQ_SYNTH_PORT_MAIN, true));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_freq_synth_set_output_enable(
                                      &synth, ESY_FREQ_SYNTH_PORT_AUX, true));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                          esy_freq_synth_set_output_enable(
                              &synth, (esy_freq_synth_port_t)2u, true));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_freq_synth_set_power_level(
                                      &synth, ESY_FREQ_SYNTH_PORT_MAIN, 2u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_freq_synth_set_power_level(&synth, ESY_FREQ_SYNTH_PORT_MAIN, 4u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_NOT_SUPPORTED,
      esy_freq_synth_set_power_level(&synth, ESY_FREQ_SYNTH_PORT_AUX, 1u));

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK,
      esy_freq_synth_get_info(&synth, ESY_FREQ_SYNTH_INFO_ACTUAL_HZ, &value));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_freq_synth_get_info(&synth, ESY_FREQ_SYNTH_INFO_REQUESTED_HZ,
                                      &value));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK,
      esy_freq_synth_get_info(&synth, ESY_FREQ_SYNTH_INFO_PFD_HZ, &value));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_freq_synth_get_info(&synth, ESY_FREQ_SYNTH_INFO_MOD, &value));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_freq_synth_get_info(&synth, ESY_FREQ_SYNTH_INFO_INT, &value));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK,
      esy_freq_synth_get_info(&synth, ESY_FREQ_SYNTH_INFO_POWER_LEVEL, &value));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK,
      esy_freq_synth_get_info(&synth, ESY_FREQ_SYNTH_INFO_RF_DIV_SEL, &value));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_NOT_SUPPORTED,
      esy_freq_synth_get_info(&synth, ESY_FREQ_SYNTH_INFO_FRAC, &value));

  esy_adf4372_deinit(&synth);
  TEST_ASSERT_FALSE(esy_freq_synth_is_attached(&synth));
  esy_adf4372_deinit(NULL);
}

void test_adf4372_rf16_path_controls_the_doubler_output(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4372_config_t config;
  esy_freq_synth_info_value_t value;

  test_config(&config);
  config.main_channel = ESY_ADF4372_CHANNEL_RF16;
  config.initial_freq_hz = 10000000000ULL;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4372_init(&synth, &config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_freq_synth_set_frequency_hz(&synth, 9000000000ULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_freq_synth_set_output_enable(
                                      &synth, ESY_FREQ_SYNTH_PORT_MAIN, true));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_NOT_SUPPORTED,
      esy_freq_synth_set_power_level(&synth, ESY_FREQ_SYNTH_PORT_MAIN, 1u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_NOT_SUPPORTED,
      esy_freq_synth_get_info(&synth, ESY_FREQ_SYNTH_INFO_POWER_LEVEL, &value));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_freq_synth_set_frequency_hz(
                                                   &synth, 7000000000ULL));
}

void test_adf4372_enabled_output_prevents_runtime_suspend(void) {
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
          .name = "adf4372",
          .dependencies = dependencies,
          .dependency_count = 1u,
          .supported_state_mask =
              ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
              ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION),
      },
  };
  esy_freq_synth_device_binding_t binding = {0};
  esy_freq_synth_t synth = {0};
  esy_adf4372_config_t config;
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
  test_config(&config);
  config.initial_output_enabled = true;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4372_init(&synth, &config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_adf4372_register_device(&binding, &synth, &registry,
                                          &synth_device_config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&registry));

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_ACTIVE, state);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_freq_synth_set_frequency_hz(&synth, 5100000000ULL));
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
  esy_adf4372_deinit(&synth);
  TEST_ASSERT_TRUE(esy_freq_synth_is_attached(&synth));
}

void test_adf4372_fast_lock_rejects_unbound_and_mismatched_calibration(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4372_config_t config;
  esy_adf4372_vco_calibration_t calibration = {
      .channel = ESY_ADF4372_CHANNEL_RF16,
  };

  memset(&synth, 0, sizeof(synth));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_adf4372_set_frequency_fast(NULL, 5000000000ULL, &calibration));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE, esy_adf4372_read_vco_calibration(&synth, &calibration));
  test_config(&config);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_adf4372_init(&synth, &config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_adf4372_set_frequency_fast(&synth, 5000000000ULL, &calibration));
}

void test_adf4372_reference_limits_and_mux_conflicts_fail_before_io(void) {
  const uint32_t invalid_refs[] = {50000u, 9999999u, 125000001u, 250000000u};
  for (size_t i = 0u; i < sizeof(invalid_refs) / sizeof(invalid_refs[0]); i++) {
    esy_freq_synth_t synth = {0};
    esy_adf4372_config_t cfg;
    setUp();
    test_config(&cfg);
    cfg.ref_clk_hz = invalid_refs[i];
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_adf4372_init(&synth, &cfg));
    TEST_ASSERT_EQUAL(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
    TEST_ASSERT_FALSE(esy_freq_synth_is_attached(&synth));
  }
  esy_freq_synth_t synth = {0};
  esy_adf4372_config_t cfg;
  test_config(&cfg);
  TEST_ASSERT_FALSE(cfg.muxout_enabled);
  cfg.muxout_enabled = true;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_adf4372_init(&synth, &cfg));
  cfg.spi_three_wire = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_init(&synth, &cfg));
}

void test_adf4372_fractional_mode_enables_secondary_modulus(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4372_config_t cfg;
  test_config(&cfg);
  cfg.initial_freq_hz = 5001234567ULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_init(&synth, &cfg));
  TEST_ASSERT_BITS_HIGH(0x10u, test_value_at(test_last_address(0x2Bu)));
  const uint16_t frac2 = (test_value_at(test_last_address(0x17u)) >> 1u) |
      ((uint16_t)test_value_at(test_last_address(0x18u)) << 7u);
  TEST_ASSERT_NOT_EQUAL(0u, frac2);
}

void test_adf4372_r32_encoding_and_minimum_alc_timeout(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4372_config_t cfg;
  test_config(&cfg);
  cfg.ref_clk_hz = 10000000u;
  cfg.r_counter = 32u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_init(&synth, &cfg));
  TEST_ASSERT_EQUAL_HEX8(0u, test_value_at(test_last_address(0x1Fu)));
  TEST_ASSERT_EQUAL_HEX8(2u, test_value_at(test_last_address(0x34u)));
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_deinit(&synth));
  fake_esy_hal_spi_reset();
  test_config(&cfg);
  cfg.ref_clk_hz = 10000000u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_init(&synth, &cfg));
  TEST_ASSERT_EQUAL_HEX8(2u, test_value_at(test_last_address(0x34u)));
}

void test_adf4372_nonintegral_pfd_register_ratio_matches_reported_frequency(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4372_config_t cfg;
  esy_freq_synth_info_value_t info;
  test_config(&cfg);
  cfg.ref_clk_hz = 10000000u;
  cfg.r_counter = 31u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_init(&synth, &cfg));
  const uint16_t integer = test_value_at(test_last_address(0x10u)) |
      ((uint16_t)test_value_at(test_last_address(0x11u)) << 8u);
  TEST_ASSERT_EQUAL_UINT16(15500u, integer);
  TEST_ASSERT_EQUAL(0u, test_value_at(test_last_address(0x14u)));
  TEST_ASSERT_EQUAL(0u, test_value_at(test_last_address(0x15u)));
  TEST_ASSERT_EQUAL(0u, test_value_at(test_last_address(0x16u)));
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_get_info(&synth, ESY_FREQ_SYNTH_INFO_ACTUAL_HZ, &info));
  TEST_ASSERT_EQUAL_UINT64(cfg.initial_freq_hz, info.u64);
}

void test_adf4372_calibration_read_failures_restore_fsm_and_preserve_output(void) {
  for (size_t failed_read = 0u; failed_read < 4u; failed_read++) {
    esy_freq_synth_t synth = {0};
    esy_adf4372_config_t cfg;
    esy_adf4372_vco_calibration_t calibration;
    memset(&calibration, 0xa5, sizeof(calibration));
    const esy_adf4372_vco_calibration_t before = calibration;
    setUp();
    test_config(&cfg);
    TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_init(&synth, &cfg));
    fake_esy_hal_spi_reset();
    const uint8_t rx[] = {0x02u, 4u, 0x7au, 5u};
    fake_esy_hal_spi_set_rx_data(&s_ctx.spi, rx, sizeof(rx));
    fake_esy_hal_spi_fail_read_after(failed_read, ESY_ERR_IO);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_adf4372_read_vco_calibration(&synth, &calibration));
    TEST_ASSERT_EQUAL_MEMORY(&before, &calibration, sizeof(calibration));
    if (failed_read != 0u)
      TEST_ASSERT_EQUAL_HEX8(0x02u, test_value_at(test_last_address(0x33u)));
    TEST_ASSERT_FALSE(esy_freq_synth_runtime(&synth)->faulted);
    TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(TEST_ADF4372_CS_PIN));
  }
}

void test_adf4372_failed_fsm_restore_faults_handle_until_deinit(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4372_config_t cfg;
  esy_adf4372_vco_calibration_t calibration = {0};
  test_config(&cfg);
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_init(&synth, &cfg));
  fake_esy_hal_spi_reset();
  const uint8_t rx[] = {0x02u, 4u, 0x7au, 5u};
  fake_esy_hal_spi_set_rx_data(&s_ctx.spi, rx, sizeof(rx));
  fake_esy_hal_spi_fail_write_after(6u, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_adf4372_read_vco_calibration(&synth, &calibration));
  TEST_ASSERT_TRUE(esy_freq_synth_runtime(&synth)->faulted);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_freq_synth_set_frequency_hz(&synth, 5000000000ULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_deinit(&synth));
  TEST_ASSERT_FALSE(esy_freq_synth_is_attached(&synth));
}

void test_adf4372_cs_errors_and_short_writes_do_not_commit_output_state(void) {
  for (size_t edge = 0u; edge < 2u; edge++) {
    esy_freq_synth_t synth = {0};
    esy_adf4372_config_t cfg;
    esy_freq_synth_info_value_t info;
    setUp();
    test_config(&cfg);
    TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_init(&synth, &cfg));
    fake_esy_hal_gpio_fail_write_after(edge, ESY_ERR_IO);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_freq_synth_set_output_enable(&synth, ESY_FREQ_SYNTH_PORT_MAIN, true));
    TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_get_info(&synth, ESY_FREQ_SYNTH_INFO_OUTPUT_ENABLED, &info));
    TEST_ASSERT_EQUAL(0u, info.u8);
    fake_esy_hal_spi_fail_write_after(0u, 2);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_freq_synth_set_output_enable(&synth, ESY_FREQ_SYNTH_PORT_MAIN, true));
    TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_deinit(&synth));
  }
}

void test_adf4372_reinit_and_failed_deinit_preserve_live_handle(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4372_config_t cfg;
  test_config(&cfg);
  cfg.initial_output_enabled = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_init(&synth, &cfg));
  const esy_freq_synth_t before = synth;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_adf4372_init(&synth, &cfg));
  TEST_ASSERT_EQUAL_MEMORY(&before, &synth, sizeof(synth));
  fake_esy_hal_spi_fail_write_after(0u, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_adf4372_deinit(&synth));
  TEST_ASSERT_TRUE(esy_freq_synth_runtime(&synth)->faulted);
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_deinit(&synth));
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_deinit(&synth));
  test_config(&cfg);
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_init(&synth, &cfg));
}

void test_adf4372_register_vectors_match_exact_reference_ratio(void) {
  static const uint32_t k_references[] = {10000000u, 100000000u, 125000000u};
  static const uint8_t k_dividers[] = {1u, 3u, 31u, 32u};
  static const uint64_t k_frequencies[] = {
      62500000ULL, 5000000000ULL, 5001234567ULL, 5000206092ULL, 7999999999ULL};
  for (size_t ref = 0u; ref < sizeof(k_references) / sizeof(k_references[0]); ref++) {
    for (size_t divider = 0u; divider < sizeof(k_dividers) / sizeof(k_dividers[0]); divider++) {
      esy_freq_synth_t synth = {0};
      esy_adf4372_config_t cfg;
      setUp();
      test_config(&cfg);
      cfg.ref_clk_hz = k_references[ref];
      cfg.r_counter = k_dividers[divider];
      TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_init(&synth, &cfg));
      for (size_t f = 0u; f < sizeof(k_frequencies) / sizeof(k_frequencies[0]); f++) {
        fake_esy_hal_spi_reset();
        TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_frequency_hz(&synth, k_frequencies[f]));
        const uint32_t integer = test_value_at(test_last_address(0x10u)) |
            ((uint32_t)test_value_at(test_last_address(0x11u)) << 8u);
        const uint32_t frac1 = test_value_at(test_last_address(0x14u)) |
            ((uint32_t)test_value_at(test_last_address(0x15u)) << 8u) |
            ((uint32_t)test_value_at(test_last_address(0x16u)) << 16u) |
            ((uint32_t)(test_value_at(test_last_address(0x17u)) & 1u) << 24u);
        const uint32_t frac2 = (test_value_at(test_last_address(0x17u)) >> 1u) |
            ((uint32_t)test_value_at(test_last_address(0x18u)) << 7u);
        const uint32_t mod2 = test_value_at(test_last_address(0x19u)) |
            ((uint32_t)test_value_at(test_last_address(0x1au)) << 8u);
        const uint8_t rf_divider = (test_value_at(test_last_address(0x24u)) >> 4u) & 7u;
        TEST_ASSERT_GREATER_OR_EQUAL(2u, mod2);
        TEST_ASSERT_LESS_THAN(mod2, frac2);
        const long double actual = (integer + (frac1 + (long double)frac2 / mod2) /
            33554432.0L) * cfg.ref_clk_hz / cfg.r_counter / (1u << rf_divider);
        esy_freq_synth_info_value_t info;
        TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_get_info(&synth, ESY_FREQ_SYNTH_INFO_ACTUAL_HZ, &info));
        TEST_ASSERT_TRUE(actual - info.u64 < 1.01L && actual - info.u64 > -0.01L);
        TEST_ASSERT_TRUE(actual - k_frequencies[f] <= 1.0L && actual - k_frequencies[f] >= -1.0L);
      }
      TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_deinit(&synth));
    }
  }
}

void test_adf4372_each_initial_transfer_failure_cleans_up_and_allows_retry(void) {
  esy_freq_synth_t synth = {0};
  esy_adf4372_config_t cfg;
  test_config(&cfg);
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_init(&synth, &cfg));
  const size_t count = fake_esy_hal_spi_get_tx_count(&s_ctx.spi);
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_deinit(&synth));
  for (size_t write = 0u; write < count; write++) {
    setUp();
    test_config(&cfg);
    fake_esy_hal_spi_fail_write_after(write, ESY_ERR_IO);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_adf4372_init(&synth, &cfg));
    TEST_ASSERT_FALSE(esy_freq_synth_is_attached(&synth));
    TEST_ASSERT_FALSE(esy_freq_synth_runtime(&synth)->faulted);
    const uint8_t revision = 0x0au;
    fake_esy_hal_spi_reset();
    fake_esy_hal_spi_set_rx_data(&s_ctx.spi, &revision, 1u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_init(&synth, &cfg));
    TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_deinit(&synth));
  }
}

void test_adf4372_each_calibration_write_error_restores_fsm(void) {
  for (size_t write = 1u; write < 6u; write++) {
    esy_freq_synth_t synth = {0};
    esy_adf4372_config_t cfg;
    esy_adf4372_vco_calibration_t calibration = {0};
    setUp();
    test_config(&cfg);
    TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_init(&synth, &cfg));
    fake_esy_hal_spi_reset();
    const uint8_t rx[] = {2u, 4u, 0x7au, 5u};
    fake_esy_hal_spi_set_rx_data(&s_ctx.spi, rx, sizeof(rx));
    fake_esy_hal_spi_fail_write_after(write, ESY_ERR_IO);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_adf4372_read_vco_calibration(&synth, &calibration));
    TEST_ASSERT_EQUAL(2u, test_value_at(test_last_address(0x33u)));
    TEST_ASSERT_FALSE(esy_freq_synth_runtime(&synth)->faulted);
    TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_deinit(&synth));
  }
}
