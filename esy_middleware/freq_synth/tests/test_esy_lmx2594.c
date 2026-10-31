/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_freq_synth_internal.h"

#include "esy_middleware/freq_synth/esy_lmx2594.h"

#include <string.h>

#include "fake_esy_hal_gpio.h"
#include "fake_esy_hal_spi.h"
#include "fake_esy_hal_system.h"
#include "unity.h"

#define TEST_LMX2594_CSB_PIN 22u
#define TEST_LMX2594_MUXOUT_PIN 28u
#define TEST_LMX2594_REGISTER_COUNT 113u

typedef struct {
  esy_spi_t spi;
  esy_gpio_t cs_gpio;
  esy_gpio_t muxout_gpio;
  uint16_t registers[TEST_LMX2594_REGISTER_COUNT];
  bool written[TEST_LMX2594_REGISTER_COUNT];
} lmx2594_test_ctx_t;

static lmx2594_test_ctx_t s_ctx;

static void test_config(esy_lmx2594_config_t *config) {
  esy_gpio_config_t gpio_config;
  if (esy_hal_gpio_is_initialized(&s_ctx.cs_gpio)) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(&s_ctx.cs_gpio));
  }
  if (esy_hal_gpio_is_initialized(&s_ctx.muxout_gpio)) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(&s_ctx.muxout_gpio));
  }

  esy_hal_gpio_default_cfg(&gpio_config);
  gpio_config.pin = TEST_LMX2594_CSB_PIN;
  gpio_config.mode = ESY_GPIO_MODE_OUTPUT;
  gpio_config.output_value = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_init(&s_ctx.cs_gpio, &gpio_config));
  gpio_config.pin = TEST_LMX2594_MUXOUT_PIN;
  gpio_config.mode = ESY_GPIO_MODE_INPUT;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_init(&s_ctx.muxout_gpio, &gpio_config));
  esy_lmx2594_default_config(config);
  config->spi = &s_ctx.spi;
  config->spi_speed_hz = 0u;
  config->cs_gpio = &s_ctx.cs_gpio;
  config->muxout_gpio = &s_ctx.muxout_gpio;
  config->ref_clk_hz = 100000000u;
  config->r_divider = 1u;
  config->output_divider = 4u;
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
    TEST_ASSERT_LESS_THAN_UINT8(TEST_LMX2594_REGISTER_COUNT, command);
    s_ctx.registers[command] = value;
    s_ctx.written[command] = true;
  }
}

static void test_assert_frequency(esy_freq_synth_t *synth, uint64_t expected,
                                  uint32_t reference) {
  static const uint16_t k_channel_dividers[] = {
      2u, 4u, 6u, 8u, 12u, 16u, 24u, 32u, 48u,
      64u, 72u, 96u, 128u, 192u, 256u, 384u, 512u, 768u};
  esy_freq_synth_info_value_t info;
  test_capture_registers();
  const uint32_t integer = ((uint32_t)(s_ctx.registers[34] & 7u) << 16u) |
                           s_ctx.registers[36];
  const uint32_t denominator = ((uint32_t)s_ctx.registers[38] << 16u) |
                               s_ctx.registers[39];
  const uint32_t numerator = ((uint32_t)s_ctx.registers[42] << 16u) |
                             s_ctx.registers[43];
  const uint32_t pre_divider = s_ctx.registers[12] & 0xfffu;
  const uint32_t post_divider = (s_ctx.registers[11] >> 4u) & 0xffu;
  const uint32_t multiplier = (s_ctx.registers[10] >> 7u) & 0x1fu;
  const uint32_t doubler = (s_ctx.registers[9] & 0x1000u) != 0u ? 2u : 1u;
  const uint8_t divider_index = (s_ctx.registers[75] >> 6u) & 0x1fu;
  TEST_ASSERT_NOT_EQUAL(0u, denominator);
  TEST_ASSERT_LESS_THAN_UINT32(denominator, numerator);
  TEST_ASSERT_NOT_EQUAL(0u, pre_divider);
  TEST_ASSERT_NOT_EQUAL(0u, post_divider);
  TEST_ASSERT_EQUAL_UINT32(1u, multiplier);
  TEST_ASSERT_LESS_THAN_UINT8(sizeof(k_channel_dividers) /
                                sizeof(k_channel_dividers[0]), divider_index);
  const uint16_t divider = ((s_ctx.registers[45] >> 11u) & 3u) == 1u
                               ? 1u : k_channel_dividers[divider_index];
  const long double pfd = (long double)reference * doubler * multiplier /
                          pre_divider / post_divider;
  const long double actual = pfd *
      (integer + (long double)numerator / denominator) / divider;
  TEST_ASSERT_TRUE_MESSAGE(actual - expected <= 1.0L &&
                               actual - expected >= -1.0L,
                           "Register ratio differs from the requested frequency");
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_get_info(
      synth, ESY_FREQ_SYNTH_INFO_ACTUAL_HZ, &info));
  TEST_ASSERT_TRUE_MESSAGE(actual - info.u64 <= 1.0L &&
                               actual - info.u64 >= -1.0L,
                           "Reported frequency differs from the register ratio");
  TEST_ASSERT_EQUAL_UINT16(4u, divider);
  TEST_ASSERT_EQUAL_UINT16(0u, (s_ctx.registers[45] >> 11u) & 3u);
  TEST_ASSERT_BITS_HIGH(0x4000u, s_ctx.registers[31]);
  TEST_ASSERT_BITS_HIGH(0x0080u, s_ctx.registers[44]);
  TEST_ASSERT_BITS_LOW(0xc000u, s_ctx.registers[0]);
  TEST_ASSERT_BITS_LOW(0x0008u, s_ctx.registers[71]);
  const uint64_t vco_hz = expected * divider;
  const uint16_t pfd_delay = (s_ctx.registers[37] >> 8u) & 0x3fu;
  if (numerator != 0u) {
    TEST_ASSERT_NOT_EQUAL(0u, s_ctx.registers[44] & 7u);
    TEST_ASSERT_BITS_HIGH(0x0020u, s_ctx.registers[44]);
    TEST_ASSERT_EQUAL_UINT16(vco_hz <= 10000000000ULL ? 3u : 4u, pfd_delay);
  } else {
    TEST_ASSERT_EQUAL_UINT16(vco_hz <= 12500000000ULL ? 1u : 2u, pfd_delay);
  }
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_hal_gpio_reset();
  fake_esy_hal_spi_reset();
  fake_esy_hal_system_reset();
}

void tearDown(void) {}

