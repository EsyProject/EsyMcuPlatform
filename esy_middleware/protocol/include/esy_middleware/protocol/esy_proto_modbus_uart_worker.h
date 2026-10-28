/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_proto_modbus_uart_worker.h
 * @brief Explicit OS workers owning independent caller-driven RTU channels.
 *
 * Zero worker storage before first create and keep its address stable. Create
 * copies configuration, creates one static task and privately owns a separate
 * channel handle/storage. It borrows UART, queue buffers, task resources and
 * callback context until successful destroy. Repeated create returns STATE.
 * Failed create with a NULL output retains nothing; a non-NULL output retains
 * resources and requires destroy retry. Never clear or reuse retained storage.
 *
 * All public calls require non-ISR task context. One lifecycle owner serializes
 * create/destroy with every API user. The worker alone drives channel polling,
 * RTU framing, physical TX and callbacks. Master submit/abort use one copied
 * command slot; concurrent commands return BUSY. An unclaimed command expires
 * after 100 one-millisecond waits. Once claimed it must finish, so callbacks
 * must be bounded and cannot wait for the submitting caller. Scheduling may
 * extend the elapsed wait. Owner completion callbacks may submit/abort directly,
 * but cannot destroy the worker. State/depth queries read independent atomic
 * snapshots. No channel pointer or alternate execution mode is exposed.
 *
 * The worker polls at 1 ms, subject to scheduling and synchronous callback time.
 * The channel retains modulo-32-bit HAL millisecond deadlines: scheduling gaps
 * and configured waits stay below 2^31 ms, with its documented RTU observation
 * and physical TX completion requirements. The worker's command/shutdown waits
 * count explicit 1 ms OS delays and retain no wrapping timestamp deadline.
 *
 * Destroy closes admission and lets the owner drain/clean its channel before
 * cooperative parking. It waits for up to 100 one-millisecond delays per call;
 * BUSY or cleanup failure retains the worker, UART and all buffers for retry.
 * Failed initialization without a task is cleaned by the lifecycle caller.
 * After successful destroy the worker can be created again. OTA policy belongs
 * to the consumer: stop producers and confirm destroy before Flash operations
 * requiring bus silence; preserve retained resources on timeout or failure.
 */

#ifndef ESY_PROTO_MODBUS_UART_WORKER_H
#define ESY_PROTO_MODBUS_UART_WORKER_H

#include "esy_middleware/protocol/esy_proto_modbus_uart_channel.h"
#include "esy_middleware/os/esy_os_task.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Independent OS owner for one caller-driven Modbus master worker. */
typedef struct esy_proto_modbus_uart_master_worker_s esy_proto_modbus_uart_master_worker_t;
/** @brief Independent OS owner for one caller-driven Modbus slave worker. */
typedef struct esy_proto_modbus_uart_slave_worker_s esy_proto_modbus_uart_slave_worker_t;
/** @brief Master worker storage includes its private channel and command slot. */
#define ESY_PROTO_MODBUS_UART_MASTER_WORKER_STORAGE_BYTES (5120u)
/** @brief Slave worker storage includes its private channel and OS lifecycle. */
#define ESY_PROTO_MODBUS_UART_SLAVE_WORKER_STORAGE_BYTES (3328u)
/** @brief Zero-initialized master storage, borrowed until successful destroy. */
typedef struct {
  alignas(max_align_t) uint8_t bytes[ESY_PROTO_MODBUS_UART_MASTER_WORKER_STORAGE_BYTES];
} esy_proto_modbus_uart_master_worker_storage_t;
/** @brief Zero-initialized slave storage, borrowed until successful destroy. */
typedef struct {
  alignas(max_align_t) uint8_t bytes[ESY_PROTO_MODBUS_UART_SLAVE_WORKER_STORAGE_BYTES];
} esy_proto_modbus_uart_slave_worker_storage_t;
/** @brief Owner-task completion with a worker handle; response is call-lifetime. */
typedef void (*esy_proto_modbus_uart_master_worker_complete_cb)(
    esy_proto_modbus_uart_master_worker_t *worker,
    esy_proto_modbus_req_status_t status, uint8_t unit_id,
    esy_proto_modbus_func_t function, const uint8_t *response,
    uint16_t response_length, void *user_ctx);
