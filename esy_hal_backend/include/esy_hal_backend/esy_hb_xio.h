/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HB_XIO_H
#define ESY_HB_XIO_H

/**
 * @file esy_hb_xio.h
 * @brief Generic XIO engine (manual instance/channel assignment only; no dynamic allocation).
 *
 * Constraints:
 * - The concrete backend is selected at build time (e.g. via CMake source selection).
 *   This header does NOT expose any backend enumeration or runtime query APIs.
 * - Handle types are concrete structs (NOT opaque pointers). The caller must provide
 *   storage and call init/deinit. Implementations MUST NOT use dynamic allocation
 *   (malloc/new). All runtime state must live in the embedded backend-private storage.
 * - Initialize program and channel objects with {0} before their first use.
 *   Serialize lifecycle/configuration with all users and stop hardware/callbacks
 *   before release/deinit. Never clear an owned live object to bypass cleanup.
 * - Pico SMs participate in native SDK claims. Built-in Wi-Fi lazy allocation
 *   shares a nonblocking assembly reservation; contention returns BUSY for retry.
 *   Serialize external raw SDK allocation with XIO claims: fixed-ID SDK claims
 *   can panic on a check/claim race. Do not unclaim an XIO-owned SM through SDK.
 * - This revision supports **manual allocation only**: the upper layer must explicitly
 *   provide the hardware "instance index" and the "channel/SM index".
 *
 * Return values:
 * - All APIs use @ref esy_err_t (see esy_common_defs.h):
 *   ESY_OK / ESY_ERR_INVALID_ARG / ESY_ERR_NO_MEM / ESY_ERR_BUSY /
 *   ESY_ERR_NOT_SUPPORTED / ESY_ERR_NOT_IMPLEMENTED / ESY_ERR_STATE / ...
 */

#include <stdint.h>
#include <stddef.h> /* max_align_t */
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Constants / Definitions                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @def ESY_XIO_INDEX_INVALID
 * @brief Invalid instance/channel index sentinel used by manual-allocation APIs.
 */
#define ESY_XIO_INDEX_INVALID 0xFFu

/**
 * @brief Event flags (bitwise OR).
 *
 * Implementations may generate one or more of these flags when notifying the
 * registered callback.
 */
enum {
  ESY_XIO_EVT_TX_EMPTY = (1u << 0), /**< TX FIFO is empty / ready for more data */
  ESY_XIO_EVT_RX_READY = (1u << 1), /**< RX FIFO has data available */
  ESY_XIO_EVT_DONE     = (1u << 2), /**< One transfer/task has completed */
  ESY_XIO_EVT_ERROR    = (1u << 31) /**< Error occurred (details are backend-defined) */
};

/**
 * @brief Event callback prototype.
 *
 * @param[in] user  User context pointer provided at registration time.
 * @param[in] flags Event flags (see ESY_XIO_EVT_* above).
 *
 * @note The callback may be invoked from interrupt context. Keep it short and
 *       non-blocking; defer heavy work to a task/thread.
 */
typedef void (*esy_hb_xio_event_cb_t)(void* user, uint32_t flags);

/**
 * @brief Backend-private storage size (bytes). Can be overridden at compile time.
 *
 * - If a backend needs more space, override these macros globally at compile
 *   time (e.g. -DESY_XIO_PROG_PRIV_BYTES=...). Every translation unit using XIO
 *   objects, including HAL/backend and consumers, must use identical values.
 *   Rebuild all of them after a change; these sizes affect public object layout.
 * - Embedded UART/SPI handle storage must also fit the resulting XIO objects;
 *   their backend compile-time assertions reject insufficient storage.
 * - Must preserve the "no dynamic allocation" rule: all runtime state must fit here.
 */
#ifndef ESY_XIO_PROG_PRIV_BYTES
#define ESY_XIO_PROG_PRIV_BYTES 64u
#endif
#ifndef ESY_XIO_SM_PRIV_BYTES
#define ESY_XIO_SM_PRIV_BYTES   64u
#endif

