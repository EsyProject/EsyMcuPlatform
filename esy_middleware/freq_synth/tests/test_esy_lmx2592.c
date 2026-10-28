/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_freq_synth_internal.h"

#include "esy_middleware/freq_synth/esy_lmx2592.h"

#include <string.h>

#include "fake_esy_hal_gpio.h"
#include "fake_esy_hal_spi.h"
#include "fake_esy_hal_system.h"
#include "unity.h"

#define TEST_LMX2592_CSB_PIN 22u
#define TEST_LMX2592_MUXOUT_PIN 28u
#define TEST_LMX2592_REGISTER_COUNT 71u

typedef struct {
  esy_spi_t spi;
  esy_gpio_t cs_gpio;
  esy_gpio_t muxout_gpio;
  uint16_t registers[TEST_LMX2592_REGISTER_COUNT];
  bool written[TEST_LMX2592_REGISTER_COUNT];
} lmx2592_test_ctx_t;

static lmx2592_test_ctx_t s_ctx;

static void test_config(esy_lmx2592_config_t *config) {
  esy_gpio_config_t gpio_config;
  if (esy_hal_gpio_is_initialized(&s_ctx.cs_gpio)) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(&s_ctx.cs_gpio));
  }
  if (esy_hal_gpio_is_initialized(&s_ctx.muxout_gpio)) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(&s_ctx.muxout_gpio));
  }

  esy_hal_gpio_default_cfg(&gpio_config);
  gpio_config.pin = TEST_LMX2592_CSB_PIN;
  gpio_config.mode = ESY_GPIO_MODE_OUTPUT;
  gpio_config.output_value = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_init(&s_ctx.cs_gpio, &gpio_config));
  gpio_config.pin = TEST_LMX2592_MUXOUT_PIN;
  gpio_config.mode = ESY_GPIO_MODE_INPUT;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_init(&s_ctx.muxout_gpio, &gpio_config));
  esy_lmx2592_default_config(config);
  config->spi = &s_ctx.spi;
  config->spi_speed_hz = 0u;
  config->cs_gpio = &s_ctx.cs_gpio;
  config->muxout_gpio = &s_ctx.muxout_gpio;
  config->ref_clk_hz = 100000000u;
  config->r_divider = 1u;
  config->output_divider = 0u;
  config->initial_freq_hz = 2000000000ULL;
}

static uint8_t test_frame(size_t index, uint16_t *value) {
  uint8_t bytes[3];
  TEST_ASSERT_EQUAL_UINT32(
      sizeof(bytes),
      fake_esy_hal_spi_copy_tx_at(&s_ctx.spi, index, bytes, sizeof(bytes)));
  if (value != NULL) *value = ((uint16_t)bytes[1] << 8u) | bytes[2];
  return bytes[0];
}

static uint16_t test_last_write(uint8_t address) {
  const size_t count = fake_esy_hal_spi_get_tx_count(&s_ctx.spi);
  for (size_t i = count; i > 0u; i--) {
    uint16_t value;
    if (test_frame(i - 1u, &value) == address) return value;
  }
  TEST_FAIL_MESSAGE("Register was not written");
  return 0u;
}

static void test_capture_registers(void) {
  const size_t count = fake_esy_hal_spi_get_tx_count(&s_ctx.spi);
  for (size_t i = 0u; i < count; i++) {
    uint16_t value;
    const uint8_t command = test_frame(i, &value);
    if ((command & 0x80u) != 0u) continue;
    TEST_ASSERT_LESS_THAN_UINT8(TEST_LMX2592_REGISTER_COUNT, command);
    s_ctx.registers[command] = value;
    s_ctx.written[command] = true;
  }
}

static uint16_t test_divider(void) {
  const uint16_t outa_mux = (s_ctx.registers[47] >> 11u) & 3u;
  TEST_ASSERT_LESS_THAN_UINT16(2u, outa_mux);
  if (outa_mux == 1u) return 1u;
  const uint16_t segments = (s_ctx.registers[36] >> 4u) & 7u;
  uint16_t divider = (s_ctx.registers[35] & 4u) != 0u ? 3u : 2u;
  if (segments >= 2u) {
    const uint16_t code = (s_ctx.registers[35] >> 9u) & 15u;
    TEST_ASSERT_TRUE(code == 1u || code == 2u || code == 4u || code == 8u);
    divider *= code == 4u ? 6u : (code == 8u ? 8u : code * 2u);
  }
  if (segments == 4u) {
    const uint16_t code = s_ctx.registers[36] & 15u;
    TEST_ASSERT_TRUE(code == 1u || code == 2u || code == 4u || code == 8u);
    divider *= code == 4u ? 6u : (code == 8u ? 8u : code * 2u);
  }
  TEST_ASSERT_TRUE(segments == 1u || segments == 2u || segments == 4u);
  return divider;
}

