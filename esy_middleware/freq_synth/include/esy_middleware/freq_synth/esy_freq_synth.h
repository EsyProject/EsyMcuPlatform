/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_freq_synth.h
 * @brief Frequency, output and diagnostic facade for concrete synthesizers.
 *
 * Initialize each caller-owned handle with `{0}` before first use. Do not copy,
 * move, reset or reinitialize a live handle. Drivers borrow one immutable
 * operations table and a backend context; the complete table and context stay
 * valid until matching deinit succeeds. Never replace ops or ctx while attached.
 * Each concrete driver shares one static const table across all its instances.
 * A custom provider supplies its own complete const table, setting unsupported
 * optional operations to NULL, and initializes ctx before publishing ops.
 *
 * Driver context occupies backend.bytes or provider-owned persistent storage.
 * Frontend lifecycle bytes are private to this module; custom providers leave
 * them zero and use detach only after their own hardware cleanup succeeds.
 * Invalid configuration preserves the handle. Failed hardware initialization
 * attempts cleanup; failed cleanup retains a faulted binding for matching deinit
 * retry. Never clear a live handle to bypass resource ownership.
 *
 * One non-ISR owner serializes operations and lifecycle. PM transitions obey
 * the same ownership contract. Serialize complete shared-SPI transactions.
 * This facade creates no tasks, allocates no memory and owns no hardware.
 */

#ifndef ESY_FREQ_SYNTH_H
#define ESY_FREQ_SYNTH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Private storage size (bytes) reserved inside @ref esy_freq_synth_t.
 *
 * Concrete drivers should validate this is large enough for their private
 * context. You may override this macro at build time if needed.
 */
#ifndef ESY_FREQ_SYNTH_PRIV_BYTES
#define ESY_FREQ_SYNTH_PRIV_BYTES 160
#endif

/**
 * @brief Output port selection.
 *
 * Many synthesizers provide two outputs (e.g., main RF output and an auxiliary
 * output). Some devices may support only one port; unsupported ports should
 * return ESY_ERR_NOT_SUPPORTED from driver implementations.
 */
typedef enum {
  ESY_FREQ_SYNTH_PORT_MAIN = 0, /**< Main output (e.g., RF_OUT). */
  ESY_FREQ_SYNTH_PORT_AUX = 1,  /**< Auxiliary output (e.g., AUX_OUT). */
} esy_freq_synth_port_t;

/**
 * @brief get_info() query identifiers (extensibility point).
 *
 * This enum provides a unified way to query device-specific information without
 * expanding the core API surface.
 *
 * @note
 * Not all synthesizers are PLL-based. For non-PLL devices (e.g., DDS or simple
 * dividers), PLL-specific items (PFD/MOD/FRAC/INT/spacing) may be unsupported
 * and should return ESY_ERR_NOT_SUPPORTED.
 */
typedef enum {
  ESY_FREQ_SYNTH_INFO_ACTUAL_HZ =
      0, /**< Current actual output frequency in Hz. */
  ESY_FREQ_SYNTH_INFO_REQUESTED_HZ, /**< Most recently requested frequency in
                                       Hz. */

  ESY_FREQ_SYNTH_INFO_PFD_HZ, /**< PLL PFD frequency in Hz (PLL-only). */
  ESY_FREQ_SYNTH_INFO_CHANNEL_SPACING_HZ, /**< Channel spacing = PFD/MOD in Hz
                                             (PLL-only). */

  ESY_FREQ_SYNTH_INFO_MOD,  /**< PLL MOD value (typically 1..4095). */
  ESY_FREQ_SYNTH_INFO_FRAC, /**< PLL FRAC value (typically 0..4095). */
  ESY_FREQ_SYNTH_INFO_INT,  /**< PLL INT value. */

  ESY_FREQ_SYNTH_INFO_OUTPUT_ENABLED, /**< Current MAIN output enable state (u8:
                                         0/1). */
  ESY_FREQ_SYNTH_INFO_POWER_LEVEL, /**< Current MAIN output power level (u8). */
  ESY_FREQ_SYNTH_INFO_RF_DIV_SEL,  /**< RF divider selector (u8, PLL-only). */
} esy_freq_synth_info_id_t;

/**
 * @brief Generic info return value container.
 *
 * This avoids proliferating different return types for different info items.
 * The driver populates the most appropriate field by convention (e.g., Hz ->
 * u64, MOD -> u16), and the caller reads the corresponding field based on the
 * queried id.
 *
 * @note This structure is intentionally "plain" to keep ABI stable.
 */
typedef struct {
  uint64_t u64;
  uint32_t u32;
  uint16_t u16;
  uint8_t u8;
} esy_freq_synth_info_value_t;

/**
 * @brief Frequency synthesizer vtable (implemented by concrete drivers).
 *
 * Each function receives a backend context pointer `ctx` that is owned and
 * managed by the concrete driver (typically stored inside
 * `synth->backend.bytes`).
 *
 * The table is immutable and remains valid while its handle is bound.
 * set_frequency_hz is required; other operations may be NULL.
 * @note Drivers should return:
 * - ESY_OK on success
 * - ESY_ERR_INVALID_ARG for invalid parameters
 * - ESY_ERR_NOT_SUPPORTED for unsupported features/ports/ids
 * - other negative error codes as appropriate
 */