void test_lmx2594_initialization_resets_then_loads_descending_registers(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  uint16_t value;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  TEST_ASSERT_EQUAL_UINT8(0u, test_frame(0u, &value));
  TEST_ASSERT_BITS_HIGH(2u, value);
  TEST_ASSERT_EQUAL_UINT8(0u, test_frame(1u, &value));
  TEST_ASSERT_BITS_LOW(2u, value);
  uint8_t previous = test_frame(2u, NULL);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT8(78u, previous);
  size_t index = 3u;
  while (previous != 0u) {
    const uint8_t address = test_frame(index++, NULL);
    TEST_ASSERT_EQUAL_UINT8(previous - 1u, address);
    previous = address;
  }
  TEST_ASSERT_GREATER_OR_EQUAL_UINT64(
      10000u, fake_esy_hal_system_get_ctx()->time_us);
  TEST_ASSERT_EQUAL_UINT8(0u, test_frame(index, &value));
  TEST_ASSERT_BITS_HIGH(0x000cu, value);
  TEST_ASSERT_BITS_LOW(0x8003u, value);
  TEST_ASSERT_BITS_HIGH(0x00c0u, test_last_write(44u));
  TEST_ASSERT_EQUAL_UINT16(4u, (test_last_write(14u) >> 4u) & 7u);
  TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(TEST_LMX2594_CSB_PIN));
  test_assert_frequency(&synth, config.initial_freq_hz, config.ref_clk_hz);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_register_ratios_cover_band_edges_and_101_fractional_steps(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  test_assert_frequency(&synth, 2000000000ULL, config.ref_clk_hz);
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, 3700000000ULL));
  test_assert_frequency(&synth, 3700000000ULL, config.ref_clk_hz);
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, 3699700000ULL));
  test_assert_frequency(&synth, 3699700000ULL, config.ref_clk_hz);
  for (uint32_t point = 0u; point < 101u; point++) {
    const uint64_t frequency = 2000000000ULL + (uint64_t)point * 300000u;
    fake_esy_hal_spi_reset();
    TEST_ASSERT_EQUAL(ESY_OK,
        esy_freq_synth_set_frequency_hz(&synth, frequency));
    const size_t count = fake_esy_hal_spi_get_tx_count(&s_ctx.spi);
    uint16_t r0;
    TEST_ASSERT_GREATER_THAN_UINT32(0u, count);
    TEST_ASSERT_EQUAL_UINT8(0u, test_frame(count - 1u, &r0));
    TEST_ASSERT_BITS_HIGH(0x0008u, r0);
    test_assert_frequency(&synth, frequency, config.ref_clk_hz);
  }
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_reference_divider_preserves_exact_frequency_ratio(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  test_config(&config);
  config.ref_clk_hz = 100000002u;
  config.r_divider = 3u;
  config.initial_freq_hz = 2000123456ULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  test_assert_frequency(&synth, config.initial_freq_hz, config.ref_clk_hz);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_low_pfd_reference_programs_calibration_adjustment(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  test_config(&config);
  config.ref_clk_hz = 10000000u;
  config.r_divider = 2u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  TEST_ASSERT_EQUAL_HEX16(0x0020u, test_last_write(0u) & 0x0060u);
  TEST_ASSERT_EQUAL_UINT16(0u, test_last_write(1u) & 7u);
  test_assert_frequency(&synth, config.initial_freq_hz, config.ref_clk_hz);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_invalid_configuration_leaves_empty_handle_without_io(void) {
  for (size_t invalid = 0u; invalid < 14u; invalid++) {
    esy_freq_synth_t synth = {0};
    esy_lmx2594_config_t config;
    setUp();
    test_config(&config);
    switch (invalid) {
      case 0u: config.spi = NULL; break;
      case 1u: config.cs_gpio = NULL; break;
      case 2u: config.ref_clk_hz = 0u; break;
      case 3u: config.r_divider = 0u; break;
      case 4u: config.output_divider = 3u; break;
      case 5u: config.initial_freq_hz = 0u; break;
      case 6u: config.initial_freq_hz = 4000000000ULL; break;
      case 7u: config.charge_pump_gain = 2u; break;
      case 8u: config.charge_pump_gain = 6u; break;
      case 9u: config.output_a_power = 64u; break;
      case 10u: config.output_b_power = 64u; break;
      case 11u: config.spi_speed_hz = 5000001u; break;
      case 12u: config.muxout_gpio = NULL; break;
      default: config.r_divider = 3u; break;
    }
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_lmx2594_init(&synth, &config));
    TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
    TEST_ASSERT_FALSE(esy_freq_synth_is_attached(&synth));
  }
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_lmx2594_init(NULL, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_lmx2594_init(&synth, NULL));
  esy_lmx2594_default_config(NULL);
}

void test_lmx2594_readback_uses_24bit_frame_then_restores_lock_detect(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  uint16_t value = 0u;
  uint16_t r0;
  const uint8_t rx[] = {0u, 0xa5u, 0x5au};
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  fake_esy_hal_spi_reset();
  fake_esy_hal_spi_set_rx_data(&s_ctx.spi, rx, sizeof(rx));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_read_register(&synth, 112u, &value));
  TEST_ASSERT_EQUAL_HEX16(0xa55au, value);
  TEST_ASSERT_EQUAL_UINT8(0u, test_frame(0u, &r0));
  TEST_ASSERT_BITS_LOW(0x000cu, r0);
  TEST_ASSERT_EQUAL_HEX8(0xf0u, test_frame(1u, NULL));
  TEST_ASSERT_EQUAL_UINT8(0u, test_frame(2u, &r0));
  TEST_ASSERT_BITS_HIGH(0x0004u, r0);
  TEST_ASSERT_BITS_LOW(0x0008u, r0);
  TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(TEST_LMX2594_CSB_PIN));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_read_error_preserves_destination_and_restores_muxout(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  uint16_t value = 0xbeefu;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  fake_esy_hal_spi_reset();
  fake_esy_hal_spi_fail_read_after(0u, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO,
      esy_lmx2594_read_register(&synth, 110u, &value));
  TEST_ASSERT_EQUAL_HEX16(0xbeefu, value);
  TEST_ASSERT_BITS_HIGH(0x0004u, test_last_write(0u));
  TEST_ASSERT_BITS_LOW(0x0008u, test_last_write(0u));
  TEST_ASSERT_FALSE(esy_freq_synth_runtime(&synth)->faulted);
  TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(TEST_LMX2594_CSB_PIN));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_failed_muxout_restore_faults_handle_until_cleanup(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  uint16_t value = 0xbeefu;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  fake_esy_hal_spi_reset();
  fake_esy_hal_spi_fail_write_after(2u, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO,
      esy_lmx2594_read_register(&synth, 110u, &value));
  TEST_ASSERT_EQUAL_HEX16(0xbeefu, value);
  TEST_ASSERT_TRUE(esy_freq_synth_runtime(&synth)->faulted);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_freq_synth_set_frequency_hz(&synth, 2100000000ULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
  TEST_ASSERT_FALSE(esy_freq_synth_is_attached(&synth));
}

void test_lmx2594_readback_validates_arguments_and_lifecycle(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  uint16_t value = 0xbeefu;
  bool locked = true;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_lmx2594_read_register(NULL, 0u, &value));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_lmx2594_read_register(&synth, 0u, &value));
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_lmx2594_read_register(&synth, 113u, &value));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_lmx2594_read_register(&synth, 0u, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_lmx2594_is_locked(NULL, &locked));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_lmx2594_is_locked(&synth, NULL));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  TEST_ASSERT_EQUAL_HEX16(0xbeefu, value);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_lmx2594_is_locked(&synth, &locked));
}