/* Decode the TI SNAS646G register map independently of the backend planner. */
static void test_assert_frequency(esy_freq_synth_t *synth, uint64_t expected,
                                  uint32_t reference, uint16_t divider) {
  static const uint16_t k_minimum_n[] = {9u, 11u, 16u, 18u, 30u};
  static const uint16_t k_pfd_delay[] = {1u, 1u, 2u, 2u, 8u};
  esy_freq_synth_info_value_t info;
  test_capture_registers();
  const uint32_t integer = (s_ctx.registers[38] >> 1u) & 0xfffu;
  const uint32_t denominator = ((uint32_t)s_ctx.registers[40] << 16u) |
                               s_ctx.registers[41];
  const uint32_t numerator = ((uint32_t)s_ctx.registers[44] << 16u) |
                             s_ctx.registers[45];
  const uint32_t pre_divider = s_ctx.registers[12] & 0xfffu;
  const uint32_t post_divider = (s_ctx.registers[11] >> 4u) & 0xffu;
  const uint32_t multiplier = (s_ctx.registers[10] >> 7u) & 0x1fu;
  const uint16_t mash_order = s_ctx.registers[46] & 7u;
  TEST_ASSERT_NOT_EQUAL(0u, denominator);
  TEST_ASSERT_LESS_THAN_UINT32(denominator, numerator);
  TEST_ASSERT_NOT_EQUAL(0u, pre_divider);
  TEST_ASSERT_NOT_EQUAL(0u, post_divider);
  TEST_ASSERT_EQUAL_UINT32(1u, multiplier);
  TEST_ASSERT_BITS_LOW(0x0800u, s_ctx.registers[9]);
  TEST_ASSERT_BITS_LOW(0x1000u, s_ctx.registers[37]);
  TEST_ASSERT_BITS_LOW(0x0001u, s_ctx.registers[30]);
  TEST_ASSERT_EQUAL_UINT16(divider, test_divider());
  const uint64_t pfd = reference / pre_divider / post_divider;
  TEST_ASSERT_EQUAL_UINT64(expected * divider * denominator,
      pfd * 2u * ((uint64_t)integer * denominator + numerator));
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_get_info(
      synth, ESY_FREQ_SYNTH_INFO_ACTUAL_HZ, &info));
  TEST_ASSERT_EQUAL_UINT64(expected, info.u64);
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_get_info(
      synth, ESY_FREQ_SYNTH_INFO_REQUESTED_HZ, &info));
  TEST_ASSERT_EQUAL_UINT64(expected, info.u64);
  TEST_ASSERT_LESS_THAN_UINT16(5u, mash_order);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT16(k_minimum_n[mash_order], integer);
  TEST_ASSERT_EQUAL_UINT16(k_pfd_delay[mash_order],
                          (s_ctx.registers[39] >> 8u) & 0x3fu);
  if (numerator == 0u) TEST_ASSERT_EQUAL_UINT16(0u, mash_order);
  else TEST_ASSERT_NOT_EQUAL(0u, mash_order);
  TEST_ASSERT_EQUAL_HEX16(0u, s_ctx.registers[42]);
  TEST_ASSERT_EQUAL_HEX16(0u, s_ctx.registers[43]);
  TEST_ASSERT_BITS_HIGH(0x0080u, s_ctx.registers[46]);
  TEST_ASSERT_BITS_HIGH(0x0400u, s_ctx.registers[31]);
  TEST_ASSERT_BITS_LOW(0x0800u, s_ctx.registers[36]);
  TEST_ASSERT_EQUAL_UINT16(1u, s_ctx.registers[61] & 1u);
  const uint64_t vco = expected * divider;
  TEST_ASSERT_EQUAL_UINT16(vco >= 6500000000ULL ? 3u : 0u,
                          (s_ctx.registers[30] >> 6u) & 3u);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT64(3550000000ULL, vco);
  TEST_ASSERT_LESS_OR_EQUAL_UINT64(7100000000ULL, vco);
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_hal_gpio_reset();
  fake_esy_hal_spi_reset();
  fake_esy_hal_system_reset();
}

void tearDown(void) {}

