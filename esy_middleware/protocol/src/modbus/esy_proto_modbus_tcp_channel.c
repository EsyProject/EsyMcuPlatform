/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/protocol/esy_proto_modbus_tcp_channel.h"

#include <string.h>

#include "esy_hal/esy_hal_system.h"

#define ESY_PROTO_MODBUS_TCP_ADU_BYTES (ESY_PROTO_MODBUS_MBAP_LEN + ESY_PROTO_MODBUS_MAX_PDU_LEN)
#define ESY_PROTO_MODBUS_TCP_PREFIX_BYTES (ESY_PROTO_MODBUS_MBAP_LEN - 1u)
#define ESY_PROTO_MODBUS_TCP_DEFAULT_TX_TIMEOUT_MS (1000u)
#define ESY_PROTO_MODBUS_TCP_DEFAULT_RX_BUDGET (256u)
#define ESY_PROTO_MODBUS_TCP_DEFAULT_RESPONSE_TIMEOUT_MS (100u)

typedef struct {
  const void *data;
  size_t size;
} modbus_tcp_range_t;

typedef struct {
  esy_network_connection_t connection;
  esy_network_connection_status_t status;
  uint64_t session_id;
  uint32_t tx_timeout_ms;
  uint32_t rx_byte_budget;
  uint32_t tx_started_ms;
  esy_err_t failure;
  uint16_t rx_length;
  uint16_t rx_target;
  uint16_t tx_length;
  bool status_valid;
  bool eof;
  bool tx_pending;
  bool tx_accepted;
  bool tx_wait_active;
  bool bad_frame;
  uint8_t rx_frame[ESY_PROTO_MODBUS_TCP_ADU_BYTES];
  uint8_t tx_frame[ESY_PROTO_MODBUS_TCP_ADU_BYTES];
} modbus_tcp_io_t;

struct esy_proto_modbus_tcp_master_channel_s {
  uint8_t allocated;
  bool processing;
  bool closing;
  bool failure_reported;
  bool aborting;
  modbus_tcp_io_t io;
  esy_proto_modbus_master_t manager;
  uint8_t manager_tx[ESY_PROTO_MODBUS_TCP_ADU_BYTES];
  esy_proto_modbus_request_t submit_request;
  esy_proto_modbus_tcp_master_channel_complete_cb on_complete;
  void *user_ctx;
};

struct esy_proto_modbus_tcp_slave_channel_s {
  uint8_t allocated;
  bool processing;
  bool closing;
  modbus_tcp_io_t io;
  esy_proto_modbus_slave_t manager;
  uint8_t manager_tx[ESY_PROTO_MODBUS_TCP_ADU_BYTES];
  uint8_t response[ESY_PROTO_MODBUS_MAX_PDU_LEN];
  esy_proto_modbus_tcp_slave_channel_request_cb on_request;
  void *user_ctx;
};

_Static_assert(sizeof(esy_proto_modbus_tcp_master_channel_t) <=
    ESY_PROTO_MODBUS_TCP_MASTER_CHANNEL_STORAGE_BYTES, "Modbus TCP master storage too small");
_Static_assert(sizeof(esy_proto_modbus_tcp_slave_channel_t) <=
    ESY_PROTO_MODBUS_TCP_SLAVE_CHANNEL_STORAGE_BYTES, "Modbus TCP slave storage too small");
_Static_assert(_Alignof(esy_proto_modbus_tcp_master_channel_storage_t) >=
    _Alignof(esy_proto_modbus_tcp_master_channel_t), "Modbus TCP master storage alignment too small");
_Static_assert(_Alignof(esy_proto_modbus_tcp_slave_channel_storage_t) >=
    _Alignof(esy_proto_modbus_tcp_slave_channel_t), "Modbus TCP slave storage alignment too small");

/* Creation validates every complete declared range before clearing any storage. */
static bool ranges_are_disjoint(const modbus_tcp_range_t *ranges, size_t count) {
  for (size_t i = 0u; i < count; ++i) {
    if (!ranges[i].data || !ranges[i].size ||
        ranges[i].size > UINTPTR_MAX - (uintptr_t)ranges[i].data) return false;
  }
  for (size_t i = 0u; i < count; ++i) {
    uintptr_t start = (uintptr_t)ranges[i].data;
    uintptr_t end = start + ranges[i].size;
    for (size_t j = i + 1u; j < count; ++j) {
      uintptr_t other = (uintptr_t)ranges[j].data;
      if (start < other + ranges[j].size && other < end) return false;
    }
  }
  return true;
}

