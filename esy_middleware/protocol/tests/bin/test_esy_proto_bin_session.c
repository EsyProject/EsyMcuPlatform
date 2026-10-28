/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The sanitizer job must fail if its compiler flags are silently ignored. */
#if defined(ESY_PROTOCOL_SANITIZER) && !defined(__SANITIZE_ADDRESS__)
#error "Protocol sanitizer tests require GCC address-sanitizer instrumentation"
#endif

#include "unity.h"

#include <string.h>

#include "esy_middleware/protocol/esy_proto_bin_session.h"
#include "fake_esy_hal_system.h"

#define TEST_TX_PAYLOAD_MAX 8u
#define TEST_RX_PAYLOAD_MAX 8u
#define TEST_QUEUE_BLOCKS   4u
#define TEST_WORK_BUF_SIZE  32u
#define TEST_SENT_MAX       8u
#define TEST_FRAME_MAX      40u
#define TEST_RX_MAX         128u
#define TEST_STREAM_BYTES   100000u

typedef struct {
  esy_proto_bin_session_t *transport;
  bool tx_busy;
  bool busy_after_send;
  bool busy_after_data_send;
  bool queue_response_on_data;
  bool try_lifecycle_on_data;
  bool try_tx_complete_on_data;
  bool try_tx_complete_on_event;
  esy_err_t callback_deinit_result;
  esy_err_t data_tx_complete_result;
  esy_err_t event_tx_complete_result;
  uint32_t send_delay_ms;
  uint32_t data_cb_delay_ms;
  uint32_t busy_query_delay_ms;
  esy_err_t send_result;

  uint8_t rx_bytes[TEST_RX_MAX];
  uint32_t rx_len;
  uint32_t rx_pos;
  uint32_t rx_stream_bytes_left;
  uint32_t rx_stream_bytes_read;

  uint8_t sent[TEST_SENT_MAX][TEST_FRAME_MAX];
  uint32_t sent_len[TEST_SENT_MAX];
  uint32_t sent_count;

  esy_proto_bin_tp_event_t events[TEST_SENT_MAX];
  uint32_t event_transaction_ids[TEST_SENT_MAX];
  uint32_t event_count;

  uint32_t data_cb_count;
  uint8_t data_cmd;
  uint8_t data_seq;
  uint8_t data_payload[TEST_RX_PAYLOAD_MAX];
  uint32_t data_payload_len;
} fake_bin_io_t;

typedef struct {
  esy_proto_bin_session_t transport;
  esy_proto_bin_session_config_t cfg;
  esy_proto_bin_session_static_resources_t resources;
  fake_bin_io_t io;
  uint8_t tx_queue[TEST_QUEUE_BLOCKS * (ESY_PROTO_BIN_TX_BLOCK_OVERHEAD_BYTES +
                                        TEST_TX_PAYLOAD_MAX)];
  uint8_t tx_work[TEST_WORK_BUF_SIZE];
  uint8_t rx_work[TEST_WORK_BUF_SIZE];
} session_fixture_t;

void setUp(void) {
  fake_esy_hal_system_reset();
  fake_esy_hal_system_set_tick(0u);
}

void tearDown(void) {}

static bool fake_is_tx_busy(void *user_data) {
  fake_bin_io_t *fake = (fake_bin_io_t *)user_data;

  fake_esy_hal_system_advance_tick(fake->busy_query_delay_ms);
  return fake->tx_busy;
}

static bool fake_is_rx_ready(void *user_data) {
  fake_bin_io_t *fake = (fake_bin_io_t *)user_data;

  return fake->rx_pos < fake->rx_len || fake->rx_stream_bytes_left != 0u;
}

static esy_err_t fake_send(void *user_data, const uint8_t *data,
                           uint32_t length) {
  fake_bin_io_t *fake = (fake_bin_io_t *)user_data;

  if (fake->send_result != ESY_OK) return fake->send_result;
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(TEST_SENT_MAX, fake->sent_count + 1u);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(TEST_FRAME_MAX, length);
  memcpy(fake->sent[fake->sent_count], data, length);
  fake->sent_len[fake->sent_count] = length;
  fake->sent_count++;
  fake_esy_hal_system_advance_tick(fake->send_delay_ms);
  if (fake->busy_after_send ||
      (fake->busy_after_data_send && data[1] == ESY_PROTO_BIN_CMD_DATA)) {
    fake->tx_busy = true;
  }
  return ESY_OK;
}

static bool fake_receive(void *user_data, uint8_t *byte) {
  fake_bin_io_t *fake = (fake_bin_io_t *)user_data;

  if (fake->rx_pos < fake->rx_len) {
    *byte = fake->rx_bytes[fake->rx_pos++];
    return true;
  }
  if (fake->rx_stream_bytes_left != 0u) {
    --fake->rx_stream_bytes_left;
    ++fake->rx_stream_bytes_read;
    *byte = 0u;
    return true;
  }
  return false;
}

static void fake_event_cb(esy_proto_bin_tp_event_t event,
                          uint32_t transaction_id, void *user_data) {
  fake_bin_io_t *fake = (fake_bin_io_t *)user_data;

  TEST_ASSERT_LESS_OR_EQUAL_UINT32(TEST_SENT_MAX, fake->event_count + 1u);
  fake->events[fake->event_count] = event;
  fake->event_transaction_ids[fake->event_count] = transaction_id;
  fake->event_count++;
  if (fake->try_tx_complete_on_event) {
    fake->event_tx_complete_result = esy_proto_bin_session_on_tx_complete(
        fake->transport, ESY_ERR_IO);
  }
}

static void fake_data_cb(const esy_proto_bin_frame_t *frame, void *user_data) {
  fake_bin_io_t *fake = (fake_bin_io_t *)user_data;
  const uint8_t *payload = esy_proto_bin_frame_get_data(frame);
  size_t payload_len = esy_proto_bin_frame_get_data_len(frame);
  uint32_t cap = 0u;
  uint8_t *tx;

  TEST_ASSERT_LESS_OR_EQUAL_UINT32(TEST_RX_PAYLOAD_MAX, payload_len);
  fake->data_cb_count++;
  fake->data_cmd = esy_proto_bin_frame_get_cmd(frame);
  fake->data_seq = esy_proto_bin_frame_get_seq(frame);
  fake->data_payload_len = (uint32_t)payload_len;
  memcpy(fake->data_payload, payload, payload_len);
  fake_esy_hal_system_advance_tick(fake->data_cb_delay_ms);
  if (fake->try_tx_complete_on_data) {
    fake->data_tx_complete_result = esy_proto_bin_session_on_tx_complete(
        fake->transport, ESY_OK);
  }

  if (fake->try_lifecycle_on_data) {
    fake->callback_deinit_result = esy_proto_bin_session_deinit(fake->transport);
    esy_proto_bin_session_poll(fake->transport);
    esy_proto_bin_session_reset_session(fake->transport);
  }
  if (fake->queue_response_on_data) {
    static const uint8_t response[] = {0xA1u, 0xA2u};

    TEST_ASSERT_NOT_NULL(fake->transport);
    tx = esy_proto_bin_session_get_tx_buffer(fake->transport, &cap);
    TEST_ASSERT_NOT_NULL(tx);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(cap, sizeof(response));
    memcpy(tx, response, sizeof(response));
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_commit_tx(fake->transport,
                                                            sizeof(response), NULL));
  }
}

static void fixture_init(session_fixture_t *fixture, uint32_t retries) {
  memset(fixture, 0, sizeof(*fixture));
  esy_proto_bin_session_default_cfg(&fixture->cfg);
  fixture->cfg.static_resources = &fixture->resources;
  fixture->resources.tx_queue_buf = fixture->tx_queue;
  fixture->resources.tx_queue_buf_size = sizeof(fixture->tx_queue);
  fixture->cfg.tx_queue_block_count = TEST_QUEUE_BLOCKS;
  fixture->cfg.tx_payload_max = TEST_TX_PAYLOAD_MAX;
  fixture->cfg.rx_payload_max = TEST_RX_PAYLOAD_MAX;
  fixture->resources.tx_work_buf = fixture->tx_work;
  fixture->resources.tx_work_buf_size = sizeof(fixture->tx_work);
  fixture->resources.rx_work_buf = fixture->rx_work;
  fixture->resources.rx_work_buf_size = sizeof(fixture->rx_work);
  fixture->cfg.tx_completion_timeout_ms = 10u;
  fixture->cfg.acknowledgement_timeout_ms = 10u;
  fixture->cfg.retries = retries;
  fixture->cfg.data_cb = fake_data_cb;
  fixture->cfg.event_cb = fake_event_cb;
  fixture->cfg.io.is_tx_busy = fake_is_tx_busy;
  fixture->cfg.io.is_rx_ready = fake_is_rx_ready;
  fixture->cfg.io.send = fake_send;
  fixture->cfg.io.receive = fake_receive;
  fixture->cfg.user_data = &fixture->io;

  TEST_ASSERT_EQUAL(
      ESY_OK, esy_proto_bin_session_init(&fixture->transport, &fixture->cfg));
  fixture->io.transport = &fixture->transport;
}