void test_lmx2592_initialization_uses_its_sparse_map_and_charge_pump_fields(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  uint16_t value;
  test_config(&config);
  config.initial_freq_hz = 3144000000ULL;
  config.initial_output_enabled = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  TEST_ASSERT_EQUAL_UINT8(0u, test_frame(0u, &value));
  TEST_ASSERT_BITS_HIGH(2u, value);
  TEST_ASSERT_EQUAL_UINT8(0u, test_frame(1u, &value));
  TEST_ASSERT_BITS_LOW(2u, value);
  uint8_t previous = test_frame(2u, NULL);
  TEST_ASSERT_EQUAL_UINT8(64u, previous);
  size_t index = 3u;
  while (previous != 0u) {
    const uint8_t address = test_frame(index++, NULL);
    TEST_ASSERT_LESS_THAN_UINT8(previous, address);
    previous = address;
  }
  TEST_ASSERT_GREATER_OR_EQUAL_UINT64(
      10000u, fake_esy_hal_system_get_ctx()->time_us);
  TEST_ASSERT_EQUAL_UINT8(0u, test_frame(index, &value));
  TEST_ASSERT_BITS_HIGH(0x201cu, value);
  TEST_ASSERT_BITS_LOW(0x0003u, value);
  TEST_ASSERT_EQUAL_HEX16(0x003eu, test_last_write(38u));
  TEST_ASSERT_EQUAL_HEX16(0u, test_last_write(40u));
  TEST_ASSERT_EQUAL_HEX16(25u, test_last_write(41u));
  TEST_ASSERT_EQUAL_HEX16(0u, test_last_write(44u));
  TEST_ASSERT_EQUAL_HEX16(11u, test_last_write(45u));
  TEST_ASSERT_EQUAL_UINT16(config.charge_pump_current,
                          (test_last_write(14u) >> 7u) & 31u);
  TEST_ASSERT_EQUAL_UINT16(config.charge_pump_current,
                          (test_last_write(14u) >> 2u) & 31u);
  TEST_ASSERT_EQUAL_UINT16(config.charge_pump_multiplier,
                          test_last_write(14u) & 3u);
  TEST_ASSERT_BITS_LOW(0x0040u, test_last_write(46u));
  TEST_ASSERT_BITS_HIGH(0x0080u, test_last_write(46u));
  TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(TEST_LMX2592_CSB_PIN));
  test_assert_frequency(&synth, config.initial_freq_hz, config.ref_clk_hz, 2u);
  TEST_ASSERT_FALSE(s_ctx.written[3]);
  TEST_ASSERT_FALSE(s_ctx.written[49]);
  TEST_ASSERT_FALSE(s_ctx.written[68]);
  TEST_ASSERT_FALSE(s_ctx.written[69]);
  TEST_ASSERT_FALSE(s_ctx.written[70]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
}

void test_lmx2592_auto_divider_covers_band_edges_and_low_n_fractional_boundary(void) {
  static const uint64_t k_frequencies[] = {
      2000000000ULL, 2944000000ULL, 3144000000ULL, 3249700000ULL,
      3250000000ULL, 3549700000ULL,
      3550000000ULL, 3550300000ULL, 3599700000ULL, 3600000000ULL,
      3600300000ULL, 3700000000ULL, 2000000000ULL};
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  test_assert_frequency(&synth, 2000000000ULL, config.ref_clk_hz, 2u);
  for (size_t i = 0u; i < sizeof(k_frequencies) / sizeof(k_frequencies[0]); i++) {
    const uint64_t frequency = k_frequencies[i];
    fake_esy_hal_spi_reset();
    TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_frequency_hz(&synth, frequency));
    test_assert_frequency(&synth, frequency, config.ref_clk_hz,
                          frequency < 3550000000ULL ? 2u : 1u);
    uint16_t r0;
    const size_t count = fake_esy_hal_spi_get_tx_count(&s_ctx.spi);
    TEST_ASSERT_EQUAL_UINT8(0u, test_frame(count - 1u, &r0));
    TEST_ASSERT_BITS_HIGH(8u, r0);
    if (frequency >= 3550000000ULL && frequency < 3600000000ULL) {
      TEST_ASSERT_EQUAL_UINT16(17u, (s_ctx.registers[38] >> 1u) & 0xfffu);
      TEST_ASSERT_EQUAL_UINT16(2u, s_ctx.registers[46] & 7u);
    }
  }
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
}

