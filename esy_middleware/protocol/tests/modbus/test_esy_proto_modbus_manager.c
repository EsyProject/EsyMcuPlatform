/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"

#include <string.h>

#include "esy_middleware/protocol/esy_proto_modbus_manager.h"
#include "fake_esy_hal_system.h"

#define TEST_MANAGER_FRAME_MAX 80u
#define TEST_MANAGER_SENT_MAX  8u
#define TEST_MANAGER_SLOTS     4u

typedef struct {
  uint8_t sent[TEST_MANAGER_SENT_MAX][TEST_MANAGER_FRAME_MAX];
  uint16_t sent_len[TEST_MANAGER_SENT_MAX];
  uint32_t sent_count;
  esy_err_t tx_result;
  bool busy;
  esy_proto_modbus_req_status_t statuses[TEST_MANAGER_SENT_MAX];
  uint32_t complete_count;
  uint32_t slave_request_count;
  uint8_t slave_unit_id;
  uint8_t slave_func;
} fake_manager_io_t;

typedef struct {
  esy_proto_modbus_master_t master;
  esy_proto_modbus_master_config_t cfg;
  esy_prio_slot_t slots[TEST_MANAGER_SLOTS];
  esy_proto_modbus_payload_t payloads[TEST_MANAGER_SLOTS];
  uint8_t tx_frame_buf[TEST_MANAGER_FRAME_MAX];
  fake_manager_io_t io;
} master_fixture_t;

typedef struct {
  esy_proto_modbus_slave_t slave;
  esy_proto_modbus_slave_config_t cfg;
  uint8_t resp_pdu_buf[16];
  uint8_t tx_frame_buf[TEST_MANAGER_FRAME_MAX];
  fake_manager_io_t io;
} slave_fixture_t;

void setUp(void) {
  fake_esy_hal_system_reset();
  fake_esy_hal_system_set_tick(0u);
}

void tearDown(void) {}

static esy_err_t fake_tx(const uint8_t *frame, uint16_t frame_len,
                         void *user_ctx) {
  fake_manager_io_t *fake = (fake_manager_io_t *)user_ctx;

  TEST_ASSERT_LESS_OR_EQUAL_UINT32(TEST_MANAGER_SENT_MAX,
                                   fake->sent_count + 1u);
  memcpy(fake->sent[fake->sent_count], frame, frame_len);
  fake->sent_len[fake->sent_count] = frame_len;
  fake->sent_count++;
  return fake->tx_result;
}

static void fake_complete(esy_proto_modbus_master_t *master,
                          esy_proto_modbus_req_status_t status, uint8_t unit_id,
                          esy_proto_modbus_func_t func_code,
                          const uint8_t *resp_pdu, uint16_t resp_pdu_len,
                          void *user_ctx) {
  fake_manager_io_t *fake = (fake_manager_io_t *)user_ctx;

  (void)master;
  (void)unit_id;
  (void)func_code;
  (void)resp_pdu;
  (void)resp_pdu_len;
  fake->statuses[fake->complete_count++] = status;
}

static esy_err_t fake_slave_request(esy_proto_modbus_slave_t *slave,
                                    uint8_t unit_id,
                                    esy_proto_modbus_func_t func_code,
                                    const uint8_t *req_pdu,
                                    uint16_t req_pdu_len, uint8_t *resp_pdu_buf,
                                    uint16_t *resp_pdu_len,
                                    uint16_t resp_pdu_buf_max, void *user_ctx) {
  fake_manager_io_t *fake = (fake_manager_io_t *)user_ctx;

  (void)slave;
  (void)req_pdu;
  (void)req_pdu_len;
  TEST_ASSERT_GREATER_OR_EQUAL_UINT16(3u, resp_pdu_buf_max);
  fake->slave_request_count++;
  fake->slave_unit_id = unit_id;
  fake->slave_func = (uint8_t)func_code;
  resp_pdu_buf[0] = (uint8_t)func_code;
  resp_pdu_buf[1] = 0xAAu;
  resp_pdu_buf[2] = 0x55u;
  *resp_pdu_len = 3u;
  return ESY_OK;
}

