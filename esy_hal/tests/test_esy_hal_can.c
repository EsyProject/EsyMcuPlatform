/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_hal/esy_hal_can.h"
#include "fake_esy_hal_can_provider.h"
#include "hal_test_fakes.h"

#include <string.h>

typedef struct {
  esy_can_t can;
  esy_can_config_t config;
  fake_esy_hal_can_provider_t *provider;
} esy_hal_can_test_ctx_t;

static esy_hal_can_test_ctx_t s_ctx;

void setUp(void) {
  s_ctx = (esy_hal_can_test_ctx_t){0};
  fake_esy_hal_can_provider_reset();
  s_ctx.provider = fake_esy_hal_can_provider_state();
  esy_hal_can_default_cfg(&s_ctx.config);
  s_ctx.config.phys_index = 1u;
}

void tearDown(void) {
  s_ctx.provider->results[FAKE_ESY_CAN_DEINIT] = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_deinit(&s_ctx.can));
}

static void initialize(void) {
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_init(&s_ctx.can, &s_ctx.config));
}

static void start(void) {
  initialize();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_start(&s_ctx.can));
}

static void invalid_configuration(const esy_can_config_t *config) {
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                        esy_hal_can_init(&s_ctx.can, config));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.provider->calls[FAKE_ESY_CAN_INIT]);
  TEST_ASSERT_NULL(s_ctx.can.ctx);
  TEST_ASSERT_NULL(s_ctx.can.ops);
}

void test_can_defaults_replace_every_field_without_selecting_a_board_clock(void) {
  esy_can_config_t config;
  memset(&config, 0xff, sizeof(config));
  esy_hal_can_default_cfg(NULL);
  esy_hal_can_default_cfg(&config);
  TEST_ASSERT_EQUAL_UINT8(0u, config.phys_index);
  TEST_ASSERT_EQUAL_INT(ESY_CAN_MODE_NORMAL, config.mode);
  TEST_ASSERT_FALSE(config.fd);
  TEST_ASSERT_TRUE(config.auto_retransmit);
  TEST_ASSERT_EQUAL_UINT16(1u, config.nominal.prescaler);
  TEST_ASSERT_EQUAL_UINT8(13u, config.nominal.segment1);
  TEST_ASSERT_EQUAL_UINT8(2u, config.nominal.segment2);
  TEST_ASSERT_EQUAL_UINT8(1u, config.nominal.sjw);
  TEST_ASSERT_EQUAL_UINT16(0u, config.data.prescaler);
  TEST_ASSERT_EQUAL_UINT8(0u, config.data.segment1);
  TEST_ASSERT_EQUAL_UINT8(0u, config.data.segment2);
  TEST_ASSERT_EQUAL_UINT8(0u, config.data.sjw);
  TEST_ASSERT_EQUAL_UINT32(ESY_CAN_DEFAULT_POLL_LIMIT, config.poll_limit);
}

void test_can_initialization_rejects_bad_arguments_timing_mode_and_poll_budget(void) {
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_init(NULL, &s_ctx.config));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_init(&s_ctx.can, NULL));
  esy_can_config_t config = s_ctx.config;
  config.nominal.prescaler = 0u;
  invalid_configuration(&config);
  config = s_ctx.config;
  config.nominal.segment1 = 0u;
  invalid_configuration(&config);
  config = s_ctx.config;
  config.nominal.segment2 = 0u;
  invalid_configuration(&config);
  config = s_ctx.config;
  config.nominal.sjw = 0u;
  invalid_configuration(&config);
  config = s_ctx.config;
  config.nominal.sjw = config.nominal.segment2 + 1u;
  invalid_configuration(&config);
  config = s_ctx.config;
  config.mode = (esy_can_mode_t)-1;
  invalid_configuration(&config);
  config.mode = (esy_can_mode_t)3;
  invalid_configuration(&config);
  config = s_ctx.config;
  config.poll_limit = 0u;
  invalid_configuration(&config);
  config.poll_limit = ESY_CAN_MAX_POLL_LIMIT + 1u;
  invalid_configuration(&config);
  config = s_ctx.config;
  config.fd = true;
  invalid_configuration(&config);
}