void test_lmx2592_exact_300khz_steps_cross_the_divider_and_modulator_boundaries(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  test_config(&config);
  config.initial_freq_hz = 3535000000ULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  test_assert_frequency(&synth, config.initial_freq_hz, config.ref_clk_hz, 2u);
  for (uint32_t point = 0u; point < 101u; point++) {
    const uint64_t frequency = 3535000000ULL + (uint64_t)point * 300000u;
    fake_esy_hal_spi_reset();
    TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_frequency_hz(&synth, frequency));
    test_assert_frequency(&synth, frequency, config.ref_clk_hz,
                          frequency < 3550000000ULL ? 2u : 1u);
  }
  for (uint32_t point = 0u; point < 101u; point++) {
    const uint64_t frequency = 3585000000ULL + (uint64_t)point * 300000u;
    fake_esy_hal_spi_reset();
    TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_frequency_hz(&synth, frequency));
    test_assert_frequency(&synth, frequency, config.ref_clk_hz, 1u);
  }
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
}

void test_lmx2592_reference_divider_preserves_exact_fraction_and_prescaler(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  test_config(&config);
  config.ref_clk_hz = 100000002u;
  config.r_divider = 3u;
  config.initial_freq_hz = 2000123456ULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  test_assert_frequency(&synth, config.initial_freq_hz, config.ref_clk_hz, 2u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
}

void test_lmx2592_fixed_divider_rejects_unreachable_frequency_without_io(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  test_config(&config);
  config.output_divider = 2u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_freq_synth_set_frequency_hz(&synth, 3700000000ULL));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  TEST_ASSERT_FALSE(esy_freq_synth_runtime(&synth)->faulted);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
}

void test_lmx2592_fixed_dividers_follow_the_ti_segment_table(void) {
  static const struct {
    uint16_t divider;
    uint64_t frequency;
  } k_cases[] = {
      {1u, 4000000000ULL}, {2u, 2000000000ULL}, {3u, 2000000000ULL},
      {4u, 1000000000ULL}, {6u, 700000000ULL}, {8u, 500000000ULL},
      {12u, 350000000ULL}, {16u, 250000000ULL}, {24u, 180000000ULL},
      {32u, 130000000ULL}, {36u, 100000000ULL}, {48u, 90000000ULL},
      {64u, 60000000ULL}, {96u, 50000000ULL}, {128u, 30000000ULL},
      {192u, 25000000ULL},
  };
  for (size_t i = 0u; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
    esy_freq_synth_t synth = {0};
    esy_lmx2592_config_t config;
    setUp();
    test_config(&config);
    config.output_divider = k_cases[i].divider;
    config.initial_freq_hz = k_cases[i].frequency;
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
    test_assert_frequency(&synth, config.initial_freq_hz,
                          config.ref_clk_hz, config.output_divider);
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
  }
}

void test_lmx2592_fixed_divider_rejects_segment_limits_even_with_valid_vco_range(void) {
  static const struct {
    uint16_t divider;
    uint64_t frequency;
  } k_cases[] = {{3u, 2300000000ULL}, {4u, 1300000000ULL}};
  for (size_t i = 0u; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
    esy_freq_synth_t synth = {0};
    esy_lmx2592_config_t config;
    setUp();
    test_config(&config);
    config.output_divider = k_cases[i].divider;
    config.initial_freq_hz = k_cases[i].frequency;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_lmx2592_init(&synth, &config));
    TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
    TEST_ASSERT_FALSE(esy_freq_synth_is_attached(&synth));
  }
}

void test_lmx2592_invalid_configuration_preserves_empty_handle_without_io(void) {
  for (size_t invalid = 0u; invalid < 19u; invalid++) {
    esy_freq_synth_t synth = {0};
    esy_lmx2592_config_t config;
    setUp();
    test_config(&config);
    switch (invalid) {
      case 0u: config.spi = NULL; break;
      case 1u: config.cs_gpio = NULL; break;
      case 2u: config.muxout_gpio = NULL; break;
      case 3u: config.ref_clk_hz = 0u; break;
      case 4u: config.r_divider = 0u; break;
      case 5u: config.output_divider = 5u; break;
      case 6u: config.output_divider = 4u; break;
      case 7u: config.initial_freq_hz = 0u; break;
      case 8u: config.initial_freq_hz = 7100000001ULL; break;
      case 9u: config.charge_pump_current = 0u; break;
      case 10u: config.charge_pump_current = 32u; break;
      case 11u: config.charge_pump_multiplier = 1u; break;
      case 12u: config.charge_pump_multiplier = 3u; break;
      case 13u: config.output_a_power = 64u; break;
      case 14u: config.output_b_power = 64u; break;
      case 15u: config.spi_speed_hz = 5000001u; break;
      case 16u: config.output_a_power = 32u; break;
      case 17u: config.output_b_power = 47u; break;
      default: config.ref_clk_hz = 100000001u; config.r_divider = 3u; break;
    }
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_lmx2592_init(&synth, &config));
    TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
    TEST_ASSERT_FALSE(esy_freq_synth_is_attached(&synth));
    TEST_ASSERT_FALSE(esy_freq_synth_runtime(&synth)->faulted);
  }
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_lmx2592_init(NULL, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_lmx2592_init(&synth, NULL));
  esy_lmx2592_default_config(NULL);
}

