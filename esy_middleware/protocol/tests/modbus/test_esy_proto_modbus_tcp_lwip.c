/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"

#include <string.h>

#include "esy_middleware/network/esy_network_lwip.h"
#include "esy_middleware/protocol/esy_proto_modbus_tcp_channel.h"
#include "fake_esy_hal_system.h"
#include "fake_lwip.h"
#include "network_lwip_dns_fixture.h"

#define TEST_RX_BYTES 8u
#define TEST_QUEUE_DEPTH 2u
#define TEST_ADU_BYTES (ESY_PROTO_MODBUS_MBAP_LEN + ESY_PROTO_MODBUS_MAX_PDU_LEN)
#define TEST_PORT 502u

typedef struct {
  esy_network_lwip_t endpoint;
  esy_network_connection_t provider;
  esy_network_connection_t connection;
  esy_network_lwip_static_resources_t network_resources;
  uint8_t network_rx[TEST_RX_BYTES];
  esy_proto_modbus_tcp_master_channel_storage_t master_storage;
  esy_proto_modbus_tcp_slave_channel_storage_t slave_storage;
  esy_proto_modbus_tcp_master_channel_t *master;
  esy_proto_modbus_tcp_slave_channel_t *slave;
  esy_prio_slot_t slots[TEST_QUEUE_DEPTH];
  esy_proto_modbus_payload_t payloads[TEST_QUEUE_DEPTH];
  uint8_t incoming[TEST_ADU_BYTES * 2u];
  uint8_t accepted[TEST_ADU_BYTES];
  uint16_t accepted_length;
  unsigned request_count;
  unsigned completion_count;
  esy_proto_modbus_req_status_t completion_status;
  uint8_t response[ESY_PROTO_MODBUS_MAX_PDU_LEN];
  uint16_t response_length;
} modbus_tcp_lwip_test_ctx_t;

static modbus_tcp_lwip_test_ctx_t s_ctx;
static const uint8_t k_request_pdu[] = {0x03u, 0x00u, 0x00u, 0x00u, 0x01u};
static const uint8_t k_response_pdu[] = {0x03u, 0x02u, 0x12u, 0x34u};

/* Observe accepted wire bytes while all I/O still uses the real lwIP adapter. */
static int32_t capture_whole_write(void *context, const void *data, size_t size) {
  int32_t result = s_ctx.provider.send_all(context, data, size);
  if (result > 0 && (size_t)result == size) {
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(sizeof(s_ctx.accepted), size);
    memcpy(s_ctx.accepted, data, size);
    s_ctx.accepted_length = (uint16_t)size;
  }
  return result;
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_hal_system_reset();
  fake_lwip_reset();
  network_lwip_dns_reset();
}

void tearDown(void) {
  fake_lwip_set_write_result(ERR_OK);
  fake_lwip_fail(ERR_OK, ERR_OK, ERR_OK, ERR_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_destroy(s_ctx.master));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_destroy(s_ctx.slave));
  if (network_lwip_dns_pending()) network_lwip_dns_complete(false);
  if (esy_network_lwip_get_state(&s_ctx.endpoint) != ESY_NETWORK_UNINITIALIZED) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_deinit(&s_ctx.endpoint));
  }
  TEST_ASSERT_EQUAL_UINT(0u, fake_lwip_live_pcbs());
  TEST_ASSERT_EQUAL_UINT(0u, fake_lwip_live_pbufs());
  TEST_ASSERT_EQUAL_UINT(0u, fake_lwip_invalid_pbuf_frees());
}

static void connect_endpoint(void) {
  s_ctx.network_resources.rx_buffer = s_ctx.network_rx;
  s_ctx.network_resources.rx_buffer_size = sizeof(s_ctx.network_rx);
  esy_network_lwip_config_t config = {.static_resources = &s_ctx.network_resources};
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_init(&s_ctx.endpoint, &config));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_network_lwip_begin(&s_ctx.endpoint, "192.0.2.1", TEST_PORT));
  TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_connect());
  TEST_ASSERT_EQUAL(ESY_NETWORK_CONNECTED, esy_network_lwip_get_state(&s_ctx.endpoint));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_network_lwip_get_connection(&s_ctx.endpoint, &s_ctx.provider));
  s_ctx.connection = s_ctx.provider;
  s_ctx.connection.send_all = capture_whole_write;
}

static uint16_t build_frame(uint8_t *bytes, size_t capacity, uint16_t transaction,
    const uint8_t *pdu, uint16_t pdu_length) {
  esy_proto_modbus_frame_t frame;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_frame_init(&frame, bytes, capacity));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_build_tcp_frame(&frame,
      transaction, UINT8_MAX, pdu, pdu_length));
  return (uint16_t)frame.len;
}