static void queue_payload(session_fixture_t *fixture,
                          const uint8_t *payload, uint32_t payload_len) {
  uint32_t cap = 0u;
  uint8_t *tx = esy_proto_bin_session_get_tx_buffer(&fixture->transport, &cap);

  TEST_ASSERT_NOT_NULL(tx);
  TEST_ASSERT_EQUAL_UINT32(TEST_TX_PAYLOAD_MAX, cap);
  memcpy(tx, payload, payload_len);
  TEST_ASSERT_EQUAL(
      ESY_OK, esy_proto_bin_session_commit_tx(&fixture->transport, payload_len, NULL));
}

static uint32_t queue_tracked_payload(session_fixture_t *fixture,
                                      const uint8_t *payload,
                                      uint32_t payload_len) {
  uint32_t cap = 0u;
  uint32_t transaction_id = 0u;
  uint8_t *tx = esy_proto_bin_session_get_tx_buffer(&fixture->transport, &cap);

  TEST_ASSERT_NOT_NULL(tx);
  TEST_ASSERT_EQUAL_UINT32(TEST_TX_PAYLOAD_MAX, cap);
  memcpy(tx, payload, payload_len);
  TEST_ASSERT_EQUAL(ESY_OK,
                    esy_proto_bin_session_commit_tx(
                        &fixture->transport, payload_len, &transaction_id));
  TEST_ASSERT_NOT_EQUAL(ESY_PROTO_BIN_INVALID_TRANSACTION_ID, transaction_id);
  return transaction_id;
}

static esy_proto_bin_frame_t sent_frame_view(const fake_bin_io_t *fake,
                                             uint32_t index) {
  esy_proto_bin_frame_t frame = {
      .buf = (uint8_t *)fake->sent[index],
      .buf_size = TEST_FRAME_MAX,
      .len = fake->sent_len[index],
  };

  return frame;
}

static void append_rx_frame(fake_bin_io_t *fake, uint8_t cmd, uint8_t seq,
                            const uint8_t *payload, uint32_t payload_len) {
  uint8_t frame_buf[TEST_FRAME_MAX];
  esy_proto_bin_frame_t frame;

  TEST_ASSERT_EQUAL(
      ESY_OK, esy_proto_bin_frame_init(&frame, frame_buf, sizeof(frame_buf)));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_put_header(&frame, cmd, seq));
  if (payload_len > 0u) {
    TEST_ASSERT_EQUAL(
        ESY_OK, esy_proto_bin_frame_put_data(&frame, payload, payload_len));
  }
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_finish(&frame));
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(TEST_RX_MAX, fake->rx_len + frame.len);
  memcpy(&fake->rx_bytes[fake->rx_len], frame.buf, frame.len);
  fake->rx_len += (uint32_t)frame.len;
}

static void configure_rx_budget(session_fixture_t *fixture,
                                 uint32_t byte_budget) {
  esy_proto_bin_session_deinit(&fixture->transport);
  fixture->cfg.rx_byte_budget = byte_budget;
  TEST_ASSERT_EQUAL(
      ESY_OK, esy_proto_bin_session_init(&fixture->transport, &fixture->cfg));
}

void test_initial_backpressure_preserves_payload_transaction_and_sequence(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x61u, 0x62u};
  fixture_init(&fixture, 1u);
  const uint32_t id = queue_tracked_payload(&fixture, payload, sizeof(payload));
  fixture.io.send_result = ESY_ERR_BUSY;
  esy_proto_bin_session_poll(&fixture.transport);
  fake_esy_hal_system_set_tick(100u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.sent_count);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);
  TEST_ASSERT_FALSE(esy_proto_bin_session_is_idle(&fixture.transport));

  fixture.io.send_result = ESY_OK;
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.sent_count);
  esy_proto_bin_frame_t sent = sent_frame_view(&fixture.io, 0u);
  TEST_ASSERT_EQUAL_UINT8(1u, esy_proto_bin_frame_get_seq(&sent));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, esy_proto_bin_frame_get_data(&sent),
                                sizeof(payload));
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_ACK, 1u, NULL, 0u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_ACK, fixture.io.events[0]);
  TEST_ASSERT_EQUAL_UINT32(id, fixture.io.event_transaction_ids[0]);
  TEST_ASSERT_TRUE(esy_proto_bin_session_is_idle(&fixture.transport));
}

void test_retry_backpressure_disarms_expired_wait_through_full_clock_period(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x63u};
  const uint32_t stalled_ticks[] = {0x7FFFFFFAu, 0xFFFFFFF0u, 5u};
  fixture_init(&fixture, 1u);
  fake_esy_hal_system_set_tick(UINT32_MAX - 5u);
  queue_payload(&fixture, payload, sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);

  fixture.io.send_result = ESY_ERR_BUSY;
  fake_esy_hal_system_set_tick(5u);
  esy_proto_bin_session_poll(&fixture.transport);
  for (size_t i = 0u; i < sizeof(stalled_ticks) / sizeof(stalled_ticks[0]); ++i) {
    fake_esy_hal_system_set_tick(stalled_ticks[i]);
    esy_proto_bin_session_poll(&fixture.transport);
  }
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.sent_count);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);

  fixture.io.send_result = ESY_OK;
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.sent_count);
  esy_proto_bin_frame_t retried = sent_frame_view(&fixture.io, 1u);
  TEST_ASSERT_EQUAL_UINT8(1u, esy_proto_bin_frame_get_seq(&retried));
  fake_esy_hal_system_set_tick(14u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);
  fake_esy_hal_system_set_tick(15u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TIMEOUT, fixture.io.events[0]);
  TEST_ASSERT_TRUE(esy_proto_bin_session_is_idle(&fixture.transport));
}

void test_zero_rx_budget_selects_default_and_continuous_rx_allows_tx_and_timeout(
    void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x21u};

  fixture_init(&fixture, 0u);
  TEST_ASSERT_EQUAL_UINT32(ESY_PROTO_BIN_DEFAULT_RX_BYTE_BUDGET,
                           fixture.cfg.rx_byte_budget);
  configure_rx_budget(&fixture, 0u);
  fixture.io.rx_stream_bytes_left = TEST_STREAM_BYTES;
  queue_payload(&fixture, payload, sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(ESY_PROTO_BIN_DEFAULT_RX_BYTE_BUDGET,
                           fixture.io.rx_stream_bytes_read);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.sent_count);

  fake_esy_hal_system_set_tick(10u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(2u * ESY_PROTO_BIN_DEFAULT_RX_BYTE_BUDGET,
                           fixture.io.rx_stream_bytes_read);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TIMEOUT, fixture.io.events[0]);
}

void test_custom_rx_budget_allows_transmission_timeout_during_continuous_rx(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x22u};

  fixture_init(&fixture, 0u);
  configure_rx_budget(&fixture, 7u);
  fixture.io.rx_stream_bytes_left = TEST_STREAM_BYTES;
  fixture.io.busy_after_send = true;
  queue_payload(&fixture, payload, sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(7u, fixture.io.rx_stream_bytes_read);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.sent_count);

  fake_esy_hal_system_set_tick(10u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(14u, fixture.io.rx_stream_bytes_read);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TX_TIMEOUT, fixture.io.events[0]);
}

void test_rx_budget_preserves_frames_split_across_polls(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x31u, 0x32u, 0x33u, 0x34u,
                             0x35u, 0x36u, 0x37u, 0x38u};

  fixture_init(&fixture, 0u);
  configure_rx_budget(&fixture, 3u);
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_DATA, 7u, payload,
                  sizeof(payload));
  const uint32_t frame_bytes = fixture.io.rx_len;
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_DATA, 8u, payload,
                  sizeof(payload));
  while (fixture.io.rx_pos < fixture.io.rx_len) {
    const uint32_t before = fixture.io.rx_pos;
    esy_proto_bin_session_poll(&fixture.transport);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(3u, fixture.io.rx_pos - before);
    if (fixture.io.rx_pos < frame_bytes) {
      TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.data_cb_count);
    }
  }
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.data_cb_count);
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.sent_count);
  TEST_ASSERT_EQUAL_UINT32(sizeof(payload), fixture.io.data_payload_len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, fixture.io.data_payload,
                                sizeof(payload));
}