void test_lmx2592_readback_uses_r68_status_and_restores_lock_detect(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  uint16_t value = 0u;
  uint16_t r0;
  const uint8_t rx[] = {0u, 4u, 0x60u};
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  fake_esy_hal_spi_reset();
  fake_esy_hal_spi_set_rx_data(&s_ctx.spi, rx, sizeof(rx));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_read_register(&synth, 68u, &value));
  TEST_ASSERT_EQUAL_HEX16(0x0460u, value);
  TEST_ASSERT_EQUAL_UINT16(2u, (value >> 9u) & 3u);
  TEST_ASSERT_EQUAL_UINT16(3u, (value >> 5u) & 7u);
  TEST_ASSERT_EQUAL_UINT8(0u, test_frame(0u, &r0));
  TEST_ASSERT_BITS_LOW(0x000cu, r0);
  TEST_ASSERT_EQUAL_HEX8(0xc4u, test_frame(1u, NULL));
  TEST_ASSERT_EQUAL_UINT8(0u, test_frame(2u, &r0));
  TEST_ASSERT_BITS_HIGH(0x0004u, r0);
  TEST_ASSERT_BITS_LOW(0x0008u, r0);
  TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(TEST_LMX2592_CSB_PIN));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
}

void test_lmx2592_read_error_preserves_destination_and_restores_muxout(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  uint16_t value = 0xbeefu;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  fake_esy_hal_spi_reset();
  fake_esy_hal_spi_fail_read_after(0u, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_lmx2592_read_register(&synth, 68u, &value));
  TEST_ASSERT_EQUAL_HEX16(0xbeefu, value);
  TEST_ASSERT_BITS_HIGH(0x0004u, test_last_write(0u));
  TEST_ASSERT_BITS_LOW(0x0008u, test_last_write(0u));
  TEST_ASSERT_FALSE(esy_freq_synth_runtime(&synth)->faulted);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
}

void test_lmx2592_failed_muxout_restore_faults_handle_until_cleanup(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  uint16_t value = 0xbeefu;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  fake_esy_hal_spi_reset();
  fake_esy_hal_spi_fail_write_after(2u, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_lmx2592_read_register(&synth, 68u, &value));
  TEST_ASSERT_EQUAL_HEX16(0xbeefu, value);
  TEST_ASSERT_TRUE(esy_freq_synth_runtime(&synth)->faulted);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_freq_synth_set_frequency_hz(&synth, 2100000000ULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
}

void test_lmx2592_readback_checks_its_own_register_limit_and_lifecycle(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  uint16_t value = 0xbeefu;
  bool locked = true;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_lmx2592_read_register(NULL, 0u, &value));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_lmx2592_read_register(&synth, 0u, &value));
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_lmx2592_read_register(&synth, 71u, &value));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_lmx2592_read_register(&synth, 0u, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_lmx2592_is_locked(NULL, &locked));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_lmx2592_is_locked(&synth, NULL));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  TEST_ASSERT_EQUAL_HEX16(0xbeefu, value);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_lmx2592_is_locked(&synth, &locked));
}

void test_lmx2592_readback_accepts_the_last_status_register(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  uint16_t value = 0u;
  const uint8_t rx[] = {0u, 1u, 0xaau};
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  fake_esy_hal_spi_reset();
  fake_esy_hal_spi_set_rx_data(&s_ctx.spi, rx, sizeof(rx));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_read_register(&synth, 70u, &value));
  TEST_ASSERT_EQUAL_HEX16(0x01aau, value);
  TEST_ASSERT_EQUAL_HEX8(0xc6u, test_frame(1u, NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
}

void test_lmx2592_each_initial_transfer_error_cleans_up_and_allows_retry(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  const size_t count = fake_esy_hal_spi_get_tx_count(&s_ctx.spi);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
  for (size_t transfer = 0u; transfer < count; transfer++) {
    setUp();
    test_config(&config);
    fake_esy_hal_spi_fail_write_after(transfer, ESY_ERR_IO);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_lmx2592_init(&synth, &config));
    TEST_ASSERT_FALSE(esy_freq_synth_is_attached(&synth));
    TEST_ASSERT_FALSE(esy_freq_synth_runtime(&synth)->faulted);
    TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(TEST_LMX2592_CSB_PIN));
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
  }
}