static bool connection_is_valid(const esy_network_connection_t *connection) {
  return connection->get_status && connection->send_all && connection->stream.recv;
}

static bool peer_is_valid(const esy_network_connection_status_t *status) {
  return status->session_id != 0u &&
      (status->state == ESY_NETWORK_CONNECTION_OPEN || status->state == ESY_NETWORK_CONNECTION_EOF);
}

static esy_err_t provider_error(esy_err_t result) {
  return result < ESY_OK ? result : ESY_ERR_IO;
}

static void cancel_tx(modbus_tcp_io_t *io) {
  io->tx_pending = false;
  io->tx_accepted = false;
  io->tx_wait_active = false;
  io->tx_length = 0u;
}

static esy_err_t fail_io(modbus_tcp_io_t *io, esy_err_t error) {
  if (io->failure == ESY_OK) io->failure = provider_error(error);
  cancel_tx(io);
  return io->failure;
}

static esy_err_t fail_frame(modbus_tcp_io_t *io) {
  if (io->failure == ESY_OK) io->bad_frame = true;
  return fail_io(io, ESY_ERR_CRC_FAIL);
}

static esy_err_t observe_peer(modbus_tcp_io_t *io) {
  esy_network_connection_status_t status;
  io->status_valid = false;
  esy_err_t err = io->connection.get_status(io->connection.stream.context, &status);
  if (err == ESY_ERR_BUSY) return err;
  if (err != ESY_OK) return fail_io(io, err);
  if (!peer_is_valid(&status) || status.session_id != io->session_id ||
      (io->eof && status.state != ESY_NETWORK_CONNECTION_EOF)) return fail_io(io, ESY_ERR_STATE);
  io->status = status;
  io->status_valid = true;
  if (status.state == ESY_NETWORK_CONNECTION_EOF) io->eof = true;
  return ESY_OK;
}

static esy_err_t progress_peer(modbus_tcp_io_t *io) {
  if (!io->connection.stream.progress) return ESY_OK;
  esy_err_t progressed = io->connection.stream.progress(io->connection.stream.context);
  esy_err_t observed = observe_peer(io);
  if (io->failure != ESY_OK) return io->failure;
  if (progressed != ESY_OK && progressed != ESY_ERR_BUSY) return fail_io(io, progressed);
  if (observed != ESY_OK) return observed;
  return progressed;
}

static bool tx_expired(const modbus_tcp_io_t *io) {
  return io->tx_wait_active &&
      (uint32_t)((uint32_t)esy_hal_system_get_tick() - io->tx_started_ms) >= io->tx_timeout_ms;
}

static esy_err_t stage_tx(modbus_tcp_io_t *io, const uint8_t *frame, uint16_t length) {
  if (io->failure != ESY_OK) return io->failure;
  if (io->tx_pending) return ESY_ERR_BUSY;
  if (!frame || length < ESY_PROTO_MODBUS_TCP_MIN_FRAME_LEN || length > sizeof(io->tx_frame))
    return ESY_ERR_INVALID_ARG;
  memcpy(io->tx_frame, frame, length);
  io->tx_length = length;
  io->tx_pending = true;
  io->tx_accepted = false;
  io->tx_wait_active = true;
  io->tx_started_ms = (uint32_t)esy_hal_system_get_tick();
  return ESY_OK;
}

/* Whole acceptance awaiting a BUSY epoch observation is never transmitted twice.
 * The same original deadline covers this confirmation window. */