void test_rx_byte_budget_limits_complete_frame_callbacks_per_poll(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x41u};

  fixture_init(&fixture, 0u);
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_DATA, 7u, payload,
                  sizeof(payload));
  const uint32_t frame_bytes = fixture.io.rx_len;
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_DATA, 8u, payload,
                  sizeof(payload));
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_DATA, 9u, payload,
                  sizeof(payload));
  configure_rx_budget(&fixture, frame_bytes);

  for (uint32_t i = 1u; i <= 3u; ++i) {
    esy_proto_bin_session_poll(&fixture.transport);
    TEST_ASSERT_EQUAL_UINT32(i * frame_bytes, fixture.io.rx_pos);
    TEST_ASSERT_EQUAL_UINT32(i, fixture.io.data_cb_count);
    TEST_ASSERT_EQUAL_UINT32(i, fixture.io.sent_count);
  }
}

void test_session_idle_tracks_queued_and_acknowledged_payload(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x42u};

  fixture_init(&fixture, 1u);
  TEST_ASSERT_TRUE(esy_proto_bin_session_is_idle(&fixture.transport));

  queue_payload(&fixture, payload, sizeof(payload));
  TEST_ASSERT_FALSE(esy_proto_bin_session_is_idle(&fixture.transport));

  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_FALSE(esy_proto_bin_session_is_idle(&fixture.transport));

  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_ACK, 1u, NULL, 0u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_TRUE(esy_proto_bin_session_is_idle(&fixture.transport));
}

void test_session_sends_queued_payload_as_data_frame(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0xA1u, 0xB2u, 0xC3u};
  esy_proto_bin_frame_t sent;

  fixture_init(&fixture, 1u);
  queue_payload(&fixture, payload, sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);

  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.sent_count);
  sent = sent_frame_view(&fixture.io, 0u);
  TEST_ASSERT_EQUAL_UINT8(ESY_PROTO_BIN_CMD_DATA,
                          esy_proto_bin_frame_get_cmd(&sent));
  TEST_ASSERT_EQUAL_UINT8(1u, esy_proto_bin_frame_get_seq(&sent));
  TEST_ASSERT_EQUAL_UINT(sizeof(payload),
                         esy_proto_bin_frame_get_data_len(&sent));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, esy_proto_bin_frame_get_data(&sent),
                                sizeof(payload));
}

void test_session_ack_completes_transaction_and_allows_next_payload(
    void) {
  session_fixture_t fixture;
  const uint8_t first[] = {0x01u};
  const uint8_t second[] = {0x02u};
  esy_proto_bin_frame_t sent;

  fixture_init(&fixture, 1u);
  queue_payload(&fixture, first, sizeof(first));
  esy_proto_bin_session_poll(&fixture.transport);
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_ACK, 1u, NULL, 0u);
  esy_proto_bin_session_poll(&fixture.transport);

  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_ACK, fixture.io.events[0]);

  queue_payload(&fixture, second, sizeof(second));
  esy_proto_bin_session_poll(&fixture.transport);

  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.sent_count);
  sent = sent_frame_view(&fixture.io, 1u);
  TEST_ASSERT_EQUAL_UINT8(ESY_PROTO_BIN_CMD_DATA,
                          esy_proto_bin_frame_get_cmd(&sent));
  TEST_ASSERT_EQUAL_UINT8(2u, esy_proto_bin_frame_get_seq(&sent));
}

void test_session_events_report_transaction_id(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x31u};
  uint32_t transaction_id;

  fixture_init(&fixture, 0u);
  transaction_id = queue_tracked_payload(&fixture, payload, sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_ACK, 1u, NULL, 0u);
  esy_proto_bin_session_poll(&fixture.transport);

  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_ACK, fixture.io.events[0]);
  TEST_ASSERT_EQUAL_UINT32(transaction_id, fixture.io.event_transaction_ids[0]);
}

void test_session_nack_retries_current_payload_immediately(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x10u, 0x11u};
  esy_proto_bin_frame_t retry;

  fixture_init(&fixture, 1u);
  queue_payload(&fixture, payload, sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_NACK, 1u, NULL, 0u);
  esy_proto_bin_session_poll(&fixture.transport);

  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_NACK, fixture.io.events[0]);
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.sent_count);
  retry = sent_frame_view(&fixture.io, 1u);
  TEST_ASSERT_EQUAL_UINT8(ESY_PROTO_BIN_CMD_DATA,
                          esy_proto_bin_frame_get_cmd(&retry));
  TEST_ASSERT_EQUAL_UINT8(1u, esy_proto_bin_frame_get_seq(&retry));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, esy_proto_bin_frame_get_data(&retry),
                                sizeof(payload));
}

void test_session_early_nack_keeps_the_original_physical_timeout(void) {
  const uint8_t payload[] = {0x12u, 0x13u};
  const uint8_t next_payload[] = {0x99u};

  for (uint32_t retries = 0u; retries < 2u; ++retries) {
    session_fixture_t fixture;
    uint8_t borrowed_work[TEST_WORK_BUF_SIZE];
    fixture_init(&fixture, retries);
    fake_esy_hal_system_set_tick(0u);
    fixture.io.busy_after_send = true;
    const uint32_t id = queue_tracked_payload(&fixture, payload, sizeof(payload));
    esy_proto_bin_session_poll(&fixture.transport);
    memcpy(borrowed_work, fixture.tx_work, sizeof(borrowed_work));

    fake_esy_hal_system_set_tick(1u);
    append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_NACK, 1u, NULL, 0u);
    esy_proto_bin_session_poll(&fixture.transport);
    fake_esy_hal_system_set_tick(9u);
    append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_NACK, 1u, NULL, 0u);
    esy_proto_bin_session_poll(&fixture.transport);
    TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.event_count);
    TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_NACK, fixture.io.events[0]);
    TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_NACK, fixture.io.events[1]);
    TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.sent_count);

    fake_esy_hal_system_set_tick(10u);
    esy_proto_bin_session_poll(&fixture.transport);
    TEST_ASSERT_EQUAL_UINT32(3u, fixture.io.event_count);
    TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TX_TIMEOUT, fixture.io.events[2]);
    TEST_ASSERT_EQUAL_UINT32(id, fixture.io.event_transaction_ids[2]);
    TEST_ASSERT_FALSE(esy_proto_bin_session_is_idle(&fixture.transport));

    queue_payload(&fixture, next_payload, sizeof(next_payload));
    fake_esy_hal_system_set_tick(100u);
    esy_proto_bin_session_poll(&fixture.transport);
    TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.sent_count);
    TEST_ASSERT_EQUAL_MEMORY(borrowed_work, fixture.tx_work, sizeof(borrowed_work));
    fixture.io.tx_busy = false;
    esy_proto_bin_session_poll(&fixture.transport);
    TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.sent_count);
    esy_proto_bin_frame_t sent = sent_frame_view(&fixture.io, 1u);
    TEST_ASSERT_EQUAL_UINT8(2u, esy_proto_bin_frame_get_seq(&sent));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(next_payload, esy_proto_bin_frame_get_data(&sent),
                                  sizeof(next_payload));
  }
}

void test_session_early_nack_retries_only_after_idle_with_a_fresh_deadline(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x14u, 0x15u};
  fixture_init(&fixture, 1u);
  fixture.io.busy_after_send = true;
  const uint32_t id = queue_tracked_payload(&fixture, payload, sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);
  fake_esy_hal_system_set_tick(1u);
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_NACK, 1u, NULL, 0u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.sent_count);

  fake_esy_hal_system_set_tick(8u);
  fixture.io.tx_busy = false;
  esy_proto_bin_session_poll(&fixture.transport);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.sent_count);
  TEST_ASSERT_EQUAL_MEMORY(fixture.io.sent[0], fixture.io.sent[1], fixture.io.sent_len[0]);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);

  fake_esy_hal_system_set_tick(9u);
  fixture.io.tx_busy = false;
  esy_proto_bin_session_poll(&fixture.transport);
  esy_proto_bin_session_poll(&fixture.transport);
  fake_esy_hal_system_set_tick(18u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_ACK, 1u, NULL, 0u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_ACK, fixture.io.events[1]);
  TEST_ASSERT_EQUAL_UINT32(id, fixture.io.event_transaction_ids[1]);
  TEST_ASSERT_TRUE(esy_proto_bin_session_is_idle(&fixture.transport));
}

void test_session_early_nack_without_retries_waits_for_physical_idle(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x16u};
  fixture_init(&fixture, 0u);
  fixture.io.busy_after_send = true;
  const uint32_t id = queue_tracked_payload(&fixture, payload, sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_NACK, 1u, NULL, 0u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);

  fake_esy_hal_system_set_tick(8u);
  fixture.io.tx_busy = false;
  esy_proto_bin_session_poll(&fixture.transport);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.sent_count);
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TIMEOUT, fixture.io.events[1]);
  TEST_ASSERT_EQUAL_UINT32(id, fixture.io.event_transaction_ids[1]);
  TEST_ASSERT_TRUE(esy_proto_bin_session_is_idle(&fixture.transport));
}

