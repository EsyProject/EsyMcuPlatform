/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/usb/esy_usb_cdc.h"

#include <string.h>

#include "tusb.h"
#include "unity.h"

typedef struct {
  uint32_t rx_count;
  uint32_t line_count;
  uint8_t last_itf;
  bool last_dtr;
  bool last_rts;
} test_callback_state_t;

typedef struct {
  esy_usb_cdc_t cdc0;
  esy_usb_cdc_t cdc1;
  test_callback_state_t cb_state;
} test_esy_usb_cdc_ctx_t;

static test_esy_usb_cdc_ctx_t s_ctx;

static void test_rx_cb(void *arg, uint8_t itf) {
  test_callback_state_t *state = (test_callback_state_t *)arg;

  state->rx_count++;
  state->last_itf = itf;
}

static void test_line_cb(void *arg, uint8_t itf, bool dtr, bool rts) {
  test_callback_state_t *state = (test_callback_state_t *)arg;

  state->line_count++;
  state->last_itf = itf;
  state->last_dtr = dtr;
  state->last_rts = rts;
}

static esy_usb_cdc_config_t make_cfg(uint8_t itf) {
  esy_usb_cdc_config_t cfg;

  esy_usb_cdc_default_config(&cfg);
  cfg.itf = itf;
  return cfg;
}

void setUp(void) {
  esy_usb_cdc_deinit(&s_ctx.cdc0);
  esy_usb_cdc_deinit(&s_ctx.cdc1);
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_tinyusb_reset();
}

void tearDown(void) {
  esy_usb_cdc_deinit(&s_ctx.cdc0);
  esy_usb_cdc_deinit(&s_ctx.cdc1);
}

void test_default_config_is_single_interface_without_callbacks(void) {
  esy_usb_cdc_config_t cfg;

  esy_usb_cdc_default_config(&cfg);
  esy_usb_cdc_default_config(NULL);

  TEST_ASSERT_EQUAL_UINT8(0u, cfg.itf);
  TEST_ASSERT_NULL(cfg.rx_cb);
  TEST_ASSERT_NULL(cfg.line_cb);
  TEST_ASSERT_NULL(cfg.user_arg);
  TEST_ASSERT_FALSE(cfg.discard_on_disconnect);
}

void test_init_rejects_invalid_arguments_duplicate_interface_and_duplicate_handle(void) {
  esy_usb_cdc_config_t cfg0 = make_cfg(0u);
  esy_usb_cdc_config_t cfg1 = make_cfg(1u);
  esy_usb_cdc_config_t invalid = make_cfg(2u);

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_usb_cdc_init(NULL, &cfg0));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_usb_cdc_init(&s_ctx.cdc0, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_usb_cdc_init(&s_ctx.cdc0, &invalid));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&s_ctx.cdc0, &cfg0));
  TEST_ASSERT_NOT_NULL(s_ctx.cdc0.ctx);

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_usb_cdc_init(&s_ctx.cdc1, &cfg0));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_usb_cdc_init(&s_ctx.cdc0, &cfg1));

  esy_usb_cdc_deinit(&s_ctx.cdc0);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&s_ctx.cdc0, &cfg1));
}

void test_first_init_accepts_uninitialized_and_nonzero_storage(void) {
  esy_usb_cdc_t cdc;
  esy_usb_cdc_config_t cfg = make_cfg(0u);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&cdc, &cfg));
  esy_usb_cdc_deinit(&cdc);
  memset(&cdc, 0xa5, sizeof(cdc));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&cdc, &cfg));
  esy_usb_cdc_deinit(&cdc);
}

