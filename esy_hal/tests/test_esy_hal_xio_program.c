/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_hal/xio/esy_hal_xio_program.h"
#include "fake_esy_hal_xio_program.h"
#include "hal_test_fakes.h"
#include <string.h>

typedef struct {
  esy_xio_program_t program;
  esy_xio_program_sm_t sm[2];
  uint32_t rx[3];
  unsigned callbacks;
  esy_err_t callback_status;
} xio_program_test_ctx_t;

static xio_program_test_ctx_t s_ctx;
static const uint16_t k_program[] = {UINT16_C(0xa042)};
static const uint32_t k_samples[] = {UINT32_C(0x00112233), UINT32_C(0x00445566), 7u};

void setUp(void) {
  fake_esy_hal_xio_program_reset();
  memset(&s_ctx, 0, sizeof(s_ctx));
}

void tearDown(void) {}

static void initialize(unsigned machines) {
  const esy_xio_program_config_t program = {
      .instructions = k_program, .instruction_count = 1u,
      .entry = 0u, .wrap_bottom = 0u, .wrap_top = 0u};
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_init(&s_ctx.program, &program));
  for (unsigned i = 0u; i < machines; ++i) {
    const esy_xio_program_sm_config_t config = {.instance = 0u, .sm_index = (uint8_t)i};
    TEST_ASSERT_EQUAL(ESY_OK,
        esy_hal_xio_program_sm_init(&s_ctx.sm[i], &s_ctx.program, &config));
  }
}

static void completed(void *user, esy_err_t status) {
  xio_program_test_ctx_t *ctx = user;
  ++ctx->callbacks;
  ctx->callback_status = status;
}

void test_xio_program_lifetime_keeps_program_until_every_sm_releases(void) {
  initialize(2u);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_xio_program_deinit(&s_ctx.program));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_sm_deinit(&s_ctx.sm[0]));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_xio_program_deinit(&s_ctx.program));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_sm_deinit(&s_ctx.sm[1]));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_deinit(&s_ctx.program));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_deinit(&s_ctx.program));
  initialize(1u);
}

void test_xio_program_group_rejects_duplicate_without_enabling_first_sm(void) {
  initialize(2u);
  esy_xio_program_sm_t *invalid[] = {&s_ctx.sm[0], &s_ctx.sm[0]};
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_xio_program_start_group(invalid, 2u));
  esy_xio_program_status_t status;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_get_status(&s_ctx.sm[0], &status));
  TEST_ASSERT_FALSE(status.enabled);
  esy_xio_program_sm_t *valid[] = {&s_ctx.sm[0], &s_ctx.sm[1]};
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_start_group(valid, 2u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_get_status(&s_ctx.sm[1], &status));
  TEST_ASSERT_TRUE(status.enabled);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_xio_program_sm_reset(&s_ctx.sm[0]));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_sm_stop(&s_ctx.sm[0]));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_sm_reset(&s_ctx.sm[0]));
}

void test_xio_program_failed_init_and_status_preserve_retry_and_output(void) {
  initialize(0u);
  const esy_xio_program_sm_config_t config = {.instance = 0u, .sm_index = 0u};
  fake_esy_hal_xio_program_fail_next(FAKE_XIO_PROGRAM_SM_INIT, ESY_ERR_BUSY);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY,
      esy_hal_xio_program_sm_init(&s_ctx.sm[0], &s_ctx.program, &config));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_hal_xio_program_sm_init(&s_ctx.sm[0], &s_ctx.program, &config));
  esy_xio_program_status_t status = {.pc = 19u};
  fake_esy_hal_xio_program_fail_next(FAKE_XIO_PROGRAM_STATUS, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_xio_program_get_status(&s_ctx.sm[0], &status));
  TEST_ASSERT_EQUAL_UINT8(19u, status.pc);
}