void test_session_pending_control_ack_preserves_an_early_nack_retry(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x17u};
  fixture_init(&fixture, 1u);
  fixture.io.busy_after_data_send = true;
  queue_payload(&fixture, payload, sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_NACK, 1u, NULL, 0u);
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_DATA, 7u, payload, sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.sent_count);

  fake_esy_hal_system_set_tick(8u);
  fixture.io.tx_busy = false;
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(3u, fixture.io.sent_count);
  esy_proto_bin_frame_t control = sent_frame_view(&fixture.io, 1u);
  TEST_ASSERT_EQUAL_UINT8(ESY_PROTO_BIN_CMD_ACK, esy_proto_bin_frame_get_cmd(&control));
  TEST_ASSERT_EQUAL_MEMORY(fixture.io.sent[0], fixture.io.sent[2], fixture.io.sent_len[0]);
}

void test_session_ignores_ack_for_different_sequence(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x22u};
  uint32_t transaction_id;

  fixture_init(&fixture, 0u);
  transaction_id = queue_tracked_payload(&fixture, payload, sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_ACK, 2u, NULL, 0u);
  esy_proto_bin_session_poll(&fixture.transport);

  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);

  fake_esy_hal_system_set_tick(10u);
  esy_proto_bin_session_poll(&fixture.transport);

  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TIMEOUT, fixture.io.events[0]);
  TEST_ASSERT_EQUAL_UINT32(transaction_id, fixture.io.event_transaction_ids[0]);
}

void test_session_receives_data_frame_and_sends_ack(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x44u, 0x55u, 0x66u};
  esy_proto_bin_frame_t ack;

  fixture_init(&fixture, 1u);
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_DATA, 7u, payload,
                  sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);

  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.data_cb_count);
  TEST_ASSERT_EQUAL_UINT8(ESY_PROTO_BIN_CMD_DATA, fixture.io.data_cmd);
  TEST_ASSERT_EQUAL_UINT8(7u, fixture.io.data_seq);
  TEST_ASSERT_EQUAL_UINT32(sizeof(payload), fixture.io.data_payload_len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, fixture.io.data_payload,
                                sizeof(payload));
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.sent_count);
  ack = sent_frame_view(&fixture.io, 0u);
  TEST_ASSERT_EQUAL_UINT8(ESY_PROTO_BIN_CMD_ACK,
                          esy_proto_bin_frame_get_cmd(&ack));
  TEST_ASSERT_EQUAL_UINT8(7u, esy_proto_bin_frame_get_seq(&ack));
}

void test_session_sends_ack_before_callback_queued_response(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x44u, 0x55u, 0x66u};
  esy_proto_bin_frame_t ack;
  esy_proto_bin_frame_t response;

  fixture_init(&fixture, 1u);
  fixture.io.queue_response_on_data = true;
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_DATA, 7u, payload,
                  sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);

  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.data_cb_count);
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.sent_count);
  ack = sent_frame_view(&fixture.io, 0u);
  TEST_ASSERT_EQUAL_UINT8(ESY_PROTO_BIN_CMD_ACK,
                          esy_proto_bin_frame_get_cmd(&ack));
  TEST_ASSERT_EQUAL_UINT8(7u, esy_proto_bin_frame_get_seq(&ack));
  response = sent_frame_view(&fixture.io, 1u);
  TEST_ASSERT_EQUAL_UINT8(ESY_PROTO_BIN_CMD_DATA,
                          esy_proto_bin_frame_get_cmd(&response));
}

void test_session_matching_data_does_not_ack_in_flight_transaction(void) {
  session_fixture_t fixture;
  const uint8_t request[] = {0x10u};
  const uint8_t response[] = {0x20u};
  uint32_t transaction_id;
  esy_proto_bin_frame_t ack;

  fixture_init(&fixture, 0u);
  transaction_id = queue_tracked_payload(&fixture, request, sizeof(request));
  esy_proto_bin_session_poll(&fixture.transport);

  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_DATA, 1u, response,
                  sizeof(response));
  esy_proto_bin_session_poll(&fixture.transport);

  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.data_cb_count);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(response, fixture.io.data_payload,
                                sizeof(response));
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.sent_count);
  ack = sent_frame_view(&fixture.io, 1u);
  TEST_ASSERT_EQUAL_UINT8(ESY_PROTO_BIN_CMD_ACK,
                          esy_proto_bin_frame_get_cmd(&ack));
  TEST_ASSERT_EQUAL_UINT8(1u, esy_proto_bin_frame_get_seq(&ack));

  fake_esy_hal_system_set_tick(10u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TIMEOUT, fixture.io.events[0]);
  TEST_ASSERT_EQUAL_UINT32(transaction_id, fixture.io.event_transaction_ids[0]);
}

void test_session_acknowledges_retransmitted_data_but_delivers_once(
    void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x44u, 0x55u};
  esy_proto_bin_frame_t first_ack;
  esy_proto_bin_frame_t second_ack;

  fixture_init(&fixture, 0u);
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_DATA, 7u, payload,
                  sizeof(payload));
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_DATA, 7u, payload,
                  sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);

  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.data_cb_count);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, fixture.io.data_payload,
                                sizeof(payload));
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.sent_count);
  first_ack = sent_frame_view(&fixture.io, 0u);
  second_ack = sent_frame_view(&fixture.io, 1u);
  TEST_ASSERT_EQUAL_UINT8(ESY_PROTO_BIN_CMD_ACK,
                          esy_proto_bin_frame_get_cmd(&first_ack));
  TEST_ASSERT_EQUAL_UINT8(7u, esy_proto_bin_frame_get_seq(&first_ack));
  TEST_ASSERT_EQUAL_UINT8(ESY_PROTO_BIN_CMD_ACK,
                          esy_proto_bin_frame_get_cmd(&second_ack));
  TEST_ASSERT_EQUAL_UINT8(7u, esy_proto_bin_frame_get_seq(&second_ack));
}

void test_session_reports_crc_error_and_recovers_parser(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x77u};

  fixture_init(&fixture, 1u);
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_DATA, 3u, payload,
                  sizeof(payload));
  fixture.io.rx_bytes[fixture.io.rx_len - 1u] ^= 0x01u;
  esy_proto_bin_session_poll(&fixture.transport);

  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_RX_CRC_ERROR, fixture.io.events[0]);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.data_cb_count);
}

void test_session_timeout_retries_then_drops_payload(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x88u};
  uint32_t cap = 0u;

  fixture_init(&fixture, 1u);
  queue_payload(&fixture, payload, sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.sent_count);

  fake_esy_hal_system_set_tick(10u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.sent_count);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);

  fake_esy_hal_system_set_tick(20u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TIMEOUT, fixture.io.events[0]);

  TEST_ASSERT_NOT_NULL(
      esy_proto_bin_session_get_tx_buffer(&fixture.transport, &cap));
  TEST_ASSERT_EQUAL_UINT32(TEST_TX_PAYLOAD_MAX, cap);
}

void test_session_starts_ack_timeout_after_transmission_completes(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x89u};

  fixture_init(&fixture, 0u);
  fixture.io.busy_after_send = true;
  queue_payload(&fixture, payload, sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);

  fake_esy_hal_system_set_tick(9u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);

  fixture.io.tx_busy = false;
  fake_esy_hal_system_set_tick(10u);
  esy_proto_bin_session_poll(&fixture.transport);

  fake_esy_hal_system_set_tick(19u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);

  fake_esy_hal_system_set_tick(20u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TIMEOUT, fixture.io.events[0]);
}

void test_session_transmission_timeout_does_not_retry(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x8Au};

  fixture_init(&fixture, 3u);
  fixture.io.busy_after_send = true;
  queue_payload(&fixture, payload, sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);

  fake_esy_hal_system_set_tick(10u);
  esy_proto_bin_session_poll(&fixture.transport);

  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.sent_count);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TX_TIMEOUT, fixture.io.events[0]);
}

void test_slow_data_callback_does_not_consume_new_transmission_timeout(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x44u};

  fixture_init(&fixture, 0u);
  fixture.io.queue_response_on_data = true;
  fixture.io.data_cb_delay_ms = 100u;
  fixture.io.busy_after_data_send = true;
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_DATA, 7u, payload,
                  sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.sent_count);

  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);
  fake_esy_hal_system_set_tick(109u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);
  fake_esy_hal_system_set_tick(110u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TX_TIMEOUT, fixture.io.events[0]);
}

void test_slow_data_callback_does_not_consume_new_ack_timeout(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x45u};

  fixture_init(&fixture, 0u);
  fixture.io.queue_response_on_data = true;
  fixture.io.data_cb_delay_ms = 100u;
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_DATA, 7u, payload,
                  sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.sent_count);

  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);
  fake_esy_hal_system_set_tick(109u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);
  fake_esy_hal_system_set_tick(110u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TIMEOUT, fixture.io.events[0]);
}