static void build_rtu_frame(uint8_t *buf, uint16_t *out_len, uint8_t unit_id,
                            const uint8_t *pdu, uint16_t pdu_len) {
  esy_proto_modbus_frame_t frame;

  TEST_ASSERT_EQUAL(
      ESY_OK, esy_proto_modbus_frame_init(&frame, buf, TEST_MANAGER_FRAME_MAX));
  TEST_ASSERT_EQUAL(
      ESY_OK, esy_proto_modbus_build_rtu_frame(&frame, unit_id, pdu, pdu_len));
  *out_len = (uint16_t)frame.len;
}

static void master_fixture_init(master_fixture_t *fixture) {
  memset(fixture, 0, sizeof(*fixture));
  fixture->io.tx_result = ESY_OK;
  fixture->cfg = esy_proto_modbus_master_default_cfg();
  fixture->cfg.tx_fn = fake_tx;
  fixture->cfg.tx_user_ctx = &fixture->io;
  fixture->cfg.timeout_ms = 10u;
  fixture->cfg.tx_frame_buf = fixture->tx_frame_buf;
  fixture->cfg.tx_frame_buf_size = sizeof(fixture->tx_frame_buf);
  fixture->cfg.slots = fixture->slots;
  fixture->cfg.payloads = fixture->payloads;
  fixture->cfg.slot_count = TEST_MANAGER_SLOTS;
  TEST_ASSERT_EQUAL(
      ESY_OK, esy_proto_modbus_master_init(&fixture->master, &fixture->cfg));
}

static void submit_request(master_fixture_t *fixture, uint8_t unit_id,
                           esy_proto_modbus_func_t func,
                           esy_proto_modbus_priority_t priority, uint8_t marker,
                           uint8_t retry_max) {
  esy_proto_modbus_request_t req;

  memset(&req, 0, sizeof(req));
  req.unit_id = unit_id;
  req.func_code = func;
  req.priority = priority;
  req.pdu[0] = (uint8_t)func;
  req.pdu[1] = marker;
  req.pdu_len = 2u;
  req.retry_max = retry_max;
  req.on_complete = fake_complete;
  req.user_ctx = &fixture->io;
  TEST_ASSERT_EQUAL(ESY_OK,
                    esy_proto_modbus_master_submit(&fixture->master, &req));
}

void test_modbus_master_rejects_uninitialized_submission_without_callback(void) {
  master_fixture_t fixture = {0};
  const esy_proto_modbus_request_t req = {
      .unit_id = 0x11u,
      .func_code = ESY_PROTO_MODBUS_FUNC_READ_COILS,
      .pdu_len = 1u,
      .pdu = {ESY_PROTO_MODBUS_FUNC_READ_COILS},
      .on_complete = fake_complete,
      .user_ctx = &fixture.io,
  };

  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_proto_modbus_master_submit(&fixture.master, &req));
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.complete_count);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.sent_count);
}

void test_modbus_master_rejects_full_queue_without_callback(void) {
  master_fixture_t fixture;
  const esy_proto_modbus_request_t req = {
      .unit_id = 0x11u,
      .func_code = ESY_PROTO_MODBUS_FUNC_READ_COILS,
      .pdu_len = 1u,
      .pdu = {ESY_PROTO_MODBUS_FUNC_READ_COILS},
      .on_complete = fake_complete,
      .user_ctx = &fixture.io,
  };

  master_fixture_init(&fixture);
  for (uint8_t i = 0u; i < TEST_MANAGER_SLOTS; ++i) {
    submit_request(&fixture, 0x11u, ESY_PROTO_MODBUS_FUNC_READ_COILS,
                   ESY_PROTO_MODBUS_PRIORITY_LOW, i, 0u);
  }

  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM,
                    esy_proto_modbus_master_submit(&fixture.master, &req));
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.complete_count);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.sent_count);
  TEST_ASSERT_EQUAL_UINT16(
      TEST_MANAGER_SLOTS,
      esy_proto_modbus_master_get_queue_depth(&fixture.master));
}

