/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_PWM_DEFS_H
#define ESY_PWM_DEFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_dma_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_pwm_defs.h
 * @brief Generic PWM type definitions.
 *
 * This header defines a platform-agnostic PWM abstraction:
 * - a unified PWM handle type,
 * - initialization/configuration structures,
 * - and a backend vtable (function table) bound by the HAL wrapper.
 *
 * The goal is to keep the upper layers independent from SoC-specific PWM blocks
 * (TIM/PWM slices, complementary outputs, dead-time units, etc.).
 */

/* -------------------------------------------------------------------------- */
/* Forward Declarations and Handle Types */
/* -------------------------------------------------------------------------- */

/**
 * @brief Forward declaration of the PWM unified handle.
 *
 * Kept consistent with the HAL backend definition so that callbacks and the
 * vtable can use @ref esy_pwm_t without including backend headers.
 */
struct esy_pwm_s;

/**
 * @brief PWM handle type.
 */
typedef struct esy_pwm_s esy_pwm_t;

/**
 * @brief Public capability bits returned by the initialized handle's get_caps.
 *
 * Bits describe implemented operations for the current handle/role, subject to
 * board resources, configuration limits and normal runtime errors. A set bit
 * neither reserves a resource nor guarantees every parameter combination.
 * DMA-named bits describe the corresponding API, not native DMA or asynchronous
 * completion. Unknown bits must be ignored; zero is a valid capability mask.
 */
typedef enum {
  ESY_PWM_CAP_DMA_OUTPUT = 1u << 0, /**< DMA-driven duty sample output entry point. */
  ESY_PWM_CAP_WRAP_IRQ = 1u << 1, /**< Wrap callback registration and interrupt delivery. */
  ESY_PWM_CAP_POLARITY = 1u << 2, /**< Output polarity selection. */
} esy_pwm_cap_t;

/* -------------------------------------------------------------------------- */
/* Configuration Structures */
/* -------------------------------------------------------------------------- */

/**
 * @brief PWM controller inner configuration (platform-specific).
 *
 * This structure carries platform-specific routing/mux information such as:
 * - output GPIO mapping,
 * - logical channel number,
 * - timer/PWM block instance identifier, etc.
 *
 * Upper layers should treat these fields as opaque semantics defined by the
 * board/platform.
 */
typedef struct {
  uint8_t gpio;    /**< Output GPIO identifier (platform-defined numbering) */
  uint8_t channel; /**< PWM channel number (platform-defined) */
  uint8_t tim_id;  /**< Timer/PWM instance ID (platform-defined semantics) */
} esy_pwm_inner_config_t;

/**
 * @brief PWM initialization configuration.
 *
 * @note @ref phys_index selects the physical PWM controller instance (mapped to
 * a timer/PWM block).
 */
typedef struct {
  uint8_t phys_index;  /**< Physical controller index */
  uint32_t freq_hz;    /**< Output frequency in Hz */
  uint32_t duty_cycle; /**< Duty in percent, encoded as 0..100 (0%..100%) */
  bool start_low;      /**< Initial output level: true=low, false=high */
  esy_pwm_inner_config_t inner; /**< Platform-specific inner configuration */
} esy_pwm_config_t;

/* -------------------------------------------------------------------------- */
/* Backend Vtable */
/* -------------------------------------------------------------------------- */

/**
 * @brief PWM backend function table (vtable).
 *
 * All functions take @ref esy_pwm_t* as the first argument.
 *
 * Return convention:
 * - Configuration/control functions return @ref esy_err_t:
 *   @ref ESY_OK on success, negative error code on failure.
 * - Optional/unsupported features should return @ref ESY_ERR_NOT_SUPPORTED.
 */
