/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_proto_modbus_manager.h
 * @par Lifecycle and concurrency
 * Zero-initialize handles and configurations before first use. Serialize all
 * operations on one owner task, never an ISR. Init rejects active handles.
 * Deinit returns ESY_ERR_BUSY without clearing resources while TX is borrowed;
 * stop or complete physical TX before retrying. Successful deinit is
 * idempotent. Completion callbacks may submit or abort requests, but must not
 * tick, inject RX, init or deinit recursively. Completion state is released
 * before callback. Abort does not cancel physical TX; its workspace stays
 * borrowed until the busy hook returns false. Slave requests arriving while
 * busy are discarded. Timeouts must be <= INT32_MAX; zero selects the
 * configured/default timeout. RTU unit-zero broadcasts complete after TX
 * acceptance without response/retry. A successful broadcast callback does not
 * itself release an async TX borrow. With defer_tx_completion enabled, broadcast
 * success and response timing wait for explicit terminal TX reporting.
 * Abort notifies every request present on entry with ABORTED. Submissions from
 * abort callbacks survive; nested abort returns BUSY. Deinit notifies remaining
 * requests with ABORTED and rejects callback submissions with STATE.
 * @brief Frame-oriented Modbus master/slave protocol manager (Scheme C opaque
 * handle).
 *
 * ## Public abstraction: frame (ADU)
 * This module uniformly exposes **frame** as the external data unit,
 * corresponding to the Modbus Application Data Unit (ADU):
 * - RTU frame (ADU) = [ADDR][PDU][CRC16]
 * - TCP frame (ADU) = [MBAP][PDU]
 *
 * The PDU always starts from the function code:
 *   [FUNC][DATA...]
 *
 * ## Design goals
 * ### Master
 * - The sole Modbus request/response transaction owner: response matching,
 *   response deadlines and retries remain here, never in an I/O adapter.
 * - Priority-based request scheduling.
 * - Built-in timeout and retry handling.
 * - No OS dependency and no dynamic allocation.
 * - Upper layer provides slots/payloads storage and tx frame working buffer.
 * - TX uses `tx_fn` to send a complete frame (ADU); asynchronous senders must
 *   supply is_tx_busy to retain the borrowed TX workspace.
 * - RX uses `on_rx_frame` and expects a complete frame (ADU).
 * - All timing behavior is driven by periodic `tick()` calls.
 * - Timing uses the uint32_t millisecond HAL system tick (wraps after 2^32 ms).
 *   Effective response waits are 1..INT32_MAX ms. The owner must call tick at
 *   intervals below 2^31 ms while waiting, including physical-TX deferral, and
 *   must not reset the timebase during an active request. A stopped HAL tick
 *   freezes the response wait; quiesce/abort before low power or a Flash pause
 *   when the selected port cannot preserve these limits. Each accepted retry
 *   starts a fresh response wait after the configured TX-completion boundary.
 *   Response completion and abort permanently disarm that request; only a new
 *   accepted submission can start another wait.
 *
 * ### Slave
 * - Passive request handling and response generation.
 * - No OS dependency and no dynamic allocation.
 * - Upper layer provides response PDU working buffer and tx frame working
 * buffer.
 * - TX uses `tx_fn` to send a complete frame (ADU); asynchronous senders must
 *   supply is_tx_busy to retain the borrowed TX workspace.
 * - RX uses `on_rx_frame` and expects a complete frame (ADU).
 *
 * ## Non-responsibilities
 * - No transport-level framing (no stream reassembly or segmentation).
 * - No UART/TCP socket handling.
 * - No physical-TX timeout, abort or RTU inter-frame/broadcast-gap policy; the
 *   I/O owner preserves the TX borrow and supplies bounded recovery separately.
 * - No OS timers or threads.
 *
 * ## Handle model (Scheme C)
 * This module uses a caller-provided, fixed-size opaque handle storage:
 * - `esy_proto_modbus_master_t` / `esy_proto_modbus_slave_t` contain only
 * aligned byte buffers.
 * - Real implementation contexts live in `.c` and are placed into these
 * buffers.
 * - `.c` must validate the buffer size using `_Static_assert`.
 */

