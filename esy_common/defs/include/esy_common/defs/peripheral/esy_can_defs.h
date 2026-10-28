/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_CAN_DEFS_H
#define ESY_CAN_DEFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum Classical CAN payload size in bytes. */
#define ESY_CAN_CLASSIC_MAX_DATA 8u
/** @brief Maximum CAN FD payload size in bytes. */
#define ESY_CAN_FD_MAX_DATA 64u
/** @brief Largest standard (11-bit) identifier. */
#define ESY_CAN_STD_ID_MAX 0x7ffu
/** @brief Largest extended (29-bit) identifier. */
#define ESY_CAN_EXT_ID_MAX 0x1fffffffu
/** @brief Default maximum register observations per control transition. */
#define ESY_CAN_DEFAULT_POLL_LIMIT 100000u
/** @brief Largest accepted register observation budget. */
#define ESY_CAN_MAX_POLL_LIMIT 1000000u

/** @brief CAN operations implemented by a provider; ignore unknown bits. */
typedef enum {
  ESY_CAN_CAP_FD = 1u << 0, /**< CAN FD frames and data-phase timing. */
  ESY_CAN_CAP_BRS = 1u << 1, /**< CAN FD bit-rate switching. Requires FD. */
  ESY_CAN_CAP_LOOPBACK = 1u << 2, /**< Internal loopback mode. */
  ESY_CAN_CAP_LISTEN_ONLY = 1u << 3, /**< Silent bus monitoring. */
  ESY_CAN_CAP_FILTER = 1u << 4, /**< Identifier/mask acceptance filters. */
  ESY_CAN_CAP_RECOVERY = 1u << 5, /**< Explicit bus-off recovery request. */
} esy_can_cap_t;

/** @brief Controller operating mode, fixed until deinitialization. */
typedef enum {
  ESY_CAN_MODE_NORMAL = 0, /**< Normal bus participation. */
  ESY_CAN_MODE_LOOPBACK, /**< Internal transmit/receive loopback. */
  ESY_CAN_MODE_LISTEN_ONLY, /**< Receive without transmitting or acknowledging. */
} esy_can_mode_t;

/** @brief Observable controller state, sampled by get_status. */
typedef enum {
  ESY_CAN_STATE_STOPPED = 0, /**< Initialized but not participating on the bus. */
  ESY_CAN_STATE_ERROR_ACTIVE, /**< Running below the error-passive threshold. */
  ESY_CAN_STATE_ERROR_PASSIVE, /**< Running with error-passive restrictions. */
  ESY_CAN_STATE_BUS_OFF, /**< Bus-off; an explicit recovery request is needed. */
  ESY_CAN_STATE_RECOVERING, /**< Recovery requested; bus idle sequence pending. */
  ESY_CAN_STATE_FAULT, /**< Control transition failed; stop/deinit must be retried. */
} esy_can_state_t;

/**
 * @brief Bit timing in peripheral input-clock quanta; no implicit bitrate choice.
 * Bitrate = input_clock_hz / (prescaler * (1 + segment1 + segment2)).
 * Consult the provider limits and actual board clock; SJW must not exceed segment2.
 */
typedef struct {
  uint16_t prescaler; /**< Input-clock division factor, nonzero. */
  uint8_t segment1; /**< Time quanta before sample point, excluding sync quantum. */
  uint8_t segment2; /**< Time quanta after sample point. */
  uint8_t sjw; /**< Resynchronization jump width in quanta. */
} esy_can_timing_t;

/**
 * @brief Owned frame value; length is bytes, not encoded DLC.
 * Classical length is 0..8. FD length is 0..8,12,16,20,24,32,48,64.
 * FD forbids RTR; BRS/ESI require FD. RTR length expresses requested bytes;
 * its data array is ignored. TX buffers are copied before send returns.
 */
typedef struct {
  uint32_t id; /**< Unshifted identifier, limited by extended. */
  uint8_t length; /**< Payload/request size in bytes. */
  bool extended; /**< True selects a 29-bit identifier. */
  bool rtr; /**< Classical remote transmission request. */
  bool fd; /**< FD frame; requires configured FD support. */
  bool brs; /**< Switch to configured data-phase timing. */
  bool esi; /**< Received FD error-state indicator; must be false on transmit. */
  uint8_t data[ESY_CAN_FD_MAX_DATA]; /**< Inline payload storage. */
} esy_can_frame_t;