void test_lmx2594_each_initial_transfer_error_cleans_up_and_allows_retry(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  const size_t count = fake_esy_hal_spi_get_tx_count(&s_ctx.spi);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
  for (size_t transfer = 0u; transfer < count; transfer++) {
    setUp();
    test_config(&config);
    fake_esy_hal_spi_fail_write_after(transfer, ESY_ERR_IO);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_lmx2594_init(&synth, &config));
    TEST_ASSERT_FALSE(esy_freq_synth_is_attached(&synth));
    TEST_ASSERT_FALSE(esy_freq_synth_runtime(&synth)->faulted);
    TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(TEST_LMX2594_CSB_PIN));
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
  }
}

void test_lmx2594_failed_initial_cleanup_retains_faulted_binding(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  test_config(&config);
  fake_esy_hal_spi_set_write_blocking_result(ESY_ERR_IO);
  TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  TEST_ASSERT_TRUE(esy_freq_synth_is_attached(&synth));
  TEST_ASSERT_TRUE(esy_freq_synth_runtime(&synth)->faulted);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_lmx2594_init(&synth, &config));
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
  TEST_ASSERT_FALSE(esy_freq_synth_is_attached(&synth));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_speed_request_error_cleans_up_before_returning(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  test_config(&config);
  config.spi_speed_hz = 5000000u;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_lmx2594_init(&synth, &config));
  TEST_ASSERT_FALSE(esy_freq_synth_is_attached(&synth));
  TEST_ASSERT_FALSE(esy_freq_synth_runtime(&synth)->faulted);
  TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(TEST_LMX2594_CSB_PIN));
}

void test_lmx2594_live_reinit_and_failed_deinit_preserve_resources(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  test_config(&config);
  config.initial_output_enabled = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  const esy_freq_synth_t before = synth;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_lmx2594_init(&synth, &config));
  TEST_ASSERT_EQUAL_MEMORY(&before, &synth, sizeof(synth));
  fake_esy_hal_spi_fail_write_after(0u, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_lmx2594_deinit(&synth));
  TEST_ASSERT_TRUE(esy_freq_synth_is_attached(&synth));
  TEST_ASSERT_TRUE(esy_freq_synth_runtime(&synth)->faulted);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_lmx2594_deinit(NULL));
}

void test_lmx2594_output_io_errors_require_cleanup_before_reuse(void) {
  for (size_t edge = 0u; edge < 3u; edge++) {
    esy_freq_synth_t synth = {0};
    esy_lmx2594_config_t config;
    esy_freq_synth_info_value_t info;
    setUp();
    test_config(&config);
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
    if (edge < 2u) fake_esy_hal_gpio_fail_write_after(edge, ESY_ERR_IO);
    else fake_esy_hal_spi_fail_write_after(0u, 2);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_freq_synth_set_output_enable(
        &synth, ESY_FREQ_SYNTH_PORT_MAIN, true));
    TEST_ASSERT_TRUE(esy_freq_synth_runtime(&synth)->faulted);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_freq_synth_get_info(
        &synth, ESY_FREQ_SYNTH_INFO_OUTPUT_ENABLED, &info));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_freq_synth_set_output_enable(
        &synth, ESY_FREQ_SYNTH_PORT_MAIN, true));
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_get_info(
        &synth, ESY_FREQ_SYNTH_INFO_OUTPUT_ENABLED, &info));
    TEST_ASSERT_EQUAL_UINT8(0u, info.u8);
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
  }
}