void test_xio_program_idle_writes_preserve_other_pins_and_reset_preserves_levels(void) {
  initialize(1u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_set_pindirs(&s_ctx.sm[0], 4u, 2u, true));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_write_pins(&s_ctx.sm[0], 4u, 1u, 1u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_write_pins(&s_ctx.sm[0], 5u, 1u, 1u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_write_pins(&s_ctx.sm[0], 4u, 1u, 0u));
  TEST_ASSERT_EQUAL_HEX32(1u << 5u, fake_esy_hal_xio_program_output_bits(&s_ctx.sm[0]));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_sm_reset(&s_ctx.sm[0]));
  TEST_ASSERT_EQUAL_HEX32(1u << 5u, fake_esy_hal_xio_program_output_bits(&s_ctx.sm[0]));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_irq_clear(&s_ctx.sm[0], 0u));
}

void test_xio_program_fifo_full_and_selected_sticky_clear_are_explicit(void) {
  initialize(1u);
  bool written = false;
  for (unsigned i = 0u; i < 4u; ++i) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_tx_try_put(&s_ctx.sm[0], i, &written));
    TEST_ASSERT_TRUE(written);
  }
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_tx_try_put(&s_ctx.sm[0], 9u, &written));
  TEST_ASSERT_FALSE(written);
  esy_xio_program_status_t status = {.tx_stall = true, .rx_stall = true};
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_xio_program_set_status(&s_ctx.sm[0], &status));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_clear_stalls(&s_ctx.sm[0], true, false));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_get_status(&s_ctx.sm[0], &status));
  TEST_ASSERT_FALSE(status.tx_stall);
  TEST_ASSERT_TRUE(status.rx_stall);
}

void test_xio_program_dma_completion_keeps_route_until_explicit_abort(void) {
  initialize(1u);
  const esy_xio_program_dma_config_t config = {
      .channel = 0u, .callback = completed, .user = &s_ctx};
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_hal_xio_program_dma_rx_start(&s_ctx.sm[0], s_ctx.rx, 3u, &config));
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_xio_program_fill_rx(&s_ctx.sm[0], k_samples, 3u));
  TEST_ASSERT_EQUAL(ESY_OK,
      fake_esy_hal_xio_program_complete_dma(&s_ctx.sm[0], ESY_XIO_PROGRAM_DMA_RX, ESY_OK));
  TEST_ASSERT_EQUAL_UINT32_ARRAY(k_samples, s_ctx.rx, 3u);
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.callbacks);
  TEST_ASSERT_EQUAL(ESY_OK, s_ctx.callback_status);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY,
      esy_hal_xio_program_dma_rx_start(&s_ctx.sm[0], s_ctx.rx, 3u, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_xio_program_sm_reset(&s_ctx.sm[0]));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_dma_abort(&s_ctx.sm[0], ESY_XIO_PROGRAM_DMA_RX));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_hal_xio_program_dma_rx_start(&s_ctx.sm[0], s_ctx.rx, 3u, &config));
}

void test_xio_program_failed_dma_abort_keeps_borrow_and_deinit_reference(void) {
  initialize(1u);
  const esy_xio_program_dma_config_t config = {.channel = 0u};
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_hal_xio_program_dma_tx_start(&s_ctx.sm[0], k_samples, 3u, &config));
  fake_esy_hal_xio_program_fail_next(FAKE_XIO_PROGRAM_DMA_ABORT, ESY_ERR_BUSY);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_xio_program_sm_deinit(&s_ctx.sm[0]));
  size_t count = 0u;
  TEST_ASSERT_EQUAL_PTR(k_samples,
      fake_esy_hal_xio_program_dma_words(&s_ctx.sm[0], ESY_XIO_PROGRAM_DMA_TX, &count));
  TEST_ASSERT_EQUAL_UINT(3u, count);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_xio_program_deinit(&s_ctx.program));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_sm_deinit(&s_ctx.sm[0]));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_deinit(&s_ctx.program));
}

void test_xio_program_dma_channels_are_exclusive_between_directions_and_sms(void) {
  initialize(2u);
  const esy_xio_program_dma_config_t config = {.channel = 0u};
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_hal_xio_program_dma_tx_start(&s_ctx.sm[0], k_samples, 3u, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY,
      esy_hal_xio_program_dma_rx_start(&s_ctx.sm[1], s_ctx.rx, 3u, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_program_dma_abort(&s_ctx.sm[0], ESY_XIO_PROGRAM_DMA_TX));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_hal_xio_program_dma_rx_start(&s_ctx.sm[1], s_ctx.rx, 3u, &config));
}