void test_lmx2592_failed_initial_cleanup_retains_faulted_binding(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  test_config(&config);
  fake_esy_hal_spi_set_write_blocking_result(ESY_ERR_IO);
  TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  TEST_ASSERT_TRUE(esy_freq_synth_is_attached(&synth));
  TEST_ASSERT_TRUE(esy_freq_synth_runtime(&synth)->faulted);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_lmx2592_init(&synth, &config));
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
  TEST_ASSERT_FALSE(esy_freq_synth_is_attached(&synth));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
}

void test_lmx2592_speed_request_error_cleans_up_before_returning(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  test_config(&config);
  config.spi_speed_hz = 5000000u;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_lmx2592_init(&synth, &config));
  TEST_ASSERT_FALSE(esy_freq_synth_is_attached(&synth));
  TEST_ASSERT_FALSE(esy_freq_synth_runtime(&synth)->faulted);
  TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(TEST_LMX2592_CSB_PIN));
}

void test_lmx2592_live_reinit_and_failed_deinit_preserve_resources(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  test_config(&config);
  config.initial_output_enabled = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  const esy_freq_synth_t before = synth;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_lmx2592_init(&synth, &config));
  TEST_ASSERT_EQUAL_MEMORY(&before, &synth, sizeof(synth));
  fake_esy_hal_spi_fail_write_after(0u, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_lmx2592_deinit(&synth));
  TEST_ASSERT_TRUE(esy_freq_synth_is_attached(&synth));
  TEST_ASSERT_TRUE(esy_freq_synth_runtime(&synth)->faulted);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_lmx2592_deinit(NULL));
}

void test_lmx2592_output_io_errors_require_cleanup_before_reuse(void) {
  for (size_t edge = 0u; edge < 3u; edge++) {
    esy_freq_synth_t synth = {0};
    esy_lmx2592_config_t config;
    esy_freq_synth_info_value_t info;
    setUp();
    test_config(&config);
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
    if (edge < 2u) fake_esy_hal_gpio_fail_write_after(edge, ESY_ERR_IO);
    else fake_esy_hal_spi_fail_write_after(0u, 2);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_freq_synth_set_output_enable(
        &synth, ESY_FREQ_SYNTH_PORT_MAIN, true));
    TEST_ASSERT_TRUE(esy_freq_synth_runtime(&synth)->faulted);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_freq_synth_get_info(
        &synth, ESY_FREQ_SYNTH_INFO_OUTPUT_ENABLED, &info));
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_get_info(
        &synth, ESY_FREQ_SYNTH_INFO_OUTPUT_ENABLED, &info));
    TEST_ASSERT_EQUAL_UINT8(0u, info.u8);
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
  }
}

void test_lmx2592_partial_retune_faults_handle_and_preserves_borrowed_resources(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  esy_freq_synth_info_value_t info;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  fake_esy_hal_spi_fail_write_after(3u, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO,
      esy_freq_synth_set_frequency_hz(&synth, 3700000000ULL));
  TEST_ASSERT_TRUE(esy_freq_synth_runtime(&synth)->faulted);
  TEST_ASSERT_TRUE(esy_freq_synth_is_attached(&synth));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_freq_synth_get_info(
      &synth, ESY_FREQ_SYNTH_INFO_REQUESTED_HZ, &info));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_freq_synth_set_frequency_hz(&synth, 2100000000ULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, 2100000000ULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
}

void test_lmx2592_lock_wait_times_out_and_propagates_gpio_error(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  esy_hal_system_delay_us(800u);
  fake_esy_hal_gpio_set_read_value_for_pin(TEST_LMX2592_MUXOUT_PIN, false);
  const uint64_t start = fake_esy_hal_system_get_ctx()->time_us;
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_lmx2592_wait_for_lock(&synth, 100u));
  TEST_ASSERT_GREATER_OR_EQUAL_UINT64(
      100u, fake_esy_hal_system_get_ctx()->time_us - start);
  fake_esy_hal_gpio_set_read_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_lmx2592_wait_for_lock(&synth, 100u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
}