void test_can_initialization_copies_config_and_validates_only_active_timing_phase(void) {
  s_ctx.config.data.sjw = 99u;
  s_ctx.config.poll_limit = ESY_CAN_MAX_POLL_LIMIT;
  initialize();
  TEST_ASSERT_NOT_NULL(s_ctx.can.ctx);
  TEST_ASSERT_NOT_NULL(s_ctx.can.ops);
  TEST_ASSERT_EQUAL_PTR(&s_ctx.can, s_ctx.provider->owner);
  TEST_ASSERT_EQUAL_INT(ESY_CAN_STATE_STOPPED, s_ctx.provider->status.state);
  TEST_ASSERT_EQUAL_UINT8(1u, s_ctx.provider->config.phys_index);
  TEST_ASSERT_EQUAL_UINT32(ESY_CAN_MAX_POLL_LIMIT, s_ctx.provider->config.poll_limit);
  s_ctx.config.phys_index = 2u;
  TEST_ASSERT_EQUAL_UINT8(1u, s_ctx.provider->config.phys_index);
  const void *owned = s_ctx.can.ctx;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_init(&s_ctx.can, &s_ctx.config));
  TEST_ASSERT_EQUAL_PTR(owned, s_ctx.can.ctx);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.provider->calls[FAKE_ESY_CAN_INIT]);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_deinit(&s_ctx.can));
  TEST_ASSERT_NULL(s_ctx.can.ctx);
  TEST_ASSERT_NULL(s_ctx.can.ops);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_deinit(&s_ctx.can));
  s_ctx.config.fd = true;
  s_ctx.config.data = s_ctx.config.nominal;
  initialize();
  TEST_ASSERT_TRUE(s_ctx.provider->config.fd);
}

void test_can_failed_init_without_resource_claim_can_be_retried(void) {
  s_ctx.provider->results[FAKE_ESY_CAN_INIT] = ESY_ERR_NOT_SUPPORTED;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                        esy_hal_can_init(&s_ctx.can, &s_ctx.config));
  TEST_ASSERT_NULL(s_ctx.can.ctx);
  TEST_ASSERT_NULL(s_ctx.can.ops);
  s_ctx.provider->results[FAKE_ESY_CAN_INIT] = ESY_OK;
  initialize();
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.provider->calls[FAKE_ESY_CAN_INIT]);
}

void test_can_failed_init_and_failed_deinit_retain_claim_until_cleanup_succeeds(void) {
  s_ctx.provider->results[FAKE_ESY_CAN_INIT] = ESY_ERR_TIMEOUT;
  s_ctx.provider->retain_failed_init = true;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT,
                        esy_hal_can_init(&s_ctx.can, &s_ctx.config));
  void *owned = s_ctx.can.ctx;
  TEST_ASSERT_NOT_NULL(owned);
  TEST_ASSERT_NULL(s_ctx.can.ops);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_start(&s_ctx.can));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_init(&s_ctx.can, &s_ctx.config));
  s_ctx.provider->results[FAKE_ESY_CAN_DEINIT] = ESY_ERR_TIMEOUT;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT, esy_hal_can_deinit(&s_ctx.can));
  TEST_ASSERT_EQUAL_PTR(owned, s_ctx.can.ctx);
  TEST_ASSERT_EQUAL_PTR(&s_ctx.can, s_ctx.provider->owner);
  s_ctx.provider->results[FAKE_ESY_CAN_DEINIT] = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_deinit(&s_ctx.can));
  TEST_ASSERT_NULL(s_ctx.can.ctx);
  TEST_ASSERT_NULL(s_ctx.can.ops);
  TEST_ASSERT_NULL(s_ctx.provider->owner);
  s_ctx.provider->results[FAKE_ESY_CAN_INIT] = ESY_OK;
  initialize();
}