static esy_err_t send_staged(modbus_tcp_io_t *io, bool allow_eof,
    bool *attempted, bool *completed) {
  *completed = false;
  if (!io->tx_pending) return ESY_OK;
  if (tx_expired(io)) return fail_io(io, ESY_ERR_TIMEOUT);
  /* A request/completion callback may have changed endpoint state since RX.
   * Never submit a staged ADU using a status sampled before that callback. */
  esy_err_t before = observe_peer(io);
  if (before != ESY_OK) return before;
  if (tx_expired(io)) return fail_io(io, ESY_ERR_TIMEOUT);
  if (io->eof && !allow_eof && !io->tx_accepted) return fail_io(io, ESY_ERR_IO);
  if (io->tx_accepted) {
    cancel_tx(io);
    *completed = true;
    return ESY_OK;
  }
  *attempted = true;
  uint16_t requested = io->tx_length;
  int32_t result = io->connection.send_all(io->connection.stream.context, io->tx_frame, requested);
  if (result == (int32_t)requested) io->tx_accepted = true;
  esy_err_t observed = observe_peer(io);
  if (io->failure != ESY_OK) return io->failure;
  if (result < ESY_OK && result != ESY_ERR_BUSY) return fail_io(io, result);
  if (result > 0 && result != (int32_t)requested) return fail_io(io, ESY_ERR_IO);
  if (tx_expired(io)) return fail_io(io, ESY_ERR_TIMEOUT);
  if (observed != ESY_OK) return observed;
  if (!io->tx_accepted) return ESY_ERR_BUSY;
  cancel_tx(io);
  *completed = true;
  return ESY_OK;
}

static esy_err_t receive_frame(modbus_tcp_io_t *io, bool *complete) {
  uint32_t budget = io->rx_byte_budget;
  *complete = false;
  for (;;) {
    if (io->rx_target == ESY_PROTO_MODBUS_TCP_PREFIX_BYTES &&
        io->rx_length == ESY_PROTO_MODBUS_TCP_PREFIX_BYTES) {
      uint16_t length = (uint16_t)(((uint16_t)io->rx_frame[4] << 8) | io->rx_frame[5]);
      if (io->rx_frame[2] != 0u || io->rx_frame[3] != 0u || length < 2u ||
          length > ESY_PROTO_MODBUS_MAX_PDU_LEN + 1u) return fail_frame(io);
      io->rx_target = (uint16_t)(ESY_PROTO_MODBUS_TCP_PREFIX_BYTES + length);
    }
    if (io->rx_length == io->rx_target) {
      if (!esy_proto_modbus_verify_tcp_frame(io->rx_frame, io->rx_length))
        return fail_frame(io);
      *complete = true;
      return ESY_OK;
    }
    /* Native providers may return IO for an empty half-closed RX stream.
     * The fresh EOF observation already proves that no more bytes can arrive. */
    if (io->status_valid && io->eof && io->status.rx_available == 0u) return ESY_OK;
    if (budget == 0u) return ESY_OK;
    size_t requested = io->rx_target - io->rx_length;
    if (requested > budget) requested = budget;
    int32_t result = io->connection.stream.recv(io->connection.stream.context,
        io->rx_frame + io->rx_length, requested);
    if (result > 0 && (size_t)result <= requested)
      io->rx_length += (uint16_t)result; /* Retained locally until epoch verification succeeds. */
    esy_err_t observed = observe_peer(io);
    if (io->failure != ESY_OK) return io->failure;
    /* Some providers first discover FIN during recv and report an empty EOF as
     * IO. Only a successful same-session EOF observation can classify that case;
     * the caller still rejects truncated frames or unanswered master requests. */
    if (result == ESY_ERR_IO && observed == ESY_OK && io->status_valid &&
        io->eof && io->status.rx_available == 0u) return ESY_OK;
    if (result < ESY_OK && result != ESY_ERR_BUSY) return fail_io(io, result);
    if (result > 0 && (size_t)result > requested) return fail_io(io, ESY_ERR_IO);
    if (observed != ESY_OK) return observed;
    if (result == ESY_ERR_BUSY) return ESY_ERR_BUSY;
    if (result == 0) return ESY_OK;
    budget -= (uint32_t)result;
  }
}

static void reset_rx(modbus_tcp_io_t *io) {
  io->rx_length = 0u;
  io->rx_target = ESY_PROTO_MODBUS_TCP_PREFIX_BYTES;
}

static void init_io(modbus_tcp_io_t *io, const esy_network_connection_t *connection,
    const esy_network_connection_status_t *status, uint32_t timeout, uint32_t budget) {
  io->connection = *connection;
  io->status = *status;
  io->session_id = status->session_id;
  io->eof = status->state == ESY_NETWORK_CONNECTION_EOF;
  io->status_valid = true;
  io->tx_timeout_ms = timeout ? timeout : ESY_PROTO_MODBUS_TCP_DEFAULT_TX_TIMEOUT_MS;
  io->rx_byte_budget = budget ? budget : ESY_PROTO_MODBUS_TCP_DEFAULT_RX_BUDGET;
  reset_rx(io);
}