typedef struct {
  /** @brief Set output frequency in Hz (single tone). */
  esy_err_t (*set_frequency_hz)(void *ctx, uint64_t freq_hz);

  /** @brief Enable or disable an output port. */
  esy_err_t (*set_output_enable)(void *ctx, esy_freq_synth_port_t port,
                                 bool enable);

  /** @brief Set output power level (device-specific mapping). */
  esy_err_t (*set_power_level)(void *ctx, esy_freq_synth_port_t port,
                               uint8_t level);

  /**
   * @brief Query device information.
   *
   * @param[in]  ctx Backend context.
   * @param[in]  id  Query item identifier.
   * @param[out] out Output value container (driver fills the agreed field).
   */
  esy_err_t (*get_info)(void *ctx, esy_freq_synth_info_id_t id,
                        esy_freq_synth_info_value_t *out);
} esy_freq_synth_vtable_t;

/** @brief Pointer-aligned storage for private frontend lifecycle state. */
#define ESY_FREQ_SYNTH_FRONTEND_STORAGE_BYTES (2u * sizeof(void *))

/**
 * @brief Caller-owned handle; initialize with `{0}` before first use.
 *
 * ops and ctx are initialized only by the concrete provider and stay immutable
 * while bound. backend belongs to that provider. frontend is module-private;
 * consumers must not inspect or change its bytes. Keep all borrowed resources
 * alive until matching backend deinit succeeds, including after cleanup errors.
 */
typedef struct esy_freq_synth_s {
  const esy_freq_synth_vtable_t *ops; /**< Borrowed immutable complete table. */
  void *ctx; /**< Borrowed driver context. */
  union {
    void *alignment; /**< Pointer alignment for frontend state. */
    uint8_t bytes[ESY_FREQ_SYNTH_FRONTEND_STORAGE_BYTES]; /**< Private lifecycle. */
  } frontend; /**< Owned exclusively by the facade and device adapter. */
  union {
    max_align_t alignment; /**< Driver context alignment guarantee. */
    uint8_t bytes[ESY_FREQ_SYNTH_PRIV_BYTES]; /**< Driver-private storage. */
  } backend; /**< Persistent concrete-driver storage. */
} esy_freq_synth_t;

/**
 * @brief Detach the handle (set to an unbound state).
 *
 * After detach, is_attached() returns false and all operations should fail with
 * ESY_ERR_STATE (implementation-defined) unless re-attached by a driver.
 * A registered logical device must be unregistered before detach.
 * This function performs no hardware cleanup. Concrete chip backends must use
 * their matching deinit instead. A faulted binding cannot be detached.
 *
 * @param[in,out] synth Synth handle.
 */
void esy_freq_synth_detach(esy_freq_synth_t *synth);

/**
 * @brief Check whether the handle is attached (bound to a driver).
 *
 * Attachment requires a backend context and its core set_frequency_hz operation.
 * A faulted handle can remain attached for cleanup; this query does not assert
 * that ordinary operations are available or that the hardware is ready.
 *
 * @param[in] synth Synth handle.
 * @retval true  Attached, including a faulted binding retained for cleanup.
 * @retval false Not attached.
 */
bool esy_freq_synth_is_attached(const esy_freq_synth_t *synth);

/**
 * @brief Set output frequency in Hz.
 *
 * @param[in,out] synth   Synth handle.
 * @param[in]     freq_hz Requested frequency in Hz.
 * @return ESY_OK on success, negative error code on failure.
 */
esy_err_t esy_freq_synth_set_frequency_hz(esy_freq_synth_t *synth,
                                          uint64_t freq_hz);

/**
 * @brief Enable or disable an output port.
 *
 * @param[in,out] synth  Synth handle.
 * @param[in]     port   Output port.
 * @param[in]     enable true to enable output, false to disable.
 * @return ESY_OK on success, negative error code on failure.
 */
esy_err_t esy_freq_synth_set_output_enable(esy_freq_synth_t *synth,
                                           esy_freq_synth_port_t port,
                                           bool enable);

/**
 * @brief Set output power level.
 *
 * The meaning of `level` is device-specific (e.g., mapping to output power
 * settings).
 *
 * @param[in,out] synth Synth handle.
 * @param[in]     port  Output port.
 * @param[in]     level Power level selector (device-specific).
 * @return ESY_OK on success, negative error code on failure.
 */
esy_err_t esy_freq_synth_set_power_level(esy_freq_synth_t *synth,
                                         esy_freq_synth_port_t port,
                                         uint8_t level);

/**
 * @brief Query synthesizer information (unified interface).
 *
 * Rules:
 * - Unsupported items should return ESY_ERR_NOT_SUPPORTED.
 * - Invalid arguments should return ESY_ERR_INVALID_ARG.
 *
 * @param[in,out] synth Synth handle.
 * @param[in]     id    Info query id.
 * @param[out]    out   Output value container (must not be NULL).
 * @return ESY_OK on success, negative error code on failure.
 */
esy_err_t esy_freq_synth_get_info(esy_freq_synth_t *synth,
                                  esy_freq_synth_info_id_t id,
                                  esy_freq_synth_info_value_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ESY_FREQ_SYNTH_H */