/** @brief Owner-task request with a worker handle and borrowed response storage. */
typedef esy_err_t (*esy_proto_modbus_uart_slave_worker_request_cb)(
    esy_proto_modbus_uart_slave_worker_t *worker, uint8_t unit_id,
    esy_proto_modbus_func_t function, const uint8_t *request, uint16_t request_length,
    uint8_t *response, uint16_t *response_length, uint16_t response_capacity,
    void *user_ctx);

/* =============================================================================
 * Master worker
 * =============================================================================
 */

/**
 * @brief Caller-owned static resources for a Modbus UART master worker.
 *
 * These resources must remain alive until the worker is destroyed.
 */
typedef struct {
  /** @brief Caller-owned priority slot storage. */
  esy_prio_slot_t *slot_storage;

  /** @brief Number of entries available in @ref slot_storage. */
  uint16_t slot_storage_count;

  /** @brief Caller-owned payload storage. */
  esy_proto_modbus_payload_t *payload_storage;

  /** @brief Number of entries available in @ref payload_storage. */
  uint16_t payload_storage_count;

  /**
   * @brief Caller-owned OS task stack buffer.
   *
   * Must be aligned to @ref ESY_OS_TASK_STACK_ALIGN.
   */
  void *task_stack_buffer;

  /** @brief Size of @ref task_stack_buffer in bytes. */
  size_t task_stack_buffer_size;

  /** @brief Caller-owned static task control storage. */
  esy_os_task_static_storage_t *task_control_storage;
} esy_proto_modbus_uart_master_worker_static_resources_t;

/**
 * @brief Declare app-owned static storage for a Modbus UART master worker.
 *
 * The declared object aggregates priority slots, protocol payloads, task
 * stack, and task control storage so application context structures can keep
 * worker SRAM ownership in one field.
 *
 * @param name Object name to declare.
 * @param slot_count Number of priority slots to allocate.
 * @param payload_count Number of protocol payload entries to allocate.
 * @param task_stack_bytes Background task stack size in bytes.
 */
#define ESY_PROTO_MODBUS_UART_MASTER_WORKER_STATIC_STORAGE(                 \
    name, slot_count, payload_count, task_stack_bytes)                 \
  struct {                                                             \
    esy_prio_slot_t slot_storage[(slot_count)];                        \
    esy_proto_modbus_payload_t payload_storage[(payload_count)];       \
    alignas(ESY_OS_TASK_STACK_ALIGN)                                   \
        uint8_t task_stack_buffer[(task_stack_bytes)];                 \
    esy_os_task_static_storage_t task_control_storage;                 \
  } name

/**
 * @brief Modbus UART master worker configuration.
 */
typedef struct {
  /**
   * @brief Frame type.
   *
   * Must be ESY_PROTO_MODBUS_FRAME_RTU; TCP framing is rejected.
   */
  esy_proto_modbus_frame_type_t frame_type;

  /**
   * @brief Initialized UART handle.
   *
   * Must be initialized externally and support the public RX stream and DMA TX.
   * This module exclusively owns stream start/stop and TX until destroy succeeds.
   */
  esy_uart_t *uart;

  /**
   * @brief Default request timeout in milliseconds.
   *
   * If 0, an internal default is used (typically 100 ms).
   */
  uint32_t timeout_ms;

  /**
   * @brief Default retry count.
   *
   * If 0, no retries are performed.
   */
  uint8_t retry_max;

  /**
   * @brief RTU idle-gap timeout in milliseconds.
   *
   * When RX is in progress and no new bytes arrive for this duration,
   * the current buffered bytes are treated as a complete RTU frame (or dropped,
   * depending on implementation policy), and passed to protocol parsing.
   *
   * Based on worker observations, not per-byte timestamps. Scheduling must not
   * hide a complete inter-frame gap. Select for the baud rate and scheduling budget.
   */
  /** @brief 0 selects 10 ms; nonzero values must be below INT32_MAX. */
  uint32_t rtu_frame_timeout_ms;

  /**
   * @brief Maximum queued request count.
   *
   * Must be > 0. The caller must provide slot/payload storage sized to this
   * depth.
   */
  uint16_t queue_depth;

  /**
   * @brief Caller-owned queue and OS resources for this worker.
   */
  esy_proto_modbus_uart_master_worker_static_resources_t static_resources;

  /**
   * @brief Optional completion callback for all requests.
   *
   * If non-NULL, called when each request finishes, including ABORTED on
   * cancellation/destruction. Shutdown callbacks cannot submit new requests.
   * Response timing and broadcast success start after physical TX completion.
   * Requests must leave on_complete and user_ctx NULL; the worker routes completion.
   */
  esy_proto_modbus_uart_master_worker_complete_cb on_complete;

  /**
   * @brief User context pointer passed to @ref on_complete.
   */
  void *user_ctx;

  /**
   * @brief OS task name (optional).
   *
   * May be NULL.
   */
  const char *task_name;

  /**
   * @brief OS task stack size in bytes.
   *
   * If 0, 1024 bytes are used. Size explicitly for application callbacks and
   * verify stack high-water marks on hardware; the default is not a callback budget.
   */
  uint32_t task_stack_size;

  /**
   * @brief OS task priority.
   */
  uint32_t task_priority;
  /** @brief Broadcast processing delay after physical TX completion, in ms.
   * Zero selects 100 ms; must be <= INT32_MAX-1. TX always waits at least the
   * configured RTU gap (zero gap selects 10 ms), plus one tick for quantization.
   * The consumer must select a gap >= t3.5 for its configured UART baud rate.
   */
  uint32_t broadcast_turnaround_ms;
} esy_proto_modbus_uart_master_worker_config_t;

