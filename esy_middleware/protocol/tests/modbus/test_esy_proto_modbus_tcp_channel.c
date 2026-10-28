/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/protocol/esy_proto_modbus_tcp_channel.h"
#include "esy_common/protocol/esy_proto_modbus.h"
#include "fake_esy_hal_system.h"
#include "fake_esy_os_task.h"
#include "unity.h"

#include <limits.h>
#include <string.h>

#define TEST_QUEUE_DEPTH 4u
#define TEST_STREAM_BYTES 4096u
#define TEST_PACKET_BYTES 300u
#define TEST_PACKET_COUNT 16u
#define TEST_COMPLETION_COUNT 16u

typedef struct {
  esy_proto_modbus_tcp_master_channel_storage_t master_storage;
  esy_proto_modbus_tcp_slave_channel_storage_t slave_storage;
  ESY_PROTO_MODBUS_TCP_MASTER_CHANNEL_STATIC_STORAGE(queues, TEST_QUEUE_DEPTH,
                                                    TEST_QUEUE_DEPTH);
  esy_proto_modbus_tcp_master_channel_t *master;
  esy_proto_modbus_tcp_slave_channel_t *slave;
  esy_network_connection_status_t status;
  uint8_t rx[TEST_STREAM_BYTES];
  size_t rx_length;
  size_t rx_offset;
  size_t received_bytes;
  uint8_t packets[TEST_PACKET_COUNT][TEST_PACKET_BYTES];
  size_t packet_lengths[TEST_PACKET_COUNT];
  uint64_t packet_sessions[TEST_PACKET_COUNT];
  uint32_t writes;
  uint32_t send_calls;
  uint32_t receive_calls;
  uint32_t empty_eof_receive_calls;
  uint32_t eof_during_receive_calls;
  uint32_t progress_calls;
  uint32_t status_calls;
  uint32_t progress_advance_ms;
  uint32_t status_advance_ms;
  uint32_t send_advance_ms;
  esy_err_t status_result;
  esy_err_t progress_result;
  int32_t send_result;
  int32_t receive_result;
  bool backpressure;
  bool partial_write;
  bool receive_overreport;
  bool eof_on_empty_receive;
  bool change_session_in_progress;
  bool change_session_in_send;
  bool change_session_in_receive;
  bool status_busy_after_send;
  bool reenter_provider;
  uint32_t completions;
  esy_proto_modbus_req_status_t completion_status[TEST_COMPLETION_COUNT];
  uint8_t completion_unit[TEST_COMPLETION_COUNT];
  esy_proto_modbus_func_t completion_function[TEST_COMPLETION_COUNT];
  uint8_t completion_pdu[TEST_COMPLETION_COUNT][16];
  uint16_t completion_length[TEST_COMPLETION_COUNT];
  uint32_t requests;
  uint8_t request_unit;
  esy_proto_modbus_func_t request_function;
  uint8_t request_pdu[16];
  uint16_t request_length;
  bool check_callback_reentry;
  bool callback_abort_then_submit;
  bool callback_nested_abort_then_submit;
  bool callback_reject_admission;
  bool change_session_in_completion;
  bool change_session_in_request;
} modbus_tcp_test_ctx_t;

static modbus_tcp_test_ctx_t s_ctx;

static esy_proto_modbus_request_t make_request(uint8_t unit) {
  esy_proto_modbus_request_t request = {0};
  request.unit_id = unit;
  request.func_code = ESY_PROTO_MODBUS_FUNC_READ_COILS;
  request.pdu[0] = (uint8_t)request.func_code;
  request.pdu[1] = 0u;
  request.pdu[2] = 0u;
  request.pdu[3] = 0u;
  request.pdu[4] = 8u;
  request.pdu_len = 5u;
  return request;
}

static esy_err_t connection_status(void *context,
    esy_network_connection_status_t *status) {
  modbus_tcp_test_ctx_t *ctx = context;
  TEST_ASSERT_EQUAL_PTR(&s_ctx, ctx);
  ++ctx->status_calls;
  fake_esy_hal_system_advance_tick(ctx->status_advance_ms);
  ctx->status_advance_ms = 0u;
  if (ctx->status_result != ESY_OK) return ctx->status_result;
  *status = ctx->status;
  status->rx_available = (uint32_t)(ctx->rx_length - ctx->rx_offset);
  return ESY_OK;
}

static int32_t connection_send(void *context, const void *data, size_t size) {
  modbus_tcp_test_ctx_t *ctx = context;
  TEST_ASSERT_EQUAL_PTR(&s_ctx, ctx);
  TEST_ASSERT_NOT_NULL(data);
  TEST_ASSERT_GREATER_THAN_size_t(0u, size);
  ++ctx->send_calls;
  fake_esy_hal_system_advance_tick(ctx->send_advance_ms);
  ctx->send_advance_ms = 0u;
  if (ctx->send_result != ESY_OK) return ctx->send_result;
  if (ctx->backpressure || size > ctx->status.send_capacity) return 0;
  TEST_ASSERT_LESS_THAN_UINT32(TEST_PACKET_COUNT, ctx->writes);
  TEST_ASSERT_LESS_OR_EQUAL_size_t(TEST_PACKET_BYTES, size);
  size_t accepted = ctx->partial_write ? size - 1u : size;
  memcpy(ctx->packets[ctx->writes], data, accepted);
  ctx->packet_lengths[ctx->writes] = accepted;
  ctx->packet_sessions[ctx->writes] = ctx->status.session_id;
  ++ctx->writes;
  ctx->status.unacked_bytes += (uint32_t)accepted;
  if (ctx->change_session_in_send) {
    ctx->change_session_in_send = false;
    ++ctx->status.session_id;
    ctx->status.unacked_bytes = 0u;
  }
  if (ctx->status_busy_after_send) ctx->status_result = ESY_ERR_BUSY;
  return (int32_t)accepted;
}

static int32_t connection_receive(void *context, void *data, size_t size) {
  modbus_tcp_test_ctx_t *ctx = context;
  TEST_ASSERT_EQUAL_PTR(&s_ctx, ctx);
  TEST_ASSERT_NOT_NULL(data);
  ++ctx->receive_calls;
  if (ctx->receive_result != ESY_OK) return ctx->receive_result;
  if (ctx->eof_on_empty_receive && ctx->rx_offset == ctx->rx_length) {
    ctx->eof_on_empty_receive = false;
    ctx->status.state = ESY_NETWORK_CONNECTION_EOF;
    ++ctx->eof_during_receive_calls;
    return ESY_ERR_IO;
  }
  if (ctx->status.state == ESY_NETWORK_CONNECTION_EOF &&
      ctx->rx_offset == ctx->rx_length) {
    ++ctx->empty_eof_receive_calls;
    return ESY_ERR_IO;
  }
  if (ctx->receive_overreport) return (int32_t)size + 1;
  if (size > ctx->rx_length - ctx->rx_offset)
    size = ctx->rx_length - ctx->rx_offset;
  memcpy(data, ctx->rx + ctx->rx_offset, size);
  ctx->rx_offset += size;
  ctx->received_bytes += size;
  if (ctx->change_session_in_receive && size) {
    ctx->change_session_in_receive = false;
    ++ctx->status.session_id;
    ctx->status.unacked_bytes = 0u;
  }
  return (int32_t)size;
}

static esy_err_t connection_progress(void *context) {
  modbus_tcp_test_ctx_t *ctx = context;
  TEST_ASSERT_EQUAL_PTR(&s_ctx, ctx);
  ++ctx->progress_calls;
  fake_esy_hal_system_advance_tick(ctx->progress_advance_ms);
  ctx->progress_advance_ms = 0u;
  if (ctx->reenter_provider) {
    if (ctx->master) {
      TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_master_channel_poll(ctx->master));
      TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_master_channel_destroy(ctx->master));
    }
    if (ctx->slave) {
      TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_slave_channel_poll(ctx->slave));
      TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_slave_channel_destroy(ctx->slave));
    }
  }
  if (ctx->change_session_in_progress) {
    ctx->change_session_in_progress = false;
    ++ctx->status.session_id;
    ctx->status.unacked_bytes = 0u;
  }
  return ctx->progress_result;
}