void test_slow_data_callback_does_not_delay_an_existing_ack_timeout(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x45u};

  fixture_init(&fixture, 0u);
  queue_payload(&fixture, payload, sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);
  fixture.io.data_cb_delay_ms = 100u;
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_DATA, 7u, payload,
                  sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);

  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TIMEOUT, fixture.io.events[0]);
}

void test_slow_send_starts_transmission_timeout_after_acceptance(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x46u};

  fixture_init(&fixture, 0u);
  fixture.io.send_delay_ms = 100u;
  fixture.io.busy_after_send = true;
  queue_payload(&fixture, payload, sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);

  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);
  fake_esy_hal_system_set_tick(109u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);
  fake_esy_hal_system_set_tick(110u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TX_TIMEOUT, fixture.io.events[0]);
}

void test_slow_retry_starts_a_fresh_ack_timeout_after_acceptance(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x47u};

  fixture_init(&fixture, 1u);
  fixture.io.send_delay_ms = 100u;
  queue_payload(&fixture, payload, sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);
  fake_esy_hal_system_set_tick(110u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.sent_count);

  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);
  fake_esy_hal_system_set_tick(219u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);
  fake_esy_hal_system_set_tick(220u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TIMEOUT, fixture.io.events[0]);
}

void test_slow_retry_starts_a_fresh_transmission_timeout_after_acceptance(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x47u};

  fixture_init(&fixture, 1u);
  queue_payload(&fixture, payload, sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);
  fixture.io.send_delay_ms = 100u;
  fixture.io.busy_after_send = true;
  fake_esy_hal_system_set_tick(10u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.sent_count);

  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);
  fake_esy_hal_system_set_tick(119u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);
  fake_esy_hal_system_set_tick(120u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TX_TIMEOUT, fixture.io.events[0]);
}

void test_slow_idle_observation_starts_ack_timeout_after_observation(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x48u};

  fixture_init(&fixture, 0u);
  fixture.io.busy_after_send = true;
  queue_payload(&fixture, payload, sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);

  fake_esy_hal_system_set_tick(1u);
  fixture.io.tx_busy = false;
  fixture.io.busy_query_delay_ms = 100u;
  esy_proto_bin_session_poll(&fixture.transport);
  fixture.io.busy_query_delay_ms = 0u;
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);
  fake_esy_hal_system_set_tick(110u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);
  fake_esy_hal_system_set_tick(111u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TIMEOUT, fixture.io.events[0]);
}

void test_slow_send_deadline_remains_correct_across_tick_wrap(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x49u};

  fixture_init(&fixture, 0u);
  fake_esy_hal_system_set_tick(UINT32_MAX - 5u);
  fixture.io.send_delay_ms = 10u;
  fixture.io.busy_after_send = true;
  queue_payload(&fixture, payload, sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);

  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);
  fake_esy_hal_system_set_tick(13u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);
  fake_esy_hal_system_set_tick(14u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TX_TIMEOUT, fixture.io.events[0]);
}

void test_maximum_phase_timeouts_expire_at_the_boundary_across_tick_wrap(void) {
  const uint8_t payload[] = {0x4Au};

  for (uint32_t transmitting = 0u; transmitting < 2u; ++transmitting) {
    session_fixture_t fixture;
    fixture_init(&fixture, 0u);
    esy_proto_bin_session_deinit(&fixture.transport);
    fixture.cfg.tx_completion_timeout_ms = INT32_MAX;
    fixture.cfg.acknowledgement_timeout_ms = INT32_MAX;
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_init(
                                  &fixture.transport, &fixture.cfg));
    fake_esy_hal_system_set_tick(UINT32_MAX - 5u);
    fixture.io.send_delay_ms = 10u;
    fixture.io.busy_after_send = transmitting != 0u;
    queue_payload(&fixture, payload, sizeof(payload));
    esy_proto_bin_session_poll(&fixture.transport);

    fake_esy_hal_system_set_tick(4u + (uint32_t)INT32_MAX - 1u);
    esy_proto_bin_session_poll(&fixture.transport);
    TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);
    fake_esy_hal_system_advance_tick(1u);
    esy_proto_bin_session_poll(&fixture.transport);
    TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
    TEST_ASSERT_EQUAL(transmitting ? ESY_PROTO_BIN_TP_EVENT_TX_TIMEOUT
                                    : ESY_PROTO_BIN_TP_EVENT_TIMEOUT,
                      fixture.io.events[0]);
    esy_proto_bin_session_deinit(&fixture.transport);
  }
}

void test_session_retry_has_its_own_transmission_phase(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x8Bu};

  fixture_init(&fixture, 1u);
  queue_payload(&fixture, payload, sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);

  fixture.io.busy_after_send = true;
  fake_esy_hal_system_set_tick(10u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.sent_count);

  fake_esy_hal_system_set_tick(19u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);

  fixture.io.tx_busy = false;
  fake_esy_hal_system_set_tick(20u);
  esy_proto_bin_session_poll(&fixture.transport);
  fake_esy_hal_system_set_tick(29u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.event_count);

  fake_esy_hal_system_set_tick(30u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TIMEOUT, fixture.io.events[0]);
}

void test_session_defers_send_while_tx_is_busy(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x99u};

  fixture_init(&fixture, 1u);
  queue_payload(&fixture, payload, sizeof(payload));

  fixture.io.tx_busy = true;
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.sent_count);

  fixture.io.tx_busy = false;
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.sent_count);
}

void test_session_reports_io_error_when_send_start_fails(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x99u};

  fixture_init(&fixture, 1u);
  queue_payload(&fixture, payload, sizeof(payload));
  fixture.io.send_result = ESY_ERR_IO;

  esy_proto_bin_session_poll(&fixture.transport);

  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.sent_count);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_IO_ERROR, fixture.io.events[0]);
  TEST_ASSERT_TRUE(esy_proto_bin_session_is_idle(&fixture.transport));
}

void test_session_rejects_invalid_configuration(void) {
  session_fixture_t fixture;

  memset(&fixture, 0, sizeof(fixture));
  esy_proto_bin_session_default_cfg(&fixture.cfg);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_proto_bin_session_init(NULL, &fixture.cfg));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_proto_bin_session_init(&fixture.transport, NULL));

  fixture.cfg.static_resources = &fixture.resources;
  fixture.resources.tx_queue_buf = fixture.tx_queue;
  fixture.resources.tx_queue_buf_size = sizeof(fixture.tx_queue);
  fixture.cfg.tx_queue_block_count = 3u;
  fixture.cfg.tx_payload_max = TEST_TX_PAYLOAD_MAX;
  fixture.cfg.rx_payload_max = TEST_RX_PAYLOAD_MAX;
  fixture.resources.tx_work_buf = fixture.tx_work;
  fixture.resources.tx_work_buf_size = sizeof(fixture.tx_work);
  fixture.resources.rx_work_buf = fixture.rx_work;
  fixture.resources.rx_work_buf_size = sizeof(fixture.rx_work);
  fixture.cfg.tx_completion_timeout_ms = 10u;
  fixture.cfg.acknowledgement_timeout_ms = 10u;
  fixture.cfg.io.send = fake_send;
  fixture.cfg.io.receive = fake_receive;
  fixture.cfg.user_data = &fixture.io;

  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_bin_session_init(
                                             &fixture.transport, &fixture.cfg));
}

void test_session_requires_readiness_send_and_receive_hooks(void) {
  session_fixture_t fixture;
  fixture_init(&fixture, 0u);
  esy_proto_bin_session_deinit(&fixture.transport);

  fixture.cfg.io.is_rx_ready = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_proto_bin_session_init(&fixture.transport, &fixture.cfg));
  TEST_ASSERT_FALSE(esy_proto_bin_session_is_initialized(&fixture.transport));
  fixture.cfg.io.is_rx_ready = fake_is_rx_ready;
  fixture.cfg.io.send = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_proto_bin_session_init(&fixture.transport, &fixture.cfg));
  fixture.cfg.io.send = fake_send;
  fixture.cfg.io.receive = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_proto_bin_session_init(&fixture.transport, &fixture.cfg));
  fixture.cfg.io.receive = fake_receive;
  TEST_ASSERT_EQUAL(ESY_OK,
                    esy_proto_bin_session_init(&fixture.transport, &fixture.cfg));
}