void test_can_live_deinit_failure_preserves_operations_and_resource_owner(void) {
  start();
  const esy_can_vtable_t *ops = s_ctx.can.ops;
  void *owned = s_ctx.can.ctx;
  s_ctx.provider->results[FAKE_ESY_CAN_DEINIT] = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_hal_can_deinit(&s_ctx.can));
  TEST_ASSERT_EQUAL_PTR(ops, s_ctx.can.ops);
  TEST_ASSERT_EQUAL_PTR(owned, s_ctx.can.ctx);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_init(&s_ctx.can, &s_ctx.config));
  s_ctx.provider->results[FAKE_ESY_CAN_DEINIT] = ESY_ERR_TIMEOUT;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT, esy_hal_can_deinit(&s_ctx.can));
  esy_can_status_t status = {0};
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_get_status(&s_ctx.can, &status));
  TEST_ASSERT_EQUAL_INT(ESY_CAN_STATE_FAULT, status.state);
  TEST_ASSERT_EQUAL_PTR(ops, s_ctx.can.ops);
  TEST_ASSERT_EQUAL_PTR(owned, s_ctx.can.ctx);
  TEST_ASSERT_EQUAL_PTR(&s_ctx.can, s_ctx.provider->owner);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_start(&s_ctx.can));
  s_ctx.provider->results[FAKE_ESY_CAN_DEINIT] = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_deinit(&s_ctx.can));
  TEST_ASSERT_NULL(s_ctx.can.ctx);
  TEST_ASSERT_NULL(s_ctx.can.ops);
  TEST_ASSERT_NULL(s_ctx.provider->owner);
  start();
}

void test_can_null_and_uninitialized_calls_do_not_reach_provider(void) {
  esy_can_frame_t frame = {0};
  esy_can_status_t status = {0};
  uint32_t caps = 0u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_deinit(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_start(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_stop(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_send(NULL, &frame));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_send(&s_ctx.can, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_receive(NULL, &frame));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_receive(&s_ctx.can, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_set_filters(NULL, NULL, 0u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_get_status(NULL, &status));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_get_status(&s_ctx.can, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_get_caps(NULL, &caps));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_get_caps(&s_ctx.can, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_recover(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_start(&s_ctx.can));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_stop(&s_ctx.can));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_send(&s_ctx.can, &frame));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_receive(&s_ctx.can, &frame));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_set_filters(&s_ctx.can, NULL, 0u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_get_status(&s_ctx.can, &status));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_get_caps(&s_ctx.can, &caps));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_recover(&s_ctx.can));
  for (size_t i = 0u; i < FAKE_ESY_CAN_HOOK_COUNT; ++i)
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.provider->calls[i]);
}

void test_can_start_stop_and_repeated_calls_follow_public_lifecycle(void) {
  initialize();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_start(&s_ctx.can));
  s_ctx.provider->results[FAKE_ESY_CAN_START] = ESY_ERR_TIMEOUT;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_start(&s_ctx.can));
  TEST_ASSERT_EQUAL_INT(ESY_CAN_STATE_ERROR_ACTIVE, s_ctx.provider->status.state);
  s_ctx.provider->results[FAKE_ESY_CAN_START] = ESY_OK;
  s_ctx.provider->status.tx_pending = 3u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_stop(&s_ctx.can));
  TEST_ASSERT_EQUAL_UINT8(0u, s_ctx.provider->status.tx_pending);
  s_ctx.provider->results[FAKE_ESY_CAN_STOP] = ESY_ERR_TIMEOUT;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_stop(&s_ctx.can));
  TEST_ASSERT_EQUAL_INT(ESY_CAN_STATE_STOPPED, s_ctx.provider->status.state);
  s_ctx.provider->results[FAKE_ESY_CAN_STOP] = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_start(&s_ctx.can));
}