static esy_network_connection_t connection(void) {
  esy_network_connection_t descriptor = {
      .stream = {.recv = connection_receive, .progress = connection_progress,
                 .context = &s_ctx},
      .send_all = connection_send, .get_status = connection_status};
  return descriptor;
}

static void master_complete(esy_proto_modbus_tcp_master_channel_t *master,
    esy_proto_modbus_req_status_t status, uint8_t unit,
    esy_proto_modbus_func_t function, const uint8_t *response,
    uint16_t length, void *context) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx, context);
  TEST_ASSERT_EQUAL_PTR(s_ctx.master, master);
  TEST_ASSERT_LESS_THAN_UINT32(TEST_COMPLETION_COUNT, s_ctx.completions);
  uint32_t index = s_ctx.completions++;
  s_ctx.completion_status[index] = status;
  s_ctx.completion_unit[index] = unit;
  s_ctx.completion_function[index] = function;
  s_ctx.completion_length[index] = length;
  if (response) {
    TEST_ASSERT_LESS_OR_EQUAL_UINT16(sizeof(s_ctx.completion_pdu[index]), length);
    memcpy(s_ctx.completion_pdu[index], response, length);
  }
  if (s_ctx.check_callback_reentry) {
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_master_channel_poll(master));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_master_channel_destroy(master));
  }
  if (s_ctx.callback_abort_then_submit) {
    s_ctx.callback_abort_then_submit = false;
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_abort_all(master, true));
    esy_proto_modbus_request_t next = make_request(0x22u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_submit(master, &next));
  }
  if (s_ctx.callback_nested_abort_then_submit) {
    s_ctx.callback_nested_abort_then_submit = false;
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_tcp_master_channel_abort_all(master, true));
    esy_proto_modbus_request_t next = make_request(0x22u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_submit(master, &next));
  }
  if (s_ctx.callback_reject_admission) {
    esy_proto_modbus_request_t next = make_request(0x22u);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_master_channel_submit(master, &next));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_master_channel_abort_all(master, true));
  }
  if (s_ctx.change_session_in_completion) {
    s_ctx.change_session_in_completion = false;
    ++s_ctx.status.session_id;
    s_ctx.status.unacked_bytes = 0u;
  }
}

static esy_err_t slave_request(esy_proto_modbus_tcp_slave_channel_t *slave,
    uint8_t unit, esy_proto_modbus_func_t function, const uint8_t *request,
    uint16_t request_length, uint8_t *response, uint16_t *response_length,
    uint16_t response_capacity, void *context) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx, context);
  TEST_ASSERT_EQUAL_PTR(s_ctx.slave, slave);
  ++s_ctx.requests;
  s_ctx.request_unit = unit;
  s_ctx.request_function = function;
  s_ctx.request_length = request_length;
  size_t copy_length = request_length;
  if (copy_length > sizeof(s_ctx.request_pdu)) copy_length = sizeof(s_ctx.request_pdu);
  memcpy(s_ctx.request_pdu, request, copy_length);
  if (s_ctx.check_callback_reentry) {
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_slave_channel_poll(slave));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_slave_channel_destroy(slave));
  }
  TEST_ASSERT_GREATER_OR_EQUAL_UINT16(3u, response_capacity);
  response[0] = (uint8_t)function;
  response[1] = 1u;
  response[2] = 0xa5u;
  *response_length = 3u;
  if (s_ctx.change_session_in_request) {
    s_ctx.change_session_in_request = false;
    ++s_ctx.status.session_id;
    s_ctx.status.unacked_bytes = 0u;
  }
  return ESY_OK;
}

static esy_proto_modbus_tcp_master_channel_config_t master_config(void) {
  esy_proto_modbus_tcp_master_channel_config_t config =
      esy_proto_modbus_tcp_master_channel_default_cfg();
  config.connection = connection();
  config.queue_depth = TEST_QUEUE_DEPTH;
  config.on_complete = master_complete;
  config.user_ctx = &s_ctx;
  config.timeout_ms = 10u;
  config.tx_timeout_ms = 10u;
  config.static_resources.slot_storage = s_ctx.queues.slot_storage;
  config.static_resources.slot_storage_count = TEST_QUEUE_DEPTH;
  config.static_resources.payload_storage = s_ctx.queues.payload_storage;
  config.static_resources.payload_storage_count = TEST_QUEUE_DEPTH;
  return config;
}

static esy_proto_modbus_tcp_slave_channel_config_t slave_config(void) {
  esy_proto_modbus_tcp_slave_channel_config_t config =
      esy_proto_modbus_tcp_slave_channel_default_cfg();
  config.connection = connection();
  config.on_request = slave_request;
  config.user_ctx = &s_ctx;
  config.unit_id = 0x11u;
  config.tx_timeout_ms = 10u;
  return config;
}

static void create_master(void) {
  esy_proto_modbus_tcp_master_channel_config_t config = master_config();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_create(
      &config, &s_ctx.master_storage, &s_ctx.master));
}

static void create_slave(void) {
  esy_proto_modbus_tcp_slave_channel_config_t config = slave_config();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_create(
      &config, &s_ctx.slave_storage, &s_ctx.slave));
}

static void submit_request(uint8_t unit) {
  esy_proto_modbus_request_t request = make_request(unit);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_submit(s_ctx.master, &request));
}

static esy_proto_modbus_frame_view_t packet(uint32_t index) {
  TEST_ASSERT_LESS_THAN_UINT32(s_ctx.writes, index);
  esy_proto_modbus_frame_view_t view;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_parse_frame(ESY_PROTO_MODBUS_FRAME_TCP,
      s_ctx.packets[index], (uint16_t)s_ctx.packet_lengths[index], &view));
  return view;
}

static void append_frame(uint16_t transaction, uint8_t unit,
    const uint8_t *pdu, uint16_t length) {
  esy_proto_modbus_frame_t frame;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_frame_init(&frame,
      s_ctx.rx + s_ctx.rx_length, (uint16_t)(sizeof(s_ctx.rx) - s_ctx.rx_length)));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_build_tcp_frame(&frame,
      transaction, unit, pdu, length));
  s_ctx.rx_length += frame.len;
}

static void append_request(uint16_t transaction, uint8_t unit) {
  const uint8_t pdu[] = {1u, 0u, 0u, 0u, 8u};
  append_frame(transaction, unit, pdu, sizeof(pdu));
}

static void append_response(uint16_t transaction, uint8_t unit) {
  const uint8_t pdu[] = {1u, 1u, 0xa5u};
  append_frame(transaction, unit, pdu, sizeof(pdu));
}

static uint16_t send_request(uint8_t unit) {
  submit_request(unit);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
  return packet(0u).transaction_id;
}

static void expect_temporary_backpressure(esy_err_t result) {
  TEST_ASSERT_TRUE(result == ESY_OK || result == ESY_ERR_BUSY);
}