void test_lmx2594_frequency_error_requires_cleanup_before_reuse(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  esy_freq_synth_info_value_t info;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  fake_esy_hal_spi_fail_write_after(0u, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO,
      esy_freq_synth_set_frequency_hz(&synth, 2100000000ULL));
  TEST_ASSERT_TRUE(esy_freq_synth_runtime(&synth)->faulted);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_freq_synth_get_info(
      &synth, ESY_FREQ_SYNTH_INFO_REQUESTED_HZ, &info));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_freq_synth_set_frequency_hz(&synth, 2100000000ULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, 2100000000ULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_lock_wait_times_out_and_propagates_gpio_error(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  bool locked = true;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  fake_esy_hal_gpio_set_read_value_for_pin(TEST_LMX2594_MUXOUT_PIN, false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_FALSE(locked);
  const uint64_t start = fake_esy_hal_system_get_ctx()->time_us;
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_lmx2594_wait_for_lock(&synth, 100u));
  TEST_ASSERT_GREATER_OR_EQUAL_UINT64(
      100u, fake_esy_hal_system_get_ctx()->time_us - start);
  fake_esy_hal_gpio_set_read_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_lmx2594_wait_for_lock(&synth, 100u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_lock_status_uses_guarded_gpio_without_spi_transactions(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  bool locked = true;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  fake_esy_hal_spi_reset();
  fake_esy_hal_gpio_set_read_value_for_pin(TEST_LMX2594_MUXOUT_PIN, true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_FALSE(locked);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  esy_hal_system_delay_us(50u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_TRUE(locked);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_expired_guard_does_not_reappear_after_a_full_counter_cycle(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  bool locked = false;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  const uint64_t calibrated_at = fake_esy_hal_system_get_ctx()->time_us;
  fake_esy_hal_gpio_set_read_value_for_pin(TEST_LMX2594_MUXOUT_PIN, true);
  esy_hal_system_delay_us(50u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_TRUE(locked);
  fake_esy_hal_system_get_ctx()->time_us =
      calibrated_at + (UINT64_C(1) << 32u) + 10u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_TRUE(locked);
  fake_esy_hal_gpio_set_read_value_for_pin(TEST_LMX2594_MUXOUT_PIN, false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_FALSE(locked);
  fake_esy_hal_gpio_set_read_value_for_pin(TEST_LMX2594_MUXOUT_PIN, true);
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, 2100000000ULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_FALSE(locked);
  esy_hal_system_delay_us(49u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_FALSE(locked);
  esy_hal_system_delay_us(1u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_TRUE(locked);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_explicit_readback_restarts_lock_guard_and_preserves_gpio_errors(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  bool locked = true;
  uint16_t value = 0u;
  const uint8_t rx[] = {0u, 4u, 0u};
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  esy_hal_system_delay_us(50u);
  fake_esy_hal_gpio_set_read_value_for_pin(TEST_LMX2594_MUXOUT_PIN, true);
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_TRUE(locked);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  fake_esy_hal_spi_set_rx_data(&s_ctx.spi, rx, sizeof(rx));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_read_register(&synth, 110u, &value));
  TEST_ASSERT_EQUAL_HEX16(0x0400u, value);
  const size_t read_count = fake_esy_hal_spi_get_tx_count(&s_ctx.spi);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_FALSE(locked);
  TEST_ASSERT_EQUAL_UINT32(read_count, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  esy_hal_system_delay_us(50u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_TRUE(locked);
  TEST_ASSERT_EQUAL_UINT32(read_count, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  locked = true;
  fake_esy_hal_gpio_set_read_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_TRUE(locked);
  TEST_ASSERT_FALSE(esy_freq_synth_runtime(&synth)->faulted);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_lmx2594_wait_for_lock(&synth, UINT32_MAX));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_generic_controls_validate_ports_and_power(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_freq_synth_set_output_enable(
      &synth, (esy_freq_synth_port_t)2u, true));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_freq_synth_set_power_level(
      &synth, ESY_FREQ_SYNTH_PORT_MAIN, 64u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_power_level(
      &synth, ESY_FREQ_SYNTH_PORT_MAIN, 50u));
  TEST_ASSERT_BITS_HIGH(0x0080u, test_last_write(44u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_output_enable(
      &synth, ESY_FREQ_SYNTH_PORT_MAIN, true));
  TEST_ASSERT_BITS_LOW(0x0040u, test_last_write(44u));
  TEST_ASSERT_BITS_HIGH(0x0080u, test_last_write(44u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_aux_output_routes_before_enable_and_powers_down_before_reroute(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  uint16_t value;
  test_config(&config);
  config.initial_output_enabled = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_output_enable(
      &synth, ESY_FREQ_SYNTH_PORT_AUX, true));
  TEST_ASSERT_EQUAL_UINT8(46u, test_frame(0u, &value));
  TEST_ASSERT_BITS_LOW(3u, value);
  TEST_ASSERT_EQUAL_UINT8(44u, test_frame(1u, &value));
  TEST_ASSERT_BITS_LOW(0x00c0u, value);
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_power_level(
      &synth, ESY_FREQ_SYNTH_PORT_AUX, 63u));
  TEST_ASSERT_EQUAL_UINT16(63u, test_last_write(45u) & 0x003fu);
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_output_enable(
      &synth, ESY_FREQ_SYNTH_PORT_AUX, false));
  TEST_ASSERT_EQUAL_UINT8(44u, test_frame(0u, &value));
  TEST_ASSERT_BITS_HIGH(0x0080u, value);
  TEST_ASSERT_BITS_LOW(0x0040u, value);
  TEST_ASSERT_EQUAL_UINT8(46u, test_frame(1u, &value));
  TEST_ASSERT_EQUAL_UINT16(1u, value & 3u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_shared_readonly_table_preserves_typed_identity(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  const esy_freq_synth_vtable_t *ops = synth.ops;
  const esy_freq_synth_vtable_t foreign_ops = *ops;
  esy_freq_synth_t foreign = {0};
  foreign.ops = &foreign_ops;
  foreign.ctx = synth.ctx;
  const size_t writes = fake_esy_hal_spi_get_tx_count(&s_ctx.spi);
  uint16_t value = 0xbeefu;
  bool locked = false;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_lmx2594_read_register(&foreign, 0u, &value));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_lmx2594_is_locked(&foreign, &locked));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_lmx2594_deinit(&foreign));
  TEST_ASSERT_EQUAL_HEX16(0xbeefu, value);
  TEST_ASSERT_EQUAL(writes, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  esy_freq_synth_detach(&foreign);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
  TEST_ASSERT_NULL(synth.ops);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  TEST_ASSERT_EQUAL_PTR(ops, synth.ops);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

static void test_assert_incremental_frames(const uint8_t *addresses,
                                           size_t count) {
  TEST_ASSERT_EQUAL_UINT32(count, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  for (size_t index = 0u; index < count; index++)
    TEST_ASSERT_EQUAL_UINT8(addresses[index], test_frame(index, NULL));
  uint16_t r0;
  TEST_ASSERT_EQUAL_UINT8(0u, test_frame(count - 1u, &r0));
  TEST_ASSERT_BITS_HIGH(0x0008u, r0);
}

void test_lmx2594_identical_frequency_keeps_fcal_and_rearms_lock_guard(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  bool locked = false;
  static const uint8_t k_same[] = {0u};
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  fake_esy_hal_gpio_set_read_value_for_pin(TEST_LMX2594_MUXOUT_PIN, true);
  esy_hal_system_delay_us(50u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_TRUE(locked);
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, config.initial_freq_hz));
  test_assert_incremental_frames(k_same, sizeof(k_same));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_FALSE(locked);
  esy_hal_system_delay_us(49u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_FALSE(locked);
  esy_hal_system_delay_us(1u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_TRUE(locked);
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_freq_synth_set_frequency_hz(&synth, 4000000000ULL));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  TEST_ASSERT_FALSE(esy_freq_synth_runtime(&synth)->faulted);
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, config.initial_freq_hz));
  test_assert_incremental_frames(k_same, sizeof(k_same));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_incremental_updates_preserve_fractional_and_integer_ratios(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  static const uint8_t k_numerator_only[] = {43u, 0u};
  static const uint8_t k_to_integer[] = {36u, 39u, 43u, 37u, 44u, 0u};
  static const uint8_t k_to_fractional[] = {39u, 43u, 37u, 44u, 0u};
  test_config(&config);
  config.initial_freq_hz = 3135300000ULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  test_assert_frequency(&synth, config.initial_freq_hz, config.ref_clk_hz);
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, 3149700000ULL));
  test_assert_incremental_frames(k_numerator_only, sizeof(k_numerator_only));
  test_assert_frequency(&synth, 3149700000ULL, config.ref_clk_hz);
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, 3150000000ULL));
  test_assert_incremental_frames(k_to_integer, sizeof(k_to_integer));
  test_assert_frequency(&synth, 3150000000ULL, config.ref_clk_hz);
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, 3150300000ULL));
  test_assert_incremental_frames(k_to_fractional, sizeof(k_to_fractional));
  test_assert_frequency(&synth, 3150300000ULL, config.ref_clk_hz);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_incremental_tuning_updates_pfd_delay_and_special_vco_start(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  static const uint8_t k_pfd_boundary[] = {36u, 43u, 37u, 0u};
  test_config(&config);
  config.initial_freq_hz = 2499700000ULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  test_assert_frequency(&synth, config.initial_freq_hz, config.ref_clk_hz);
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, 2500300000ULL));
  test_assert_incremental_frames(k_pfd_boundary, sizeof(k_pfd_boundary));
  test_assert_frequency(&synth, 2500300000ULL, config.ref_clk_hz);
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, 2974900000ULL));
  test_assert_frequency(&synth, 2974900000ULL, config.ref_clk_hz);
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, 2975200000ULL));
  TEST_ASSERT_EQUAL_UINT16(4u, (test_last_write(20u) >> 11u) & 7u);
  test_assert_frequency(&synth, 2975200000ULL, config.ref_clk_hz);
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, 3025300000ULL));
  TEST_ASSERT_EQUAL_UINT16(7u, (test_last_write(20u) >> 11u) & 7u);
  test_assert_frequency(&synth, 3025300000ULL, config.ref_clk_hz);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_incremental_tuning_preserves_live_output_enable_and_power(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  esy_freq_synth_info_value_t info;
  static const uint8_t k_same[] = {0u};
  test_config(&config);
  config.initial_freq_hz = 3135300000ULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_power_level(
      &synth, ESY_FREQ_SYNTH_PORT_MAIN, 7u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_power_level(
      &synth, ESY_FREQ_SYNTH_PORT_AUX, 11u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_output_enable(
      &synth, ESY_FREQ_SYNTH_PORT_MAIN, true));
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_output_enable(
      &synth, ESY_FREQ_SYNTH_PORT_AUX, true));
  test_capture_registers();
  const uint16_t enabled_r44 = s_ctx.registers[44];
  const uint16_t auxiliary_r45 = s_ctx.registers[45];
  const uint16_t auxiliary_r46 = s_ctx.registers[46];
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, 3150000000ULL));
  TEST_ASSERT_EQUAL_HEX16(enabled_r44 & ~7u, test_last_write(44u));
  test_capture_registers();
  TEST_ASSERT_EQUAL_HEX16(auxiliary_r45, s_ctx.registers[45]);
  TEST_ASSERT_EQUAL_HEX16(auxiliary_r46, s_ctx.registers[46]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_get_info(
      &synth, ESY_FREQ_SYNTH_INFO_OUTPUT_ENABLED, &info));
  TEST_ASSERT_EQUAL_UINT8(1u, info.u8);
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_get_info(
      &synth, ESY_FREQ_SYNTH_INFO_POWER_LEVEL, &info));
  TEST_ASSERT_EQUAL_UINT8(7u, info.u8);
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_output_enable(
      &synth, ESY_FREQ_SYNTH_PORT_MAIN, false));
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_output_enable(
      &synth, ESY_FREQ_SYNTH_PORT_AUX, false));
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, 3150000000ULL));
  test_assert_incremental_frames(k_same, sizeof(k_same));
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_power_level(
      &synth, ESY_FREQ_SYNTH_PORT_MAIN, 19u));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, 3150300000ULL));
  TEST_ASSERT_BITS_HIGH(0x00c0u, test_last_write(44u));
  TEST_ASSERT_EQUAL_UINT16(19u, (test_last_write(44u) >> 8u) & 63u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_each_incremental_write_failure_requires_cleanup_and_full_reinit(void) {
  for (size_t failed_write = 0u; failed_write < 6u; failed_write++) {
    esy_freq_synth_t synth = {0};
    esy_lmx2594_config_t config;
    setUp();
    test_config(&config);
    config.initial_freq_hz = 3135300000ULL;
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
    fake_esy_hal_spi_reset();
    fake_esy_hal_spi_fail_write_after(failed_write, ESY_ERR_IO);
    TEST_ASSERT_EQUAL(ESY_ERR_IO,
        esy_freq_synth_set_frequency_hz(&synth, 3150000000ULL));
    TEST_ASSERT_EQUAL_UINT32(failed_write + 1u,
                            fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
    TEST_ASSERT_TRUE(esy_freq_synth_runtime(&synth)->faulted);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_freq_synth_set_frequency_hz(&synth, 3150000000ULL));
    TEST_ASSERT_EQUAL_UINT32(failed_write + 1u,
                            fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
    fake_esy_hal_spi_fail_write_after(0u, ESY_ERR_IO);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_lmx2594_deinit(&synth));
    TEST_ASSERT_TRUE(esy_freq_synth_is_attached(&synth));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_lmx2594_init(&synth, &config));
    fake_esy_hal_spi_reset();
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
    fake_esy_hal_spi_reset();
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
    TEST_ASSERT_EQUAL_UINT32(82u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
    fake_esy_hal_spi_reset();
    TEST_ASSERT_EQUAL(ESY_OK,
        esy_freq_synth_set_frequency_hz(&synth, config.initial_freq_hz));
    TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
    TEST_ASSERT_EQUAL_UINT8(0u, test_frame(0u, NULL));
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
  }
}

static esy_lmx2594_vco_calibration_t test_snapshot(
    const esy_lmx2594_config_t *config, uint64_t frequency_hz) {
  const esy_lmx2594_vco_calibration_t snapshot = {
      .frequency_hz = frequency_hz,
      .ref_clk_hz = config->ref_clk_hz,
      .pfd_hz = config->ref_clk_hz / config->r_divider,
      .output_divider = config->output_divider,
      .daciset = 302u, .vco_core = 5u, .capctrl = 135u,
  };
  return snapshot;
}

static void test_capture_response(uint8_t core, uint8_t cap, uint16_t amplitude) {
  const uint16_t r110 = (uint16_t)(0x0400u | ((uint16_t)core << 5u));
  const uint8_t received[] = {
      0u, (uint8_t)(r110 >> 8u), (uint8_t)r110,
      0u, 0u, cap, 0u, (uint8_t)(amplitude >> 8u), (uint8_t)amplitude,
  };
  fake_esy_hal_spi_set_rx_data(&s_ctx.spi, received, sizeof(received));
}

static void test_ready_to_capture(void) {
  fake_esy_hal_gpio_set_read_value_for_pin(TEST_LMX2594_MUXOUT_PIN, true);
  esy_hal_system_delay_us(50u);
  fake_esy_hal_spi_reset();
  test_capture_response(5u, 135u, 302u);
}

void test_lmx2594_capture_batches_readback_and_requires_relock_after_restore(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  esy_lmx2594_vco_calibration_t snapshot;
  bool locked = true;
  test_config(&config);
  config.initial_freq_hz = 3150300000ULL;
  config.r_divider = 2u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  test_ready_to_capture();
  const uint64_t start = fake_esy_hal_system_get_ctx()->time_us;
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_lmx2594_read_vco_calibration(&synth, 200u, &snapshot));
  TEST_ASSERT_EQUAL_UINT64(50u, fake_esy_hal_system_get_ctx()->time_us - start);
  TEST_ASSERT_EQUAL_UINT64(config.initial_freq_hz, snapshot.frequency_hz);
  TEST_ASSERT_EQUAL_UINT32(config.ref_clk_hz, snapshot.ref_clk_hz);
  TEST_ASSERT_EQUAL_UINT32(50000000u, snapshot.pfd_hz);
  TEST_ASSERT_EQUAL_UINT16(4u, snapshot.output_divider);
  TEST_ASSERT_EQUAL_UINT8(5u, snapshot.vco_core);
  TEST_ASSERT_EQUAL_UINT8(135u, snapshot.capctrl);
  TEST_ASSERT_EQUAL_UINT16(302u, snapshot.daciset);
  static const uint8_t k_addresses[] = {0u, 0xeeu, 0xefu, 0xf0u, 0u};
  TEST_ASSERT_EQUAL_UINT32(sizeof(k_addresses),
                          fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  for (size_t i = 0u; i < sizeof(k_addresses); i++)
    TEST_ASSERT_EQUAL_UINT8(k_addresses[i], test_frame(i, NULL));
  uint16_t r0;
  test_frame(0u, &r0);
  TEST_ASSERT_BITS_LOW(0x000cu, r0);
  test_frame(4u, &r0);
  TEST_ASSERT_BITS_HIGH(4u, r0);
  TEST_ASSERT_BITS_LOW(8u, r0);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_FALSE(locked);
  esy_hal_system_delay_us(49u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_FALSE(locked);
  esy_hal_system_delay_us(1u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_TRUE(locked);
  fake_esy_hal_system_get_ctx()->time_us += UINT64_C(1) << 32u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_TRUE(locked);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_fast_mode_skips_fcal_preserves_outputs_and_exits_on_normal_tune(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  esy_lmx2594_vco_calibration_t snapshot;
  bool locked = true;
  test_config(&config);
  config.initial_freq_hz = 3150300000ULL;
  config.initial_output_enabled = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  test_capture_registers();
  test_ready_to_capture();
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_lmx2594_read_vco_calibration(&synth, 200u, &snapshot));
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_power_level(
      &synth, ESY_FREQ_SYNTH_PORT_MAIN, 7u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_output_enable(
      &synth, ESY_FREQ_SYNTH_PORT_AUX, true));
  const uint16_t output_bits = test_last_write(44u) & ~7u;
  test_capture_registers();
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_set_frequency_fast(
      &synth, snapshot.frequency_hz, &snapshot));
  static const uint8_t k_enter[] = {16u, 19u, 20u, 8u};
  TEST_ASSERT_EQUAL_UINT32(sizeof(k_enter),
                          fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  for (size_t i = 0u; i < sizeof(k_enter); i++)
    TEST_ASSERT_EQUAL_UINT8(k_enter[i], test_frame(i, NULL));
  TEST_ASSERT_BITS_HIGH(0x4800u, test_last_write(8u));
  TEST_ASSERT_BITS_HIGH(0x0400u, test_last_write(20u));
  TEST_ASSERT_EQUAL_HEX16(0x2787u, test_last_write(19u));
  TEST_ASSERT_EQUAL_UINT16(302u, test_last_write(16u));
  esy_freq_synth_info_value_t info;
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_get_info(
      &synth, ESY_FREQ_SYNTH_INFO_OUTPUT_ENABLED, &info));
  TEST_ASSERT_EQUAL_UINT8(1u, info.u8);
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_get_info(
      &synth, ESY_FREQ_SYNTH_INFO_POWER_LEVEL, &info));
  TEST_ASSERT_EQUAL_UINT8(7u, info.u8);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_FALSE(locked);
  esy_hal_system_delay_us(50u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_TRUE(locked);
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_lmx2594_read_vco_calibration(&synth, 200u, &snapshot));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_set_frequency_fast(
      &synth, snapshot.frequency_hz, &snapshot));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_FALSE(locked);
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, snapshot.frequency_hz));
  static const uint8_t k_exit[] = {8u, 20u, 0u};
  test_assert_incremental_frames(k_exit, sizeof(k_exit));
  TEST_ASSERT_BITS_LOW(0x4800u, test_last_write(8u));
  TEST_ASSERT_BITS_LOW(0x0400u, test_last_write(20u));
  TEST_ASSERT_EQUAL_HEX16(output_bits, s_ctx.registers[44] & ~7u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_fast_hops_update_exact_ratio_and_only_changed_calibration(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  test_config(&config);
  config.initial_freq_hz = 3135300000ULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  test_assert_frequency(&synth, config.initial_freq_hz, config.ref_clk_hz);
  esy_lmx2594_vco_calibration_t first = test_snapshot(&config, 3135300000ULL);
  esy_lmx2594_vco_calibration_t second = test_snapshot(&config, 3150300000ULL);
  second.capctrl = 134u;
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_lmx2594_set_frequency_fast(&synth, first.frequency_hz, &first));
  test_capture_registers();
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_lmx2594_set_frequency_fast(&synth, second.frequency_hz, &second));
  static const uint8_t k_hop[] = {36u, 43u, 19u};
  TEST_ASSERT_EQUAL_UINT32(sizeof(k_hop),
                          fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  for (size_t i = 0u; i < sizeof(k_hop); i++)
    TEST_ASSERT_EQUAL_UINT8(k_hop[i], test_frame(i, NULL));
  test_assert_frequency(&synth, second.frequency_hz, config.ref_clk_hz);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_fast_snapshots_reject_mismatched_inputs_without_io(void) {
  esy_freq_synth_t synth = {0}, empty = {0};
  esy_lmx2594_config_t config;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  const esy_lmx2594_vco_calibration_t valid =
      test_snapshot(&config, config.initial_freq_hz);
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_lmx2594_set_frequency_fast(NULL, valid.frequency_hz, &valid));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_lmx2594_set_frequency_fast(&empty, valid.frequency_hz, &valid));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_lmx2594_set_frequency_fast(&synth, valid.frequency_hz, NULL));
  for (size_t edge = 0u; edge < 7u; edge++) {
    esy_lmx2594_vco_calibration_t invalid = valid;
    switch (edge) {
      case 0u: invalid.frequency_hz++; break;
      case 1u: invalid.ref_clk_hz++; break;
      case 2u: invalid.pfd_hz++; break;
      case 3u: invalid.output_divider++; break;
      case 4u: invalid.vco_core = 0u; break;
      case 5u: invalid.vco_core = 8u; break;
      default: invalid.daciset = 512u; break;
    }
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_lmx2594_set_frequency_fast(&synth, valid.frequency_hz, &invalid));
  }
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  TEST_ASSERT_FALSE(esy_freq_synth_runtime(&synth)->faulted);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_assistance_rejects_special_vco_interval_including_endpoints(void) {
  static const uint64_t k_unsupported[] = {
      2975000000ULL, 3000000000ULL, 3025000000ULL};
  for (size_t i = 0u; i < sizeof(k_unsupported) / sizeof(k_unsupported[0]); i++) {
    esy_freq_synth_t synth = {0};
    esy_lmx2594_config_t config;
    esy_lmx2594_vco_calibration_t out;
    setUp();
    test_config(&config);
    config.initial_freq_hz = k_unsupported[i];
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
    const esy_lmx2594_vco_calibration_t snapshot =
        test_snapshot(&config, config.initial_freq_hz);
    fake_esy_hal_spi_reset();
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED,
        esy_lmx2594_set_frequency_fast(&synth, snapshot.frequency_hz, &snapshot));
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED,
        esy_lmx2594_read_vco_calibration(&synth, 200u, &out));
    TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
    TEST_ASSERT_FALSE(esy_freq_synth_runtime(&synth)->faulted);
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
  }
}

