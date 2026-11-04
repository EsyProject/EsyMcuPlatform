/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_TIMER_CAPTURE_DEFS_H
#define ESY_TIMER_CAPTURE_DEFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"

/**
 * @file esy_timer_capture_defs.h
 * @brief Generic timer input-capture types and vtable definitions.
 *
 * This file provides a unified Timer Capture abstraction, including
 * opaque handles, configuration structures, callback types, and
 * backend function tables (vtables).
 */

#define ESY_TIMER_CAPTURE_PRIV_BYTES 64

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Forward declarations and handle types */
/* -------------------------------------------------------------------------- */

/**
 * @brief Forward declaration of the Timer Capture handle.
 *
 * Kept consistent with the definition in esy_hb_timer_capture.h,
 * so it can be used safely in callbacks and vtables.
 */
struct esy_timer_capture_s;

/**
 * @brief Timer Capture handle type.
 */
typedef struct esy_timer_capture_s esy_timer_capture_t;

/**
 * @brief Public capability bits returned by the initialized handle's get_caps.
 *
 * Bits describe implemented operations for the current handle/role, subject to
 * board resources, configuration limits and normal runtime errors. A set bit
 * neither reserves a resource nor guarantees every parameter combination.
 * HARDWARE_LATCH means the edge captures the native timer counter before ISR
 * dispatch. Unknown bits must be ignored; zero is a valid capability mask.
 */
typedef enum {
  ESY_TIMER_CAPTURE_CAP_EDGE_CALLBACK = 1u << 0, /**< Callback delivery for captured input edges. */
  ESY_TIMER_CAPTURE_CAP_INTERVAL = 1u << 1, /**< Interval query between successive captures. */
  ESY_TIMER_CAPTURE_CAP_HARDWARE_LATCH = 1u << 2, /**< Edge time latched by a hardware capture timer, not ISR timestamping. */
} esy_timer_capture_cap_t;

/* -------------------------------------------------------------------------- */
/* Callback types */
/* -------------------------------------------------------------------------- */

/**
 * @brief Timer capture callback type.
 *
 * @param[in] timestamp Native capture-counter value converted to microseconds.
 * The counter domain and wrap period are port-specific; see the HAL contract.
 * @param[in] arg       User-provided context pointer.
 *
 * @note
 * The callback receives the timestamp and caller-provided context. The caller
 * may include its capture handle in that context when needed.
 */
typedef void (*esy_timer_capture_callback_t)(uint32_t timestamp, void *arg);

/* -------------------------------------------------------------------------- */
/* Configuration structures */
/* -------------------------------------------------------------------------- */

/**
 * @brief Internal timer capture hardware configuration.
 *
 * Describes hardware-specific binding such as GPIO, timer channel,
 * timer instance, and interrupt priority.
 */
typedef struct {
  uint8_t gpio;         /**< Input capture GPIO pin number */
  uint8_t channel;      /**< STM32 timer channel; ESP32 group-local logical slot 0..2. */
  uint8_t tim_id;       /**< Timer identifier; ESP32 MCPWM capture group 0..1. */
  uint8_t irq_priority; /**< Interrupt priority */
} esy_timer_capture_inner_config_t;

/**
 * @brief Timer input capture configuration.
 *
 * @note
 * @ref phys_index specifies a platform-defined capture reservation index.
 * On ESP32, indices 0..2 select MCPWM group 0 and 3..5 select group 1;
 * inner.tim_id must equal phys_index / 3 and inner.channel phys_index % 3.
 * The IDF driver selects the native channel within that group; the logical slot
 * does not request a particular hardware channel. All group owners use the
 * same IRQ-allocation core and identical irq_priority (0 selects SDK default,
 * otherwise 1..3). ESP32 configures an input with internal pulls disabled;
 * provide any required bias externally. Its MCPWM capture group and GPIO must
 * not be configured or owned by a separate native SDK driver. Pico timer capture
 * is unsupported.
 * All hardware-specific resource identities are provided via @ref inner before
 * initialization. A board configure hook may configure these resources but must
 * not replace the GPIO, timer, or channel after ownership has been reserved.
 */
typedef struct {
  uint8_t phys_index; /**< Platform-defined capture reservation index. */
  bool rising_edge;   /**< true: capture rising edge; false: falling edge */
  esy_timer_capture_callback_t callback; /**< Capture event callback */
  void *callback_arg;                    /**< Callback context argument */
  esy_timer_capture_inner_config_t
      inner; /**< Hardware-specific configuration */
} esy_timer_capture_config_t;

/* -------------------------------------------------------------------------- */
/* Vtable definition */
/* -------------------------------------------------------------------------- */

/**
 * @brief Timer capture backend function table.
 *
 * All functions take @ref esy_timer_capture_t* as the first argument.
 * Return value convention:
 * - @ref ESY_OK on success
 * - negative error code on failure
 */
typedef struct {
  /**
   * @brief Get the most recent captured value.
   *
   * @param[in]  cap      Timer Capture handle.
   * @param[out] value_us Captured timestamp in microseconds.
   * @return Execution result.
   */
  esy_err_t (*get_value)(const esy_timer_capture_t *cap, uint32_t *value_us);

  /**
   * @brief Get the interval between the two most recent capture events.
   *
   * @param[in]  cap         Timer Capture handle.
   * @param[out] interval_us Capture interval in microseconds.
   * @return Execution result.
   */
  esy_err_t (*get_interval)(const esy_timer_capture_t *cap,
                            uint32_t *interval_us);

  /* --------------------- Optional query interfaces --------------------- */

  /**
   * @brief Get the physical controller index.
   *
   * @param[in]  cap       Timer Capture handle.
   * @param[out] out_index Physical controller index.
   * @return Execution result.
   */
  esy_err_t (*get_phys_index)(const esy_timer_capture_t *cap,
                              uint8_t *out_index);

  /**
   * @brief Get hardware capability flags.
   *
   * @param[in]  cap      Timer Capture handle.
   * @param[out] out_caps Capability bitmask.
   * @return Execution result.
   */
  esy_err_t (*get_caps)(const esy_timer_capture_t *cap, uint32_t *out_caps);
} esy_timer_capture_vtable_t;

/* -------------------------------------------------------------------------- */
/* Handle definition */
/* -------------------------------------------------------------------------- */

/** @brief Fixed caller-owned HAL runtime capacity; contents are private. */
#define ESY_TIMER_CAPTURE_HAL_STORAGE_BYTES (2u * sizeof(void *))

/**
 * @brief Timer Capture handle.
 *
 * Initialize with {0} before first use. The HAL binds a shared immutable table;
 * custom implementations may bind an immutable table that outlives the handle.
 * The backend owns @ref ctx and @ref backend. Callers and backends must not
 * inspect or modify the separate HAL-private storage.
 */
struct esy_timer_capture_s {
  const esy_timer_capture_vtable_t *ops; /**< Borrowed immutable operations table. */
  void *ctx;                           /**< Backend-private context. */
  union {
    void *alignment; /**< HAL runtime alignment. */
    uint8_t bytes[ESY_TIMER_CAPTURE_HAL_STORAGE_BYTES]; /**< HAL-private storage. */
  } hal; /**< HAL-owned storage; preserve until successful deinit. */

  union {
    max_align_t _a;                              /**< Alignment guarantee */
    uint8_t bytes[ESY_TIMER_CAPTURE_PRIV_BYTES]; /**< Private storage */
  } backend;
};

#ifdef __cplusplus
}
#endif

#endif /* ESY_TIMER_CAPTURE_DEFS_H */