static void observe_master_over_full_clock_cycle(esy_err_t expected) {
  const uint32_t steps[] = {INT32_MAX, INT32_MAX, 2u};
  for (size_t i = 0u; i < sizeof(steps) / sizeof(steps[0]); ++i) {
    fake_esy_hal_system_advance_tick(steps[i]);
    TEST_ASSERT_EQUAL(expected, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  }
}

static void reset_context(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_hal_system_reset();
  fake_esy_os_task_reset();
  s_ctx.status.state = ESY_NETWORK_CONNECTION_OPEN;
  s_ctx.status.session_id = 41u;
  s_ctx.status.send_capacity = TEST_PACKET_BYTES;
}

void setUp(void) { reset_context(); }

void tearDown(void) {
  s_ctx.callback_abort_then_submit = false;
  s_ctx.callback_nested_abort_then_submit = false;
  if (s_ctx.master) TEST_ASSERT_EQUAL(ESY_OK,
      esy_proto_modbus_tcp_master_channel_destroy(s_ctx.master));
  if (s_ctx.slave) TEST_ASSERT_EQUAL(ESY_OK,
      esy_proto_modbus_tcp_slave_channel_destroy(s_ctx.slave));
  TEST_ASSERT_EQUAL(0u, fake_esy_os_task_get_create_static_count());
  TEST_ASSERT_EQUAL(0u, fake_esy_os_task_get_delete_count());
  TEST_ASSERT_EQUAL(0u, fake_esy_os_task_get_total_delay_ms());
  TEST_ASSERT_EQUAL(0u, fake_esy_hal_system_get_ctx()->last_delay_ms);
  TEST_ASSERT_EQUAL(0u, s_ctx.empty_eof_receive_calls);
}

void test_tcp_master_standard_request_response_copies_configuration_and_request(void) {
  esy_proto_modbus_tcp_master_channel_config_t config = master_config();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_create(
      &config, &s_ctx.master_storage, &s_ctx.master));
  memset(&config, 0, sizeof(config));
  esy_proto_modbus_request_t request = make_request(0x11u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_submit(s_ctx.master, &request));
  memset(&request, 0, sizeof(request));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
  esy_proto_modbus_frame_view_t sent = packet(0u);
  TEST_ASSERT_EQUAL(0x11u, sent.unit_id);
  TEST_ASSERT_EQUAL(1u, sent.func);
  TEST_ASSERT_EQUAL(5u, sent.pdu_len);
  append_response(sent.transaction_id, sent.unit_id);
  s_ctx.check_callback_reentry = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, s_ctx.completion_status[0]);
  TEST_ASSERT_EQUAL(3u, s_ctx.completion_length[0]);
  const uint8_t expected[] = {1u, 1u, 0xa5u};
  TEST_ASSERT_EQUAL_MEMORY(expected, s_ctx.completion_pdu[0], sizeof(expected));
  TEST_ASSERT_TRUE(esy_proto_modbus_tcp_master_channel_is_initialized(s_ctx.master));
}