void test_failed_first_init_preserves_storage_and_allows_cleanup_and_retry(void) {
  esy_usb_cdc_t cdc;
  unsigned char before[sizeof(cdc)];
  esy_usb_cdc_config_t cfg = make_cfg(0u);

  memset(&cdc, 0xa5, sizeof(cdc));
  memcpy(before, &cdc, sizeof(cdc));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&s_ctx.cdc0, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_usb_cdc_init(&cdc, &cfg));
  TEST_ASSERT_EQUAL_MEMORY(before, &cdc, sizeof(cdc));
  esy_usb_cdc_deinit(&cdc);
  TEST_ASSERT_NULL(cdc.ctx);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_usb_cdc_init(&cdc, &cfg));
  esy_usb_cdc_deinit(&s_ctx.cdc0);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&cdc, &cfg));
  esy_usb_cdc_deinit(&cdc);
  esy_usb_cdc_deinit(&cdc);
}

void test_rejected_reinit_preserves_original_binding_and_callbacks(void) {
  esy_usb_cdc_config_t cfg = make_cfg(0u);
  esy_usb_cdc_config_t other = make_cfg(1u);

  cfg.rx_cb = test_rx_cb;
  cfg.user_arg = &s_ctx.cb_state;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&s_ctx.cdc0, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_usb_cdc_init(&s_ctx.cdc0, &other));
  tud_cdc_rx_cb(0u);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.cb_state.rx_count);
  TEST_ASSERT_EQUAL_UINT8(0u, s_ctx.cb_state.last_itf);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&s_ctx.cdc1, &other));
}

void test_deinit_detaches_only_initialized_handles(void) {
  esy_usb_cdc_config_t cfg = make_cfg(0u);

  esy_usb_cdc_deinit(NULL);
  esy_usb_cdc_deinit(&s_ctx.cdc0);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&s_ctx.cdc0, &cfg));
  esy_usb_cdc_deinit(&s_ctx.cdc1);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_usb_cdc_init(&s_ctx.cdc1, &cfg));

  esy_usb_cdc_deinit(&s_ctx.cdc0);
  TEST_ASSERT_NULL(s_ctx.cdc0.ctx);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&s_ctx.cdc1, &cfg));
}

void test_connected_requires_tinyusb_connection_and_dtr(void) {
  esy_usb_cdc_config_t cfg = make_cfg(1u);
  fake_tinyusb_cdc_itf_state_t *itf = fake_tinyusb_cdc_get_itf(1u);

  TEST_ASSERT_FALSE(esy_usb_cdc_connected(NULL));
  TEST_ASSERT_FALSE(esy_usb_cdc_connected(&s_ctx.cdc0));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&s_ctx.cdc0, &cfg));
  TEST_ASSERT_FALSE(esy_usb_cdc_connected(&s_ctx.cdc0));

  itf->connected = true;
  TEST_ASSERT_FALSE(esy_usb_cdc_connected(&s_ctx.cdc0));

  itf->line_state = 0x01u;
  TEST_ASSERT_TRUE(esy_usb_cdc_connected(&s_ctx.cdc0));
}