void test_lmx2592_lock_status_uses_guarded_gpio_without_spi_transactions(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  bool locked = true;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  fake_esy_hal_spi_reset();
  fake_esy_hal_gpio_set_read_value_for_pin(TEST_LMX2592_MUXOUT_PIN, true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_is_locked(&synth, &locked));
  TEST_ASSERT_FALSE(locked);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_lmx2592_wait_for_lock(&synth, 0u));
  esy_hal_system_delay_us(799u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_is_locked(&synth, &locked));
  TEST_ASSERT_FALSE(locked);
  esy_hal_system_delay_us(1u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_is_locked(&synth, &locked));
  TEST_ASSERT_TRUE(locked);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_wait_for_lock(&synth, 0u));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
}

void test_lmx2592_explicit_readback_restarts_lock_guard_and_preserves_gpio_errors(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  bool locked = true;
  uint16_t value = 0u;
  const uint8_t rx[] = {0u, 4u, 0u};
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  esy_hal_system_delay_us(800u);
  fake_esy_hal_gpio_set_read_value_for_pin(TEST_LMX2592_MUXOUT_PIN, true);
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_is_locked(&synth, &locked));
  TEST_ASSERT_TRUE(locked);
  fake_esy_hal_spi_set_rx_data(&s_ctx.spi, rx, sizeof(rx));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_read_register(&synth, 68u, &value));
  TEST_ASSERT_EQUAL_HEX16(0x0400u, value);
  const size_t read_count = fake_esy_hal_spi_get_tx_count(&s_ctx.spi);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_is_locked(&synth, &locked));
  TEST_ASSERT_FALSE(locked);
  TEST_ASSERT_EQUAL_UINT32(read_count, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  esy_hal_system_delay_us(800u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_is_locked(&synth, &locked));
  TEST_ASSERT_TRUE(locked);
  TEST_ASSERT_EQUAL_UINT32(read_count, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  locked = true;
  fake_esy_hal_gpio_set_read_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_lmx2592_is_locked(&synth, &locked));
  TEST_ASSERT_TRUE(locked);
  TEST_ASSERT_FALSE(esy_freq_synth_runtime(&synth)->faulted);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_lmx2592_wait_for_lock(&synth, UINT32_MAX));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
}

void test_lmx2592_calibration_guard_scales_with_reference_clock(void) {
  static const struct {
    uint32_t reference;
    uint16_t r_divider;
    uint16_t cal_divider;
    uint32_t guard_us;
  } k_cases[] = {{10000000u, 2u, 0u, 8000u},
                 {100000000u, 1u, 0u, 800u},
                 {200000000u, 2u, 1u, 800u}};
  for (size_t i = 0u; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
    esy_freq_synth_t synth = {0};
    esy_lmx2592_config_t config;
    bool locked = true;
    setUp();
    test_config(&config);
    config.ref_clk_hz = k_cases[i].reference;
    config.r_divider = k_cases[i].r_divider;
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
    if (i == 0u)
      TEST_ASSERT_EQUAL_HEX16(0x0020u, test_last_write(0u) & 0x0060u);
    TEST_ASSERT_EQUAL_UINT16(k_cases[i].cal_divider, test_last_write(1u) & 7u);
    fake_esy_hal_gpio_set_read_value_for_pin(TEST_LMX2592_MUXOUT_PIN, true);
    esy_hal_system_delay_us(k_cases[i].guard_us - 1u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_is_locked(&synth, &locked));
    TEST_ASSERT_FALSE(locked);
    esy_hal_system_delay_us(1u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_is_locked(&synth, &locked));
    TEST_ASSERT_TRUE(locked);
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
  }
}

void test_lmx2592_calibration_guard_survives_microsecond_counter_wrap(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  bool locked = true;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  fake_esy_hal_system_get_ctx()->time_us = UINT32_MAX - 799u;
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, 3144000000ULL));
  fake_esy_hal_gpio_set_read_value_for_pin(TEST_LMX2592_MUXOUT_PIN, true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_is_locked(&synth, &locked));
  TEST_ASSERT_FALSE(locked);
  esy_hal_system_delay_us(800u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_is_locked(&synth, &locked));
  TEST_ASSERT_TRUE(locked);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
}