/* -------------------------------------------------------------------------- */
/* Program object & descriptor (caller-provided storage)                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief Program descriptor (input parameters used to initialize a program object).
 */
typedef struct {
  /**
   * @brief Instruction array (e.g. PIO 16-bit instructions; other backends may ignore).
   *
   * If @ref native_prog is provided, this field may be NULL. Whether it is used
   * is backend-defined.
   */
  const uint16_t* instructions;

  uint32_t length;  /**< Number of instructions */
  int32_t  origin;  /**< Desired load origin; -1 means auto/ignore */
  const char* name; /**< Debug name (optional; may be NULL) */

  /**
   * @brief Backend-native program pointer (e.g. RP2040 can pass `pio_program*`).
   * Set to NULL if not available.
   */
  const void* native_prog;
} esy_hb_xio_program_desc_t;

/**
 * @brief Program object; caller initializes storage with {0} before first init.
 */
typedef struct esy_hb_xio_program_s {
  esy_hb_xio_program_desc_t desc; /**< Cached (copied) descriptor used at init time */

  /** Backend private runtime state (e.g. load offset / refcount / owning instance). */
  union {
    max_align_t _a; /**< Alignment guarantee */
    uint8_t bytes[ESY_XIO_PROG_PRIV_BYTES];
  } backend;

  uint8_t initialized; /**< 0 = not initialized, 1 = initialized */
  uint8_t _rsvd[3];
} esy_hb_xio_program_t;

/* -------------------------------------------------------------------------- */
/* Channel (state machine) object & configuration (caller-provided storage)    */
/* -------------------------------------------------------------------------- */

/**
 * @brief Channel (state machine) initialization configuration (kept generic on purpose).
 */
typedef struct {
  /** @brief Clock ratio: effective divider is clk_div_div / clk_div_mul.
   * Each zero field independently means one. Pico accepts the exact ratio only
   * in [1, 65536], then rounds down to the 16:8 divider's 1/256 resolution.
   * Exactly 65536 is supported using the native zero integer encoding. Ratios
   * outside the range return INVALID_ARG before any SM/pin mutation; the
   * existing claim remains owned and may be reconfigured or released. */
  uint32_t clk_div_mul; /**< Ratio denominator; zero defaults to one. */
  uint32_t clk_div_div; /**< Ratio numerator; zero defaults to one. */

  /**
   * @name Pin mapping (contiguous base + width).
   * Unused mapping can be expressed by base=ESY_PIN_UNUSED or count=0.
   * Pico mappings must fit entirely within GPIO 0..31 and the GPIOs present
   * on the selected chip. OUT/IN widths are at most 32; SET/side-set widths
   * are at most five. Invalid mappings are rejected before hardware changes.
   */
  /** @{ */
  uint8_t out_pin_base;
  uint8_t out_pin_count;
  uint8_t in_pin_base;
  uint8_t in_pin_count;
  uint8_t set_pin_base;
  uint8_t set_pin_count;
  uint8_t sideset_pin_base;
  uint8_t sideset_pin_count; /**< Physical side-set pins, excluding the optional enable bit. */
  /** @} */

  uint8_t sideset_optional; /**< 0: 0..5 physical pins; 1: 1..4 physical pins plus an enable bit. */
  uint8_t jmp_pin;          /**< Jump pin; set ESY_PIN_UNUSED if not used */

  /* Shifts and FIFO behavior (common for streaming-style programs). */
  uint8_t out_shift_right; /**< 1 = shift right (LSB-first), 0 = shift left (MSB-first) */
  uint8_t out_autopull;    /**< 1 = autopull from TX FIFO */
  uint8_t out_pull_thresh; /**< 1..32, 0 = backend default */
  uint8_t in_shift_right;  /**< Input shift direction */
  uint8_t in_autopush;     /**< 1 = autopush to RX FIFO */
  uint8_t in_push_thresh;  /**< 1..32, 0 = backend default */

  uint8_t enable_on_init;  /**< Enable immediately after init if non-zero */
} esy_hb_xio_sm_config_t;