/* EOF blocks starting another transaction. An existing response wait can still
 * expire through the manager; its attempted EOF retry is rejected by master_tx. */
static bool master_busy(void *arg) {
  esy_proto_modbus_tcp_master_channel_t *channel = arg;
  return channel->io.tx_pending || (!channel->closing &&
      (channel->io.failure != ESY_OK || (channel->io.eof &&
       esy_proto_modbus_master_get_state(&channel->manager) == ESY_PROTO_MODBUS_MASTER_STATE_IDLE)));
}

static esy_err_t master_tx(const uint8_t *frame, uint16_t length, void *arg) {
  esy_proto_modbus_tcp_master_channel_t *channel = arg;
  if (channel->closing || channel->io.eof) return ESY_ERR_STATE;
  return stage_tx(&channel->io, frame, length);
}

static void master_complete(esy_proto_modbus_master_t *manager,
    esy_proto_modbus_req_status_t status, uint8_t unit, esy_proto_modbus_func_t function,
    const uint8_t *response, uint16_t response_length, void *arg) {
  (void)manager;
  esy_proto_modbus_tcp_master_channel_t *channel = arg;
  if (status == ESY_PROTO_MODBUS_REQ_STATUS_ABORTED && channel->io.failure != ESY_OK)
    status = channel->io.bad_frame ? ESY_PROTO_MODBUS_REQ_STATUS_BAD_FRAME
                                   : ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR;
  if (channel->on_complete)
    channel->on_complete(channel, status, unit, function, response, response_length, channel->user_ctx);
}

static esy_err_t master_failure(esy_proto_modbus_tcp_master_channel_t *channel, esy_err_t error) {
  (void)fail_io(&channel->io, error);
  if (!channel->failure_reported) {
    channel->failure_reported = true;
    (void)esy_proto_modbus_master_abort_all(&channel->manager, true);
  }
  return channel->io.failure;
}

static bool master_pending(const esy_proto_modbus_tcp_master_channel_t *channel) {
  return esy_proto_modbus_master_get_queue_depth(&channel->manager) != 0u ||
      esy_proto_modbus_master_get_state(&channel->manager) != ESY_PROTO_MODBUS_MASTER_STATE_IDLE;
}

static esy_err_t master_eof(esy_proto_modbus_tcp_master_channel_t *channel) {
  modbus_tcp_io_t *io = &channel->io;
  if (!io->eof || !io->status_valid || io->status.rx_available != 0u || io->tx_pending)
    return ESY_OK;
  if (io->rx_length != 0u) return master_failure(channel, fail_frame(io));
  if (master_pending(channel)) return master_failure(channel, ESY_ERR_IO);
  return ESY_OK;
}

static bool slave_busy(void *arg) {
  esy_proto_modbus_tcp_slave_channel_t *channel = arg;
  return channel->io.tx_pending;
}

static esy_err_t slave_tx(const uint8_t *frame, uint16_t length, void *arg) {
  esy_proto_modbus_tcp_slave_channel_t *channel = arg;
  if (channel->closing) return ESY_ERR_STATE;
  return stage_tx(&channel->io, frame, length);
}

static esy_err_t slave_request(esy_proto_modbus_slave_t *manager, uint8_t unit,
    esy_proto_modbus_func_t function, const uint8_t *request, uint16_t request_length,
    uint8_t *response, uint16_t *response_length, uint16_t response_capacity, void *arg) {
  (void)manager;
  esy_proto_modbus_tcp_slave_channel_t *channel = arg;
  return channel->on_request(channel, unit, function, request, request_length,
      response, response_length, response_capacity, channel->user_ctx);
}

static esy_err_t slave_eof(esy_proto_modbus_tcp_slave_channel_t *channel) {
  modbus_tcp_io_t *io = &channel->io;
  if (io->eof && io->status_valid && io->status.rx_available == 0u &&
      !io->tx_pending && io->rx_length != 0u) return fail_frame(io);
  return ESY_OK;
}