void test_read_write_available_flush_and_task_forward_to_selected_interface(void) {
  const uint8_t rx_data[] = {0x11u, 0x22u, 0x33u};
  const uint8_t tx_data[] = {0x44u, 0x55u};
  uint8_t read_buf[sizeof(rx_data)] = {0};
  esy_usb_cdc_config_t cfg = make_cfg(1u);
  fake_tinyusb_cdc_itf_state_t *itf = fake_tinyusb_cdc_get_itf(1u);

  TEST_ASSERT_EQUAL_UINT32(0u, esy_usb_cdc_available(NULL));
  TEST_ASSERT_EQUAL_UINT32(0u, esy_usb_cdc_write_available(NULL));
  TEST_ASSERT_EQUAL_UINT32(0u, esy_usb_cdc_read(NULL, read_buf, sizeof(read_buf)));
  TEST_ASSERT_EQUAL_UINT32(0u, esy_usb_cdc_write(NULL, tx_data, sizeof(tx_data)));
  esy_usb_cdc_flush(NULL);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&s_ctx.cdc0, &cfg));
  fake_tinyusb_cdc_set_rx_data(1u, rx_data, sizeof(rx_data));

  TEST_ASSERT_EQUAL_UINT32(sizeof(rx_data), esy_usb_cdc_available(&s_ctx.cdc0));
  TEST_ASSERT_EQUAL_UINT32(FAKE_TINYUSB_FIFO_BYTES,
                           esy_usb_cdc_write_available(&s_ctx.cdc0));
  TEST_ASSERT_EQUAL_UINT32(0u, esy_usb_cdc_read(&s_ctx.cdc0, NULL, sizeof(read_buf)));
  TEST_ASSERT_EQUAL_UINT32(sizeof(rx_data),
                           esy_usb_cdc_read(&s_ctx.cdc0, read_buf, sizeof(read_buf)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(rx_data, read_buf, sizeof(rx_data));
  TEST_ASSERT_EQUAL_UINT8(1u, itf->last_read_itf);

  TEST_ASSERT_EQUAL_UINT32(0u, esy_usb_cdc_write(&s_ctx.cdc0, NULL, sizeof(tx_data)));
  TEST_ASSERT_EQUAL_UINT32(sizeof(tx_data),
                           esy_usb_cdc_write(&s_ctx.cdc0, tx_data, sizeof(tx_data)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(tx_data, itf->written_data, sizeof(tx_data));
  TEST_ASSERT_EQUAL_UINT8(1u, itf->last_write_itf);

  esy_usb_cdc_flush(&s_ctx.cdc0);
  TEST_ASSERT_EQUAL_UINT32(1u, itf->flush_count);
  TEST_ASSERT_EQUAL_UINT8(1u, itf->last_flush_itf);

  esy_usb_cdc_task();
  TEST_ASSERT_EQUAL_UINT32(1u, fake_tinyusb_cdc_get_itf(0u)->task_count);
}

void test_tinyusb_callbacks_dispatch_to_registered_instance(void) {
  esy_usb_cdc_config_t cfg = make_cfg(1u);

  cfg.rx_cb = test_rx_cb;
  cfg.line_cb = test_line_cb;
  cfg.user_arg = &s_ctx.cb_state;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&s_ctx.cdc0, &cfg));

  tud_cdc_rx_cb(0u);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.cb_state.rx_count);

  tud_cdc_rx_cb(1u);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.cb_state.rx_count);
  TEST_ASSERT_EQUAL_UINT8(1u, s_ctx.cb_state.last_itf);

  tud_cdc_line_state_cb(1u, true, false);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.cb_state.line_count);
  TEST_ASSERT_TRUE(s_ctx.cb_state.last_dtr);
  TEST_ASSERT_FALSE(s_ctx.cb_state.last_rts);
}

void test_multiple_interfaces_operate_independently(void) {
  const uint8_t data0[] = {0x10u};
  const uint8_t data1[] = {0x20u, 0x21u};
  uint8_t out0[sizeof(data0)] = {0};
  uint8_t out1[sizeof(data1)] = {0};
  esy_usb_cdc_config_t cfg0 = make_cfg(0u);
  esy_usb_cdc_config_t cfg1 = make_cfg(1u);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&s_ctx.cdc0, &cfg0));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&s_ctx.cdc1, &cfg1));

  fake_tinyusb_cdc_set_rx_data(0u, data0, sizeof(data0));
  fake_tinyusb_cdc_set_rx_data(1u, data1, sizeof(data1));

  TEST_ASSERT_EQUAL_UINT32(sizeof(data0),
                           esy_usb_cdc_read(&s_ctx.cdc0, out0, sizeof(out0)));
  TEST_ASSERT_EQUAL_UINT32(sizeof(data1),
                           esy_usb_cdc_read(&s_ctx.cdc1, out1, sizeof(out1)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(data0, out0, sizeof(data0));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(data1, out1, sizeof(data1));
  TEST_ASSERT_EQUAL_UINT8(0u, fake_tinyusb_cdc_get_itf(0u)->last_read_itf);
  TEST_ASSERT_EQUAL_UINT8(1u, fake_tinyusb_cdc_get_itf(1u)->last_read_itf);
}

void test_callbacks_stop_after_deinit(void) {
  esy_usb_cdc_config_t cfg = make_cfg(1u);

  cfg.rx_cb = test_rx_cb;
  cfg.line_cb = test_line_cb;
  cfg.user_arg = &s_ctx.cb_state;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&s_ctx.cdc0, &cfg));
  esy_usb_cdc_deinit(&s_ctx.cdc0);

  tud_cdc_rx_cb(1u);
  tud_cdc_line_state_cb(1u, true, true);

  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.cb_state.rx_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.cb_state.line_count);
}

