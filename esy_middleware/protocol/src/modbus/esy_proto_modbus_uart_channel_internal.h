/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_PROTO_MODBUS_UART_CHANNEL_INTERNAL_H
#define ESY_PROTO_MODBUS_UART_CHANNEL_INTERNAL_H
#include "esy_middleware/protocol/esy_proto_modbus_uart_channel.h"
#include "esy_middleware/io_scheduler/esy_uart_tx_scheduler.h"
#include <stdatomic.h>

#ifndef ESY_PROTO_MODBUS_UART_RX_BUF_SIZE
/* RTU max: addr(1) + PDU(<=252) + CRC(2) = 255; keep guard space. */
#define ESY_PROTO_MODBUS_UART_RX_BUF_SIZE   (ESY_PROTO_MODBUS_MANAGER_MAX_PDU_LEN + 16u)
#endif

#ifndef ESY_PROTO_MODBUS_UART_TX_FRAME_BUF_SIZE
/* Use one conservative frame work buffer for RTU and TCP-sized frames. */
#define ESY_PROTO_MODBUS_UART_TX_FRAME_BUF_SIZE  (ESY_PROTO_MODBUS_MANAGER_MAX_PDU_LEN + 32u)
#endif

#ifndef ESY_PROTO_MODBUS_UART_SLAVE_RESP_PDU_BUF_SIZE
/* PDU: FUNC(1) + DATA; keep guard space below the protocol maximum. */
#define ESY_PROTO_MODBUS_UART_SLAVE_RESP_PDU_BUF_SIZE (ESY_PROTO_MODBUS_MANAGER_MAX_PDU_LEN + 8u)
#endif

#define ESY_PROTO_MODBUS_UART_RX_STREAM_SIZE (512u)


typedef void (*esy_proto_modbus_uart_frame_handler_t)(
    void          *ctx,
    const uint8_t *frame,
    uint16_t       length);

typedef struct {
  esy_uart_t *uart;
  uint8_t stream_storage[ESY_PROTO_MODBUS_UART_RX_STREAM_SIZE];
  uint8_t frame[ESY_PROTO_MODBUS_UART_RX_BUF_SIZE];
  uint16_t length;
  uint32_t last_tick;
  bool discarding;
  bool active;
  atomic_bool error;
} esy_proto_modbus_uart_rx_t;

struct esy_proto_modbus_uart_master_channel_s {
  uint8_t allocated;
  bool ready;
  bool initialized;

  esy_uart_t *uart;
  bool processing;

  uint32_t rtu_frame_timeout_ms;
  uint32_t broadcast_turnaround_ms;
  uint32_t tx_not_before;
  bool tx_gap_pending;
  bool last_tx_broadcast;

  /* RX framing */
  esy_proto_modbus_uart_rx_t rx;
  esy_uart_tx_scheduler_t tx;
  esy_uart_tx_scheduler_tx_request_t tx_requests[2];

  /* protocol */
  esy_proto_modbus_master_t master;

  /* caller-owned queue storage */
  esy_prio_slot_t            *slots;
  esy_proto_modbus_payload_t *payloads;
  uint16_t                    queue_depth;

  /* master tx frame work buffer (required by modbus_manager) */
  uint8_t tx_frame_buf[ESY_PROTO_MODBUS_UART_TX_FRAME_BUF_SIZE];
  bool stopping;

  /* Owner-only staging avoids a full request on callback/reentry stacks. */
  esy_proto_modbus_request_t submit_request;

  /* channel callback */
  esy_proto_modbus_uart_master_channel_complete_cb on_complete;
  void *user_ctx;
};

struct esy_proto_modbus_uart_slave_channel_s {
  uint8_t allocated;
  uint8_t unit_id;
  bool ready;
  bool initialized;

  esy_uart_t *uart;
  bool processing;

  uint32_t rtu_frame_timeout_ms;
  esy_proto_modbus_uart_slave_channel_request_cb on_request;
  void *user_ctx;

  /* RX framing */
  esy_proto_modbus_uart_rx_t rx;
  esy_uart_tx_scheduler_t tx;
  esy_uart_tx_scheduler_tx_request_t tx_requests[2];

  /* protocol */
  esy_proto_modbus_slave_t slave;

  /* buffers required by slave manager */
  uint8_t resp_pdu_buf[ESY_PROTO_MODBUS_UART_SLAVE_RESP_PDU_BUF_SIZE];
  uint8_t tx_frame_buf[ESY_PROTO_MODBUS_UART_TX_FRAME_BUF_SIZE];
  bool stopping;
};

#endif