void test_tcp_master_unit_zero_requires_a_response_and_ignores_wrong_tid_and_unit(void) {
  create_master();
  uint16_t transaction = send_request(0u);
  TEST_ASSERT_EQUAL(0u, s_ctx.completions);
  append_response((uint16_t)(transaction + 1u), 0u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  append_response(transaction, 1u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(0u, s_ctx.completions);
  append_response(transaction, 0u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, s_ctx.completion_status[0]);
  TEST_ASSERT_EQUAL(0u, s_ctx.completion_unit[0]);
}

void test_tcp_master_exception_and_response_timeout_retry_use_manager_semantics(void) {
  esy_proto_modbus_tcp_master_channel_config_t config = master_config();
  config.retry_max = 1u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_create(
      &config, &s_ctx.master_storage, &s_ctx.master));
  uint16_t transaction = send_request(0x11u);
  fake_esy_hal_system_set_tick(9u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
  fake_esy_hal_system_set_tick(10u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(2u, s_ctx.writes);
  TEST_ASSERT_EQUAL(transaction, packet(1u).transaction_id);
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.packets[0], s_ctx.packets[1], s_ctx.packet_lengths[0]);
  const uint8_t exception[] = {0x81u, 2u};
  append_frame(transaction, 0x11u, exception, sizeof(exception));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_EXCEPTION, s_ctx.completion_status[0]);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_FUNC_READ_COILS, s_ctx.completion_function[0]);
  TEST_ASSERT_EQUAL_MEMORY(exception, s_ctx.completion_pdu[0], sizeof(exception));
  submit_request(0x11u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  fake_esy_hal_system_set_tick(20u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  fake_esy_hal_system_set_tick(30u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(2u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TIMEOUT, s_ctx.completion_status[1]);
}

void test_tcp_slave_echoes_transaction_id_and_dispatches_one_of_coalesced_frames(void) {
  create_slave();
  s_ctx.check_callback_reentry = true;
  append_request(0x1234u, 0x11u);
  append_request(0x5678u, 0x11u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(1u, s_ctx.requests);
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
  TEST_ASSERT_EQUAL(0x1234u, packet(0u).transaction_id);
  TEST_ASSERT_EQUAL(0x11u, packet(0u).unit_id);
  TEST_ASSERT_EQUAL(1u, packet(0u).func);
  TEST_ASSERT_LESS_THAN_size_t(s_ctx.rx_length, s_ctx.rx_offset);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(2u, s_ctx.requests);
  TEST_ASSERT_EQUAL(2u, s_ctx.writes);
  TEST_ASSERT_EQUAL(0x5678u, packet(1u).transaction_id);
}

void test_tcp_mbap_header_and_payload_remain_valid_across_fragmented_reads(void) {
  esy_proto_modbus_tcp_slave_channel_config_t config = slave_config();
  config.rx_byte_budget = 3u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_create(
      &config, &s_ctx.slave_storage, &s_ctx.slave));
  append_request(0x1122u, 0x11u);
  size_t complete_length = s_ctx.rx_length;
  for (size_t consumed = 3u; consumed < complete_length; consumed += 3u) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
    TEST_ASSERT_EQUAL_size_t(consumed, s_ctx.rx_offset);
    TEST_ASSERT_EQUAL(0u, s_ctx.requests);
  }
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL_size_t(complete_length, s_ctx.rx_offset);
  TEST_ASSERT_EQUAL(1u, s_ctx.requests);
  TEST_ASSERT_EQUAL(0x1122u, packet(0u).transaction_id);
}

void test_tcp_fragmented_mbap_waits_for_remaining_payload_without_idle_gap(void) {
  create_slave();
  append_request(7u, 0x11u);
  size_t complete_length = s_ctx.rx_length;
  s_ctx.rx_length = 6u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(0u, s_ctx.requests);
  s_ctx.rx_length = complete_length;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(1u, s_ctx.requests);
  TEST_ASSERT_EQUAL(7u, packet(0u).transaction_id);
}

void test_tcp_slave_backpressure_keeps_the_reply_and_next_request_until_whole_write(void) {
  create_slave();
  append_request(1u, 0x11u);
  append_request(2u, 0x11u);
  s_ctx.backpressure = true;
  expect_temporary_backpressure(esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(1u, s_ctx.requests);
  size_t consumed = s_ctx.rx_offset;
  TEST_ASSERT_EQUAL(0u, s_ctx.writes);
  expect_temporary_backpressure(esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(1u, s_ctx.requests);
  TEST_ASSERT_EQUAL_size_t(consumed, s_ctx.rx_offset);
  s_ctx.backpressure = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
  TEST_ASSERT_EQUAL(1u, packet(0u).transaction_id);
  if (s_ctx.requests == 1u)
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(2u, s_ctx.requests);
  if (s_ctx.writes == 1u)
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(2u, s_ctx.writes);
  TEST_ASSERT_EQUAL(2u, packet(1u).transaction_id);
}

void test_tcp_master_backpressure_preserves_request_until_whole_frame_acceptance(void) {
  create_master();
  s_ctx.backpressure = true;
  submit_request(0x11u);
  expect_temporary_backpressure(esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(0u, s_ctx.writes);
  fake_esy_hal_system_set_tick(9u);
  expect_temporary_backpressure(esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(0u, s_ctx.completions);
  s_ctx.backpressure = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
  append_response(packet(0u).transaction_id, 0x11u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, s_ctx.completion_status[0]);
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
}

void test_tcp_zero_budget_uses_default_and_limits_a_maximum_sized_adu(void) {
  esy_proto_modbus_tcp_slave_channel_config_t config = slave_config();
  config.rx_byte_budget = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_create(
      &config, &s_ctx.slave_storage, &s_ctx.slave));
  uint8_t pdu[ESY_PROTO_MODBUS_MAX_PDU_LEN] = {1u};
  append_frame(0x55aau, 0x11u, pdu, sizeof(pdu));
  TEST_ASSERT_GREATER_THAN_size_t(256u, s_ctx.rx_length);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL_size_t(256u, s_ctx.received_bytes);
  TEST_ASSERT_EQUAL(0u, s_ctx.requests);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(1u, s_ctx.requests);
  TEST_ASSERT_EQUAL(sizeof(pdu), s_ctx.request_length);
  TEST_ASSERT_EQUAL(0x55aau, packet(0u).transaction_id);
}

void test_tcp_slave_uid_zero_replies_and_other_units_do_not_invoke_the_handler(void) {
  esy_proto_modbus_tcp_slave_channel_config_t config = slave_config();
  config.unit_id = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_create(
      &config, &s_ctx.slave_storage, &s_ctx.slave));
  append_request(10u, 1u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(0u, s_ctx.requests);
  TEST_ASSERT_EQUAL(0u, s_ctx.writes);
  append_request(11u, 0u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(1u, s_ctx.requests);
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
  TEST_ASSERT_EQUAL(0u, packet(0u).unit_id);
  TEST_ASSERT_EQUAL(11u, packet(0u).transaction_id);
}

void test_tcp_master_framing_errors_are_sticky_and_cancel_each_request_once(void) {
  for (uint32_t malformed = 0u; malformed < 3u; ++malformed) {
    create_master();
    submit_request(0x11u);
    submit_request(0x22u);
    append_response(1u, 0x11u);
    if (malformed == 0u) {
      s_ctx.rx[3] = 1u;
    } else if (malformed == 1u) {
      s_ctx.rx[4] = 0u; s_ctx.rx[5] = 1u;
    } else {
      s_ctx.rx[4] = 0xffu; s_ctx.rx[5] = 0xffu;
    }
    s_ctx.check_callback_reentry = true;
    s_ctx.callback_reject_admission = true;
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
    TEST_ASSERT_EQUAL(2u, s_ctx.completions);
    TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_BAD_FRAME, s_ctx.completion_status[0]);
    TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_BAD_FRAME, s_ctx.completion_status[1]);
    TEST_ASSERT_TRUE(esy_proto_modbus_tcp_master_channel_is_initialized(s_ctx.master));
    uint32_t provider_calls = s_ctx.progress_calls + s_ctx.status_calls + s_ctx.receive_calls + s_ctx.send_calls;
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
    TEST_ASSERT_EQUAL(provider_calls, s_ctx.progress_calls + s_ctx.status_calls + s_ctx.receive_calls + s_ctx.send_calls);
    TEST_ASSERT_EQUAL(2u, s_ctx.completions);
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_destroy(s_ctx.master));
    s_ctx.master = NULL;
    reset_context();
  }
}

void test_tcp_slave_bad_mbap_and_overreported_receive_never_dispatch_or_reply(void) {
  for (uint32_t malformed = 0u; malformed < 3u; ++malformed) {
    create_slave();
    append_request(1u, 0x11u);
    esy_err_t expected = ESY_ERR_CRC_FAIL;
    if (malformed == 0u) {
      s_ctx.rx[2] = 1u;
    } else if (malformed == 1u) {
      s_ctx.rx[4] = 0u; s_ctx.rx[5] = 0u;
    } else {
      s_ctx.receive_overreport = true;
      expected = ESY_ERR_IO;
    }
    TEST_ASSERT_EQUAL(expected, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
    TEST_ASSERT_EQUAL(0u, s_ctx.requests);
    TEST_ASSERT_EQUAL(0u, s_ctx.writes);
    TEST_ASSERT_EQUAL(expected, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_destroy(s_ctx.slave));
    s_ctx.slave = NULL;
    reset_context();
  }
}

void test_tcp_positive_prefix_write_is_fatal_and_is_never_retried(void) {
  create_master();
  s_ctx.partial_write = true;
  submit_request(0x11u);
  submit_request(0x22u);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
  TEST_ASSERT_EQUAL(2u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR, s_ctx.completion_status[0]);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR, s_ctx.completion_status[1]);
  s_ctx.partial_write = false;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
  TEST_ASSERT_EQUAL(2u, s_ctx.completions);
}

void test_tcp_provider_errors_preserve_first_failure_and_never_replay_cancellation(void) {
  for (uint32_t provider = 0u; provider < 5u; ++provider) {
    create_master();
    submit_request(0x11u);
    submit_request(0x22u);
    if (provider == 0u) s_ctx.progress_result = ESY_ERR_FAIL;
    if (provider == 1u) s_ctx.status_result = ESY_ERR_FAIL;
    if (provider == 2u) s_ctx.send_result = ESY_ERR_FAIL;
    if (provider == 3u) {
      append_response(1u, 0x11u);
      s_ctx.receive_result = ESY_ERR_FAIL;
    }
    esy_err_t expected = provider == 4u ? ESY_ERR_CRC_FAIL : ESY_ERR_FAIL;
    if (provider == 4u) s_ctx.progress_result = expected;
    TEST_ASSERT_EQUAL(expected, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
    TEST_ASSERT_EQUAL(2u, s_ctx.completions);
    TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR, s_ctx.completion_status[0]);
    TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR, s_ctx.completion_status[1]);
    s_ctx.progress_result = ESY_OK;
    s_ctx.status_result = ESY_OK;
    s_ctx.send_result = ESY_OK;
    s_ctx.receive_result = ESY_OK;
    ++s_ctx.status.session_id;
    TEST_ASSERT_EQUAL(expected, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
    TEST_ASSERT_EQUAL(2u, s_ctx.completions);
    TEST_ASSERT_FALSE(esy_proto_modbus_tcp_master_channel_is_peer_drained(s_ctx.master));
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_destroy(s_ctx.master));
    s_ctx.master = NULL;
    reset_context();
  }
}

void test_tcp_provider_busy_is_temporary_for_progress_status_send_and_receive(void) {
  for (uint32_t provider = 0u; provider < 4u; ++provider) {
    create_master();
    if (provider == 3u) {
      (void)send_request(0x11u);
      append_response(packet(0u).transaction_id, 0x11u);
      s_ctx.receive_result = ESY_ERR_BUSY;
    } else {
      submit_request(0x11u);
      if (provider == 0u) s_ctx.progress_result = ESY_ERR_BUSY;
      if (provider == 1u) s_ctx.status_result = ESY_ERR_BUSY;
      if (provider == 2u) s_ctx.send_result = ESY_ERR_BUSY;
    }
    esy_err_t result = esy_proto_modbus_tcp_master_channel_poll(s_ctx.master);
    TEST_ASSERT_TRUE(result == ESY_OK || result == ESY_ERR_BUSY);
    TEST_ASSERT_EQUAL(0u, s_ctx.completions);
    TEST_ASSERT_TRUE(esy_proto_modbus_tcp_master_channel_is_initialized(s_ctx.master));
    s_ctx.progress_result = ESY_OK;
    s_ctx.status_result = ESY_OK;
    s_ctx.send_result = ESY_OK;
    s_ctx.receive_result = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
    TEST_ASSERT_EQUAL(1u, s_ctx.writes);
    if (provider != 3u) {
      append_response(packet(0u).transaction_id, 0x11u);
      TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
    }
    TEST_ASSERT_EQUAL(1u, s_ctx.completions);
    TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, s_ctx.completion_status[0]);
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_destroy(s_ctx.master));
    s_ctx.master = NULL;
    reset_context();
  }
}