/**
 * @brief Channel object; caller initializes storage with {0} before first claim.
 */
typedef struct esy_hb_xio_sm_s {
  esy_hb_xio_program_t* program; /**< Associated program object (lifetime managed by caller) */

  esy_hb_xio_event_cb_t cb; /**< Event callback */
  void* user;                   /**< Callback user context */

  /** Backend private runtime state (e.g. instance index, SM id, entry offset). */
  union {
    max_align_t _a; /**< Alignment guarantee */
    uint8_t bytes[ESY_XIO_SM_PRIV_BYTES];
  } backend;

  uint8_t claimed; /**< 0 = not claimed, 1 = claimed (hardware channel reserved) */
  uint8_t started; /**< 0 = not started, 1 = started */
  uint8_t _rsvd[2];
} esy_hb_xio_sm_t;

/* -------------------------------------------------------------------------- */
/* Program lifecycle (no dynamic allocation)                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialize a program object (no dynamic allocation).
 *
 * @param[in,out] prog Program object (caller-provided storage).
 * @param[in]     desc Program descriptor.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code (e.g. ESY_ERR_INVALID_ARG / ESY_ERR_NO_MEM / ESY_ERR_STATE).
 *
 * @note If a backend must load the program into instruction memory, it may do so
 *       here or lazily at the first channel claim, depending on implementation.
 * The first call requires {0}. Repeated init of a live program returns STATE
 * unchanged. Failed init leaves an uninitialized object that may be retried.
 */
extern esy_err_t esy_hb_xio_program_init(
    esy_hb_xio_program_t* prog, const esy_hb_xio_program_desc_t* desc);

/**
 * @brief Deinitialize a program object.
 *
 * @param[in,out] prog Program object.
 *
 * @note If the backend uses reference counting / shared loading, the actual unload
 *       timing is backend-defined.
 * Release every channel first; with live channel references this call leaves the
 * program unchanged. Successful cleanup returns the whole object to {0}.
 */
extern void esy_hb_xio_program_deinit(esy_hb_xio_program_t* prog);

/* -------------------------------------------------------------------------- */
/* Channel (state machine) lifecycle (manual allocation only)                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief Claim a hardware channel and bind it to a program (manual allocation only).
 *
 * @param[in,out] prog     Target program (must be initialized).
 * @param[in,out] sm       Channel object, initialized with {0} before first claim.
 * @param[in]     instance Hardware instance index (backend-defined; e.g. RP2040: 0=PIO0, 1=PIO1).
 * @param[in]     sm_index Channel/SM index (backend-defined; e.g. RP2040: 0..3).
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code (ESY_ERR_INVALID_ARG / ESY_ERR_STATE / ESY_ERR_BUSY / ESY_ERR_NO_MEM).
 *
 * @note If the backend needs to load the program into the selected instance's
 *       instruction memory, this call may attempt to load and reserve instruction space.
 * A claimed object returns STATE unchanged. Failure leaves the unclaimed object
 * available for retry. Successful release returns the whole object to {0}.
 */
extern esy_err_t esy_hb_xio_sm_claim_manual(esy_hb_xio_program_t* prog,
                                               esy_hb_xio_sm_t* sm,
                                               uint8_t instance,
                                               uint8_t sm_index);

/**
 * @brief Release the hardware channel; decrease program load reference if applicable.
 *
 * @param[in,out] sm Channel object.
 */
extern void esy_hb_xio_sm_release(esy_hb_xio_sm_t* sm);

/**
 * @brief Get the hardware location (instance index and channel/SM index).
 *
 * Useful for mapping DMA endpoints or doing board-level diagnostics.
 *
 * @param[in]  sm           Channel object.
 * @param[out] out_instance Returned instance index.
 * @param[out] out_sm_index Returned channel/SM index.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code (ESY_ERR_INVALID_ARG / ESY_ERR_STATE).
 */