void test_can_start_timeout_requires_successful_stop_before_restart(void) {
  initialize();
  const esy_can_vtable_t *ops = s_ctx.can.ops;
  void *owned = s_ctx.can.ctx;
  s_ctx.provider->results[FAKE_ESY_CAN_START] = ESY_ERR_TIMEOUT;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT, esy_hal_can_start(&s_ctx.can));
  esy_can_status_t status = {0};
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_get_status(&s_ctx.can, &status));
  TEST_ASSERT_EQUAL_INT(ESY_CAN_STATE_FAULT, status.state);
  TEST_ASSERT_EQUAL_PTR(ops, s_ctx.can.ops);
  TEST_ASSERT_EQUAL_PTR(owned, s_ctx.can.ctx);
  TEST_ASSERT_EQUAL_PTR(&s_ctx.can, s_ctx.provider->owner);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_start(&s_ctx.can));
  s_ctx.provider->results[FAKE_ESY_CAN_START] = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_start(&s_ctx.can));
  esy_can_frame_t frame = {.id = 0x123u};
  s_ctx.provider->results[FAKE_ESY_CAN_SEND] = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_send(&s_ctx.can, &frame));
  s_ctx.provider->results[FAKE_ESY_CAN_SEND] = ESY_OK;
  s_ctx.provider->results[FAKE_ESY_CAN_RECEIVE] = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_receive(&s_ctx.can, &frame));
  s_ctx.provider->results[FAKE_ESY_CAN_FILTERS] = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_set_filters(&s_ctx.can, NULL, 0u));
  s_ctx.provider->results[FAKE_ESY_CAN_FILTERS] = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_recover(&s_ctx.can));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_stop(&s_ctx.can));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_get_status(&s_ctx.can, &status));
  TEST_ASSERT_EQUAL_INT(ESY_CAN_STATE_STOPPED, status.state);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_start(&s_ctx.can));
}

void test_can_stop_timeout_retains_fault_and_pending_tx_until_cleanup(void) {
  start();
  s_ctx.provider->status.tx_pending = 3u;
  s_ctx.provider->results[FAKE_ESY_CAN_STOP] = ESY_ERR_TIMEOUT;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT, esy_hal_can_stop(&s_ctx.can));
  esy_can_status_t status = {0};
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_get_status(&s_ctx.can, &status));
  TEST_ASSERT_EQUAL_INT(ESY_CAN_STATE_FAULT, status.state);
  TEST_ASSERT_EQUAL_UINT8(3u, status.tx_pending);
  TEST_ASSERT_EQUAL_PTR(&s_ctx.can, s_ctx.provider->owner);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_start(&s_ctx.can));
  esy_can_frame_t frame = {.id = 0x123u};
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_send(&s_ctx.can, &frame));
  s_ctx.provider->results[FAKE_ESY_CAN_STOP] = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_stop(&s_ctx.can));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_get_status(&s_ctx.can, &status));
  TEST_ASSERT_EQUAL_INT(ESY_CAN_STATE_STOPPED, status.state);
  TEST_ASSERT_EQUAL_UINT8(0u, status.tx_pending);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_start(&s_ctx.can));
}

void test_can_recovery_timeout_requires_cleanup_before_reinitialization(void) {
  start();
  s_ctx.provider->status.state = ESY_CAN_STATE_BUS_OFF;
  s_ctx.provider->results[FAKE_ESY_CAN_RECOVER] = ESY_ERR_TIMEOUT;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT, esy_hal_can_recover(&s_ctx.can));
  esy_can_status_t status = {0};
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_get_status(&s_ctx.can, &status));
  TEST_ASSERT_EQUAL_INT(ESY_CAN_STATE_FAULT, status.state);
  TEST_ASSERT_EQUAL_PTR(&s_ctx.can, s_ctx.provider->owner);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_recover(&s_ctx.can));
  s_ctx.provider->results[FAKE_ESY_CAN_RECOVER] = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_recover(&s_ctx.can));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_start(&s_ctx.can));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_init(&s_ctx.can, &s_ctx.config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_deinit(&s_ctx.can));
  TEST_ASSERT_NULL(s_ctx.can.ctx);
  TEST_ASSERT_NULL(s_ctx.can.ops);
  TEST_ASSERT_NULL(s_ctx.provider->owner);
  start();
}