void test_tcp_epoch_changes_in_each_io_stage_cancel_once_and_never_touch_the_new_peer(void) {
  for (uint32_t provider = 0u; provider < 3u; ++provider) {
    create_master();
    if (provider == 2u) {
      (void)send_request(0x11u);
      append_response(packet(0u).transaction_id, 0x11u);
      s_ctx.change_session_in_receive = true;
    } else {
      submit_request(0x11u);
      if (provider == 0u) s_ctx.change_session_in_progress = true;
      if (provider == 1u) s_ctx.change_session_in_send = true;
    }
    submit_request(0x22u);
    s_ctx.callback_reject_admission = true;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
    TEST_ASSERT_EQUAL(2u, s_ctx.completions);
    TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR, s_ctx.completion_status[0]);
    TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR, s_ctx.completion_status[1]);
    uint32_t writes = s_ctx.writes;
    size_t received = s_ctx.rx_offset;
    append_response(2u, 0x22u);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
    TEST_ASSERT_EQUAL(writes, s_ctx.writes);
    TEST_ASSERT_EQUAL_size_t(received, s_ctx.rx_offset);
    TEST_ASSERT_EQUAL(2u, s_ctx.completions);
    for (uint32_t index = 0u; index < writes; ++index)
      TEST_ASSERT_EQUAL_UINT64(41u, s_ctx.packet_sessions[index]);
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_destroy(s_ctx.master));
    s_ctx.master = NULL;
    reset_context();
  }
}

void test_tcp_observed_disconnect_is_sticky_but_destroy_and_recreate_bind_the_next_peer(void) {
  create_master();
  submit_request(0x11u);
  s_ctx.status.state = ESY_NETWORK_CONNECTION_INACTIVE;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR, s_ctx.completion_status[0]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_destroy(s_ctx.master));
  TEST_ASSERT_FALSE(esy_proto_modbus_tcp_master_channel_is_initialized(s_ctx.master));
  s_ctx.master = NULL;
  s_ctx.status.state = ESY_NETWORK_CONNECTION_OPEN;
  s_ctx.status.session_id = 42u;
  create_master();
  uint16_t transaction = send_request(0x11u);
  TEST_ASSERT_EQUAL_UINT64(42u, s_ctx.packet_sessions[0]);
  append_response(transaction, 0x11u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(2u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, s_ctx.completion_status[1]);
}

void test_tcp_slave_eof_drains_request_and_final_reply_then_waits_for_fresh_peer_ack(void) {
  s_ctx.status.state = ESY_NETWORK_CONNECTION_EOF;
  append_request(0x77aau, 0x11u);
  create_slave();
  TEST_ASSERT_FALSE(esy_proto_modbus_tcp_slave_channel_is_peer_drained(s_ctx.slave));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(1u, s_ctx.requests);
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
  TEST_ASSERT_EQUAL(0x77aau, packet(0u).transaction_id);
  TEST_ASSERT_FALSE(esy_proto_modbus_tcp_slave_channel_is_peer_drained(s_ctx.slave));
  uint32_t progress_calls = s_ctx.progress_calls;
  uint32_t status_calls = s_ctx.status_calls;
  s_ctx.status.unacked_bytes = 0u;
  TEST_ASSERT_TRUE(esy_proto_modbus_tcp_slave_channel_is_peer_drained(s_ctx.slave));
  TEST_ASSERT_GREATER_THAN_UINT32(status_calls, s_ctx.status_calls);
  TEST_ASSERT_EQUAL(progress_calls, s_ctx.progress_calls);
  s_ctx.status_result = ESY_ERR_BUSY;
  TEST_ASSERT_FALSE(esy_proto_modbus_tcp_slave_channel_is_peer_drained(s_ctx.slave));
  s_ctx.status_result = ESY_OK;
  TEST_ASSERT_TRUE(esy_proto_modbus_tcp_slave_channel_is_peer_drained(s_ctx.slave));
}

void test_tcp_master_eof_delivers_buffered_response_without_sending_another_request(void) {
  create_master();
  uint16_t transaction = send_request(0x11u);
  append_response(transaction, 0x11u);
  s_ctx.status.state = ESY_NETWORK_CONNECTION_EOF;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, s_ctx.completion_status[0]);
  TEST_ASSERT_FALSE(esy_proto_modbus_tcp_master_channel_is_peer_drained(s_ctx.master));
  s_ctx.status.unacked_bytes = 0u;
  TEST_ASSERT_TRUE(esy_proto_modbus_tcp_master_channel_is_peer_drained(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
}

void test_tcp_master_eof_without_response_terminates_instead_of_retrying(void) {
  esy_proto_modbus_tcp_master_channel_config_t config = master_config();
  config.retry_max = 2u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_create(
      &config, &s_ctx.master_storage, &s_ctx.master));
  (void)send_request(0x11u);
  s_ctx.status.state = ESY_NETWORK_CONNECTION_EOF;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR, s_ctx.completion_status[0]);
  fake_esy_hal_system_set_tick(1000u);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
  TEST_ASSERT_FALSE(esy_proto_modbus_tcp_master_channel_is_peer_drained(s_ctx.master));
}

void test_tcp_truncated_eof_is_a_framing_failure_for_master_and_slave(void) {
  create_master();
  (void)send_request(0x11u);
  append_response(1u, 0x11u);
  s_ctx.rx_length = 7u;
  s_ctx.status.state = ESY_NETWORK_CONNECTION_EOF;
  TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_BAD_FRAME, s_ctx.completion_status[0]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_destroy(s_ctx.master));
  s_ctx.master = NULL;
  reset_context();
  create_slave();
  append_request(1u, 0x11u);
  s_ctx.rx_length = 3u;
  s_ctx.status.state = ESY_NETWORK_CONNECTION_EOF;
  TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(0u, s_ctx.requests);
  TEST_ASSERT_EQUAL(0u, s_ctx.writes);
}