typedef struct {
  /**
   * @brief Set duty cycle.
   *
   * @param[in] pwm        PWM handle
   * @param[in] duty_cycle Duty cycle as a ratio (0.0 .. 1.0)
   * @return Result code
   */
  esy_err_t (*set_duty)(esy_pwm_t *pwm, float duty_cycle);

  /**
   * @brief Set PWM frequency (typically by updating the period/wrap value).
   *
   * @param[in] pwm     PWM handle
   * @param[in] freq_hz Target frequency in Hz
   * @return Result code
   */
  esy_err_t (*set_wrap)(esy_pwm_t *pwm, uint32_t freq_hz);

  /**
   * @brief Enable or disable PWM output.
   *
   * @param[in] pwm    PWM handle
   * @param[in] enable true=enable, false=disable
   * @return Result code
   */
  esy_err_t (*enable)(esy_pwm_t *pwm, bool enable);

  /**
   * @brief Set output polarity / initial level.
   *
   * @param[in] pwm       PWM handle
   * @param[in] start_low true=start low, false=start high
   * @return Result code
   */
  esy_err_t (*set_polarity)(esy_pwm_t *pwm, bool start_low);

  /**
   * @brief Enable/disable trigger output (e.g., to trigger ADC conversions).
   *
   * @param[in] pwm     PWM handle
   * @param[in] enabled true=enable, false=disable
   * @return Result code
   */
  esy_err_t (*enable_trigger_output)(esy_pwm_t *pwm, bool enabled);

  /**
   * @brief Drive/update PWM output via DMA.
   *
   * Typical use cases include updating compare values or streaming a waveform.
   *
   * @param[in] pwm     PWM handle
   * @param[in] dma_cfg DMA configuration
   * @param[in] loop    true=loop mode, false=one-shot
   * @return Result code
   */
  esy_err_t (*dma_output)(esy_pwm_t *pwm, esy_dma_config_t *dma_cfg, bool loop);

  /** @brief Retire DMA and its callback storage on the serialized owner. */
  esy_err_t (*abort_dma)(esy_pwm_t *pwm);

  /** @brief Deliver bounded pending DMA notifications on the owner context. */
  esy_err_t (*poll)(esy_pwm_t *pwm);

  /**
   * @brief Configure the wrap/period interrupt handler.
   *
   * The callback may run in interrupt context depending on platform
   * implementation; avoid blocking operations in the handler.
   *
   * @param[in] pwm     PWM handle
   * @param[in] handler Callback function pointer
   * @param[in] arg     Callback argument passed to @p handler
   * @return Result code
   */
  esy_err_t (*set_wrap_handler)(esy_pwm_t *pwm, void (*handler)(void *),
                                void *arg);

  /**
   * @brief Suspend PWM (optional, low-power).
   *
   * @param[in] pwm PWM handle
   * @return Result code
   */
  esy_err_t (*suspend)(esy_pwm_t *pwm);

  /**
   * @brief Resume PWM (optional, low-power).
   *
   * @param[in] pwm PWM handle
   * @return Result code
   */
  esy_err_t (*resume)(esy_pwm_t *pwm);

  /**
   * @brief Enable/disable low-power run mode (optional).
   *
   * @param[in] pwm          PWM handle
   * @param[in] enable_lprun true=enable, false=disable
   * @return Result code
   */
  esy_err_t (*set_low_power_mode)(esy_pwm_t *pwm, bool enable_lprun);

  /**
   * @brief Select clock source (optional).
   *
   * @param[in] pwm          PWM handle
   * @param[in] clock_source Clock source id (platform-defined)
   * @return Result code
   */
  esy_err_t (*set_clock_source)(esy_pwm_t *pwm, uint8_t clock_source);

  /**
   * @brief Configure dead-time for complementary PWM outputs (optional).
   *
   * @param[in] pwm          PWM handle
   * @param[in] dead_time_ns Dead-time in nanoseconds
   * @return Result code
   */
  esy_err_t (*set_dead_time)(esy_pwm_t *pwm, uint32_t dead_time_ns);

  /**
   * @brief Enable/disable complementary outputs (optional).
   *
   * @param[in] pwm    PWM handle
   * @param[in] enable true=enable, false=disable
   * @return Result code
   */
  esy_err_t (*enable_complementary)(esy_pwm_t *pwm, bool enable);

  /**
   * @brief Query hardware capability flags (optional).
   *
   * @param[in]  pwm      PWM handle
   * @param[out] out_caps Capability bitmask defined by the platform
   * @return Result code
   */
  esy_err_t (*get_caps)(const esy_pwm_t *pwm, uint32_t *out_caps);

  /**
   * @brief Query physical controller index (optional; useful for debugging).
   *
   * @param[in]  pwm       PWM handle
   * @param[out] out_index Physical controller index
   * @return Result code
   */
  esy_err_t (*get_phys_index)(const esy_pwm_t *pwm, uint8_t *out_index);
} esy_pwm_vtable_t;

/* -------------------------------------------------------------------------- */
/* Handle Definition */
/* -------------------------------------------------------------------------- */

/** @brief Fixed caller-owned HAL runtime capacity; contents are private. */
#define ESY_PWM_HAL_STORAGE_BYTES (8u * sizeof(void *))

/**
 * @brief Caller-owned PWM handle; initialize with {0} before first use.
 *
 * The HAL binds a shared immutable operations table. Custom implementations may
 * bind their own immutable table whose lifetime covers the handle. The backend
 * owns @ref ctx. Callers and backends must not inspect or modify HAL storage.
 */
struct esy_pwm_s {
  const esy_pwm_vtable_t *ops; /**< Borrowed immutable operations table. */
  void *ctx;                 /**< Backend-private context. */
  union {
    void *alignment; /**< HAL runtime alignment. */
    uint8_t bytes[ESY_PWM_HAL_STORAGE_BYTES]; /**< HAL-private runtime storage. */
  } hal; /**< HAL-owned storage; preserve until successful deinit. */
};

#ifdef __cplusplus
}
#endif

#endif /* ESY_PWM_DEFS_H */