void test_can_send_rejects_identifier_length_and_flag_errors_before_dispatch(void) {
  start();
  esy_can_frame_t frame = {.id = ESY_CAN_STD_ID_MAX + 1u};
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_send(&s_ctx.can, &frame));
  frame.extended = true;
  frame.id = ESY_CAN_EXT_ID_MAX + 1u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_send(&s_ctx.can, &frame));
  frame.id = 0u;
  frame.length = ESY_CAN_CLASSIC_MAX_DATA + 1u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_send(&s_ctx.can, &frame));
  frame.length = 0u;
  frame.brs = true;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_send(&s_ctx.can, &frame));
  frame.fd = true;
  frame.rtr = true;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_send(&s_ctx.can, &frame));
  frame.rtr = false;
  frame.esi = true;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_send(&s_ctx.can, &frame));
  frame.esi = false;
  for (uint8_t length = 9u; length < ESY_CAN_FD_MAX_DATA; ++length) {
    if (length == 12u || length == 16u || length == 20u || length == 24u ||
        length == 32u || length == 48u) continue;
    frame.length = length;
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_send(&s_ctx.can, &frame));
  }
  frame.length = ESY_CAN_FD_MAX_DATA + 1u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_can_send(&s_ctx.can, &frame));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.provider->calls[FAKE_ESY_CAN_SEND]);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.provider->calls[FAKE_ESY_CAN_CAPS]);
}

void test_can_send_accepts_classic_boundaries_and_copies_before_return(void) {
  start();
  esy_can_frame_t frame = {.id = ESY_CAN_STD_ID_MAX};
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_send(&s_ctx.can, &frame));
  frame.extended = true;
  frame.id = ESY_CAN_EXT_ID_MAX;
  frame.length = ESY_CAN_CLASSIC_MAX_DATA;
  for (uint8_t i = 0u; i < frame.length; ++i) frame.data[i] = i + 1u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_send(&s_ctx.can, &frame));
  memset(frame.data, 0u, sizeof(frame.data));
  TEST_ASSERT_EQUAL_UINT8(1u, s_ctx.provider->tx_frame.data[0]);
  TEST_ASSERT_EQUAL_UINT8(8u, s_ctx.provider->tx_frame.data[7]);
  frame.rtr = true;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_send(&s_ctx.can, &frame));
  TEST_ASSERT_TRUE(s_ctx.provider->tx_frame.rtr);
  s_ctx.provider->results[FAKE_ESY_CAN_SEND] = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_hal_can_send(&s_ctx.can, &frame));
  s_ctx.provider->results[FAKE_ESY_CAN_SEND] = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_hal_can_send(&s_ctx.can, &frame));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.provider->calls[FAKE_ESY_CAN_CAPS]);
}

void test_can_fd_dispatch_requires_capabilities_and_accepts_only_canonical_lengths(void) {
  s_ctx.config.fd = true;
  s_ctx.config.data = s_ctx.config.nominal;
  start();
  esy_can_frame_t frame = {.fd = true, .length = 12u};
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_hal_can_send(&s_ctx.can, &frame));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.provider->calls[FAKE_ESY_CAN_SEND]);
  s_ctx.provider->caps |= ESY_CAN_CAP_FD;
  frame.brs = true;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_hal_can_send(&s_ctx.can, &frame));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.provider->calls[FAKE_ESY_CAN_SEND]);
  s_ctx.provider->caps |= ESY_CAN_CAP_BRS;
  static const uint8_t k_lengths[] = {0u, 8u, 12u, 16u, 20u, 24u, 32u, 48u, 64u};
  for (size_t i = 0u; i < sizeof(k_lengths); ++i) {
    frame.length = k_lengths[i];
    TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_send(&s_ctx.can, &frame));
    TEST_ASSERT_EQUAL_UINT8(k_lengths[i], s_ctx.provider->tx_frame.length);
  }
  s_ctx.provider->results[FAKE_ESY_CAN_CAPS] = ESY_ERR_IO;
  uint32_t send_calls = s_ctx.provider->calls[FAKE_ESY_CAN_SEND];
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_hal_can_send(&s_ctx.can, &frame));
  TEST_ASSERT_EQUAL_UINT32(send_calls, s_ctx.provider->calls[FAKE_ESY_CAN_SEND]);
}