void test_rx_callback_without_user_callback_preserves_data_for_polling(void) {
  const uint8_t rx_data[] = {1u, 2u, 3u, 4u, 5u};
  uint8_t out[sizeof(rx_data)] = {0};
  esy_usb_cdc_config_t cfg = make_cfg(0u);
  fake_tinyusb_cdc_itf_state_t *itf = fake_tinyusb_cdc_get_itf(0u);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&s_ctx.cdc0, &cfg));
  fake_tinyusb_cdc_set_rx_data(0u, rx_data, sizeof(rx_data));

  tud_cdc_rx_cb(0u);

  TEST_ASSERT_EQUAL_UINT32(sizeof(rx_data), itf->available);
  TEST_ASSERT_EQUAL_UINT32(0u, itf->read_count);
  TEST_ASSERT_EQUAL_UINT32(sizeof(out), esy_usb_cdc_read(&s_ctx.cdc0, out, sizeof(out)));
  TEST_ASSERT_EQUAL_MEMORY(rx_data, out, sizeof(out));
}

void test_repeated_rx_notifications_preserve_fifo_until_caller_reads(void) {
  uint8_t rx_data[130] = {0};
  uint8_t out[sizeof(rx_data)] = {0};
  esy_usb_cdc_config_t cfg = make_cfg(0u);
  fake_tinyusb_cdc_itf_state_t *itf = fake_tinyusb_cdc_get_itf(0u);

  for (uint32_t i = 0u; i < sizeof(rx_data); ++i) {
    rx_data[i] = (uint8_t)i;
  }

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&s_ctx.cdc0, &cfg));
  fake_tinyusb_cdc_set_rx_data(0u, rx_data, sizeof(rx_data));

  tud_cdc_rx_cb(0u);
  tud_cdc_rx_cb(0u);

  TEST_ASSERT_EQUAL_UINT32(sizeof(rx_data), itf->available);
  TEST_ASSERT_EQUAL_UINT32(0u, itf->read_count);
  TEST_ASSERT_EQUAL_UINT32(sizeof(out), esy_usb_cdc_read(&s_ctx.cdc0, out, sizeof(out)));
  TEST_ASSERT_EQUAL_MEMORY(rx_data, out, sizeof(out));
}

void test_io_reports_short_reads_and_writes_without_losing_pending_rx(void) {
  const uint8_t data[] = {1u, 2u, 3u};
  uint8_t out[8] = {0};
  esy_usb_cdc_config_t cfg = make_cfg(0u);
  fake_tinyusb_cdc_itf_state_t *itf = fake_tinyusb_cdc_get_itf(0u);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&s_ctx.cdc0, &cfg));
  fake_tinyusb_cdc_set_rx_data(0u, data, sizeof(data));
  TEST_ASSERT_EQUAL_UINT32(0u, esy_usb_cdc_read(&s_ctx.cdc0, out, 0u));
  TEST_ASSERT_EQUAL_UINT32(sizeof(data), esy_usb_cdc_available(&s_ctx.cdc0));
  TEST_ASSERT_EQUAL_UINT32(sizeof(data), esy_usb_cdc_read(&s_ctx.cdc0, out, sizeof(out)));
  TEST_ASSERT_EQUAL_MEMORY(data, out, sizeof(data));
  itf->write_available = 1u;
  TEST_ASSERT_EQUAL_UINT32(1u, esy_usb_cdc_write(&s_ctx.cdc0, data, sizeof(data)));
  TEST_ASSERT_EQUAL_UINT32(0u, esy_usb_cdc_write(&s_ctx.cdc0, data, sizeof(data)));
}