#ifndef ESY_PROTO_MODBUS_MANAGER_H
#define ESY_PROTO_MODBUS_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h> /* max_align_t */

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/protocol/esy_proto_modbus.h"
#include "esy_common/ds/esy_prio_slot_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Basic limits                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief Maximum PDU length (excluding address, MBAP, and CRC).
 *
 * Default is the codec library limit of 252 bytes; the protocol permits 253.
 * Overrides must be in 1..ESY_PROTO_MODBUS_MAX_PDU_LEN and identical when
 * compiling the manager and its consumers. The codec limit still governs
 * complete received frames and encoded slave responses.
 */
#ifndef ESY_PROTO_MODBUS_MANAGER_MAX_PDU_LEN
#define ESY_PROTO_MODBUS_MANAGER_MAX_PDU_LEN ESY_PROTO_MODBUS_MAX_PDU_LEN
#endif

#if ESY_PROTO_MODBUS_MANAGER_MAX_PDU_LEN < 1 || \
    ESY_PROTO_MODBUS_MANAGER_MAX_PDU_LEN > ESY_PROTO_MODBUS_MAX_PDU_LEN
#error "ESY_PROTO_MODBUS_MANAGER_MAX_PDU_LEN must be in 1..ESY_PROTO_MODBUS_MAX_PDU_LEN"
#endif

/* -------------------------------------------------------------------------- */
/* Opaque handle storage (Scheme C)                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief Opaque storage size (bytes) for `esy_proto_modbus_master_t`.
 *
 * You may override this macro at build time if needed.
 *
 * @note The `.c` implementation must validate the size:
 * @code
 * _Static_assert(ESY_PROTO_MODBUS_MASTER_PRIV_BYTES >= sizeof(esy_proto_modbus_master_ctx_t),
 *                "ESY_PROTO_MODBUS_MASTER_PRIV_BYTES too small");
 * @endcode
 */
#ifndef ESY_PROTO_MODBUS_MASTER_PRIV_BYTES
#define ESY_PROTO_MODBUS_MASTER_PRIV_BYTES 512u
#endif

/**
 * @brief Opaque storage size (bytes) for `esy_proto_modbus_slave_t`.
 *
 * You may override this macro at build time if needed.
 *
 * @note The `.c` implementation must validate the size:
 * @code
 * _Static_assert(ESY_PROTO_MODBUS_SLAVE_PRIV_BYTES >= sizeof(esy_proto_modbus_slave_ctx_t),
 *                "ESY_PROTO_MODBUS_SLAVE_PRIV_BYTES too small");
 * @endcode
 */
#ifndef ESY_PROTO_MODBUS_SLAVE_PRIV_BYTES
#define ESY_PROTO_MODBUS_SLAVE_PRIV_BYTES 256u
#endif

/**
 * @brief Modbus master opaque handle (caller-provided storage).
 *
 * The caller owns the storage. Do not access internal bytes directly.
 */
typedef struct esy_proto_modbus_master_s {
  union {
    max_align_t _align; /**< Ensures maximum alignment. */
    uint8_t     bytes[ESY_PROTO_MODBUS_MASTER_PRIV_BYTES]; /**< Opaque private storage. */
  } priv;
} esy_proto_modbus_master_t;

/**
 * @brief Modbus slave opaque handle (caller-provided storage).
 *
 * The caller owns the storage. Do not access internal bytes directly.
 */
typedef struct esy_proto_modbus_slave_s {
  union {
    max_align_t _align; /**< Ensures maximum alignment. */
    uint8_t     bytes[ESY_PROTO_MODBUS_SLAVE_PRIV_BYTES]; /**< Opaque private storage. */
  } priv;
} esy_proto_modbus_slave_t;