void test_lmx2594_capture_timeouts_preserve_destination_restore_and_rearm_guard(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  esy_lmx2594_vco_calibration_t out;
  memset(&out, 0xa5, sizeof(out));
  const esy_lmx2594_vco_calibration_t before = out;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_lmx2594_read_vco_calibration(&synth, UINT32_MAX, &out));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_lmx2594_read_vco_calibration(&synth, 200u, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT,
      esy_lmx2594_read_vco_calibration(&synth, 0u, &out));
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT,
      esy_lmx2594_read_vco_calibration(&synth, 55u, &out));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  test_ready_to_capture();
  fake_esy_hal_system_get_ctx()->time_us = UINT32_MAX - 20u;
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT,
      esy_lmx2594_read_vco_calibration(&synth, 49u, &out));
  TEST_ASSERT_EQUAL_UINT32(2u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  TEST_ASSERT_EQUAL_MEMORY(&before, &out, sizeof(out));
  TEST_ASSERT_FALSE(esy_freq_synth_runtime(&synth)->faulted);
  bool locked = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_is_locked(&synth, &locked));
  TEST_ASSERT_FALSE(locked);
  test_ready_to_capture();
  fake_esy_hal_system_get_ctx()->time_us = UINT32_MAX - 20u;
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_lmx2594_read_vco_calibration(&synth, INT32_MAX, &out));
  TEST_ASSERT_EQUAL_UINT8(5u, out.vco_core);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_capture_poll_timeout_and_bad_readback_are_recoverable(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  esy_lmx2594_vco_calibration_t out;
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  test_ready_to_capture();
  fake_esy_hal_spi_set_rx_data(&s_ctx.spi, NULL, 0u);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT,
      esy_lmx2594_read_vco_calibration(&synth, 60u, &out));
  TEST_ASSERT_BITS_HIGH(4u, test_last_write(0u));
  TEST_ASSERT_FALSE(esy_freq_synth_runtime(&synth)->faulted);
  test_ready_to_capture();
  test_capture_response(0u, 135u, 302u);
  TEST_ASSERT_EQUAL(ESY_ERR_IO,
      esy_lmx2594_read_vco_calibration(&synth, 200u, &out));
  TEST_ASSERT_FALSE(esy_freq_synth_runtime(&synth)->faulted);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_capture_restores_after_each_failure_and_faults_failed_restore(void) {
  for (size_t failure = 0u; failure < 5u; failure++) {
    esy_freq_synth_t synth = {0};
    esy_lmx2594_config_t config;
    esy_lmx2594_vco_calibration_t out;
    memset(&out, 0xa5, sizeof(out));
    const esy_lmx2594_vco_calibration_t before = out;
    setUp();
    test_config(&config);
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
    test_ready_to_capture();
    fake_esy_hal_spi_fail_write_after(failure, ESY_ERR_IO);
    TEST_ASSERT_EQUAL(failure == 4u ? ESY_ERR_STATE : ESY_ERR_IO,
        esy_lmx2594_read_vco_calibration(&synth, 200u, &out));
    TEST_ASSERT_EQUAL_MEMORY(&before, &out, sizeof(out));
    TEST_ASSERT_EQUAL(failure == 4u,
                      esy_freq_synth_runtime(&synth)->faulted);
    TEST_ASSERT_BITS_HIGH(4u, test_last_write(0u));
    TEST_ASSERT_BITS_LOW(8u, test_last_write(0u));
    TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(TEST_LMX2594_CSB_PIN));
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
  }
}