extern esy_err_t esy_hb_xio_sm_get_location(esy_hb_xio_sm_t* sm,
                                                uint8_t* out_instance,
                                                uint8_t* out_sm_index);

/**
 * @brief Get the program entry offset for this channel (if supported).
 *
 * Used when a backend relocates or wraps the program and an upper layer needs
 * to compute jump/IRQ addresses.
 *
 * @param[in]  sm           Channel object.
 * @param[out] entry_offset Returned entry offset.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code (ESY_ERR_INVALID_ARG / ESY_ERR_STATE / ESY_ERR_NOT_SUPPORTED).
 */
extern esy_err_t esy_hb_xio_sm_get_entry_offset(esy_hb_xio_sm_t* sm,
                                                    uint32_t* entry_offset);

/**
 * @brief Initialize a channel using the provided configuration.
 *
 * Applies pin mapping, shifts/thresholds, clock divider, wrap, etc.
 *
 * @param[in,out] sm  Channel object (must be claimed).
 * @param[in]     cfg Initialization configuration.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code (ESY_ERR_INVALID_ARG / ESY_ERR_STATE / ESY_ERR_NOT_SUPPORTED).
 *
 * @note If @ref esy_hb_xio_sm_config_t::enable_on_init is non-zero, the
 *       backend will enable the channel immediately after configuration.
 */
extern esy_err_t esy_hb_xio_sm_init(esy_hb_xio_sm_t* sm,
                                       const esy_hb_xio_sm_config_t* cfg);

/* -------------------------------------------------------------------------- */
/* Run control                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief Enable or disable a channel.
 *
 * @param[in,out] sm     Channel object.
 * @param[in]     enable true to enable; false to disable.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code (ESY_ERR_INVALID_ARG / ESY_ERR_STATE).
 */
extern esy_err_t esy_hb_xio_sm_set_enabled(esy_hb_xio_sm_t* sm, bool enable);

/**
 * @brief Start a channel.
 *
 * Typically equivalent to set_enabled(true), possibly with extra backend-defined
 * steps (e.g. clearing FIFOs, resetting PC, etc.).
 *
 * @param[in,out] sm Channel object.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hb_xio_sm_start(esy_hb_xio_sm_t* sm);

/**
 * @brief Stop a channel.
 *
 * Typically equivalent to set_enabled(false), possibly with extra backend-defined
 * cleanup.
 *
 * @param[in,out] sm Channel object.
 */
extern void esy_hb_xio_sm_stop(esy_hb_xio_sm_t* sm);

/* -------------------------------------------------------------------------- */
/* FIFO I/O (32-bit primitives)                                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief Non-blocking write to TX FIFO.
 *
 * @param[in,out] sm     Channel object.
 * @param[in]     value  32-bit value to push.
 * @param[out]    out_ok Set to true if written; false if FIFO is full (may be NULL).
 *
 * @retval ESY_OK Success (and @p out_ok indicates whether the write occurred).
 * @retval <0     Error code (ESY_ERR_INVALID_ARG / ESY_ERR_STATE).
 */
extern esy_err_t esy_hb_xio_tx_try_put32(esy_hb_xio_sm_t* sm,
                                            uint32_t value,
                                            bool* out_ok);

/**
 * @brief Blocking write to TX FIFO (waits until writable or error).
 *
 * @param[in,out] sm    Channel object.
 * @param[in]     value 32-bit value to push.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hb_xio_tx_put32_blocking(esy_hb_xio_sm_t* sm,
                                                 uint32_t value);

/**
 * @brief Non-blocking read from RX FIFO.
 *
 * @param[in,out] sm        Channel object.
 * @param[out]    out_value Returned 32-bit value.
 * @param[out]    out_ok    Set to true if read; false if FIFO is empty (may be NULL).
 *
 * @retval ESY_OK Success (and @p out_ok indicates whether the read occurred).
 * @retval <0     Error code (ESY_ERR_INVALID_ARG / ESY_ERR_STATE).
 */