/* -------------------------------------------------------------------------- */
/* Common enums                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief Request priority levels for Modbus master scheduling.
 */
typedef enum {
  ESY_PROTO_MODBUS_PRIORITY_LOW    = 0,
  ESY_PROTO_MODBUS_PRIORITY_MEDIUM = 1,
  ESY_PROTO_MODBUS_PRIORITY_HIGH   = 2,
} esy_proto_modbus_priority_t;

/**
 * @brief Modbus function code enumeration (PDU FUNC field).
 *
 * Notes:
 * - Numeric values match standard Modbus function codes.
 * - Unknown function codes may be cast to this enum.
 * - For exception responses (FUNC | 0x80), use (raw & 0x7F) to obtain base code.
 */
typedef enum {
  ESY_PROTO_MODBUS_FUNC_UNKNOWN                  = 0x00,

  ESY_PROTO_MODBUS_FUNC_READ_COILS               = 0x01,
  ESY_PROTO_MODBUS_FUNC_READ_DISCRETE_INPUTS     = 0x02,
  ESY_PROTO_MODBUS_FUNC_READ_HOLDING_REGISTERS   = 0x03,
  ESY_PROTO_MODBUS_FUNC_READ_INPUT_REGISTERS     = 0x04,
  ESY_PROTO_MODBUS_FUNC_WRITE_SINGLE_COIL        = 0x05,
  ESY_PROTO_MODBUS_FUNC_WRITE_SINGLE_REGISTER    = 0x06,
  ESY_PROTO_MODBUS_FUNC_WRITE_MULTIPLE_COILS     = 0x0F,
  ESY_PROTO_MODBUS_FUNC_WRITE_MULTIPLE_REGISTERS = 0x10,
  ESY_PROTO_MODBUS_FUNC_READ_WRITE_MULTIPLE_REGS = 0x17,
} esy_proto_modbus_func_t;

/**
 * @brief Frame transmission function type (shared by master and slave).
 *
 * The function must send a **complete Modbus frame (ADU)**:
 * - RTU: [ADDR][PDU][CRC]
 * - TCP: [MBAP][PDU]
 *
 * @param[in] frame      Pointer to frame bytes.
 * @param[in] frame_len  Frame length in bytes.
 * @param[in] user_ctx   User-defined context pointer.
 *
 * @retval ESY_OK        Transmission accepted.
 * @retval <0            Transport-level error.
 */
typedef esy_err_t (*esy_proto_modbus_tx_frame_fn)(
    const uint8_t *frame,
    uint16_t       frame_len,
    void          *user_ctx);

/* -------------------------------------------------------------------------- */
/* Master types                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief Modbus master internal state (public enum, internal state machine).
 */
typedef enum {
  ESY_PROTO_MODBUS_MASTER_STATE_IDLE = 0,  /**< No active transaction. */
  ESY_PROTO_MODBUS_MASTER_STATE_WAIT_RESP, /**< Waiting for response, or explicit
                                               TX completion when opted in. */
} esy_proto_modbus_master_state_t;

/**
 * @brief Completion status of an accepted Modbus master request.
 */
typedef enum {
  ESY_PROTO_MODBUS_REQ_STATUS_OK = 0,          /**< Normal response received. */
  ESY_PROTO_MODBUS_REQ_STATUS_TIMEOUT,         /**< Response timeout. */
  ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR, /**< Transport send failed. */
  ESY_PROTO_MODBUS_REQ_STATUS_BAD_FRAME, /**< Frame parse or match error. */
  ESY_PROTO_MODBUS_REQ_STATUS_ABORTED,   /**< Request was aborted. */
  ESY_PROTO_MODBUS_REQ_STATUS_EXCEPTION,  /**< Modbus exception response. */
} esy_proto_modbus_req_status_t;