void test_modbus_master_schedules_high_priority_request_first(void) {
  master_fixture_t fixture;
  esy_proto_modbus_frame_view_t view;

  master_fixture_init(&fixture);
  submit_request(&fixture, 0x11u, ESY_PROTO_MODBUS_FUNC_READ_COILS,
                 ESY_PROTO_MODBUS_PRIORITY_LOW, 0x10u, 0u);
  submit_request(&fixture, 0x22u, ESY_PROTO_MODBUS_FUNC_READ_HOLDING_REGISTERS,
                 ESY_PROTO_MODBUS_PRIORITY_HIGH, 0x20u, 0u);
  esy_proto_modbus_master_tick(&fixture.master);

  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_parse_frame(
                                ESY_PROTO_MODBUS_FRAME_RTU, fixture.io.sent[0],
                                fixture.io.sent_len[0], &view));
  TEST_ASSERT_EQUAL_UINT8(0x22u, view.unit_id);
}

void test_modbus_master_completes_response_and_starts_next(void) {
  master_fixture_t fixture;
  uint8_t response[TEST_MANAGER_FRAME_MAX];
  uint16_t response_len = 0u;
  const uint8_t response_pdu[] = {ESY_PROTO_MODBUS_FUNC_READ_COILS, 0x01u};

  master_fixture_init(&fixture);
  submit_request(&fixture, 0x11u, ESY_PROTO_MODBUS_FUNC_READ_COILS,
                 ESY_PROTO_MODBUS_PRIORITY_LOW, 0x10u, 0u);
  submit_request(&fixture, 0x22u, ESY_PROTO_MODBUS_FUNC_READ_HOLDING_REGISTERS,
                 ESY_PROTO_MODBUS_PRIORITY_LOW, 0x20u, 0u);
  esy_proto_modbus_master_tick(&fixture.master);
  build_rtu_frame(response, &response_len, 0x11u, response_pdu,
                  sizeof(response_pdu));
  esy_proto_modbus_master_on_rx_frame(&fixture.master, response, response_len);

  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.complete_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, fixture.io.statuses[0]);
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.sent_count);
}

void test_modbus_master_reports_exception_response_status(void) {
  master_fixture_t fixture;
  uint8_t response[TEST_MANAGER_FRAME_MAX];
  uint16_t response_len = 0u;
  const uint8_t exception_pdu[] = {0x81u, 0x02u};

  master_fixture_init(&fixture);
  submit_request(&fixture, 0x11u, ESY_PROTO_MODBUS_FUNC_READ_COILS,
                 ESY_PROTO_MODBUS_PRIORITY_LOW, 0x10u, 0u);
  esy_proto_modbus_master_tick(&fixture.master);
  build_rtu_frame(response, &response_len, 0x11u, exception_pdu,
                  sizeof(exception_pdu));
  esy_proto_modbus_master_on_rx_frame(&fixture.master, response, response_len);

  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.complete_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_EXCEPTION,
                    fixture.io.statuses[0]);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_MASTER_STATE_IDLE,
                    esy_proto_modbus_master_get_state(&fixture.master));
}

void test_modbus_master_ignores_response_for_other_unit(void) {
  master_fixture_t fixture;
  uint8_t response[TEST_MANAGER_FRAME_MAX];
  uint16_t response_len = 0u;
  const uint8_t response_pdu[] = {ESY_PROTO_MODBUS_FUNC_READ_COILS, 0x01u};

  master_fixture_init(&fixture);
  submit_request(&fixture, 0x11u, ESY_PROTO_MODBUS_FUNC_READ_COILS,
                 ESY_PROTO_MODBUS_PRIORITY_LOW, 0x10u, 0u);
  esy_proto_modbus_master_tick(&fixture.master);
  build_rtu_frame(response, &response_len, 0x22u, response_pdu,
                  sizeof(response_pdu));
  esy_proto_modbus_master_on_rx_frame(&fixture.master, response, response_len);

  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.complete_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_MASTER_STATE_WAIT_RESP,
                    esy_proto_modbus_master_get_state(&fixture.master));
}

void test_modbus_master_retries_then_times_out(void) {
  master_fixture_t fixture;

  master_fixture_init(&fixture);
  submit_request(&fixture, 0x11u, ESY_PROTO_MODBUS_FUNC_READ_COILS,
                 ESY_PROTO_MODBUS_PRIORITY_LOW, 0x10u, 1u);
  esy_proto_modbus_master_tick(&fixture.master);
  fake_esy_hal_system_set_tick(10u);
  esy_proto_modbus_master_tick(&fixture.master);
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.sent_count);
  fake_esy_hal_system_set_tick(20u);
  esy_proto_modbus_master_tick(&fixture.master);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TIMEOUT,
                    fixture.io.statuses[0]);
}