void test_can_receive_preserves_output_on_empty_and_error_then_copies_owned_frame(void) {
  initialize();
  esy_can_frame_t frame = {.id = 0x123u, .length = 1u, .data = {0xa5u}};
  s_ctx.provider->rx_frame.id = 0x321u;
  s_ctx.provider->rx_frame.length = 1u;
  s_ctx.provider->rx_frame.data[0] = 0x5au;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_hal_can_receive(&s_ctx.can, &frame));
  TEST_ASSERT_EQUAL_UINT32(0x123u, frame.id);
  TEST_ASSERT_EQUAL_UINT8(0xa5u, frame.data[0]);
  s_ctx.provider->results[FAKE_ESY_CAN_RECEIVE] = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_hal_can_receive(&s_ctx.can, &frame));
  TEST_ASSERT_EQUAL_UINT32(0x123u, frame.id);
  s_ctx.provider->results[FAKE_ESY_CAN_RECEIVE] = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_receive(&s_ctx.can, &frame));
  TEST_ASSERT_EQUAL_UINT32(0x321u, frame.id);
  TEST_ASSERT_EQUAL_UINT8(0x5au, frame.data[0]);
  TEST_ASSERT_EQUAL_UINT8(0u, frame.data[ESY_CAN_FD_MAX_DATA - 1u]);
  s_ctx.provider->rx_frame.data[0] = 0u;
  TEST_ASSERT_EQUAL_UINT8(0x5au, frame.data[0]);
}

void test_can_filters_validate_all_rules_before_provider_mutation(void) {
  initialize();
  esy_can_filter_t filters[2] = {
      {.id = ESY_CAN_STD_ID_MAX, .mask = ESY_CAN_STD_ID_MAX},
      {.id = ESY_CAN_EXT_ID_MAX, .mask = ESY_CAN_EXT_ID_MAX, .extended = true,
       .match_rtr = true, .rtr = true},
  };
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                        esy_hal_can_set_filters(&s_ctx.can, NULL, 1u));
  filters[1].id = ESY_CAN_EXT_ID_MAX + 1u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                        esy_hal_can_set_filters(&s_ctx.can, filters, 2u));
  filters[1].id = ESY_CAN_EXT_ID_MAX;
  filters[0].mask = ESY_CAN_STD_ID_MAX + 1u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                        esy_hal_can_set_filters(&s_ctx.can, filters, 2u));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.provider->calls[FAKE_ESY_CAN_FILTERS]);
  filters[0].mask = ESY_CAN_STD_ID_MAX;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_set_filters(&s_ctx.can, filters, 2u));
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.provider->filter_count);
  TEST_ASSERT_TRUE(s_ctx.provider->filters[1].extended);
  TEST_ASSERT_TRUE(s_ctx.provider->filters[1].match_rtr);
  TEST_ASSERT_TRUE(s_ctx.provider->filters[1].rtr);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_set_filters(&s_ctx.can, NULL, 0u));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.provider->filter_count);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_start(&s_ctx.can));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                        esy_hal_can_set_filters(&s_ctx.can, filters, 2u));
  s_ctx.provider->results[FAKE_ESY_CAN_FILTERS] = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                        esy_hal_can_set_filters(&s_ctx.can, filters, 2u));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_stop(&s_ctx.can));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO,
                        esy_hal_can_set_filters(&s_ctx.can, filters, 2u));
}