/**
 * @brief Master request completion callback.
 *
 * @note The response PDU pointer refers to internal frame storage and
 *       becomes invalid after the callback returns.
 *
 * @param[in] master        Master handle.
 * @param[in] status        Request result.
 * @param[in] unit_id       RTU: slave address / TCP: unit identifier.
 * @param[in] func_code     Base function code (recommended: raw & 0x7F).
 * @param[in] resp_pdu      Pointer to response PDU (FUNC + DATA).
 * @param[in] resp_pdu_len  Length of response PDU in bytes.
 * @param[in] user_ctx      Per-request user context.
 */
typedef void (*esy_proto_modbus_request_complete_cb)(
    esy_proto_modbus_master_t     *master,
    esy_proto_modbus_req_status_t  status,
    uint8_t                        unit_id,
    esy_proto_modbus_func_t        func_code,
    const uint8_t                 *resp_pdu,
    uint16_t                       resp_pdu_len,
    void                          *user_ctx);

/**
 * @brief Master request descriptor (submission-time object).
 *
 * - PDU starts from function code: [FUNC][DATA...]
 * - timeout_ms / retry_max set to 0 means using master default values.
 */
typedef struct {
  uint8_t                     unit_id;
  esy_proto_modbus_func_t      func_code;

  uint16_t                    pdu_len;
  uint8_t                     pdu[ESY_PROTO_MODBUS_MANAGER_MAX_PDU_LEN];

  esy_proto_modbus_priority_t  priority;

  uint32_t                    timeout_ms; /**< 0 = use master default */
  uint8_t                     retry_max;  /**< 0 = use master default */

  esy_proto_modbus_request_complete_cb on_complete; /**< Optional per-request callback */
  void                      *user_ctx;
} esy_proto_modbus_request_t;

/**
 * @brief Master payload storage (one-to-one with slot index).
 *
 * The priority queue manages slot metadata only.
 * Actual request data is stored in this payload array,
 * indexed by slot ID.
 */
typedef struct {
  uint8_t                     unit_id;
  esy_proto_modbus_func_t      func_code;

  uint16_t                    pdu_len;
  uint8_t                     pdu[ESY_PROTO_MODBUS_MANAGER_MAX_PDU_LEN];

  uint8_t                     priority;

  uint32_t                    timeout_ms;
  uint8_t                     retry_max;

  esy_proto_modbus_request_complete_cb on_complete;
  void                      *user_ctx;
} esy_proto_modbus_payload_t;

/**
 * @brief Modbus master configuration.
 *
 * The caller must provide all external storage and working buffers.
 */
typedef struct {
  esy_proto_modbus_frame_type_t frame_type;

  /** Optional asynchronous borrow guard. NULL requires tx_fn to copy/finish
   * before returning. True retains tx_frame_buf; false releases it. Uses
   * tx_user_ctx. Failure from tx_fn must not retain the buffer. */
  bool (*is_tx_busy)(void* user_ctx);
  esy_proto_modbus_tx_frame_fn  tx_fn;
  void                         *tx_user_ctx;

  uint32_t                      timeout_ms; /**< Default timeout (0 → internal default) */
  uint8_t                       retry_max;  /**< Default retry count (0 → no retry) */

  /** @brief Working buffer for constructing complete frames (ADU). */
  uint8_t                      *tx_frame_buf;
  uint16_t                      tx_frame_buf_size;

  /**
   * @brief External queue storage (slot metadata) and payload storage.
   *
   * `slots` and `payloads` must have the same element count (`slot_count`).
   */
  esy_prio_slot_t              *slots;
  esy_proto_modbus_payload_t   *payloads;
  uint16_t                      slot_count;  /**< Must be > 0 */
  /** @brief Select asynchronous terminal reporting; false uses synchronous TX.
   * When true, call on_tx_complete exactly once after each accepted TX, after
   * tx_fn returns and the borrow ends. Response timing starts at successful TX
   * completion; broadcasts also wait for completion. Requires is_tx_busy to
   * protect old transfers even after abort. No response is accepted meanwhile. */
  bool defer_tx_completion;
} esy_proto_modbus_master_config_t;