void test_modbus_master_retry_and_timeout_boundaries_cross_tick_wrap(void) {
  master_fixture_t fixture;

  master_fixture_init(&fixture);
  fake_esy_hal_system_set_tick(UINT32_MAX - 4u);
  submit_request(&fixture, 0x11u, ESY_PROTO_MODBUS_FUNC_READ_COILS,
                 ESY_PROTO_MODBUS_PRIORITY_LOW, 0x10u, 1u);
  esy_proto_modbus_master_tick(&fixture.master);
  fake_esy_hal_system_advance_tick(9u);
  esy_proto_modbus_master_tick(&fixture.master);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.sent_count);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.complete_count);

  fake_esy_hal_system_advance_tick(1u);
  esy_proto_modbus_master_tick(&fixture.master);
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.sent_count);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.complete_count);
  fake_esy_hal_system_advance_tick(9u);
  esy_proto_modbus_master_tick(&fixture.master);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.complete_count);
  fake_esy_hal_system_advance_tick(1u);
  esy_proto_modbus_master_tick(&fixture.master);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.complete_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TIMEOUT,
                    fixture.io.statuses[0]);
  fake_esy_hal_system_advance_tick(1u);
  esy_proto_modbus_master_tick(&fixture.master);
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.sent_count);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.complete_count);
  fake_esy_hal_system_advance_tick(UINT32_MAX);
  esy_proto_modbus_master_tick(&fixture.master);
  fake_esy_hal_system_advance_tick(1u);
  esy_proto_modbus_master_tick(&fixture.master);
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.sent_count);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.complete_count);
}

void test_modbus_master_response_completion_stays_disarmed_for_full_tick_cycle(
    void) {
  master_fixture_t fixture;
  uint8_t response[TEST_MANAGER_FRAME_MAX];
  uint16_t response_len;
  const uint8_t response_pdu[] = {ESY_PROTO_MODBUS_FUNC_READ_COILS, 0x01u};

  master_fixture_init(&fixture);
  fake_esy_hal_system_set_tick(100u);
  submit_request(&fixture, 0x11u, ESY_PROTO_MODBUS_FUNC_READ_COILS,
                 ESY_PROTO_MODBUS_PRIORITY_LOW, 0x10u, 1u);
  esy_proto_modbus_master_tick(&fixture.master);
  build_rtu_frame(response, &response_len, 0x11u, response_pdu,
                  sizeof(response_pdu));
  esy_proto_modbus_master_on_rx_frame(&fixture.master, response, response_len);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, fixture.io.statuses[0]);

  fake_esy_hal_system_advance_tick(UINT32_MAX);
  esy_proto_modbus_master_tick(&fixture.master);
  fake_esy_hal_system_advance_tick(1u);
  esy_proto_modbus_master_tick(&fixture.master);
  fake_esy_hal_system_advance_tick(10u);
  esy_proto_modbus_master_tick(&fixture.master);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.sent_count);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.complete_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_MASTER_STATE_IDLE,
                    esy_proto_modbus_master_get_state(&fixture.master));
}

void test_modbus_master_abort_stays_disarmed_until_new_submission(void) {
  master_fixture_t fixture;

  master_fixture_init(&fixture);
  fake_esy_hal_system_set_tick(100u);
  submit_request(&fixture, 0x11u, ESY_PROTO_MODBUS_FUNC_READ_COILS,
                 ESY_PROTO_MODBUS_PRIORITY_LOW, 0x10u, 1u);
  esy_proto_modbus_master_tick(&fixture.master);
  TEST_ASSERT_EQUAL(ESY_OK,
                    esy_proto_modbus_master_abort_all(&fixture.master, true));
  fake_esy_hal_system_advance_tick(UINT32_MAX);
  esy_proto_modbus_master_tick(&fixture.master);
  fake_esy_hal_system_advance_tick(1u);
  esy_proto_modbus_master_tick(&fixture.master);
  fake_esy_hal_system_advance_tick(10u);
  esy_proto_modbus_master_tick(&fixture.master);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.sent_count);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.complete_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_ABORTED, fixture.io.statuses[0]);

  submit_request(&fixture, 0x22u, ESY_PROTO_MODBUS_FUNC_READ_COILS,
                 ESY_PROTO_MODBUS_PRIORITY_LOW, 0x20u, 0u);
  esy_proto_modbus_master_tick(&fixture.master);
  fake_esy_hal_system_advance_tick(9u);
  esy_proto_modbus_master_tick(&fixture.master);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.complete_count);
  fake_esy_hal_system_advance_tick(1u);
  esy_proto_modbus_master_tick(&fixture.master);
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.sent_count);
  TEST_ASSERT_EQUAL_UINT32(2u, fixture.io.complete_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TIMEOUT,
                    fixture.io.statuses[1]);
}