/**
 * @brief One acceptance rule: (received_id & mask) == (id & mask).
 * Rules always match the identifier format. Set mask=0 for all IDs of that
 * format. All rules feed the provider's owner-polled receive FIFO.
 */
typedef struct {
  uint32_t id; /**< Unshifted reference ID. */
  uint32_t mask; /**< Significant identifier bits. */
  bool extended; /**< Required identifier format. */
  bool match_rtr; /**< Also compare the RTR bit when true. */
  bool rtr; /**< Required RTR value when match_rtr is true. */
} esy_can_filter_t;

/**
 * @brief Configuration copied at init; controller remains stopped afterward.
 * The consumer prepares pins, peripheral input clock and external transceiver
 * before init and retains them until successful deinit. No board hook or pin
 * mapping is inferred. poll_limit bounds each register handshake by observation
 * count, not milliseconds; a timeout faults the handle and requires stop/deinit.
 */
typedef struct {
  uint8_t phys_index; /**< Provider controller number; STM32F4 uses 1 or 2. */
  esy_can_mode_t mode; /**< Operating mode. */
  esy_can_timing_t nominal; /**< Arbitration/Classical timing. */
  esy_can_timing_t data; /**< Data-phase timing, used only when fd is true. */
  bool fd; /**< Request FD; unsupported providers reject before hardware access. */
  bool auto_retransmit; /**< Retry arbitration/transmission errors in hardware. */
  uint32_t poll_limit; /**< 1..ESY_CAN_MAX_POLL_LIMIT observations per handshake. */
} esy_can_config_t;

/**
 * @brief Diagnostic snapshot; separate hardware register reads are not atomic.
 * No status query clears sticky diagnostics. Deinit/init clears provider state.
 */
typedef struct {
  esy_can_state_t state; /**< Current state, including explicit recovery progress. */
  uint16_t tx_error_count; /**< Native transmit error counter. */
  uint16_t rx_error_count; /**< Native receive error counter. */
  uint8_t last_error; /**< Provider error code; STM32F4 uses raw ESR LEC (0..7). */
  bool warning; /**< Native error-warning threshold reached. */
  bool rx_overrun; /**< Sticky receive overrun since init. */
  uint8_t tx_pending; /**< Outstanding hardware transmit requests. */
  uint8_t rx_pending; /**< Frames currently available in the receive FIFO. */
} esy_can_status_t;

/** @brief Forward declaration for caller-owned CAN handles. */
typedef struct esy_can_s esy_can_t;

/**
 * @brief Immutable operations table; all calls share one serialized non-ISR owner.
 * See esy_hal_can.h for validation, lifecycle and data-operation contracts.
 */
typedef struct {
  esy_err_t (*start)(esy_can_t *can); /**< Start a stopped controller. */
  esy_err_t (*stop)(esy_can_t *can); /**< Abort TX and quiesce the controller. */
  esy_err_t (*send)(esy_can_t *can, const esy_can_frame_t *frame); /**< Enqueue one frame. */
  esy_err_t (*receive)(esy_can_t *can, esy_can_frame_t *frame); /**< Copy one received frame. */
  esy_err_t (*set_filters)(esy_can_t *can, const esy_can_filter_t *filters,
                           size_t count); /**< Replace rules while stopped. */
  esy_err_t (*get_status)(esy_can_t *can, esy_can_status_t *status); /**< Sample diagnostics. */
  esy_err_t (*recover)(esy_can_t *can); /**< Request bus-off recovery. */
  esy_err_t (*get_caps)(const esy_can_t *can, uint32_t *caps); /**< Query public capabilities. */
} esy_can_vtable_t;

/**
 * @brief Noncopyable caller-owned handle, initialized with {0} before first use.
 * Retain storage until deinit succeeds; never clear a live or faulted handle.
 * The HAL owns ops and the backend owns ctx. Both pointers are implementation
 * details; custom providers may install an immutable table covering handle life.
 */
struct esy_can_s {
  const esy_can_vtable_t *ops; /**< Immutable operation dispatch. */
  void *ctx; /**< Private provider state. */
};

#ifdef __cplusplus
}
#endif
#endif /* ESY_CAN_DEFS_H */