void test_fake_tinyusb_rejects_invalid_interfaces_without_state_changes(void) {
  const uint8_t data[] = {1u, 2u};
  uint8_t out[sizeof(data)] = {0};

  TEST_ASSERT_NULL(fake_tinyusb_cdc_get_itf(FAKE_TINYUSB_CDC_MAX_IFACES));

  fake_tinyusb_cdc_set_rx_data(FAKE_TINYUSB_CDC_MAX_IFACES, data, sizeof(data));
  TEST_ASSERT_FALSE(tud_cdc_n_connected(FAKE_TINYUSB_CDC_MAX_IFACES));
  TEST_ASSERT_EQUAL_UINT8(0u, tud_cdc_n_get_line_state(FAKE_TINYUSB_CDC_MAX_IFACES));
  TEST_ASSERT_EQUAL_UINT32(0u, tud_cdc_n_available(FAKE_TINYUSB_CDC_MAX_IFACES));
  TEST_ASSERT_EQUAL_UINT32(
      0u, tud_cdc_n_read(FAKE_TINYUSB_CDC_MAX_IFACES, out, sizeof(out)));
  TEST_ASSERT_EQUAL_UINT32(
      0u, tud_cdc_n_write(FAKE_TINYUSB_CDC_MAX_IFACES, data, sizeof(data)));

  tud_cdc_n_write_flush(FAKE_TINYUSB_CDC_MAX_IFACES);
  tud_cdc_n_read_flush(FAKE_TINYUSB_CDC_MAX_IFACES);
  TEST_ASSERT_FALSE(tud_cdc_n_write_clear(FAKE_TINYUSB_CDC_MAX_IFACES));
}

void test_default_policy_preserves_fifo_bytes_when_port_closes(void) {
  esy_usb_cdc_config_t cfg = make_cfg(0u);
  const uint8_t bytes[] = {0x91u, 0x92u};
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&s_ctx.cdc0, &cfg));
  fake_tinyusb_cdc_set_rx_data(0u, bytes, sizeof(bytes));
  TEST_ASSERT_EQUAL_UINT32(sizeof(bytes),
                           esy_usb_cdc_write(&s_ctx.cdc0, bytes, sizeof(bytes)));
  tud_cdc_line_state_cb(0u, false, false);
  esy_usb_cdc_task();
  TEST_ASSERT_EQUAL_UINT32(sizeof(bytes), esy_usb_cdc_available(&s_ctx.cdc0));
  TEST_ASSERT_EQUAL_UINT32(FAKE_TINYUSB_FIFO_BYTES - sizeof(bytes),
                           esy_usb_cdc_write_available(&s_ctx.cdc0));
  TEST_ASSERT_EQUAL_UINT32(0u, esy_usb_cdc_get_session_id(&s_ctx.cdc0));
}