void test_modbus_master_supports_maximum_response_wait_across_tick_wrap(void) {
  master_fixture_t fixture;

  master_fixture_init(&fixture);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_master_deinit(&fixture.master));
  fixture.cfg.timeout_ms = (uint32_t)INT32_MAX + 1u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_proto_modbus_master_init(&fixture.master, &fixture.cfg));
  fixture.cfg.timeout_ms = INT32_MAX;
  TEST_ASSERT_EQUAL(ESY_OK,
                    esy_proto_modbus_master_init(&fixture.master, &fixture.cfg));
  fake_esy_hal_system_set_tick(UINT32_MAX - 4u);
  submit_request(&fixture, 0x11u, ESY_PROTO_MODBUS_FUNC_READ_COILS,
                 ESY_PROTO_MODBUS_PRIORITY_LOW, 0x10u, 0u);
  esy_proto_modbus_master_tick(&fixture.master);
  fake_esy_hal_system_advance_tick((uint32_t)INT32_MAX - 1u);
  esy_proto_modbus_master_tick(&fixture.master);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.complete_count);
  fake_esy_hal_system_advance_tick(1u);
  esy_proto_modbus_master_tick(&fixture.master);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.complete_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TIMEOUT,
                    fixture.io.statuses[0]);
}

void test_modbus_master_abort_current_reports_aborted(void) {
  master_fixture_t fixture;

  master_fixture_init(&fixture);
  submit_request(&fixture, 0x11u, ESY_PROTO_MODBUS_FUNC_READ_COILS,
                 ESY_PROTO_MODBUS_PRIORITY_LOW, 0x10u, 0u);
  esy_proto_modbus_master_tick(&fixture.master);
  TEST_ASSERT_EQUAL(ESY_OK,
                    esy_proto_modbus_master_abort_all(&fixture.master, true));
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_ABORTED,
                    fixture.io.statuses[0]);
}

void test_modbus_slave_dispatches_matching_request_and_sends_response(void) {
  slave_fixture_t fixture;
  uint8_t request[TEST_MANAGER_FRAME_MAX];
  uint16_t request_len = 0u;
  const uint8_t request_pdu[] = {ESY_PROTO_MODBUS_FUNC_READ_COILS, 0x00u};

  memset(&fixture, 0, sizeof(fixture));
  fixture.io.tx_result = ESY_OK;
  fixture.cfg.unit_id = 0x11u;
  fixture.cfg.tx_fn = fake_tx;
  fixture.cfg.tx_user_ctx = &fixture.io;
  fixture.cfg.resp_pdu_buf = fixture.resp_pdu_buf;
  fixture.cfg.resp_pdu_buf_size = sizeof(fixture.resp_pdu_buf);
  fixture.cfg.on_request = fake_slave_request;
  fixture.cfg.user_ctx = &fixture.io;
  TEST_ASSERT_EQUAL(ESY_OK,
                    esy_proto_modbus_slave_init(&fixture.slave, &fixture.cfg,
                                                fixture.tx_frame_buf,
                                                sizeof(fixture.tx_frame_buf)));

  build_rtu_frame(request, &request_len, 0x11u, request_pdu,
                  sizeof(request_pdu));
  esy_proto_modbus_slave_on_rx_frame(&fixture.slave, request, request_len);

  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.slave_request_count);
  TEST_ASSERT_EQUAL_UINT8(0x11u, fixture.io.slave_unit_id);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.sent_count);
}