void test_lmx2594_each_fast_write_failure_faults_binding_and_requires_cleanup(void) {
  for (size_t failure = 0u; failure < 9u; failure++) {
    esy_freq_synth_t synth = {0};
    esy_lmx2594_config_t config;
    setUp();
    test_config(&config);
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
    const esy_lmx2594_vco_calibration_t snapshot =
        test_snapshot(&config, 3135300000ULL);
    fake_esy_hal_spi_reset();
    fake_esy_hal_spi_fail_write_after(failure, ESY_ERR_IO);
    TEST_ASSERT_EQUAL(ESY_ERR_IO,
        esy_lmx2594_set_frequency_fast(&synth, snapshot.frequency_hz, &snapshot));
    TEST_ASSERT_TRUE(esy_freq_synth_runtime(&synth)->faulted);
    TEST_ASSERT_EQUAL_UINT32(failure + 1u,
                            fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_lmx2594_set_frequency_fast(&synth, snapshot.frequency_hz, &snapshot));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_freq_synth_set_frequency_hz(&synth, snapshot.frequency_hz));
    fake_esy_hal_spi_fail_write_after(0u, ESY_ERR_IO);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_lmx2594_deinit(&synth));
    TEST_ASSERT_TRUE(esy_freq_synth_is_attached(&synth));
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
  }
}