void test_can_diagnostic_queries_isolate_failed_outputs_and_propagate_snapshots(void) {
  initialize();
  uint32_t caps = 0xdeadbeefu;
  esy_can_status_t status = {.state = ESY_CAN_STATE_FAULT, .tx_error_count = 42u};
  s_ctx.provider->results[FAKE_ESY_CAN_CAPS] = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_hal_can_get_caps(&s_ctx.can, &caps));
  TEST_ASSERT_EQUAL_UINT32(0xdeadbeefu, caps);
  s_ctx.provider->results[FAKE_ESY_CAN_CAPS] = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_get_caps(&s_ctx.can, &caps));
  TEST_ASSERT_EQUAL_UINT32(s_ctx.provider->caps, caps);
  s_ctx.provider->results[FAKE_ESY_CAN_STATUS] = ESY_ERR_TIMEOUT;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT, esy_hal_can_get_status(&s_ctx.can, &status));
  TEST_ASSERT_EQUAL_INT(ESY_CAN_STATE_FAULT, status.state);
  TEST_ASSERT_EQUAL_UINT16(42u, status.tx_error_count);
  s_ctx.provider->results[FAKE_ESY_CAN_STATUS] = ESY_OK;
  s_ctx.provider->status.state = ESY_CAN_STATE_ERROR_PASSIVE;
  s_ctx.provider->status.tx_error_count = 128u;
  s_ctx.provider->status.rx_error_count = 2u;
  s_ctx.provider->status.warning = true;
  s_ctx.provider->status.rx_overrun = true;
  s_ctx.provider->status.last_error = 3u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_get_status(&s_ctx.can, &status));
  TEST_ASSERT_EQUAL_INT(ESY_CAN_STATE_ERROR_PASSIVE, status.state);
  TEST_ASSERT_EQUAL_UINT16(128u, status.tx_error_count);
  TEST_ASSERT_EQUAL_UINT16(2u, status.rx_error_count);
  TEST_ASSERT_TRUE(status.warning);
  TEST_ASSERT_TRUE(status.rx_overrun);
  TEST_ASSERT_EQUAL_UINT8(3u, status.last_error);
}

void test_can_recovery_dispatch_preserves_pending_state_and_requires_explicit_bus_off(void) {
  start();
  esy_can_status_t status = {0};
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_recover(&s_ctx.can));
  s_ctx.provider->status.state = ESY_CAN_STATE_BUS_OFF;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_recover(&s_ctx.can));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_get_status(&s_ctx.can, &status));
  TEST_ASSERT_EQUAL_INT(ESY_CAN_STATE_RECOVERING, status.state);
  s_ctx.provider->results[FAKE_ESY_CAN_RECOVER] = ESY_ERR_TIMEOUT;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_hal_can_recover(&s_ctx.can));
  TEST_ASSERT_EQUAL_INT(ESY_CAN_STATE_RECOVERING, s_ctx.provider->status.state);
  s_ctx.provider->results[FAKE_ESY_CAN_RECOVER] = ESY_OK;
  s_ctx.provider->status.state = ESY_CAN_STATE_ERROR_ACTIVE;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_can_get_status(&s_ctx.can, &status));
  TEST_ASSERT_EQUAL_INT(ESY_CAN_STATE_ERROR_ACTIVE, status.state);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_can_recover(&s_ctx.can));
}

void test_can_missing_optional_operations_report_not_implemented(void) {
  initialize();
  const esy_can_vtable_t *original = s_ctx.can.ops;
  esy_can_vtable_t absent = {0};
  s_ctx.can.ops = &absent;
  esy_can_frame_t frame = {0};
  esy_can_status_t status = {0};
  uint32_t caps = 1u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED, esy_hal_can_start(&s_ctx.can));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED, esy_hal_can_stop(&s_ctx.can));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED, esy_hal_can_send(&s_ctx.can, &frame));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED, esy_hal_can_receive(&s_ctx.can, &frame));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED, esy_hal_can_set_filters(&s_ctx.can, NULL, 0u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED, esy_hal_can_get_status(&s_ctx.can, &status));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED, esy_hal_can_recover(&s_ctx.can));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED, esy_hal_can_get_caps(&s_ctx.can, &caps));
  TEST_ASSERT_EQUAL_UINT32(1u, caps);
  s_ctx.can.ops = original;
}