void test_session_allows_synchronous_sender_without_busy_hook(void) {
  session_fixture_t fixture;
  const uint8_t payload[] = {0x81u};
  fixture_init(&fixture, 0u);
  esy_proto_bin_session_deinit(&fixture.transport);
  fixture.cfg.io.is_tx_busy = NULL;
  TEST_ASSERT_EQUAL(ESY_OK,
                    esy_proto_bin_session_init(&fixture.transport, &fixture.cfg));
  queue_payload(&fixture, payload, sizeof(payload));
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.sent_count);
  append_rx_frame(&fixture.io, ESY_PROTO_BIN_CMD_ACK, 1u, NULL, 0u);
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_ACK, fixture.io.events[0]);
}

void test_cancel_uncommitted_tx_preserves_committed_frames(void) {
  session_fixture_t fixture;
  fixture_init(&fixture, 0u);
  const uint8_t first[] = {1u, 2u};
  queue_payload(&fixture, first, sizeof(first));
  TEST_ASSERT_NOT_NULL(esy_proto_bin_session_get_tx_buffer(&fixture.transport, NULL));
  TEST_ASSERT_NULL(esy_proto_bin_session_get_tx_buffer(&fixture.transport, NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_cancel_tx(&fixture.transport));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_cancel_tx(&fixture.transport));
  esy_proto_bin_session_poll(&fixture.transport);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.sent_count);
  TEST_ASSERT_NOT_NULL(esy_proto_bin_session_get_tx_buffer(&fixture.transport, NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_cancel_tx(&fixture.transport));
  esy_proto_bin_session_deinit(&fixture.transport);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_bin_session_cancel_tx(&fixture.transport));
}

void test_bin_ack_deferred_while_busy_and_rx_limit_enforced(void) {
  session_fixture_t f;
  fixture_init(&f, 0);
  const uint8_t payload[9] = {1};
  f.io.tx_busy = true;
  append_rx_frame(&f.io, ESY_PROTO_BIN_CMD_DATA, 1, payload, 1);
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(1, f.io.data_cb_count);
  TEST_ASSERT_EQUAL_UINT(0, f.io.sent_count);
  f.io.tx_busy = false;
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(1, f.io.sent_count);
  TEST_ASSERT_EQUAL_HEX8(ESY_PROTO_BIN_CMD_ACK, f.io.sent[0][1]);
  append_rx_frame(&f.io, ESY_PROTO_BIN_CMD_DATA, 2, payload, sizeof(payload));
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(1, f.io.data_cb_count);
}
void test_bin_rejects_oversized_payload_and_timeout_and_active_init(void) {
  session_fixture_t f;
  fixture_init(&f, 0);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
                    esy_proto_bin_session_init(&f.transport, &f.cfg));
  esy_proto_bin_session_deinit(&f.transport);
  f.cfg.tx_payload_max = UINT32_MAX - 8;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_proto_bin_session_init(&f.transport, &f.cfg));
  f.cfg.tx_payload_max = 8;
  f.cfg.acknowledgement_timeout_ms = 0x80000001u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_proto_bin_session_init(&f.transport, &f.cfg));
}
void test_bin_odd_stride_unaligned_queue_storage_and_aligned_handle(void) {
  session_fixture_t f;
  fixture_init(&f, 0);
  esy_proto_bin_session_deinit(&f.transport);
  f.resources.tx_queue_buf = f.tx_queue + 1;
  f.resources.tx_queue_buf_size = sizeof(f.tx_queue) - 1u;
  f.cfg.tx_payload_max = 3;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_init(&f.transport, &f.cfg));
  for (unsigned i = 0; i < 2; i++) {
    uint8_t* p = esy_proto_bin_session_get_tx_buffer(&f.transport, NULL);
    TEST_ASSERT_NOT_NULL(p);
    *p = 1;
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_commit_tx(&f.transport, 1, NULL));
  }
  esy_proto_bin_session_poll(&f.transport);
  append_rx_frame(&f.io, ESY_PROTO_BIN_CMD_ACK, 1, NULL, 0);
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(2, f.io.sent_count);
  TEST_ASSERT_EQUAL_UINT(
      0, (uintptr_t)&f.transport % ESY_PROTO_BIN_SESSION_ALIGNMENT);
}

void test_deferred_ack_does_not_extend_the_data_transmission_phase(void) {
  session_fixture_t f;
  fixture_init(&f, 0);
  f.io.busy_after_send = true;
  const uint8_t payload = 1;
  queue_payload(&f, &payload, 1);
  esy_proto_bin_session_poll(&f.transport);
  append_rx_frame(&f.io, ESY_PROTO_BIN_CMD_DATA, 7, &payload, 1);
  esy_proto_bin_session_poll(&f.transport);
  fake_esy_hal_system_set_tick(9);
  f.io.tx_busy = false;
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(2, f.io.sent_count);
  fake_esy_hal_system_set_tick(10);
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(0, f.io.event_count);
}

void test_session_invalid_and_inactive_operations_preserve_outputs_and_state(void) {
  esy_proto_bin_session_t session = {0};
  esy_proto_bin_sender_t sender = {0};
  esy_proto_bin_sender_t before = sender;
  uint32_t capacity = 99u;
  uint32_t id = 99u;
  TEST_ASSERT_FALSE(esy_proto_bin_session_is_initialized(NULL));
  TEST_ASSERT_FALSE(esy_proto_bin_session_is_idle(&session));
  TEST_ASSERT_NULL(esy_proto_bin_session_get_tx_buffer(NULL, &capacity));
  TEST_ASSERT_EQUAL_UINT32(0u, capacity);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_proto_bin_session_commit_tx(NULL, 1u, &id));
  TEST_ASSERT_EQUAL_UINT32(0u, id);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_bin_session_commit_tx(&session, 1u, &id));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_bin_session_cancel_tx(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_bin_session_cancel_tx(&session));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_bin_session_deinit(NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_deinit(&session));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_bin_session_get_sender(NULL, &sender));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_bin_session_get_sender(&session, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_bin_session_get_sender(&session, &sender));
  TEST_ASSERT_EQUAL_MEMORY(&before, &sender, sizeof(sender));
  esy_proto_bin_session_poll(NULL);
  esy_proto_bin_session_poll(&session);
  esy_proto_bin_session_reset_rx(NULL);
  esy_proto_bin_session_reset_rx(&session);
  esy_proto_bin_session_reset_session(NULL);
  esy_proto_bin_session_reset_session(&session);
  esy_proto_bin_session_default_cfg(NULL);
}

void test_sender_reservation_identity_and_failed_commit_are_retained(void) {
  session_fixture_t f;
  fixture_init(&f, 0u);
  esy_proto_bin_sender_t sender;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_get_sender(&f.transport, &sender));
  uint8_t *buffer = NULL;
  size_t capacity = 0u;
  uint32_t id = 99u;
  TEST_ASSERT_EQUAL(ESY_OK, sender.acquire(sender.context, &buffer, &capacity));
  TEST_ASSERT_EQUAL_UINT(TEST_TX_PAYLOAD_MAX, capacity);
  uint8_t *again = buffer;
  size_t again_capacity = capacity;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, sender.acquire(sender.context, &again, &again_capacity));
  TEST_ASSERT_NULL(again);
  TEST_ASSERT_EQUAL_UINT(0u, again_capacity);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      sender.commit(sender.context, buffer + 1u, 1u, &id));
  TEST_ASSERT_EQUAL_UINT32(0u, id);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      sender.commit(sender.context, buffer, capacity + 1u, &id));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      sender.commit(sender.context, buffer, 1u, NULL));
#if SIZE_MAX > UINT32_MAX
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      sender.commit(sender.context, buffer, (size_t)UINT32_MAX + 1u, &id));
#endif
  TEST_ASSERT_FALSE(esy_proto_bin_session_is_idle(&f.transport));
  buffer[0] = 0x5Au;
  TEST_ASSERT_EQUAL(ESY_OK, sender.commit(sender.context, buffer, 1u, &id));
  TEST_ASSERT_NOT_EQUAL(ESY_PROTO_BIN_INVALID_TRANSACTION_ID, id);
  TEST_ASSERT_EQUAL_UINT(0u, f.io.sent_count);
  TEST_ASSERT_EQUAL_UINT(0u, f.io.event_count);
  TEST_ASSERT_EQUAL(ESY_OK, sender.cancel(sender.context));
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(1u, f.io.sent_count);
  TEST_ASSERT_EQUAL(ESY_OK, sender.acquire(sender.context, &buffer, &capacity));
  TEST_ASSERT_EQUAL(ESY_OK, sender.cancel(sender.context));
  TEST_ASSERT_EQUAL(ESY_OK, sender.acquire(sender.context, &buffer, &capacity));
  TEST_ASSERT_EQUAL(ESY_OK, sender.cancel(sender.context));
}