void test_lmx2594_each_full_assist_exit_failure_faults_before_another_tune(void) {
  for (size_t failure = 0u; failure < 3u; failure++) {
    esy_freq_synth_t synth = {0};
    esy_lmx2594_config_t config;
    setUp();
    test_config(&config);
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
    const esy_lmx2594_vco_calibration_t snapshot =
        test_snapshot(&config, config.initial_freq_hz);
    TEST_ASSERT_EQUAL(ESY_OK,
        esy_lmx2594_set_frequency_fast(&synth, snapshot.frequency_hz, &snapshot));
    fake_esy_hal_spi_reset();
    fake_esy_hal_spi_fail_write_after(failure, ESY_ERR_IO);
    TEST_ASSERT_EQUAL(ESY_ERR_IO,
        esy_freq_synth_set_frequency_hz(&synth, snapshot.frequency_hz));
    TEST_ASSERT_TRUE(esy_freq_synth_runtime(&synth)->faulted);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_freq_synth_set_frequency_hz(&synth, snapshot.frequency_hz));
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
  }
}

void test_lmx2594_capture_budget_counts_io_and_cleanup_can_extend_deadline(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  esy_lmx2594_vco_calibration_t out;
  test_config(&config);
  config.cs_high_delay_us = 5u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  test_ready_to_capture();
  const uint64_t started = fake_esy_hal_system_get_ctx()->time_us;
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT,
      esy_lmx2594_read_vco_calibration(&synth, 65u, &out));
  /* 5 us selection + 50 us guard + 2 reads + 5 us mandatory restoration. */
  TEST_ASSERT_EQUAL_UINT64(70u,
      fake_esy_hal_system_get_ctx()->time_us - started);
  TEST_ASSERT_EQUAL_UINT32(4u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  TEST_ASSERT_EQUAL_UINT8(0xefu, test_frame(2u, NULL));
  TEST_ASSERT_EQUAL_UINT8(0u, test_frame(3u, NULL));
  TEST_ASSERT_FALSE(esy_freq_synth_runtime(&synth)->faulted);
  test_ready_to_capture();
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_lmx2594_read_vco_calibration(&synth, 70u, &out));
  TEST_ASSERT_EQUAL_UINT32(5u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

static esy_err_t test_device_transition(esy_device_t *device,
                                        esy_device_pm_action_t action,
                                        esy_power_state_t system_state) {
  (void)device;
  (void)action;
  (void)system_state;
  return ESY_OK;
}

void test_lmx2594_direct_apis_balance_device_references_on_all_exit_paths(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  esy_lmx2594_vco_calibration_t out;
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  const esy_device_registry_config_t registry_config = {
      .static_resources = &resources};
  esy_device_registry_t registry = {0};
  esy_device_t device = {0};
  static const esy_device_ops_t k_ops = {.pm_action = test_device_transition};
  const esy_device_config_t device_config = {
      .name = "lmx", .ops = &k_ops,
      .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE)};
  test_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  const esy_lmx2594_vco_calibration_t snapshot =
      test_snapshot(&config, config.initial_freq_hz);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_register(&registry, &device, &device_config));
  /* Module-internal binding injection isolates reference ownership from GPIO
   * profile/PM adapter behavior already covered by lifecycle regressions. */
  esy_freq_synth_runtime(&synth)->device = &device;
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY,
      esy_lmx2594_set_frequency_fast(&synth, snapshot.frequency_hz, &snapshot));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY,
      esy_lmx2594_read_vco_calibration(&synth, 200u, &out));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
  test_ready_to_capture();
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_lmx2594_read_vco_calibration(&synth, 200u, &out));
  uint32_t usage = UINT32_MAX;
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&device, &usage));
  TEST_ASSERT_EQUAL_UINT32(0u, usage);
  test_ready_to_capture();
  fake_esy_hal_spi_fail_read_after(1u, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO,
      esy_lmx2594_read_vco_calibration(&synth, 200u, &out));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&device, &usage));
  TEST_ASSERT_EQUAL_UINT32(0u, usage);
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_lmx2594_set_frequency_fast(&synth, snapshot.frequency_hz, &snapshot));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&device, &usage));
  TEST_ASSERT_EQUAL_UINT32(0u, usage);
  esy_lmx2594_vco_calibration_t other = snapshot;
  other.capctrl++;
  fake_esy_hal_spi_fail_write_after(0u, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO,
      esy_lmx2594_set_frequency_fast(&synth, other.frequency_hz, &other));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&device, &usage));
  TEST_ASSERT_EQUAL_UINT32(0u, usage);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_lmx2594_deinit(&synth));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
  esy_freq_synth_runtime(&synth)->device = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_fast_chip_select_failures_fault_without_leaking_resources(void) {
  for (size_t edge = 0u; edge < 2u; edge++) {
    esy_freq_synth_t synth = {0};
    esy_lmx2594_config_t config;
    setUp();
    test_config(&config);
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
    const esy_lmx2594_vco_calibration_t snapshot =
        test_snapshot(&config, config.initial_freq_hz);
    fake_esy_hal_gpio_fail_write_after(edge, ESY_ERR_IO);
    TEST_ASSERT_EQUAL(ESY_ERR_IO,
        esy_lmx2594_set_frequency_fast(&synth, snapshot.frequency_hz, &snapshot));
    TEST_ASSERT_TRUE(esy_freq_synth_runtime(&synth)->faulted);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_lmx2594_set_frequency_fast(&synth, snapshot.frequency_hz, &snapshot));
    TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
    TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(TEST_LMX2594_CSB_PIN));
  }
}