static esy_proto_modbus_frame_view_t accepted_frame(void) {
  esy_proto_modbus_frame_view_t view;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_parse_frame(ESY_PROTO_MODBUS_FRAME_TCP,
      s_ctx.accepted, s_ctx.accepted_length, &view));
  return view;
}

static void assert_accepted_response(uint16_t transaction) {
  esy_proto_modbus_frame_view_t view = accepted_frame();
  TEST_ASSERT_EQUAL_UINT16(transaction, view.transaction_id);
  TEST_ASSERT_EQUAL_UINT8(UINT8_MAX, view.unit_id);
  TEST_ASSERT_EQUAL_UINT16(sizeof(k_response_pdu), view.pdu_len);
  TEST_ASSERT_EQUAL_MEMORY(k_response_pdu, view.pdu, sizeof(k_response_pdu));
}

static esy_err_t slave_request(esy_proto_modbus_tcp_slave_channel_t *channel,
    uint8_t unit, esy_proto_modbus_func_t function, const uint8_t *request,
    uint16_t request_length, uint8_t *response, uint16_t *response_length,
    uint16_t response_capacity, void *argument) {
  modbus_tcp_lwip_test_ctx_t *ctx = argument;
  TEST_ASSERT_EQUAL_PTR(s_ctx.slave, channel);
  TEST_ASSERT_EQUAL_UINT8(UINT8_MAX, unit);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_FUNC_READ_HOLDING_REGISTERS, function);
  TEST_ASSERT_EQUAL_UINT16(sizeof(k_request_pdu), request_length);
  TEST_ASSERT_EQUAL_MEMORY(k_request_pdu, request, sizeof(k_request_pdu));
  TEST_ASSERT_GREATER_OR_EQUAL_UINT16(sizeof(k_response_pdu), response_capacity);
  memcpy(response, k_response_pdu, sizeof(k_response_pdu));
  *response_length = sizeof(k_response_pdu);
  ++ctx->request_count;
  return ESY_OK;
}

static void master_complete(esy_proto_modbus_tcp_master_channel_t *channel,
    esy_proto_modbus_req_status_t status, uint8_t unit,
    esy_proto_modbus_func_t function, const uint8_t *response,
    uint16_t response_length, void *argument) {
  modbus_tcp_lwip_test_ctx_t *ctx = argument;
  TEST_ASSERT_EQUAL_PTR(s_ctx.master, channel);
  TEST_ASSERT_EQUAL_UINT8(UINT8_MAX, unit);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_FUNC_READ_HOLDING_REGISTERS, function);
  TEST_ASSERT_LESS_OR_EQUAL_UINT16(sizeof(ctx->response), response_length);
  if (response_length != 0u) memcpy(ctx->response, response, response_length);
  ctx->response_length = response_length;
  ctx->completion_status = status;
  ++ctx->completion_count;
}

static void create_slave(void) {
  esy_proto_modbus_tcp_slave_channel_config_t config =
      esy_proto_modbus_tcp_slave_channel_default_cfg();
  config.connection = s_ctx.connection;
  config.on_request = slave_request;
  config.user_ctx = &s_ctx;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_create(
      &config, &s_ctx.slave_storage, &s_ctx.slave));
}

static void create_master(void) {
  esy_proto_modbus_tcp_master_channel_config_t config =
      esy_proto_modbus_tcp_master_channel_default_cfg();
  config.connection = s_ctx.connection;
  config.queue_depth = TEST_QUEUE_DEPTH;
  config.static_resources.slot_storage = s_ctx.slots;
  config.static_resources.slot_storage_count = TEST_QUEUE_DEPTH;
  config.static_resources.payload_storage = s_ctx.payloads;
  config.static_resources.payload_storage_count = TEST_QUEUE_DEPTH;
  config.on_complete = master_complete;
  config.user_ctx = &s_ctx;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_create(
      &config, &s_ctx.master_storage, &s_ctx.master));
}

static void submit_request(void) {
  esy_proto_modbus_request_t request = {0};
  request.unit_id = UINT8_MAX;
  request.func_code = ESY_PROTO_MODBUS_FUNC_READ_HOLDING_REGISTERS;
  request.pdu_len = sizeof(k_request_pdu);
  memcpy(request.pdu, k_request_pdu, sizeof(k_request_pdu));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_proto_modbus_tcp_master_channel_submit(s_ctx.master, &request));
}