/**
 * @brief Get a master worker configuration populated with default values.
 *
 * @return Default configuration object.
 */
esy_proto_modbus_uart_master_worker_config_t
esy_proto_modbus_uart_master_worker_default_cfg(void);

/**
 * @brief Create and initialize a Modbus UART master worker.
 *
 * @p cfg must provide slot and payload storage for @ref queue_depth.
 *
 * @param[in]  cfg     Configuration pointer (must not be NULL).
 * @param[in]  storage Caller-owned worker storage.
 * @param[out] out_gw  Output handle pointer (must not be NULL).
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure.
 */
esy_err_t esy_proto_modbus_uart_master_worker_create(
    const esy_proto_modbus_uart_master_worker_config_t *cfg,
    esy_proto_modbus_uart_master_worker_storage_t      *storage,
    esy_proto_modbus_uart_master_worker_t             **out_gw);

/* =============================================================================
 * Slave worker
 * =============================================================================
 */

/**
 * @brief Caller-owned static resources for a Modbus UART slave worker.
 *
 * These resources must remain alive until the worker is destroyed.
 */
typedef struct {
  /**
   * @brief Caller-owned OS task stack buffer.
   *
   * Must be aligned to @ref ESY_OS_TASK_STACK_ALIGN.
   */
  void *task_stack_buffer;

  /** @brief Size of @ref task_stack_buffer in bytes. */
  size_t task_stack_buffer_size;

  /** @brief Caller-owned static task control storage. */
  esy_os_task_static_storage_t *task_control_storage;
} esy_proto_modbus_uart_slave_worker_static_resources_t;

/**
 * @brief Declare app-owned static storage for a Modbus UART slave worker.
 *
 * The declared object aggregates task stack and task control storage so
 * application context structures can keep worker SRAM ownership in one field.
 *
 * @param name Object name to declare.
 * @param task_stack_bytes Background task stack size in bytes.
 */
#define ESY_PROTO_MODBUS_UART_SLAVE_WORKER_STATIC_STORAGE(name, task_stack_bytes) \
  struct {                                                                   \
    alignas(ESY_OS_TASK_STACK_ALIGN)                                         \
        uint8_t task_stack_buffer[(task_stack_bytes)];                       \
    esy_os_task_static_storage_t task_control_storage;                       \
  } name

/**
 * @brief Modbus UART slave worker configuration.
 */