esy_proto_modbus_tcp_master_channel_config_t esy_proto_modbus_tcp_master_channel_default_cfg(void) {
  esy_proto_modbus_tcp_master_channel_config_t config = {0};
  config.timeout_ms = ESY_PROTO_MODBUS_TCP_DEFAULT_RESPONSE_TIMEOUT_MS;
  config.tx_timeout_ms = ESY_PROTO_MODBUS_TCP_DEFAULT_TX_TIMEOUT_MS;
  config.rx_byte_budget = ESY_PROTO_MODBUS_TCP_DEFAULT_RX_BUDGET;
  return config;
}

esy_proto_modbus_tcp_slave_channel_config_t esy_proto_modbus_tcp_slave_channel_default_cfg(void) {
  esy_proto_modbus_tcp_slave_channel_config_t config = {0};
  config.unit_id = UINT8_MAX;
  config.tx_timeout_ms = ESY_PROTO_MODBUS_TCP_DEFAULT_TX_TIMEOUT_MS;
  config.rx_byte_budget = ESY_PROTO_MODBUS_TCP_DEFAULT_RX_BUDGET;
  return config;
}

esy_err_t esy_proto_modbus_tcp_master_channel_create(
    const esy_proto_modbus_tcp_master_channel_config_t *config,
    esy_proto_modbus_tcp_master_channel_storage_t *storage,
    esy_proto_modbus_tcp_master_channel_t **out_channel) {
  if (!config || !storage || !out_channel || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  if ((uintptr_t)config % _Alignof(esy_proto_modbus_tcp_master_channel_config_t) != 0u ||
      (uintptr_t)storage % _Alignof(esy_proto_modbus_tcp_master_channel_storage_t) != 0u ||
      (uintptr_t)out_channel % _Alignof(esy_proto_modbus_tcp_master_channel_t *) != 0u)
    return ESY_ERR_INVALID_ARG;
  const modbus_tcp_range_t descriptors[] = {
    {storage, sizeof(*storage)}, {config, sizeof(*config)}, {out_channel, sizeof(*out_channel)},
  };
  if (!ranges_are_disjoint(descriptors, sizeof(descriptors) / sizeof(descriptors[0])))
    return ESY_ERR_INVALID_ARG;
  const esy_proto_modbus_tcp_master_channel_static_resources_t *resources = &config->static_resources;
  if (!connection_is_valid(&config->connection) || config->timeout_ms > INT32_MAX ||
      config->tx_timeout_ms > INT32_MAX || config->queue_depth == 0u || config->queue_depth > INT16_MAX ||
      resources->slot_storage_count < config->queue_depth || resources->payload_storage_count < config->queue_depth ||
      (uintptr_t)resources->slot_storage % _Alignof(esy_prio_slot_t) != 0u ||
      (uintptr_t)resources->payload_storage % _Alignof(esy_proto_modbus_payload_t) != 0u)
    return ESY_ERR_INVALID_ARG;
  const modbus_tcp_range_t ranges[] = {
    {storage, sizeof(*storage)}, {config, sizeof(*config)}, {out_channel, sizeof(*out_channel)},
    {resources->slot_storage, (size_t)resources->slot_storage_count * sizeof(*resources->slot_storage)},
    {resources->payload_storage, (size_t)resources->payload_storage_count * sizeof(*resources->payload_storage)},
  };
  if (!ranges_are_disjoint(ranges, sizeof(ranges) / sizeof(ranges[0]))) return ESY_ERR_INVALID_ARG;
  *out_channel = NULL;
  if (storage->bytes[0] != 0u) return ESY_ERR_STATE;
  esy_network_connection_status_t status;
  esy_err_t err = config->connection.get_status(config->connection.stream.context, &status);
  if (err != ESY_OK) return provider_error(err);
  if (!peer_is_valid(&status)) return ESY_ERR_STATE;
  esy_proto_modbus_tcp_master_channel_t *channel = (esy_proto_modbus_tcp_master_channel_t *)storage->bytes;
  memset(channel, 0, sizeof(*channel));
  init_io(&channel->io, &config->connection, &status, config->tx_timeout_ms, config->rx_byte_budget);
  channel->on_complete = config->on_complete;
  channel->user_ctx = config->user_ctx;
  esy_proto_modbus_master_config_t manager = esy_proto_modbus_master_default_cfg();
  manager.frame_type = ESY_PROTO_MODBUS_FRAME_TCP;
  manager.tx_fn = master_tx;
  manager.is_tx_busy = master_busy;
  manager.tx_user_ctx = channel;
  manager.defer_tx_completion = true;
  manager.timeout_ms = config->timeout_ms;
  manager.retry_max = config->retry_max;
  manager.tx_frame_buf = channel->manager_tx;
  manager.tx_frame_buf_size = sizeof(channel->manager_tx);
  manager.slots = resources->slot_storage;
  manager.payloads = resources->payload_storage;
  manager.slot_count = config->queue_depth;
  err = esy_proto_modbus_master_init(&channel->manager, &manager);
  if (err != ESY_OK) {
    memset(channel, 0, sizeof(*channel));
    return err;
  }
  channel->allocated = 1u;
  *out_channel = channel;
  return ESY_OK;
}

esy_err_t esy_proto_modbus_tcp_slave_channel_create(
    const esy_proto_modbus_tcp_slave_channel_config_t *config,
    esy_proto_modbus_tcp_slave_channel_storage_t *storage,
    esy_proto_modbus_tcp_slave_channel_t **out_channel) {
  if (!config || !storage || !out_channel || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  if ((uintptr_t)config % _Alignof(esy_proto_modbus_tcp_slave_channel_config_t) != 0u ||
      (uintptr_t)storage % _Alignof(esy_proto_modbus_tcp_slave_channel_storage_t) != 0u ||
      (uintptr_t)out_channel % _Alignof(esy_proto_modbus_tcp_slave_channel_t *) != 0u)
    return ESY_ERR_INVALID_ARG;
  const modbus_tcp_range_t ranges[] = {
    {storage, sizeof(*storage)}, {config, sizeof(*config)}, {out_channel, sizeof(*out_channel)},
  };
  if (!ranges_are_disjoint(ranges, sizeof(ranges) / sizeof(ranges[0]))) return ESY_ERR_INVALID_ARG;
  *out_channel = NULL;
  if (!connection_is_valid(&config->connection) || !config->on_request || config->tx_timeout_ms > INT32_MAX)
    return ESY_ERR_INVALID_ARG;
  if (storage->bytes[0] != 0u) return ESY_ERR_STATE;
  esy_network_connection_status_t status;
  esy_err_t err = config->connection.get_status(config->connection.stream.context, &status);
  if (err != ESY_OK) return provider_error(err);
  if (!peer_is_valid(&status)) return ESY_ERR_STATE;
  esy_proto_modbus_tcp_slave_channel_t *channel = (esy_proto_modbus_tcp_slave_channel_t *)storage->bytes;
  memset(channel, 0, sizeof(*channel));
  init_io(&channel->io, &config->connection, &status, config->tx_timeout_ms, config->rx_byte_budget);
  channel->on_request = config->on_request;
  channel->user_ctx = config->user_ctx;
  esy_proto_modbus_slave_config_t manager = {0};
  manager.frame_type = ESY_PROTO_MODBUS_FRAME_TCP;
  manager.unit_id = config->unit_id;
  manager.tx_fn = slave_tx;
  manager.is_tx_busy = slave_busy;
  manager.tx_user_ctx = channel;
  manager.resp_pdu_buf = channel->response;
  manager.resp_pdu_buf_size = sizeof(channel->response);
  manager.on_request = slave_request;
  manager.user_ctx = channel;
  err = esy_proto_modbus_slave_init(&channel->manager, &manager,
      channel->manager_tx, sizeof(channel->manager_tx));
  if (err != ESY_OK) {
    memset(channel, 0, sizeof(*channel));
    return err;
  }
  channel->allocated = 1u;
  *out_channel = channel;
  return ESY_OK;
}

esy_err_t esy_proto_modbus_tcp_master_channel_poll(esy_proto_modbus_tcp_master_channel_t *channel) {
  if (!channel || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  if (!channel->allocated || channel->processing) return ESY_ERR_STATE;
  if (channel->io.failure != ESY_OK) return channel->io.failure;
  channel->processing = true;
  modbus_tcp_io_t *io = &channel->io;
  esy_err_t err;
  if (tx_expired(io)) {
    err = master_failure(channel, ESY_ERR_TIMEOUT);
    goto done;
  }
  /* This only advances local manager state. Provider I/O still requires a fresh
   * session observation below, even when a deadline schedules a retry/new ADU. */
  esy_proto_modbus_master_tick(&channel->manager);
  err = observe_peer(io);
  if (err != ESY_OK) goto provider_result;
  err = progress_peer(io);
  if (err != ESY_OK) goto provider_result;
  if (io->eof && io->tx_pending && !io->tx_accepted) {
    err = master_failure(channel, ESY_ERR_IO);
    goto done;
  }
  bool attempted = false;
  bool completed = false;
  err = send_staged(io, false, &attempted, &completed);
  if (err != ESY_OK) goto provider_result;
  if (completed) (void)esy_proto_modbus_master_on_tx_complete(&channel->manager, ESY_OK);
  esy_proto_modbus_master_tick(&channel->manager);
  if (io->tx_pending) { err = ESY_OK; goto done; }
  bool frame = false;
  err = receive_frame(io, &frame);
  if (err != ESY_OK && err != ESY_ERR_BUSY) goto provider_result;
  if (frame) {
    /* recv/provider callbacks may consume time; observe expiry before dispatch. */
    esy_proto_modbus_master_tick(&channel->manager);
    if (!io->tx_pending) {
      esy_proto_modbus_master_on_rx_frame(&channel->manager, io->rx_frame, io->rx_length);
      reset_rx(io);
    }
  }
  {
    esy_err_t eof = master_eof(channel);
    if (eof != ESY_OK) err = eof;
  }
  goto done;
provider_result:
  if (io->failure != ESY_OK) err = master_failure(channel, io->failure);
done:
  if (io->failure == ESY_OK && tx_expired(io)) err = master_failure(channel, ESY_ERR_TIMEOUT);
  if (io->failure == ESY_OK && err == ESY_ERR_BUSY)
    esy_proto_modbus_master_tick(&channel->manager);
  channel->processing = false;
  return err;
}

esy_err_t esy_proto_modbus_tcp_slave_channel_poll(esy_proto_modbus_tcp_slave_channel_t *channel) {
  if (!channel || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  if (!channel->allocated || channel->processing) return ESY_ERR_STATE;
  if (channel->io.failure != ESY_OK) return channel->io.failure;
  channel->processing = true;
  modbus_tcp_io_t *io = &channel->io;
  esy_err_t err;
  if (tx_expired(io)) { err = fail_io(io, ESY_ERR_TIMEOUT); goto done; }
  err = observe_peer(io);
  if (err != ESY_OK) goto done;
  err = progress_peer(io);
  if (err != ESY_OK) goto done;
  bool attempted = false;
  bool completed = false;
  err = send_staged(io, true, &attempted, &completed);
  if (err != ESY_OK) goto done;
  bool frame = false;
  err = receive_frame(io, &frame);
  if (err != ESY_OK && err != ESY_ERR_BUSY) goto done;
  if (frame) {
    esy_proto_modbus_slave_on_rx_frame(&channel->manager, io->rx_frame, io->rx_length);
    reset_rx(io);
    if (io->tx_pending && !attempted) err = send_staged(io, true, &attempted, &completed);
  }
  if (io->failure == ESY_OK) {
    esy_err_t eof = slave_eof(channel);
    if (eof != ESY_OK) err = eof;
  }
done:
  if (io->failure == ESY_OK && tx_expired(io)) err = fail_io(io, ESY_ERR_TIMEOUT);
  channel->processing = false;
  return err;
}

esy_err_t esy_proto_modbus_tcp_master_channel_submit(esy_proto_modbus_tcp_master_channel_t *channel,
    const esy_proto_modbus_request_t *request) {
  if (!channel || !request || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  if (!channel->allocated || channel->closing || channel->io.failure != ESY_OK || channel->io.eof)
    return ESY_ERR_STATE;
  if (request->on_complete || request->user_ctx) return ESY_ERR_INVALID_ARG;
  bool processing = channel->processing;
  channel->processing = true;
  channel->submit_request = *request;
  channel->submit_request.on_complete = master_complete;
  channel->submit_request.user_ctx = channel;
  esy_err_t err = esy_proto_modbus_master_submit(&channel->manager, &channel->submit_request);
  channel->processing = processing;
  return err;
}

esy_err_t esy_proto_modbus_tcp_master_channel_abort_all(esy_proto_modbus_tcp_master_channel_t *channel,
    bool abort_current) {
  if (!channel || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  if (!channel->allocated || channel->closing || channel->io.failure != ESY_OK) return ESY_ERR_STATE;
  if (channel->aborting) return ESY_ERR_BUSY;
  bool processing = channel->processing;
  channel->processing = true;
  channel->aborting = true;
  if (abort_current) cancel_tx(&channel->io);
  esy_err_t err = esy_proto_modbus_master_abort_all(&channel->manager, abort_current);
  channel->aborting = false;
  channel->processing = processing;
  return err;
}

esy_err_t esy_proto_modbus_tcp_master_channel_destroy(esy_proto_modbus_tcp_master_channel_t *channel) {
  if (esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  if (!channel || !channel->allocated) return ESY_OK;
  if (channel->processing) return ESY_ERR_STATE;
  channel->processing = true;
  channel->closing = true;
  cancel_tx(&channel->io);
  esy_err_t err = esy_proto_modbus_master_deinit(&channel->manager);
  if (err == ESY_OK) memset(channel, 0, sizeof(*channel));
  else channel->processing = false;
  return err;
}

esy_err_t esy_proto_modbus_tcp_slave_channel_destroy(esy_proto_modbus_tcp_slave_channel_t *channel) {
  if (esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  if (!channel || !channel->allocated) return ESY_OK;
  if (channel->processing) return ESY_ERR_STATE;
  channel->processing = true;
  channel->closing = true;
  cancel_tx(&channel->io);
  esy_err_t err = esy_proto_modbus_slave_deinit(&channel->manager);
  if (err == ESY_OK) memset(channel, 0, sizeof(*channel));
  else channel->processing = false;
  return err;
}

bool esy_proto_modbus_tcp_master_channel_is_initialized(const esy_proto_modbus_tcp_master_channel_t *channel) {
  return channel && channel->allocated != 0u;
}

bool esy_proto_modbus_tcp_slave_channel_is_initialized(const esy_proto_modbus_tcp_slave_channel_t *channel) {
  return channel && channel->allocated != 0u;
}

uint16_t esy_proto_modbus_tcp_master_channel_get_queue_depth(const esy_proto_modbus_tcp_master_channel_t *channel) {
  return esy_proto_modbus_tcp_master_channel_is_initialized(channel)
      ? esy_proto_modbus_master_get_queue_depth(&channel->manager) : 0u;
}

esy_proto_modbus_master_state_t esy_proto_modbus_tcp_master_channel_get_state(
    const esy_proto_modbus_tcp_master_channel_t *channel) {
  return esy_proto_modbus_tcp_master_channel_is_initialized(channel)
      ? esy_proto_modbus_master_get_state(&channel->manager) : ESY_PROTO_MODBUS_MASTER_STATE_IDLE;
}

uint8_t esy_proto_modbus_tcp_slave_channel_get_addr(const esy_proto_modbus_tcp_slave_channel_t *channel) {
  return esy_proto_modbus_tcp_slave_channel_is_initialized(channel)
      ? esy_proto_modbus_slave_get_unit_id(&channel->manager) : 0u;
}

bool esy_proto_modbus_tcp_master_channel_is_peer_drained(esy_proto_modbus_tcp_master_channel_t *channel) {
  if (!channel || !channel->allocated || channel->processing || channel->io.failure != ESY_OK ||
      esy_hal_system_is_in_isr()) return false;
  channel->processing = true;
  esy_err_t err = observe_peer(&channel->io);
  if (channel->io.failure != ESY_OK) (void)master_failure(channel, channel->io.failure);
  bool drained = err == ESY_OK && channel->io.eof && channel->io.status.rx_available == 0u &&
      channel->io.status.unacked_bytes == 0u && channel->io.rx_length == 0u &&
      !channel->io.tx_pending && !master_pending(channel);
  channel->processing = false;
  return drained;
}

bool esy_proto_modbus_tcp_slave_channel_is_peer_drained(esy_proto_modbus_tcp_slave_channel_t *channel) {
  if (!channel || !channel->allocated || channel->processing || channel->io.failure != ESY_OK ||
      esy_hal_system_is_in_isr()) return false;
  channel->processing = true;
  esy_err_t err = observe_peer(&channel->io);
  bool drained = err == ESY_OK && channel->io.eof && channel->io.status.rx_available == 0u &&
      channel->io.status.unacked_bytes == 0u && channel->io.rx_length == 0u && !channel->io.tx_pending;
  channel->processing = false;
  return drained;
}