void test_sender_invalid_calls_do_not_lose_a_reservation_or_invoke_callbacks(void) {
  session_fixture_t f;
  fixture_init(&f, 0u);
  esy_proto_bin_sender_t sender;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_get_sender(&f.transport, &sender));
  uint8_t *buffer = (uint8_t *)1;
  size_t capacity = 99u;
  uint32_t id = 99u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, sender.acquire(NULL, &buffer, &capacity));
  TEST_ASSERT_NULL(buffer);
  TEST_ASSERT_EQUAL_UINT(0u, capacity);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, sender.acquire(sender.context, NULL, &capacity));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, sender.acquire(sender.context, &buffer, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, sender.commit(NULL, &f.tx_work[0], 1u, &id));
  TEST_ASSERT_EQUAL_UINT32(0u, id);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, sender.commit(sender.context, NULL, 1u, &id));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, sender.cancel(NULL));
  TEST_ASSERT_EQUAL(ESY_OK, sender.acquire(sender.context, &buffer, &capacity));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_deinit(&f.transport));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, sender.acquire(sender.context, &buffer, &capacity));
  TEST_ASSERT_NULL(buffer);
  TEST_ASSERT_EQUAL_UINT(0u, capacity);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, sender.cancel(sender.context));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, sender.commit(sender.context, f.tx_work, 1u, &id));
  TEST_ASSERT_EQUAL_UINT(0u, f.io.sent_count);
  TEST_ASSERT_EQUAL_UINT(0u, f.io.event_count);
}

void test_sender_full_queue_fails_without_creating_an_extra_reservation(void) {
  session_fixture_t f;
  fixture_init(&f, 0u);
  esy_proto_bin_sender_t sender;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_get_sender(&f.transport, &sender));
  uint8_t *buffer;
  size_t capacity;
  uint32_t id;
  for (uint32_t i = 0u; i < TEST_QUEUE_BLOCKS - 1u; ++i) {
    TEST_ASSERT_EQUAL(ESY_OK, sender.acquire(sender.context, &buffer, &capacity));
    buffer[0] = (uint8_t)i;
    TEST_ASSERT_EQUAL(ESY_OK, sender.commit(sender.context, buffer, 1u, &id));
  }
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, sender.acquire(sender.context, &buffer, &capacity));
  TEST_ASSERT_NULL(buffer);
  TEST_ASSERT_EQUAL_UINT(0u, capacity);
  TEST_ASSERT_EQUAL(ESY_OK, sender.cancel(sender.context));
  esy_proto_bin_session_poll(&f.transport);
  append_rx_frame(&f.io, ESY_PROTO_BIN_CMD_ACK, 1u, NULL, 0u);
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL(ESY_OK, sender.acquire(sender.context, &buffer, &capacity));
}

void test_deinit_keeps_an_accepted_async_borrow_even_after_early_protocol_ack(void) {
  session_fixture_t f;
  fixture_init(&f, 0u);
  f.io.busy_after_send = true;
  const uint8_t payload = 0xA5u;
  queue_payload(&f, &payload, 1u);
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_bin_session_deinit(&f.transport));
  TEST_ASSERT_TRUE(esy_proto_bin_session_is_initialized(&f.transport));
  append_rx_frame(&f.io, ESY_PROTO_BIN_CMD_ACK, 1u, NULL, 0u);
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(1u, f.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_ACK, f.io.events[0]);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_bin_session_deinit(&f.transport));
  f.io.tx_busy = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_deinit(&f.transport));
  TEST_ASSERT_FALSE(esy_proto_bin_session_is_initialized(&f.transport));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_init(&f.transport, &f.cfg));
}

void test_callbacks_cannot_destroy_or_recursively_drive_the_session(void) {
  session_fixture_t f;
  fixture_init(&f, 0u);
  f.io.try_lifecycle_on_data = true;
  const uint8_t payload = 0xA5u;
  append_rx_frame(&f.io, ESY_PROTO_BIN_CMD_DATA, 1u, &payload, 1u);
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, f.io.callback_deinit_result);
  TEST_ASSERT_TRUE(esy_proto_bin_session_is_initialized(&f.transport));
  TEST_ASSERT_EQUAL_UINT(1u, f.io.data_cb_count);
  TEST_ASSERT_EQUAL_UINT(1u, f.io.sent_count);
  TEST_ASSERT_TRUE(esy_proto_bin_session_is_idle(&f.transport));
}

void test_init_copies_resource_descriptor_and_rejects_short_or_overlapping_storage(void) {
  session_fixture_t f;
  fixture_init(&f, 0u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_deinit(&f.transport));
  esy_proto_bin_session_t before = f.transport;
  f.resources.tx_queue_buf_size--;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_bin_session_init(&f.transport, &f.cfg));
  TEST_ASSERT_EQUAL_MEMORY(&before, &f.transport, sizeof(before));
  f.resources.tx_queue_buf_size++;
  uint8_t *work = f.resources.tx_work_buf;
  f.resources.tx_work_buf = f.resources.rx_work_buf;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_bin_session_init(&f.transport, &f.cfg));
  f.resources.tx_work_buf = f.transport.priv.bytes;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_bin_session_init(&f.transport, &f.cfg));
  f.resources.tx_work_buf = work;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_init(&f.transport, &f.cfg));
  f.resources = (esy_proto_bin_session_static_resources_t){0};
  f.cfg = (esy_proto_bin_session_config_t){0};
  const uint8_t payload = 0xA5u;
  queue_payload(&f, &payload, 1u);
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(1u, f.io.sent_count);
}

void test_async_data_failure_precedes_readable_ack_and_preserves_the_next_transaction(void) {
  session_fixture_t f;
  fixture_init(&f, 0u);
  f.io.busy_after_send = true;
  const uint8_t first = 0x41u;
  const uint8_t second = 0x42u;
  uint32_t first_id = queue_tracked_payload(&f, &first, 1u);
  uint32_t second_id = queue_tracked_payload(&f, &second, 1u);
  esy_proto_bin_session_poll(&f.transport);
  uint8_t borrowed_frame[TEST_WORK_BUF_SIZE];
  memcpy(borrowed_frame, f.tx_work, sizeof(borrowed_frame));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY,
      esy_proto_bin_session_on_tx_complete(&f.transport, ESY_ERR_IO));
  TEST_ASSERT_EQUAL_UINT(0u, f.io.event_count);
  TEST_ASSERT_EQUAL_MEMORY(borrowed_frame, f.tx_work, sizeof(borrowed_frame));

  append_rx_frame(&f.io, ESY_PROTO_BIN_CMD_ACK, 1u, NULL, 0u);
  f.io.tx_busy = false;
  f.io.try_tx_complete_on_event = true;
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_proto_bin_session_on_tx_complete(&f.transport, ESY_ERR_IO));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, f.io.event_tx_complete_result);
  TEST_ASSERT_EQUAL_UINT(1u, f.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_IO_ERROR, f.io.events[0]);
  TEST_ASSERT_EQUAL_UINT32(first_id, f.io.event_transaction_ids[0]);
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_proto_bin_session_on_tx_complete(&f.transport, ESY_ERR_IO));
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(2u, f.io.sent_count);
  TEST_ASSERT_EQUAL_UINT(1u, f.io.event_count);
  esy_proto_bin_frame_t sent = sent_frame_view(&f.io, 1u);
  TEST_ASSERT_EQUAL_UINT8(2u, esy_proto_bin_frame_get_seq(&sent));
  TEST_ASSERT_EQUAL_MEMORY(&second, esy_proto_bin_frame_get_data(&sent), 1u);
  f.io.tx_busy = false;
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_proto_bin_session_on_tx_complete(&f.transport, ESY_OK));
  append_rx_frame(&f.io, ESY_PROTO_BIN_CMD_ACK, 2u, NULL, 0u);
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(2u, f.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_ACK, f.io.events[1]);
  TEST_ASSERT_EQUAL_UINT32(second_id, f.io.event_transaction_ids[1]);
}

void test_async_data_completion_after_early_ack_never_fails_the_next_queued_transaction(void) {
  session_fixture_t f;
  fixture_init(&f, 0u);
  f.io.busy_after_send = true;
  const uint8_t payload = 0x43u;
  uint32_t first_id = queue_tracked_payload(&f, &payload, 1u);
  uint32_t second_id = queue_tracked_payload(&f, &payload, 1u);
  esy_proto_bin_session_poll(&f.transport);
  append_rx_frame(&f.io, ESY_PROTO_BIN_CMD_ACK, 1u, NULL, 0u);
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(1u, f.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_ACK, f.io.events[0]);
  TEST_ASSERT_EQUAL_UINT32(first_id, f.io.event_transaction_ids[0]);
  TEST_ASSERT_EQUAL_UINT(1u, f.io.sent_count);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_bin_session_deinit(&f.transport));
  f.io.tx_busy = false;
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_proto_bin_session_on_tx_complete(&f.transport, ESY_ERR_IO));
  TEST_ASSERT_EQUAL_UINT(1u, f.io.event_count);
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(2u, f.io.sent_count);
  f.io.tx_busy = false;
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_proto_bin_session_on_tx_complete(&f.transport, ESY_OK));
  append_rx_frame(&f.io, ESY_PROTO_BIN_CMD_ACK, 2u, NULL, 0u);
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(2u, f.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_ACK, f.io.events[1]);
  TEST_ASSERT_EQUAL_UINT32(second_id, f.io.event_transaction_ids[1]);
}