/**
 * @brief Get a master configuration populated with recommended defaults.
 *
 * @note Pointers are set to NULL; caller must fill storage/buffers.
 */
esy_proto_modbus_master_config_t esy_proto_modbus_master_default_cfg(void);

/* Master API */

/**
 * @brief Initialize the Modbus master.
 *
 * @param[in,out] master  Master handle storage (caller-provided).
 * @param[in]     config  Configuration.
 *
 * @retval ESY_OK              Success.
 * @retval ESY_ERR_INVALID_ARG Invalid arguments.
 * @retval ESY_ERR_FAIL        Initialization failure (e.g., queue init).
 */
esy_err_t esy_proto_modbus_master_init(
    esy_proto_modbus_master_t              *master,
    const esy_proto_modbus_master_config_t *config);

/**
 * @return ESY_OK, or ESY_ERR_BUSY retaining ownership while asynchronous TX is
 * active.
 * @brief Deinitialize the Modbus master and clear internal state.
 */
esy_err_t esy_proto_modbus_master_deinit(esy_proto_modbus_master_t* master);

/**
 * @brief Check whether the master has been initialized.
 */
bool esy_proto_modbus_master_is_initialized(const esy_proto_modbus_master_t *master);

/**
 * @brief Get current master state.
 */
esy_proto_modbus_master_state_t esy_proto_modbus_master_get_state(
    const esy_proto_modbus_master_t *master);

/**
 * @brief Get current queued request count.
 */
uint16_t esy_proto_modbus_master_get_queue_depth(const esy_proto_modbus_master_t *master);

/**
 * @brief Submit a Modbus request (copied into payload storage and queued).
 *
 * @retval ESY_OK                     Success.
 * @retval ESY_ERR_INVALID_ARG        Invalid arguments.
 * @retval ESY_ERR_NO_MEM             Queue full / no free slots.
 * @retval ESY_ERR_STATE              Submission during deinit completion.
 * @note An uninitialized handle returns ESY_ERR_INVALID_ARG.
 *       Rejected submissions do not invoke the completion callback.
 */
esy_err_t esy_proto_modbus_master_submit(
    esy_proto_modbus_master_t        *master,
    const esy_proto_modbus_request_t *req);

/**
 * @brief Feed a received complete Modbus frame (ADU) into the master.
 *
 * @note This function does NOT perform stream reassembly.
 *       The caller must guarantee that the frame is complete.
 */
void esy_proto_modbus_master_on_rx_frame(
    esy_proto_modbus_master_t *master,
    const uint8_t             *frame,
    uint16_t                   frame_len);

/** @brief Report terminal asynchronous TX from the owner context, never ISR.
 * Valid only with defer_tx_completion. result is ESY_OK for wire completion or
 * a transport error. An aborted transfer's late completion is ignored; the
 * busy hook must prevent starting another transfer before this call.
 * Returns INVALID_ARG for NULL, STATE if inactive/not opted in, otherwise OK.
 */
esy_err_t esy_proto_modbus_master_on_tx_complete(
    esy_proto_modbus_master_t *master, esy_err_t result);

/**
 * @brief Periodic tick function.
 *
 * Drives transaction initiation, timeout detection, retries,
 * and scheduling of the next request.
 *
 * Internally uses esy_hal_system_get_tick().
 */
void esy_proto_modbus_master_tick(esy_proto_modbus_master_t *master);

/**
 * @brief Abort all queued transactions (optionally abort the current in-flight one).
 *
 * @param[in] abort_current  Whether to abort the in-flight transaction.
 *
 * @retval ESY_OK       Success.
 * @retval <0           Failure.
 */
esy_err_t esy_proto_modbus_master_abort_all(
    esy_proto_modbus_master_t *master,
    bool                       abort_current);