void test_tcp_tx_deadline_expires_at_the_boundary_across_wrap_even_when_progress_is_busy(void) {
  fake_esy_hal_system_set_tick(UINT32_MAX - 4u);
  create_master();
  s_ctx.backpressure = true;
  submit_request(0x11u);
  expect_temporary_backpressure(esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  fake_esy_hal_system_set_tick(4u);
  expect_temporary_backpressure(esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(0u, s_ctx.completions);
  s_ctx.progress_result = ESY_ERR_BUSY;
  fake_esy_hal_system_set_tick(5u);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR, s_ctx.completion_status[0]);
  TEST_ASSERT_EQUAL(0u, s_ctx.writes);
  observe_master_over_full_clock_cycle(ESY_ERR_TIMEOUT);
  TEST_ASSERT_EQUAL(1u, s_ctx.completions);
}

void test_tcp_completed_tx_and_cancelled_staging_do_not_reactivate_after_a_full_tick_cycle(void) {
  create_master();
  s_ctx.backpressure = true;
  submit_request(0x11u);
  expect_temporary_backpressure(esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_abort_all(s_ctx.master, true));
  TEST_ASSERT_EQUAL(1u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_ABORTED, s_ctx.completion_status[0]);
  s_ctx.backpressure = false;
  observe_master_over_full_clock_cycle(ESY_OK);
  TEST_ASSERT_EQUAL(0u, s_ctx.writes);
  submit_request(0x11u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
  append_response(packet(0u).transaction_id, 0x11u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(2u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, s_ctx.completion_status[1]);
  observe_master_over_full_clock_cycle(ESY_OK);
  TEST_ASSERT_EQUAL(2u, s_ctx.completions);
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
}

void test_tcp_slave_tx_timeout_retains_no_second_request_and_is_sticky(void) {
  create_slave();
  s_ctx.backpressure = true;
  append_request(1u, 0x11u);
  append_request(2u, 0x11u);
  expect_temporary_backpressure(esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(1u, s_ctx.requests);
  fake_esy_hal_system_set_tick(10u);
  s_ctx.status_result = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  s_ctx.status_result = ESY_OK;
  s_ctx.backpressure = false;
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(1u, s_ctx.requests);
  TEST_ASSERT_EQUAL(0u, s_ctx.writes);
}

void test_tcp_accepted_write_waiting_for_busy_epoch_observation_is_not_sent_twice(void) {
  create_master();
  s_ctx.status_busy_after_send = true;
  submit_request(0x11u);
  esy_err_t result = esy_proto_modbus_tcp_master_channel_poll(s_ctx.master);
  TEST_ASSERT_TRUE(result == ESY_OK || result == ESY_ERR_BUSY);
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
  fake_esy_hal_system_set_tick(9u);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
  TEST_ASSERT_EQUAL(0u, s_ctx.completions);
  s_ctx.status_busy_after_send = false;
  s_ctx.status_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
  append_response(packet(0u).transaction_id, 0x11u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, s_ctx.completion_status[0]);
}

void test_tcp_accepted_write_still_waiting_for_session_confirmation_expires_without_resending(void) {
  fake_esy_hal_system_set_tick(UINT32_MAX - 4u);
  create_master();
  s_ctx.status_busy_after_send = true;
  submit_request(0x11u);
  esy_err_t result = esy_proto_modbus_tcp_master_channel_poll(s_ctx.master);
  TEST_ASSERT_TRUE(result == ESY_OK || result == ESY_ERR_BUSY);
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
  fake_esy_hal_system_set_tick(4u);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(0u, s_ctx.completions);
  fake_esy_hal_system_set_tick(5u);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
  TEST_ASSERT_EQUAL(1u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR, s_ctx.completion_status[0]);
  s_ctx.status_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
}

void test_tcp_response_deadline_is_observed_while_progress_or_status_remains_busy(void) {
  for (uint32_t provider = 0u; provider < 2u; ++provider) {
    create_master();
    (void)send_request(0x11u);
    if (provider == 0u) s_ctx.progress_result = ESY_ERR_BUSY;
    else s_ctx.status_result = ESY_ERR_BUSY;
    fake_esy_hal_system_set_tick(9u);
    esy_err_t result = esy_proto_modbus_tcp_master_channel_poll(s_ctx.master);
    TEST_ASSERT_TRUE(result == ESY_OK || result == ESY_ERR_BUSY);
    TEST_ASSERT_EQUAL(0u, s_ctx.completions);
    fake_esy_hal_system_set_tick(10u);
    result = esy_proto_modbus_tcp_master_channel_poll(s_ctx.master);
    TEST_ASSERT_TRUE(result == ESY_OK || result == ESY_ERR_BUSY);
    TEST_ASSERT_EQUAL(1u, s_ctx.completions);
    TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TIMEOUT, s_ctx.completion_status[0]);
    TEST_ASSERT_EQUAL(1u, s_ctx.writes);
    s_ctx.progress_result = ESY_OK;
    s_ctx.status_result = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
    observe_master_over_full_clock_cycle(ESY_OK);
    TEST_ASSERT_EQUAL(1u, s_ctx.completions);
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_destroy(s_ctx.master));
    s_ctx.master = NULL;
    reset_context();
  }
}

void test_tcp_completion_callback_may_abort_and_submit_but_not_drive_or_destroy_the_channel(void) {
  create_master();
  uint16_t transaction = send_request(0x11u);
  submit_request(0x33u);
  s_ctx.check_callback_reentry = true;
  s_ctx.callback_abort_then_submit = true;
  append_response(transaction, 0x11u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(2u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, s_ctx.completion_status[0]);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_ABORTED, s_ctx.completion_status[1]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(2u, s_ctx.writes);
  TEST_ASSERT_EQUAL(0x22u, packet(1u).unit_id);
}

void test_tcp_optional_provider_progress_and_destroy_leave_the_endpoint_owned_by_the_caller(void) {
  esy_proto_modbus_tcp_slave_channel_config_t config = slave_config();
  config.connection.stream.progress = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_create(
      &config, &s_ctx.slave_storage, &s_ctx.slave));
  append_request(1u, 0x11u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(0u, s_ctx.progress_calls);
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_destroy(s_ctx.slave));
  TEST_ASSERT_EQUAL(ESY_NETWORK_CONNECTION_OPEN, s_ctx.status.state);
  TEST_ASSERT_EQUAL_UINT64(41u, s_ctx.status.session_id);
  TEST_ASSERT_NOT_EQUAL(0u, s_ctx.status.unacked_bytes);
  TEST_ASSERT_FALSE(esy_proto_modbus_tcp_slave_channel_is_initialized(s_ctx.slave));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_destroy(s_ctx.slave));
  s_ctx.slave = NULL;
  create_slave();
  TEST_ASSERT_TRUE(esy_proto_modbus_tcp_slave_channel_is_initialized(s_ctx.slave));
}

void test_tcp_defaults_and_null_queries_publish_only_existing_capabilities(void) {
  esy_proto_modbus_tcp_master_channel_config_t master =
      esy_proto_modbus_tcp_master_channel_default_cfg();
  esy_proto_modbus_tcp_slave_channel_config_t slave =
      esy_proto_modbus_tcp_slave_channel_default_cfg();
  TEST_ASSERT_EQUAL(100u, master.timeout_ms);
  TEST_ASSERT_EQUAL(0u, master.retry_max);
  TEST_ASSERT_EQUAL(1000u, master.tx_timeout_ms);
  TEST_ASSERT_EQUAL(256u, master.rx_byte_budget);
  TEST_ASSERT_EQUAL(1000u, slave.tx_timeout_ms);
  TEST_ASSERT_EQUAL(256u, slave.rx_byte_budget);
  TEST_ASSERT_EQUAL(0xffu, slave.unit_id);
  TEST_ASSERT_FALSE(esy_proto_modbus_tcp_master_channel_is_initialized(NULL));
  TEST_ASSERT_FALSE(esy_proto_modbus_tcp_slave_channel_is_initialized(NULL));
  TEST_ASSERT_FALSE(esy_proto_modbus_tcp_master_channel_is_peer_drained(NULL));
  TEST_ASSERT_FALSE(esy_proto_modbus_tcp_slave_channel_is_peer_drained(NULL));
  TEST_ASSERT_EQUAL(0u, esy_proto_modbus_tcp_master_channel_get_queue_depth(NULL));
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_MASTER_STATE_IDLE,
      esy_proto_modbus_tcp_master_channel_get_state(NULL));
  TEST_ASSERT_EQUAL(0u, esy_proto_modbus_tcp_slave_channel_get_addr(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_master_channel_poll(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_slave_channel_poll(NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_destroy(NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_destroy(NULL));
  esy_proto_modbus_request_t request = make_request(0x11u);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_master_channel_submit(NULL, &request));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_master_channel_abort_all(NULL, true));
}

void test_tcp_create_rejects_missing_callbacks_and_resources_before_provider_access(void) {
  esy_proto_modbus_tcp_master_channel_config_t config = master_config();
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_master_channel_create(
      NULL, &s_ctx.master_storage, &s_ctx.master));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_master_channel_create(
      &config, NULL, &s_ctx.master));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_master_channel_create(
      &config, &s_ctx.master_storage, NULL));
  for (uint32_t invalid = 0u; invalid < 9u; ++invalid) {
    config = master_config();
    if (invalid == 0u) config.connection.get_status = NULL;
    if (invalid == 1u) config.connection.send_all = NULL;
    if (invalid == 2u) config.connection.stream.recv = NULL;
    if (invalid == 3u) config.queue_depth = 0u;
    if (invalid == 4u) config.static_resources.slot_storage = NULL;
    if (invalid == 5u) config.static_resources.payload_storage_count = TEST_QUEUE_DEPTH - 1u;
    if (invalid == 6u) config.timeout_ms = (uint32_t)INT32_MAX + 1u;
    if (invalid == 7u) config.tx_timeout_ms = (uint32_t)INT32_MAX + 1u;
    if (invalid == 8u) config.static_resources.slot_storage =
        (esy_prio_slot_t *)(void *)((uint8_t *)s_ctx.queues.slot_storage + 1u);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_master_channel_create(
        &config, &s_ctx.master_storage, &s_ctx.master));
    TEST_ASSERT_NULL(s_ctx.master);
  }
  TEST_ASSERT_EQUAL(0u, s_ctx.status_calls);
  config = master_config();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_create(
      &config, &s_ctx.master_storage, &s_ctx.master));
  esy_proto_modbus_request_t request = make_request(0x11u);
  request.user_ctx = &s_ctx;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_master_channel_submit(s_ctx.master, &request));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_master_channel_submit(s_ctx.master, NULL));
  TEST_ASSERT_EQUAL(0u, esy_proto_modbus_tcp_master_channel_get_queue_depth(s_ctx.master));
}