void test_modbus_slave_ignores_other_unit_and_suppresses_broadcast_reply(void) {
  slave_fixture_t fixture;
  uint8_t request[TEST_MANAGER_FRAME_MAX];
  uint16_t request_len = 0u;
  const uint8_t request_pdu[] = {ESY_PROTO_MODBUS_FUNC_READ_COILS, 0x00u};

  memset(&fixture, 0, sizeof(fixture));
  fixture.io.tx_result = ESY_OK;
  fixture.cfg.unit_id = 0x11u;
  fixture.cfg.tx_fn = fake_tx;
  fixture.cfg.tx_user_ctx = &fixture.io;
  fixture.cfg.resp_pdu_buf = fixture.resp_pdu_buf;
  fixture.cfg.resp_pdu_buf_size = sizeof(fixture.resp_pdu_buf);
  fixture.cfg.on_request = fake_slave_request;
  fixture.cfg.user_ctx = &fixture.io;
  TEST_ASSERT_EQUAL(ESY_OK,
                    esy_proto_modbus_slave_init(&fixture.slave, &fixture.cfg,
                                                fixture.tx_frame_buf,
                                                sizeof(fixture.tx_frame_buf)));

  build_rtu_frame(request, &request_len, 0x22u, request_pdu,
                  sizeof(request_pdu));
  esy_proto_modbus_slave_on_rx_frame(&fixture.slave, request, request_len);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.slave_request_count);

  build_rtu_frame(request, &request_len, 0x00u, request_pdu,
                  sizeof(request_pdu));
  esy_proto_modbus_slave_on_rx_frame(&fixture.slave, request, request_len);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.io.slave_request_count);
  TEST_ASSERT_EQUAL_UINT32(0u, fixture.io.sent_count);
}