/* -------------------------------------------------------------------------- */
/* Slave types                                                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief Slave request handler callback.
 *
 * Input: request PDU (FUNC + DATA...).
 * Output: response PDU written into resp_pdu_buf.
 *
 * @retval ESY_OK  Response should be sent (non-broadcast).
 * @retval other   No response is sent.
 */
typedef esy_err_t (*esy_proto_modbus_slave_request_cb)(
    esy_proto_modbus_slave_t *slave,
    uint8_t                   unit_id,
    esy_proto_modbus_func_t   func_code,
    const uint8_t            *req_pdu,
    uint16_t                  req_pdu_len,
    uint8_t                  *resp_pdu_buf,
    uint16_t                 *resp_pdu_len,
    uint16_t                  resp_pdu_buf_max,
    void                     *user_ctx);

/**
 * @brief Modbus slave configuration.
 */
typedef struct {
  esy_proto_modbus_frame_type_t  frame_type;

  /**
   * @brief Slave unit identifier.
   *
   * RTU: slave address.
   * TCP: Unit Identifier.
   * 0 may be treated as broadcast (implementation-defined).
   */
  uint8_t                        unit_id;

  /** Optional asynchronous borrow guard. NULL requires tx_fn to copy/finish
   * before returning. True retains tx_frame_buf; false releases it. Uses
   * tx_user_ctx. Failure from tx_fn must not retain the buffer. */
  bool (*is_tx_busy)(void* user_ctx);
  esy_proto_modbus_tx_frame_fn    tx_fn;
  void                           *tx_user_ctx;

  /** @brief Response PDU working buffer (provided by upper layer). */
  uint8_t                        *resp_pdu_buf;
  uint16_t                        resp_pdu_buf_size;

  esy_proto_modbus_slave_request_cb on_request;
  void                           *user_ctx;
} esy_proto_modbus_slave_config_t;

/* Slave API */

/**
 * @brief Initialize the Modbus slave.
 *
 * @param[in,out] slave           Slave handle storage (caller-provided).
 * @param[in]     cfg             Configuration.
 * @param[in]     tx_frame_buf    Working buffer for building response frames (ADU).
 * @param[in]     tx_frame_buf_size Buffer size in bytes.
 *
 * @retval ESY_OK              Success.
 * @retval ESY_ERR_INVALID_ARG Invalid arguments.
 * @retval ESY_ERR_FAIL        Initialization failure.
 */
esy_err_t esy_proto_modbus_slave_init(
    esy_proto_modbus_slave_t              *slave,
    const esy_proto_modbus_slave_config_t *cfg,
    uint8_t                               *tx_frame_buf,
    uint16_t                               tx_frame_buf_size);

/**
 * @return ESY_OK, or ESY_ERR_BUSY retaining ownership while asynchronous TX is
 * active.
 * @brief Deinitialize the Modbus slave and clear internal state.
 */
esy_err_t esy_proto_modbus_slave_deinit(esy_proto_modbus_slave_t* slave);

/**
 * @brief Check whether the slave has been initialized.
 */
bool esy_proto_modbus_slave_is_initialized(const esy_proto_modbus_slave_t *slave);

/**
 * @brief Feed a received complete Modbus frame (ADU) into the slave.
 *
 * @note This function does NOT perform stream reassembly.
 *       The caller must guarantee that the frame is complete.
 */
void esy_proto_modbus_slave_on_rx_frame(
    esy_proto_modbus_slave_t *slave,
    const uint8_t            *frame,
    uint16_t                  frame_len);

/**
 * @brief Get the configured slave unit identifier (address).
 *
 * RTU: slave address (ADDR).
 * TCP: Unit Identifier (MBAP UID).
 *
 * @param[in] slave Slave handle.
 *
 * @return Unit identifier. Returns 0 if `slave` is NULL or not initialized.
 */
uint8_t esy_proto_modbus_slave_get_unit_id(const esy_proto_modbus_slave_t *slave);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_PROTO_MODBUS_MANAGER_H */