void test_async_data_failure_after_physical_timeout_does_not_report_a_second_failure(void) {
  session_fixture_t f;
  fixture_init(&f, 1u);
  f.io.busy_after_send = true;
  const uint8_t payload = 0x44u;
  uint32_t id = queue_tracked_payload(&f, &payload, 1u);
  esy_proto_bin_session_poll(&f.transport);
  fake_esy_hal_system_set_tick(10u);
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(1u, f.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TX_TIMEOUT, f.io.events[0]);
  TEST_ASSERT_EQUAL_UINT32(id, f.io.event_transaction_ids[0]);
  f.io.tx_busy = false;
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_proto_bin_session_on_tx_complete(&f.transport, ESY_ERR_IO));
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(1u, f.io.sent_count);
  TEST_ASSERT_EQUAL_UINT(1u, f.io.event_count);
  TEST_ASSERT_TRUE(esy_proto_bin_session_is_idle(&f.transport));
}

void test_async_control_failure_retries_its_sequence_without_overwriting_another_pending_ack(void) {
  session_fixture_t f;
  fixture_init(&f, 0u);
  f.io.busy_after_send = true;
  const uint8_t payload = 0x45u;
  append_rx_frame(&f.io, ESY_PROTO_BIN_CMD_DATA, 11u, &payload, 1u);
  append_rx_frame(&f.io, ESY_PROTO_BIN_CMD_DATA, 12u, &payload, 1u);
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(2u, f.io.data_cb_count);
  TEST_ASSERT_EQUAL_UINT(1u, f.io.sent_count);
  f.io.tx_busy = false;
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_proto_bin_session_on_tx_complete(&f.transport, ESY_ERR_IO));
  TEST_ASSERT_EQUAL_UINT(1u, f.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_IO_ERROR, f.io.events[0]);
  TEST_ASSERT_EQUAL_UINT32(ESY_PROTO_BIN_INVALID_TRANSACTION_ID,
      f.io.event_transaction_ids[0]);
  TEST_ASSERT_FALSE(esy_proto_bin_session_is_idle(&f.transport));
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(2u, f.io.sent_count);
  esy_proto_bin_frame_t retried = sent_frame_view(&f.io, 1u);
  TEST_ASSERT_EQUAL_UINT8(ESY_PROTO_BIN_CMD_ACK, esy_proto_bin_frame_get_cmd(&retried));
  TEST_ASSERT_EQUAL_UINT8(11u, esy_proto_bin_frame_get_seq(&retried));
  f.io.tx_busy = false;
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_proto_bin_session_on_tx_complete(&f.transport, ESY_OK));
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(3u, f.io.sent_count);
  esy_proto_bin_frame_t pending = sent_frame_view(&f.io, 2u);
  TEST_ASSERT_EQUAL_UINT8(12u, esy_proto_bin_frame_get_seq(&pending));
  f.io.tx_busy = false;
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_proto_bin_session_on_tx_complete(&f.transport, ESY_OK));
  append_rx_frame(&f.io, ESY_PROTO_BIN_CMD_DATA, 12u, &payload, 1u);
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(2u, f.io.data_cb_count);
  TEST_ASSERT_EQUAL_UINT(4u, f.io.sent_count);
  TEST_ASSERT_EQUAL_UINT(1u, f.io.event_count);
}

void test_async_control_failure_does_not_fail_data_already_waiting_for_a_peer_ack(void) {
  session_fixture_t f;
  fixture_init(&f, 0u);
  f.io.busy_after_send = true;
  const uint8_t payload = 0x46u;
  uint32_t id = queue_tracked_payload(&f, &payload, 1u);
  esy_proto_bin_session_poll(&f.transport);
  f.io.tx_busy = false;
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_proto_bin_session_on_tx_complete(&f.transport, ESY_OK));
  append_rx_frame(&f.io, ESY_PROTO_BIN_CMD_DATA, 21u, &payload, 1u);
  esy_proto_bin_session_poll(&f.transport);
  f.io.tx_busy = false;
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_proto_bin_session_on_tx_complete(&f.transport, ESY_ERR_IO));
  TEST_ASSERT_EQUAL_UINT(1u, f.io.event_count);
  TEST_ASSERT_EQUAL_UINT32(ESY_PROTO_BIN_INVALID_TRANSACTION_ID,
      f.io.event_transaction_ids[0]);
  append_rx_frame(&f.io, ESY_PROTO_BIN_CMD_ACK, 1u, NULL, 0u);
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(3u, f.io.sent_count);
  TEST_ASSERT_EQUAL_UINT(2u, f.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_ACK, f.io.events[1]);
  TEST_ASSERT_EQUAL_UINT32(id, f.io.event_transaction_ids[1]);
}

void test_async_success_starts_ack_deadline_at_notification_across_clock_wrap(void) {
  session_fixture_t f;
  fixture_init(&f, 0u);
  f.io.busy_after_send = true;
  const uint8_t payload = 0x47u;
  fake_esy_hal_system_set_tick(UINT32_MAX - 5u);
  uint32_t id = queue_tracked_payload(&f, &payload, 1u);
  esy_proto_bin_session_poll(&f.transport);
  fake_esy_hal_system_set_tick(UINT32_MAX - 1u);
  f.io.tx_busy = false;
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_proto_bin_session_on_tx_complete(&f.transport, ESY_OK));
  fake_esy_hal_system_set_tick(7u);
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(0u, f.io.event_count);
  fake_esy_hal_system_set_tick(8u);
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL_UINT(1u, f.io.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TIMEOUT, f.io.events[0]);
  TEST_ASSERT_EQUAL_UINT32(id, f.io.event_transaction_ids[0]);
}

void test_notified_completions_do_not_reactivate_after_an_entire_clock_cycle(void) {
  const uint8_t payload = 0x48u;
  for (unsigned failure = 0u; failure < 2u; ++failure) {
    session_fixture_t f;
    fixture_init(&f, 0u);
    f.io.busy_after_send = true;
    fake_esy_hal_system_set_tick(0u);
    queue_payload(&f, &payload, 1u);
    esy_proto_bin_session_poll(&f.transport);
    f.io.tx_busy = false;
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_on_tx_complete(&f.transport,
        failure ? ESY_ERR_IO : ESY_OK));
    if (!failure) {
      append_rx_frame(&f.io, ESY_PROTO_BIN_CMD_ACK, 1u, NULL, 0u);
      esy_proto_bin_session_poll(&f.transport);
    }
    for (unsigned quarter = 0u; quarter < 4u; ++quarter) {
      fake_esy_hal_system_advance_tick(UINT32_C(0x40000000));
      TEST_ASSERT_EQUAL(ESY_OK,
          esy_proto_bin_session_on_tx_complete(&f.transport, ESY_ERR_IO));
      esy_proto_bin_session_poll(&f.transport);
      TEST_ASSERT_EQUAL_UINT(1u, f.io.event_count);
      TEST_ASSERT_EQUAL_UINT(1u, f.io.sent_count);
      TEST_ASSERT_TRUE(esy_proto_bin_session_is_idle(&f.transport));
    }
  }
}

void test_tx_completion_rejects_invalid_lifetime_isr_and_callback_reentry(void) {
  esy_proto_bin_session_t inactive = {0};
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_proto_bin_session_on_tx_complete(NULL, ESY_OK));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_proto_bin_session_on_tx_complete(&inactive, ESY_OK));
  session_fixture_t f;
  fixture_init(&f, 0u);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_proto_bin_session_on_tx_complete(&f.transport, (esy_err_t)1));
  fake_esy_hal_system_get_ctx()->is_in_isr = true;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_proto_bin_session_on_tx_complete(&f.transport, ESY_OK));
  fake_esy_hal_system_get_ctx()->is_in_isr = false;
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_proto_bin_session_on_tx_complete(&f.transport, ESY_ERR_IO));
  f.io.busy_after_send = true;
  f.io.try_tx_complete_on_data = true;
  const uint8_t payload = 0x49u;
  append_rx_frame(&f.io, ESY_PROTO_BIN_CMD_DATA, 31u, &payload, 1u);
  esy_proto_bin_session_poll(&f.transport);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, f.io.data_tx_complete_result);
  TEST_ASSERT_EQUAL_UINT(0u, f.io.event_count);
  f.io.tx_busy = false;
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_proto_bin_session_on_tx_complete(&f.transport, ESY_OK));
}