static bool fake_busy(void* arg) { return ((fake_manager_io_t*)arg)->busy; }
void test_manager_send_failure_completes_and_releases_slot(void) {
  master_fixture_t f;
  master_fixture_init(&f);
  f.io.tx_result = ESY_ERR_IO;
  submit_request(&f, 1, ESY_PROTO_MODBUS_FUNC_READ_COILS,
                 ESY_PROTO_MODBUS_PRIORITY_LOW, 0, 0);
  esy_proto_modbus_master_tick(&f.master);
  TEST_ASSERT_EQUAL_UINT(1, f.io.complete_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR,
                    f.io.statuses[0]);
  TEST_ASSERT_EQUAL_UINT(0, esy_proto_modbus_master_get_queue_depth(&f.master));
  f.io.tx_result = ESY_OK;
  submit_request(&f, 1, ESY_PROTO_MODBUS_FUNC_READ_COILS,
                 ESY_PROTO_MODBUS_PRIORITY_LOW, 0, 1);
  esy_proto_modbus_master_tick(&f.master);
  f.io.tx_result = ESY_ERR_IO;
  fake_esy_hal_system_set_tick(10);
  esy_proto_modbus_master_tick(&f.master);
  TEST_ASSERT_EQUAL_UINT(2, f.io.complete_count);
}
void test_manager_async_borrow_survives_abort_and_blocks_deinit_and_next_send(
    void) {
  master_fixture_t f;
  master_fixture_init(&f);
  esy_proto_modbus_master_deinit(&f.master);
  f.cfg.is_tx_busy = fake_busy;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_master_init(&f.master, &f.cfg));
  submit_request(&f, 1, ESY_PROTO_MODBUS_FUNC_READ_COILS,
                 ESY_PROTO_MODBUS_PRIORITY_LOW, 1, 1);
  esy_proto_modbus_master_tick(&f.master);
  f.io.busy = true;
  uint8_t saved[TEST_MANAGER_FRAME_MAX];
  memcpy(saved, f.tx_frame_buf, sizeof(saved));
  fake_esy_hal_system_set_tick(10);
  esy_proto_modbus_master_tick(&f.master);
  TEST_ASSERT_EQUAL_UINT(1, f.io.sent_count);
  esy_proto_modbus_master_abort_all(&f.master, true);
  submit_request(&f, 1, ESY_PROTO_MODBUS_FUNC_READ_COILS,
                 ESY_PROTO_MODBUS_PRIORITY_LOW, 2, 0);
  esy_proto_modbus_master_tick(&f.master);
  TEST_ASSERT_EQUAL_MEMORY(saved, f.tx_frame_buf, sizeof(saved));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_master_deinit(&f.master));
  f.io.busy = false;
  esy_proto_modbus_master_tick(&f.master);
  TEST_ASSERT_EQUAL_UINT(2, f.io.sent_count);
}
void test_manager_tcp_wrong_transaction_id_ignored_and_broadcast_not_retried(
    void) {
  master_fixture_t f;
  master_fixture_init(&f);
  esy_proto_modbus_master_deinit(&f.master);
  f.cfg.frame_type = ESY_PROTO_MODBUS_FRAME_TCP;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_master_init(&f.master, &f.cfg));
  submit_request(&f, 1, ESY_PROTO_MODBUS_FUNC_READ_COILS,
                 ESY_PROTO_MODBUS_PRIORITY_LOW, 1, 0);
  esy_proto_modbus_master_tick(&f.master);
  uint8_t bytes[32], pdu[] = {1, 1};
  esy_proto_modbus_frame_t response;
  esy_proto_modbus_frame_init(&response, bytes, sizeof(bytes));
  esy_proto_modbus_build_tcp_frame(&response, 999, 1, pdu, sizeof(pdu));
  esy_proto_modbus_master_on_rx_frame(&f.master, bytes, response.len);
  TEST_ASSERT_EQUAL_UINT(0, f.io.complete_count);
  esy_proto_modbus_build_tcp_frame(&response, 1, 1, pdu, sizeof(pdu));
  esy_proto_modbus_master_on_rx_frame(&f.master, bytes, response.len);
  TEST_ASSERT_EQUAL_UINT(1, f.io.complete_count);
  master_fixture_init(&f);
  submit_request(&f, 0, ESY_PROTO_MODBUS_FUNC_WRITE_SINGLE_REGISTER,
                 ESY_PROTO_MODBUS_PRIORITY_LOW, 1, 1);
  esy_proto_modbus_master_tick(&f.master);
  fake_esy_hal_system_set_tick(100);
  esy_proto_modbus_master_tick(&f.master);
  TEST_ASSERT_EQUAL_UINT(1, f.io.sent_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, f.io.statuses[0]);
}
static void abort_in_completion(esy_proto_modbus_master_t* m,
                                esy_proto_modbus_req_status_t st, uint8_t unit,
                                esy_proto_modbus_func_t fc, const uint8_t* pdu,
                                uint16_t len, void* arg) {
  fake_complete(m, st, unit, fc, pdu, len, arg);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_master_abort_all(m, true));
}
void test_manager_completion_can_abort_without_recursive_completion(void) {
  master_fixture_t f;
  master_fixture_init(&f);
  esy_proto_modbus_request_t req = {0};
  req.unit_id = 1;
  req.pdu[0] = 1;
  req.pdu_len = 1;
  req.func_code = ESY_PROTO_MODBUS_FUNC_READ_COILS;
  req.on_complete = abort_in_completion;
  req.user_ctx = &f.io;
  esy_proto_modbus_master_submit(&f.master, &req);
  esy_proto_modbus_master_tick(&f.master);
  esy_proto_modbus_master_abort_all(&f.master, true);
  TEST_ASSERT_EQUAL_UINT(1, f.io.complete_count);
}

void test_slave_async_response_borrow_blocks_overwrite_and_deinit(void) {
  slave_fixture_t f = {0};
  f.io.tx_result = ESY_OK;
  f.cfg.unit_id = 1;
  f.cfg.tx_fn = fake_tx;
  f.cfg.tx_user_ctx = &f.io;
  f.cfg.is_tx_busy = fake_busy;
  f.cfg.resp_pdu_buf = f.resp_pdu_buf;
  f.cfg.resp_pdu_buf_size = sizeof(f.resp_pdu_buf);
  f.cfg.on_request = fake_slave_request;
  f.cfg.user_ctx = &f.io;
  TEST_ASSERT_EQUAL(
      ESY_OK, esy_proto_modbus_slave_init(&f.slave, &f.cfg, f.tx_frame_buf,
                                          sizeof(f.tx_frame_buf)));
  uint8_t request[TEST_MANAGER_FRAME_MAX], pdu[] = {1, 0};
  uint16_t len;
  build_rtu_frame(request, &len, 1, pdu, sizeof(pdu));
  esy_proto_modbus_slave_on_rx_frame(&f.slave, request, len);
  f.io.busy = true;
  uint8_t saved[TEST_MANAGER_FRAME_MAX];
  memcpy(saved, f.tx_frame_buf, sizeof(saved));
  esy_proto_modbus_slave_on_rx_frame(&f.slave, request, len);
  TEST_ASSERT_EQUAL_UINT(1, f.io.slave_request_count);
  TEST_ASSERT_EQUAL_MEMORY(saved, f.tx_frame_buf, sizeof(saved));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_slave_deinit(&f.slave));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_slave_init(
                                       &f.slave, &f.cfg, f.tx_frame_buf,
                                       sizeof(f.tx_frame_buf)));
  f.io.busy = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_slave_deinit(&f.slave));
  TEST_ASSERT_FALSE(esy_proto_modbus_slave_is_initialized(&f.slave));
}