void test_tcp_slave_create_rejects_invalid_configuration_and_allows_corrected_retry(void) {
  esy_proto_modbus_tcp_slave_channel_config_t config = slave_config();
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_slave_channel_create(
      NULL, &s_ctx.slave_storage, &s_ctx.slave));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_slave_channel_create(
      &config, NULL, &s_ctx.slave));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_slave_channel_create(
      &config, &s_ctx.slave_storage, NULL));
  config.on_request = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_slave_channel_create(
      &config, &s_ctx.slave_storage, &s_ctx.slave));
  config = slave_config();
  config.tx_timeout_ms = (uint32_t)INT32_MAX + 1u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_slave_channel_create(
      &config, &s_ctx.slave_storage, &s_ctx.slave));
  TEST_ASSERT_NULL(s_ctx.slave);
  TEST_ASSERT_EQUAL(0u, s_ctx.status_calls);
  config = slave_config();
  config.tx_timeout_ms = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_create(
      &config, &s_ctx.slave_storage, &s_ctx.slave));
  TEST_ASSERT_EQUAL(0x11u, esy_proto_modbus_tcp_slave_channel_get_addr(s_ctx.slave));
}

void test_tcp_create_requires_a_connected_nonzero_session_and_provider_errors_retain_nothing(void) {
  esy_proto_modbus_tcp_master_channel_config_t config = master_config();
  s_ctx.status.state = ESY_NETWORK_CONNECTION_INACTIVE;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_master_channel_create(
      &config, &s_ctx.master_storage, &s_ctx.master));
  TEST_ASSERT_NULL(s_ctx.master);
  s_ctx.status.state = ESY_NETWORK_CONNECTION_OPEN;
  s_ctx.status.session_id = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_master_channel_create(
      &config, &s_ctx.master_storage, &s_ctx.master));
  s_ctx.status.session_id = 41u;
  s_ctx.status_result = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_tcp_master_channel_create(
      &config, &s_ctx.master_storage, &s_ctx.master));
  TEST_ASSERT_NULL(s_ctx.master);
  s_ctx.status_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_modbus_tcp_master_channel_create(
      &config, &s_ctx.master_storage, &s_ctx.master));
  TEST_ASSERT_NULL(s_ctx.master);
  s_ctx.status_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_create(
      &config, &s_ctx.master_storage, &s_ctx.master));
  esy_proto_modbus_tcp_master_channel_t *duplicate = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_master_channel_create(
      &config, &s_ctx.master_storage, &duplicate));
  TEST_ASSERT_NULL(duplicate);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_destroy(s_ctx.master));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_create(
      &config, &s_ctx.master_storage, &s_ctx.master));
}

void test_tcp_overlapping_or_wrapping_resource_ranges_are_rejected_before_writes(void) {
  esy_proto_modbus_tcp_master_channel_config_t config = master_config();
  config.static_resources.slot_storage = (esy_prio_slot_t *)(void *)s_ctx.master_storage.bytes;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_master_channel_create(
      &config, &s_ctx.master_storage, &s_ctx.master));
  config = master_config();
  config.static_resources.payload_storage =
      (esy_proto_modbus_payload_t *)(void *)s_ctx.master_storage.bytes;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_master_channel_create(
      &config, &s_ctx.master_storage, &s_ctx.master));
  config = master_config();
  config.static_resources.slot_storage = (esy_prio_slot_t *)(void *)s_ctx.queues.payload_storage;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_master_channel_create(
      &config, &s_ctx.master_storage, &s_ctx.master));
  config = master_config();
  config.static_resources.slot_storage = (esy_prio_slot_t *)(uintptr_t)
      (UINTPTR_MAX - (alignof(esy_prio_slot_t) - 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_master_channel_create(
      &config, &s_ctx.master_storage, &s_ctx.master));
  config = master_config();
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_master_channel_create(
      &config, &s_ctx.master_storage,
      (esy_proto_modbus_tcp_master_channel_t **)(void *)s_ctx.master_storage.bytes));
  TEST_ASSERT_EQUAL(0u, s_ctx.status_calls);
  create_master();
}

void test_tcp_slave_configuration_and_output_cannot_alias_opaque_storage(void) {
  esy_proto_modbus_tcp_slave_channel_config_t config = slave_config();
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_slave_channel_create(
      &config, &s_ctx.slave_storage,
      (esy_proto_modbus_tcp_slave_channel_t **)(void *)s_ctx.slave_storage.bytes));
  esy_proto_modbus_tcp_slave_channel_storage_t storage = {0};
  esy_proto_modbus_tcp_slave_channel_config_t *aliased =
      (esy_proto_modbus_tcp_slave_channel_config_t *)(void *)storage.bytes;
  *aliased = slave_config();
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_slave_channel_create(
      aliased, &storage, &s_ctx.slave));
  TEST_ASSERT_EQUAL(0u, s_ctx.status_calls);
  create_slave();
  esy_proto_modbus_tcp_slave_channel_t *duplicate = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_slave_channel_create(
      &config, &s_ctx.slave_storage, &duplicate));
  TEST_ASSERT_NULL(duplicate);
}

void test_tcp_non_isr_owner_and_provider_callback_reentry_are_enforced(void) {
  esy_proto_modbus_tcp_master_channel_config_t master = master_config();
  esy_proto_modbus_tcp_slave_channel_config_t slave = slave_config();
  fake_esy_hal_system_get_ctx()->is_in_isr = true;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_master_channel_create(
      &master, &s_ctx.master_storage, &s_ctx.master));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_slave_channel_create(
      &slave, &s_ctx.slave_storage, &s_ctx.slave));
  fake_esy_hal_system_get_ctx()->is_in_isr = false;
  create_master();
  s_ctx.reenter_provider = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  esy_proto_modbus_request_t request = make_request(0x11u);
  fake_esy_hal_system_get_ctx()->is_in_isr = true;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_master_channel_submit(s_ctx.master, &request));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_master_channel_destroy(s_ctx.master));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_tcp_master_channel_abort_all(s_ctx.master, true));
  fake_esy_hal_system_get_ctx()->is_in_isr = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_destroy(s_ctx.master));
  s_ctx.master = NULL;
  create_slave();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
}

void test_tcp_destroy_cancels_pending_requests_once_and_never_flushes_them_to_the_provider(void) {
  create_master();
  submit_request(0x11u);
  submit_request(0x22u);
  s_ctx.backpressure = true;
  expect_temporary_backpressure(esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  s_ctx.check_callback_reentry = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_destroy(s_ctx.master));
  TEST_ASSERT_EQUAL(2u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_ABORTED, s_ctx.completion_status[0]);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_ABORTED, s_ctx.completion_status[1]);
  TEST_ASSERT_EQUAL(0u, s_ctx.writes);
  TEST_ASSERT_EQUAL(ESY_NETWORK_CONNECTION_OPEN, s_ctx.status.state);
  TEST_ASSERT_EQUAL_UINT64(41u, s_ctx.status.session_id);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_destroy(s_ctx.master));
  TEST_ASSERT_EQUAL(2u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  esy_proto_modbus_request_t request = make_request(0x11u);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_master_channel_submit(s_ctx.master, &request));
}