typedef struct {
  /**
   * @brief Frame type.
   *
   * Must be @ref ESY_PROTO_MODBUS_FRAME_RTU; TCP framing is rejected.
   */
  esy_proto_modbus_frame_type_t frame_type;

  /**
   * @brief Initialized UART handle.
   *
   * Must be initialized externally; its RX stream and TX are exclusively borrowed.
   */
  esy_uart_t *uart;

  /**
   * @brief Slave address (unit id).
   *
   * Valid RTU slave addresses are typically 1..247.
   * Broadcast requests are processed without sending a response.
   */
  uint8_t unit_id;

  /**
   * @brief RTU idle-gap timeout in milliseconds.
   *
   * Determines RTU frame boundary using idle time on UART RX.
   */
  /** @brief 0 selects 10 ms; nonzero values must not exceed INT32_MAX. */
  uint32_t rtu_frame_timeout_ms;

  /**
   * @brief Mandatory request handler callback.
   *
   * Called with a request PDU (FUNC + DATA...) and expected to fill a response PDU.
   */
  esy_proto_modbus_uart_slave_worker_request_cb on_request;

  /**
   * @brief User context pointer passed through to @ref on_request.
   */
  void *user_ctx;

  /**
   * @brief OS task name (optional).
   */
  const char *task_name;

  /**
   * @brief OS task stack size in bytes.
   *
   * If 0, 1024 bytes are used; size explicitly for request callbacks and verify
   * stack high-water marks on hardware.
   */
  uint32_t task_stack_size;

  /**
   * @brief Caller-owned queue and OS resources for this worker.
   */
  esy_proto_modbus_uart_slave_worker_static_resources_t static_resources;

  /**
   * @brief OS task priority.
   */
  uint32_t task_priority;
} esy_proto_modbus_uart_slave_worker_config_t;

/**
 * @brief Get a slave worker configuration populated with default values.
 *
 * @return Default configuration object.
 */
esy_proto_modbus_uart_slave_worker_config_t
esy_proto_modbus_uart_slave_worker_default_cfg(void);

/**
 * @brief Create and initialize a Modbus UART slave worker.
 *
 * @param[in]  cfg     Configuration pointer (must not be NULL).
 * @param[in]  storage Caller-owned worker storage.
 * @param[out] out_gw  Output handle pointer (must not be NULL).
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure.
 */
esy_err_t esy_proto_modbus_uart_slave_worker_create(
    const esy_proto_modbus_uart_slave_worker_config_t *cfg,
    esy_proto_modbus_uart_slave_worker_storage_t      *storage,
    esy_proto_modbus_uart_slave_worker_t             **out_gw);

/** @brief Stop admission, drain/clean the owned channel and delete a parked worker.
 * Lifecycle is externally serialized with API callers. NULL/already released is OK;
 * calling from the worker or a callback returns STATE. BUSY/provider errors retain
 * all storage, UART and callbacks for retry. Never clear retained storage.
 */
esy_err_t esy_proto_modbus_uart_master_worker_destroy(esy_proto_modbus_uart_master_worker_t *worker);
/** @brief True after successful create until the owner completes channel cleanup. */
bool esy_proto_modbus_uart_master_worker_is_initialized(const esy_proto_modbus_uart_master_worker_t *worker);
/** @brief Copy a request to the owner command slot and wait for its exact result.
 * Requests must leave on_complete/user_ctx NULL. Owner completion callbacks may
 * submit directly. Other callers can return BUSY when the command slot is occupied
 * or when an unclaimed command expires after 100 ms; a claimed command must finish.
 */
esy_err_t esy_proto_modbus_uart_master_worker_submit(esy_proto_modbus_uart_master_worker_t *worker,
    const esy_proto_modbus_request_t *request);
/** @brief Owner command equivalent of channel abort_all; callbacks run on worker. */
esy_err_t esy_proto_modbus_uart_master_worker_abort_all(esy_proto_modbus_uart_master_worker_t *worker,
    bool abort_current);
/** @brief Read the last owner-published queue depth snapshot. */
uint16_t esy_proto_modbus_uart_master_worker_get_queue_depth(const esy_proto_modbus_uart_master_worker_t *worker);
/** @brief Read the last owner-published state snapshot, independently of depth. */
esy_proto_modbus_master_state_t esy_proto_modbus_uart_master_worker_get_state(
    const esy_proto_modbus_uart_master_worker_t *worker);
/** @brief Stop, drain and release a slave worker under the master destroy contract. */
esy_err_t esy_proto_modbus_uart_slave_worker_destroy(esy_proto_modbus_uart_slave_worker_t *worker);
/** @brief True after successful create until owner cleanup completes. */
bool esy_proto_modbus_uart_slave_worker_is_initialized(const esy_proto_modbus_uart_slave_worker_t *worker);
/** @brief Read the configured slave address; zero for NULL or an inactive worker. */
uint8_t esy_proto_modbus_uart_slave_worker_get_addr(const esy_proto_modbus_uart_slave_worker_t *worker);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_PROTO_MODBUS_UART_WORKER_H */