void test_explicit_async_tx_delays_broadcast_and_response_timeout(void) {
  master_fixture_t f;
  master_fixture_init(&f);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_master_on_tx_complete(&f.master, ESY_OK));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_master_on_tx_complete(NULL, ESY_OK));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_master_deinit(&f.master));
  f.cfg.defer_tx_completion = true;
  f.cfg.is_tx_busy = fake_busy;
  f.cfg.timeout_ms = 2u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_master_init(&f.master, &f.cfg));
  submit_request(&f, 0, ESY_PROTO_MODBUS_FUNC_WRITE_SINGLE_REGISTER,
      ESY_PROTO_MODBUS_PRIORITY_LOW, 2u, 0u);
  esy_proto_modbus_master_tick(&f.master);
  fake_esy_hal_system_set_tick(100u);
  esy_proto_modbus_master_tick(&f.master);
  TEST_ASSERT_EQUAL(0u, f.io.complete_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_master_on_tx_complete(&f.master, ESY_ERR_IO));
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR, f.io.statuses[0]);
  fake_esy_hal_system_set_tick(UINT32_MAX - 2u);
  submit_request(&f, 1, ESY_PROTO_MODBUS_FUNC_READ_COILS,
      ESY_PROTO_MODBUS_PRIORITY_LOW, 2u, 0u);
  esy_proto_modbus_master_tick(&f.master);
  fake_esy_hal_system_set_tick(UINT32_MAX);
  esy_proto_modbus_master_tick(&f.master);
  TEST_ASSERT_EQUAL(1u, f.io.complete_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_master_on_tx_complete(&f.master, ESY_OK));
  fake_esy_hal_system_advance_tick(1u);
  esy_proto_modbus_master_tick(&f.master);
  TEST_ASSERT_EQUAL(1u, f.io.complete_count);
  fake_esy_hal_system_advance_tick(1u);
  esy_proto_modbus_master_tick(&f.master);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TIMEOUT, f.io.statuses[1]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_master_deinit(&f.master));
}

void test_async_abort_keeps_physical_borrow_until_late_completion(void) {
  master_fixture_t f;
  master_fixture_init(&f);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_master_deinit(&f.master));
  f.cfg.defer_tx_completion = true;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_master_init(&f.master, &f.cfg));
  f.cfg.is_tx_busy = fake_busy;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_master_init(&f.master, &f.cfg));
  submit_request(&f, 1, ESY_PROTO_MODBUS_FUNC_READ_COILS,
      ESY_PROTO_MODBUS_PRIORITY_LOW, 0u, 0u);
  esy_proto_modbus_master_tick(&f.master);
  f.io.busy = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_master_abort_all(&f.master, true));
  TEST_ASSERT_EQUAL(1u, f.io.complete_count);
  submit_request(&f, 2, ESY_PROTO_MODBUS_FUNC_READ_COILS,
      ESY_PROTO_MODBUS_PRIORITY_LOW, 0u, 0u);
  esy_proto_modbus_master_tick(&f.master);
  TEST_ASSERT_EQUAL(1u, f.io.sent_count);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_master_deinit(&f.master));
  f.io.busy = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_master_on_tx_complete(&f.master, ESY_ERR_IO));
  TEST_ASSERT_EQUAL(1u, f.io.complete_count);
  esy_proto_modbus_master_tick(&f.master);
  TEST_ASSERT_EQUAL(2u, f.io.sent_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_master_on_tx_complete(&f.master, ESY_ERR_IO));
  TEST_ASSERT_EQUAL(2u, f.io.complete_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR, f.io.statuses[1]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_master_deinit(&f.master));
}