void test_tcp_slave_session_loss_discards_its_staged_reply_before_rebinding(void) {
  create_slave();
  append_request(1u, 0x11u);
  append_request(2u, 0x11u);
  s_ctx.backpressure = true;
  expect_temporary_backpressure(esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(1u, s_ctx.requests);
  TEST_ASSERT_EQUAL(0u, s_ctx.writes);
  s_ctx.change_session_in_progress = true;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  s_ctx.backpressure = false;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(0u, s_ctx.writes);
  TEST_ASSERT_EQUAL(1u, s_ctx.requests);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_destroy(s_ctx.slave));
  s_ctx.slave = NULL;
  s_ctx.rx_length = 0u;
  s_ctx.rx_offset = 0u;
  create_slave();
  append_request(3u, 0x11u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(2u, s_ctx.requests);
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
  TEST_ASSERT_EQUAL_UINT64(42u, s_ctx.packet_sessions[0]);
  TEST_ASSERT_EQUAL(3u, packet(0u).transaction_id);
}

void test_tcp_maximum_tx_wait_expires_at_its_boundary_and_zero_selects_the_default(void) {
  esy_proto_modbus_tcp_master_channel_config_t master = master_config();
  master.tx_timeout_ms = INT32_MAX;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_create(
      &master, &s_ctx.master_storage, &s_ctx.master));
  s_ctx.backpressure = true;
  submit_request(0x11u);
  expect_temporary_backpressure(esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  fake_esy_hal_system_set_tick((uint32_t)INT32_MAX - 1u);
  expect_temporary_backpressure(esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(0u, s_ctx.completions);
  fake_esy_hal_system_set_tick(INT32_MAX);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_destroy(s_ctx.master));
  s_ctx.master = NULL;
  reset_context();
  esy_proto_modbus_tcp_slave_channel_config_t slave = slave_config();
  slave.tx_timeout_ms = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_create(
      &slave, &s_ctx.slave_storage, &s_ctx.slave));
  s_ctx.backpressure = true;
  append_request(1u, 0x11u);
  expect_temporary_backpressure(esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  fake_esy_hal_system_set_tick(999u);
  expect_temporary_backpressure(esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  fake_esy_hal_system_set_tick(1000u);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(1u, s_ctx.requests);
  TEST_ASSERT_EQUAL(0u, s_ctx.writes);
}

void test_tcp_nested_abort_returns_busy_without_cancelling_current_staging_or_callback_submissions(void) {
  create_master();
  s_ctx.backpressure = true;
  submit_request(0x11u);
  expect_temporary_backpressure(esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  submit_request(0x33u);
  s_ctx.check_callback_reentry = true;
  s_ctx.callback_nested_abort_then_submit = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_abort_all(s_ctx.master, false));
  TEST_ASSERT_EQUAL(1u, s_ctx.completions);
  TEST_ASSERT_EQUAL(0x33u, s_ctx.completion_unit[0]);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_ABORTED, s_ctx.completion_status[0]);
  s_ctx.backpressure = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
  TEST_ASSERT_EQUAL(0x11u, packet(0u).unit_id);
  append_response(packet(0u).transaction_id, 0x11u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(2u, s_ctx.completions);
  TEST_ASSERT_EQUAL(0x11u, s_ctx.completion_unit[1]);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, s_ctx.completion_status[1]);
  if (s_ctx.writes == 1u)
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(2u, s_ctx.writes);
  TEST_ASSERT_EQUAL(0x22u, packet(1u).unit_id);
  append_response(packet(1u).transaction_id, 0x22u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(3u, s_ctx.completions);
  TEST_ASSERT_EQUAL(0x22u, s_ctx.completion_unit[2]);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, s_ctx.completion_status[2]);
}

void test_tcp_elapsed_provider_time_is_charged_to_the_original_tx_deadline(void) {
  for (uint32_t provider = 0u; provider < 3u; ++provider) {
    create_master();
    s_ctx.backpressure = true;
    submit_request(0x11u);
    expect_temporary_backpressure(esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
    s_ctx.backpressure = false;
    if (provider == 0u) {
      s_ctx.progress_advance_ms = 10u;
      s_ctx.progress_result = ESY_ERR_BUSY;
    } else if (provider == 1u) {
      s_ctx.status_advance_ms = 10u;
      s_ctx.status_result = ESY_ERR_BUSY;
    } else {
      s_ctx.send_advance_ms = 10u;
    }
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
    TEST_ASSERT_EQUAL(provider == 2u ? 1u : 0u, s_ctx.writes);
    TEST_ASSERT_EQUAL(1u, s_ctx.completions);
    TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR, s_ctx.completion_status[0]);
    s_ctx.progress_result = ESY_OK;
    s_ctx.status_result = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
    TEST_ASSERT_EQUAL(provider == 2u ? 1u : 0u, s_ctx.writes);
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_destroy(s_ctx.master));
    s_ctx.master = NULL;
    reset_context();
  }
}

void test_tcp_callback_epoch_change_cannot_send_a_reply_or_next_request_to_the_new_peer(void) {
  create_slave();
  s_ctx.change_session_in_request = true;
  append_request(1u, 0x11u);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(1u, s_ctx.requests);
  TEST_ASSERT_EQUAL(0u, s_ctx.writes);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_destroy(s_ctx.slave));
  s_ctx.slave = NULL;
  reset_context();
  create_master();
  uint16_t transaction = send_request(0x11u);
  submit_request(0x22u);
  s_ctx.change_session_in_completion = true;
  append_response(transaction, 0x11u);
  esy_err_t result = esy_proto_modbus_tcp_master_channel_poll(s_ctx.master);
  TEST_ASSERT_TRUE(result == ESY_OK || result == ESY_ERR_STATE);
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(2u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, s_ctx.completion_status[0]);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR, s_ctx.completion_status[1]);
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
  TEST_ASSERT_EQUAL_UINT64(41u, s_ctx.packet_sessions[0]);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
}

void test_tcp_fin_first_observed_inside_receive_uses_buffer_and_request_state_for_eof(void) {
  create_slave();
  s_ctx.eof_on_empty_receive = true;
  TEST_ASSERT_EQUAL(ESY_NETWORK_CONNECTION_OPEN, s_ctx.status.state);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(1u, s_ctx.eof_during_receive_calls);
  TEST_ASSERT_EQUAL(0u, s_ctx.requests);
  TEST_ASSERT_EQUAL(0u, s_ctx.writes);
  TEST_ASSERT_TRUE(esy_proto_modbus_tcp_slave_channel_is_peer_drained(s_ctx.slave));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_destroy(s_ctx.slave));
  s_ctx.slave = NULL;
  reset_context();

  create_slave();
  append_request(1u, 0x11u);
  s_ctx.rx_length = 3u;
  s_ctx.eof_on_empty_receive = true;
  TEST_ASSERT_EQUAL(ESY_NETWORK_CONNECTION_OPEN, s_ctx.status.state);
  TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(1u, s_ctx.eof_during_receive_calls);
  TEST_ASSERT_EQUAL(0u, s_ctx.requests);
  TEST_ASSERT_EQUAL(0u, s_ctx.writes);
  TEST_ASSERT_FALSE(esy_proto_modbus_tcp_slave_channel_is_peer_drained(s_ctx.slave));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_destroy(s_ctx.slave));
  s_ctx.slave = NULL;
  reset_context();

  create_master();
  (void)send_request(0x11u);
  s_ctx.eof_on_empty_receive = true;
  TEST_ASSERT_EQUAL(ESY_NETWORK_CONNECTION_OPEN, s_ctx.status.state);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.eof_during_receive_calls);
  TEST_ASSERT_EQUAL(1u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR, s_ctx.completion_status[0]);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(1u, s_ctx.completions);
  TEST_ASSERT_EQUAL(1u, s_ctx.writes);
}