void test_tcp_slave_real_lwip_split_and_coalesced_requests_drain_fin_after_reply_ack(void) {
  connect_endpoint();
  create_slave();
  struct tcp_pcb *pcb = fake_lwip_last_pcb();
  uint16_t first_length = build_frame(s_ctx.incoming, sizeof(s_ctx.incoming),
      0x1234u, k_request_pdu, sizeof(k_request_pdu));
  uint16_t second_length = build_frame(s_ctx.incoming + first_length,
      sizeof(s_ctx.incoming) - first_length, 0x5678u,
      k_request_pdu, sizeof(k_request_pdu));
  TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_receive(pcb, s_ctx.incoming, 3u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.request_count);
  TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_receive(pcb, s_ctx.incoming + 3u,
      (uint16_t)(first_length + second_length - 3u)));
  TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_fin(pcb));

  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.request_count);
  TEST_ASSERT_EQUAL_UINT(1u, fake_lwip_writes());
  assert_accepted_response(0x1234u);
  TEST_ASSERT_FALSE(esy_proto_modbus_tcp_slave_channel_is_peer_drained(s_ctx.slave));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.request_count);
  TEST_ASSERT_EQUAL_UINT(2u, fake_lwip_writes());
  assert_accepted_response(0x5678u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_FALSE(esy_proto_modbus_tcp_slave_channel_is_peer_drained(s_ctx.slave));
  fake_lwip_ack(pcb);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_TRUE(esy_proto_modbus_tcp_slave_channel_is_peer_drained(s_ctx.slave));

  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_slave_channel_destroy(s_ctx.slave));
  s_ctx.slave = NULL;
  uint8_t byte;
  /* The protocol borrower is detached before probing the provider's empty EOF. */
  TEST_ASSERT_EQUAL(ESY_ERR_IO,
      s_ctx.provider.stream.recv(s_ctx.provider.stream.context, &byte, sizeof(byte)));
  TEST_ASSERT_EQUAL_UINT(1u, fake_lwip_live_pcbs());
}

void test_tcp_master_real_lwip_response_and_fin_complete_before_peer_ack_drain(void) {
  connect_endpoint();
  create_master();
  submit_request();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL_UINT(1u, fake_lwip_writes());
  uint16_t length = build_frame(s_ctx.incoming, sizeof(s_ctx.incoming),
      accepted_frame().transaction_id, k_response_pdu, sizeof(k_response_pdu));
  struct tcp_pcb *pcb = fake_lwip_last_pcb();
  TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_receive(pcb, s_ctx.incoming, length));
  TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_fin(pcb));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.completion_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, s_ctx.completion_status);
  TEST_ASSERT_EQUAL_UINT16(sizeof(k_response_pdu), s_ctx.response_length);
  TEST_ASSERT_EQUAL_MEMORY(k_response_pdu, s_ctx.response, sizeof(k_response_pdu));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_FALSE(esy_proto_modbus_tcp_master_channel_is_peer_drained(s_ctx.master));
  fake_lwip_ack(pcb);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_TRUE(esy_proto_modbus_tcp_master_channel_is_peer_drained(s_ctx.master));
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.completion_count);
  TEST_ASSERT_EQUAL_UINT(1u, fake_lwip_writes());
}

void test_tcp_master_real_lwip_write_backpressure_recovers_without_duplicate_acceptance(void) {
  connect_endpoint();
  create_master();
  submit_request();
  fake_lwip_set_write_result(ERR_MEM);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  fake_esy_hal_system_advance_tick(200u);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.completion_count);
  TEST_ASSERT_EQUAL_UINT(0u, fake_lwip_writes());
  fake_lwip_set_write_result(ERR_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL_UINT(1u, fake_lwip_writes());
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL_UINT(1u, fake_lwip_writes());
  uint16_t length = build_frame(s_ctx.incoming, sizeof(s_ctx.incoming),
      accepted_frame().transaction_id, k_response_pdu, sizeof(k_response_pdu));
  TEST_ASSERT_EQUAL(ERR_OK,
      fake_lwip_receive(fake_lwip_last_pcb(), s_ctx.incoming, length));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.completion_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, s_ctx.completion_status);
  TEST_ASSERT_EQUAL_UINT(1u, fake_lwip_writes());
}

void test_tcp_master_real_lwip_replacement_session_never_receives_the_old_staged_request(void) {
  connect_endpoint();
  create_master();
  submit_request();
  fake_lwip_set_write_result(ERR_MEM);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  esy_network_connection_status_t before;
  TEST_ASSERT_EQUAL(ESY_OK,
      s_ctx.provider.get_status(s_ctx.provider.stream.context, &before));
  fake_lwip_error(fake_lwip_last_pcb());
  /* Replace the failed native peer between channel observations. */
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_close(&s_ctx.endpoint));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_network_lwip_begin(&s_ctx.endpoint, "192.0.2.1", TEST_PORT));
  TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_connect());
  esy_network_connection_status_t after;
  TEST_ASSERT_EQUAL(ESY_OK,
      s_ctx.provider.get_status(s_ctx.provider.stream.context, &after));
  TEST_ASSERT_NOT_EQUAL(before.session_id, after.session_id);
  fake_lwip_set_write_result(ERR_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR, s_ctx.completion_status);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_tcp_master_channel_poll(s_ctx.master));
  TEST_ASSERT_EQUAL_UINT(0u, fake_lwip_writes());
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.completion_count);
}