extern esy_err_t esy_hb_xio_rx_try_get32(esy_hb_xio_sm_t* sm,
                                            uint32_t* out_value,
                                            bool* out_ok);

/**
 * @brief Blocking read from RX FIFO (waits until readable or error).
 *
 * @param[in,out] sm        Channel object.
 * @param[out]    out_value Returned 32-bit value.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hb_xio_rx_get32_blocking(esy_hb_xio_sm_t* sm,
                                                 uint32_t* out_value);

/**
 * @brief Query TX FIFO level (backend-defined semantics).
 *
 * @param[in]  sm        Channel object.
 * @param[out] out_level Returned level (0..depth).
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 *
 * @note The meaning (used slots vs free slots) is backend-defined. It is
 *       recommended to return "number of used entries".
 */
extern esy_err_t esy_hb_xio_tx_level(esy_hb_xio_sm_t* sm,
                                        uint32_t* out_level /*0..depth*/);

/**
 * @brief Query RX FIFO level (backend-defined semantics).
 *
 * @param[in]  sm        Channel object.
 * @param[out] out_level Returned level (0..depth).
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 *
 * @note The meaning (used slots vs free slots) is backend-defined. It is
 *       recommended to return "number of used entries".
 */
extern esy_err_t esy_hb_xio_rx_level(esy_hb_xio_sm_t* sm,
                                        uint32_t* out_level /*0..depth*/);

/* -------------------------------------------------------------------------- */
/* Events / Interrupts (optional)                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief Register or update the event callback.
 *
 * @param[in,out] sm   Channel object.
 * @param[in]     cb   Callback function; NULL to unregister.
 * @param[in]     user User context passed to callback.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hb_xio_set_event_cb(esy_hb_xio_sm_t* sm,
                                            esy_hb_xio_event_cb_t cb,
                                            void* user);

/* -------------------------------------------------------------------------- */
/* Convenience helpers (optional)                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief Set direction for a consecutive range of pins.
 *
 * @param[in,out] sm       Channel object.
 * @param[in]     pin_base First physical GPIO; Pico accepts 0..31 on the chip.
 * @param[in]     count    Nonzero range width, at most 32. The whole range must
 *                        fit in GPIO 0..31 and the selected chip's GPIO bank.
 * @param[in]     out      true = output; false = input.
 *
 * @retval ESY_OK Success.
 * @retval ESY_ERR_INVALID_ARG Invalid handle or pin range; no hardware changes.
 * @note The configuration owner serializes this call with all SM users.
 */
extern esy_err_t esy_hb_xio_set_consecutive_pindirs(esy_hb_xio_sm_t* sm,
                                                       uint8_t pin_base,
                                                       uint8_t count,
                                                       bool out);

/**
 * @brief Write a bitmask to a consecutive range of pins.
 *
 * @param[in,out] sm       Channel object.
 * @param[in]     pin_base First physical GPIO; Pico accepts 0..31 on the chip.
 * @param[in]     count    Nonzero range width, at most 32. The whole range must
 *                        fit in GPIO 0..31 and the selected chip's GPIO bank.
 * @param[in]     mask     Values for the selected range; LSB corresponds to
 *                        @p pin_base. Bits above @p count are ignored.
 *
 * @retval ESY_OK Success.
 * @retval ESY_ERR_INVALID_ARG Invalid handle or pin range; no hardware changes.
 * @note Only the selected pins change; every other pin's output latch is
 * preserved, including pins owned by other SMs. The configuration owner must
 * stop this SM and serialize the call with all its users.
 */
extern esy_err_t esy_hb_xio_write_pins_mask(esy_hb_xio_sm_t* sm,
                                               uint8_t pin_base,
                                               uint8_t count,
                                               uint32_t mask);

/**
 * @brief Restart/reset a channel (clear PC/FIFOs/flags; backend-defined).
 *
 * @param[in,out] sm Channel object.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hb_xio_restart(esy_hb_xio_sm_t* sm);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_HB_XIO_H */