void test_lmx2594_normal_calibration_uses_special_cap_start_at_inclusive_boundaries(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  test_config(&config);
  config.initial_freq_hz = 3000000000ULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  TEST_ASSERT_EQUAL_HEX16(0x0003u, test_last_write(78u));
  static const uint64_t k_frequencies[] = {
      3025000001ULL, 3025000000ULL, 2975000000ULL, 2974999999ULL};
  static const uint16_t k_cap_start[] = {0x016fu, 0x0003u, 0x0003u, 0x016fu};
  for (size_t i = 0u; i < sizeof(k_frequencies) / sizeof(k_frequencies[0]); i++) {
    fake_esy_hal_spi_reset();
    TEST_ASSERT_EQUAL(ESY_OK,
        esy_freq_synth_set_frequency_hz(&synth, k_frequencies[i]));
    test_capture_registers();
    TEST_ASSERT_EQUAL_HEX16(k_cap_start[i], s_ctx.registers[78u]);
    TEST_ASSERT_BITS_LOW(0x0200u, s_ctx.registers[78u]);
  }
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}

void test_lmx2594_special_cap_start_write_failure_faults_before_fcal(void) {
  esy_freq_synth_t synth = {0};
  esy_lmx2594_config_t config;
  test_config(&config);
  config.initial_freq_hz = 3000000000ULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_init(&synth, &config));
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, 3025000001ULL));
  size_t cap_frame = fake_esy_hal_spi_get_tx_count(&s_ctx.spi);
  for (size_t i = 0u; i < cap_frame; i++) {
    if (test_frame(i, NULL) == 78u) { cap_frame = i; break; }
  }
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_freq_synth_set_frequency_hz(&synth, config.initial_freq_hz));
  fake_esy_hal_spi_reset();
  fake_esy_hal_spi_fail_write_after(cap_frame, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO,
      esy_freq_synth_set_frequency_hz(&synth, 3025000001ULL));
  const size_t count = fake_esy_hal_spi_get_tx_count(&s_ctx.spi);
  TEST_ASSERT_EQUAL_UINT8(78u, test_frame(count - 1u, NULL));
  TEST_ASSERT_TRUE(esy_freq_synth_runtime(&synth)->faulted);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lmx2594_deinit(&synth));
}