void test_lmx2592_expired_guard_does_not_reappear_after_a_full_counter_cycle(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  bool locked = false;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  const uint64_t calibrated_at = fake_esy_hal_system_get_ctx()->time_us;
  fake_esy_hal_gpio_set_read_value_for_pin(TEST_LMX2592_MUXOUT_PIN, true);
  esy_hal_system_delay_us(800u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_is_locked(&synth, &locked));
  TEST_ASSERT_TRUE(locked);
  fake_esy_hal_system_get_ctx()->time_us =
      calibrated_at + (UINT64_C(1) << 32u) + 10u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_is_locked(&synth, &locked));
  TEST_ASSERT_TRUE(locked);
  fake_esy_hal_gpio_set_read_value_for_pin(TEST_LMX2592_MUXOUT_PIN, false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_is_locked(&synth, &locked));
  TEST_ASSERT_FALSE(locked);
  fake_esy_hal_gpio_set_read_value_for_pin(TEST_LMX2592_MUXOUT_PIN, true);
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, 3144000000ULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_is_locked(&synth, &locked));
  TEST_ASSERT_FALSE(locked);
  esy_hal_system_delay_us(800u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_is_locked(&synth, &locked));
  TEST_ASSERT_TRUE(locked);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
}

void test_lmx2592_generic_controls_validate_ports_and_power(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_freq_synth_set_output_enable(
      &synth, (esy_freq_synth_port_t)2u, true));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_freq_synth_set_power_level(
      &synth, ESY_FREQ_SYNTH_PORT_MAIN, 64u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_power_level(
      &synth, ESY_FREQ_SYNTH_PORT_MAIN, 50u));
  TEST_ASSERT_EQUAL_UINT16(50u, (test_last_write(46u) >> 8u) & 63u);
  TEST_ASSERT_BITS_HIGH(0x0080u, test_last_write(46u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_output_enable(
      &synth, ESY_FREQ_SYNTH_PORT_MAIN, true));
  TEST_ASSERT_BITS_LOW(0x0040u, test_last_write(46u));
  TEST_ASSERT_BITS_HIGH(0x0080u, test_last_write(46u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
}

void test_lmx2592_aux_output_follows_auto_divider_and_disables_its_distribution(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  test_config(&config);
  config.initial_freq_hz = 3144000000ULL;
  config.initial_output_enabled = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_output_enable(
      &synth, ESY_FREQ_SYNTH_PORT_AUX, true));
  test_capture_registers();
  TEST_ASSERT_BITS_LOW(0x00c0u, s_ctx.registers[46]);
  TEST_ASSERT_BITS_HIGH(0x0800u, s_ctx.registers[36]);
  TEST_ASSERT_EQUAL_UINT16(0u, s_ctx.registers[48] & 3u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_power_level(
      &synth, ESY_FREQ_SYNTH_PORT_AUX, 63u));
  TEST_ASSERT_EQUAL_UINT16(63u, test_last_write(47u) & 63u);
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, 3700000000ULL));
  test_capture_registers();
  TEST_ASSERT_BITS_LOW(0x00c0u, s_ctx.registers[46]);
  TEST_ASSERT_BITS_LOW(0x0400u, s_ctx.registers[31]);
  TEST_ASSERT_EQUAL_UINT16(1u, s_ctx.registers[48] & 3u);
  TEST_ASSERT_EQUAL_UINT16(63u, s_ctx.registers[47] & 63u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_output_enable(
      &synth, ESY_FREQ_SYNTH_PORT_AUX, false));
  test_capture_registers();
  TEST_ASSERT_BITS_HIGH(0x0080u, s_ctx.registers[46]);
  TEST_ASSERT_BITS_LOW(0x0040u, s_ctx.registers[46]);
  TEST_ASSERT_BITS_HIGH(0x0400u, s_ctx.registers[31]);
  TEST_ASSERT_BITS_LOW(0x0800u, s_ctx.registers[36]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
}

void test_lmx2592_shared_readonly_table_preserves_typed_identity(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2592_config_t config;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  const esy_freq_synth_vtable_t *ops = synth.ops;
  const esy_freq_synth_vtable_t foreign_ops = *ops;
  esy_freq_synth_t foreign = {0};
  foreign.ops = &foreign_ops;
  foreign.ctx = synth.ctx;
  const size_t writes = fake_esy_hal_spi_get_tx_count(&s_ctx.spi);
  uint16_t value = 0xbeefu;
  bool locked = false;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_lmx2592_read_register(&foreign, 0u, &value));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_lmx2592_is_locked(&foreign, &locked));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_lmx2592_deinit(&foreign));
  TEST_ASSERT_EQUAL_HEX16(0xbeefu, value);
  TEST_ASSERT_EQUAL(writes, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  esy_freq_synth_detach(&foreign);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
  TEST_ASSERT_NULL(synth.ops);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_init(&synth, &config));
  TEST_ASSERT_EQUAL_PTR(ops, synth.ops);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2592_deinit(&synth));
}