void test_session_policy_purges_closed_fifo_and_keeps_reopened_rx(void) {
  esy_usb_cdc_config_t cfg = make_cfg(0u);
  const uint8_t bytes[] = {0xa1u, 0xa2u};
  cfg.discard_on_disconnect = true;
  TEST_ASSERT_EQUAL_UINT32(0u, esy_usb_cdc_get_session_id(NULL));
  TEST_ASSERT_EQUAL_UINT32(0u, esy_usb_cdc_get_session_id(&s_ctx.cdc0));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&s_ctx.cdc0, &cfg));
  fake_tinyusb_cdc_itf_state_t *itf = fake_tinyusb_cdc_get_itf(0u);
  itf->connected = true;
  itf->line_state = 1u;
  tud_cdc_line_state_cb(0u, true, false);
  uint32_t session = esy_usb_cdc_get_session_id(&s_ctx.cdc0);
  TEST_ASSERT_NOT_EQUAL(0u, session);
  fake_tinyusb_cdc_set_rx_data(0u, bytes, sizeof(bytes));
  TEST_ASSERT_EQUAL_UINT32(sizeof(bytes),
                           esy_usb_cdc_write(&s_ctx.cdc0, bytes, sizeof(bytes)));
  itf->line_state = 0u;
  tud_cdc_line_state_cb(0u, false, false);
  TEST_ASSERT_NOT_EQUAL(session, esy_usb_cdc_get_session_id(&s_ctx.cdc0));
  TEST_ASSERT_EQUAL_UINT32(0u, esy_usb_cdc_available(&s_ctx.cdc0));
  TEST_ASSERT_EQUAL_UINT32(FAKE_TINYUSB_FIFO_BYTES,
                           esy_usb_cdc_write_available(&s_ctx.cdc0));
  session = esy_usb_cdc_get_session_id(&s_ctx.cdc0);
  tud_cdc_line_state_cb(0u, false, true);
  tud_cdc_line_state_cb(0u, false, true);
  TEST_ASSERT_EQUAL_UINT32(session, esy_usb_cdc_get_session_id(&s_ctx.cdc0));
  itf->line_state = 1u;
  tud_cdc_line_state_cb(0u, true, false);
  fake_tinyusb_cdc_set_rx_data(0u, bytes, sizeof(bytes));
  esy_usb_cdc_task();
  TEST_ASSERT_EQUAL_UINT32(sizeof(bytes), esy_usb_cdc_available(&s_ctx.cdc0));
  TEST_ASSERT_EQUAL_UINT32(session, esy_usb_cdc_get_session_id(&s_ctx.cdc0));
  tud_cdc_line_state_cb(0u, true, true);
  TEST_ASSERT_EQUAL_UINT32(session, esy_usb_cdc_get_session_id(&s_ctx.cdc0));
}

void test_session_policy_observes_physical_disconnect_and_drops_closed_rx(void) {
  esy_usb_cdc_config_t cfg = make_cfg(0u);
  const uint8_t bytes[] = {0xb1u};
  cfg.discard_on_disconnect = true;
  cfg.rx_cb = test_rx_cb;
  cfg.user_arg = &s_ctx.cb_state;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&s_ctx.cdc0, &cfg));
  fake_tinyusb_cdc_itf_state_t *itf = fake_tinyusb_cdc_get_itf(0u);
  itf->connected = true;
  itf->line_state = 1u;
  esy_usb_cdc_task();
  uint32_t session = esy_usb_cdc_get_session_id(&s_ctx.cdc0);
  itf->connected = false;
  fake_tinyusb_cdc_set_rx_data(0u, bytes, sizeof(bytes));
  esy_usb_cdc_task();
  TEST_ASSERT_NOT_EQUAL(session, esy_usb_cdc_get_session_id(&s_ctx.cdc0));
  TEST_ASSERT_EQUAL_UINT32(0u, esy_usb_cdc_available(&s_ctx.cdc0));
  session = esy_usb_cdc_get_session_id(&s_ctx.cdc0);
  fake_tinyusb_cdc_set_rx_data(0u, bytes, sizeof(bytes));
  tud_cdc_rx_cb(0u);
  TEST_ASSERT_EQUAL_UINT32(0u, esy_usb_cdc_available(&s_ctx.cdc0));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.cb_state.rx_count);
  esy_usb_cdc_task();
  TEST_ASSERT_EQUAL_UINT32(session, esy_usb_cdc_get_session_id(&s_ctx.cdc0));
}
