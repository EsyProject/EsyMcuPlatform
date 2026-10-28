/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_XPT2046_H
#define ESY_XPT2046_H

/**
 * @file esy_xpt2046.h
 * @brief XPT2046 resistive touch controller driver interface.
 *
 * This driver stores its private context inside @ref esy_touch_t opaque storage
 * (Scheme C). The context type is hidden from users.
 */

#include <stdint.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_gpio_defs.h"
#include "esy_common/defs/display/esy_lcd_defs.h"
#include "esy_middleware/touch/esy_touch.h"
#include "esy_common/defs/peripheral/esy_spi_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

struct esy_device_s;

#define ESY_XPT2046_Q16_16_SHIFT 16
#define ESY_XPT2046_Q16_16_ONE   (1L << ESY_XPT2046_Q16_16_SHIFT)
#define ESY_XPT2046_Q16_16_FROM_RATIO(num, den) \
  ((int32_t)(((int64_t)(num) * ESY_XPT2046_Q16_16_ONE) / (int64_t)(den)))
#define ESY_XPT2046_CALIBRATION_BLOB_MAGIC   0x58435443u /* "XCTC" */
#define ESY_XPT2046_CALIBRATION_BLOB_VERSION 1u

typedef enum {
  ESY_XPT2046_CALIBRATION_LINEAR = 0,
  ESY_XPT2046_CALIBRATION_RANGE = 1,
  ESY_XPT2046_CALIBRATION_AFFINE = 2,
} esy_xpt2046_calibration_mode_t;

typedef enum {
  ESY_XPT2046_CALIBRATION_FLAG_SWAP_XY = (1u << 0),
  ESY_XPT2046_CALIBRATION_FLAG_INVERT_X = (1u << 1),
  ESY_XPT2046_CALIBRATION_FLAG_INVERT_Y = (1u << 2),
} esy_xpt2046_calibration_flags_t;

typedef enum {
  ESY_XPT2046_TOUCH_DETECT_ALWAYS = 0,
  ESY_XPT2046_TOUCH_DETECT_IRQ = 1,
  ESY_XPT2046_TOUCH_DETECT_PRESSURE_THRESHOLD = 2,
} esy_xpt2046_touch_detect_mode_t;

typedef struct {
  int32_t x_scale_q16_16; /**< [in] Raw-to-screen X scale factor (Q16.16). */
  int32_t y_scale_q16_16; /**< [in] Raw-to-screen Y scale factor (Q16.16). */
  int32_t x_offset;       /**< [in] Raw-to-screen X offset in pixels. */
  int32_t y_offset;       /**< [in] Raw-to-screen Y offset in pixels. */
} esy_xpt2046_linear_calibration_t;

/**
 * @brief Raw endpoints for range calibration.
 * Each raw axis maps across the complete logical destination axis. With SWAP_XY,
 * raw X spans height and raw Y spans width before swapping; inversion then uses
 * the final logical width/height. Reversed endpoints reverse that raw axis.
 */
typedef struct {
  uint16_t raw_x_min; /**< [in] Minimum raw X reading. */
  uint16_t raw_x_max; /**< [in] Maximum raw X reading. */
  uint16_t raw_y_min; /**< [in] Minimum raw Y reading. */
  uint16_t raw_y_max; /**< [in] Maximum raw Y reading. */
} esy_xpt2046_range_calibration_t;

typedef struct {
  int32_t ax_q16_16; /**< [in] X coefficient for raw X. */
  int32_t bx_q16_16; /**< [in] X coefficient for raw Y. */
  int32_t cx_q16_16; /**< [in] X offset coefficient (Q16.16 pixels). */
  int32_t ay_q16_16; /**< [in] Y coefficient for raw X. */
  int32_t by_q16_16; /**< [in] Y coefficient for raw Y. */
  int32_t cy_q16_16; /**< [in] Y offset coefficient (Q16.16 pixels). */
} esy_xpt2046_affine_calibration_t;

/**
 * @brief XPT2046 coordinate calibration for the selected orientation.
 *
 * Scale factors and affine coefficients use signed Q16.16 fixed-point format.
 * For example:
 * - 1.0  => ESY_XPT2046_Q16_16_ONE
 * - 0.5  => ESY_XPT2046_Q16_16_FROM_RATIO(1, 2)
 * - -0.5 => ESY_XPT2046_Q16_16_FROM_RATIO(-1, 2)
 */
typedef struct {
  esy_xpt2046_calibration_mode_t mode; /**< [in] Calibration mode. */
  uint32_t flags; /**< [in] @ref esy_xpt2046_calibration_flags_t. */
  esy_xpt2046_linear_calibration_t
      linear;                            /**< [in] Linear scale/offset model. */
  esy_xpt2046_range_calibration_t range; /**< [in] Raw min/max range model. */
  esy_xpt2046_affine_calibration_t affine; /**< [in] Affine transform model. */
} esy_xpt2046_calibration_t;

typedef struct {
  uint32_t magic;   /**< [in] Must be ESY_XPT2046_CALIBRATION_BLOB_MAGIC. */
  uint16_t version; /**< [in] Must be ESY_XPT2046_CALIBRATION_BLOB_VERSION. */
  uint16_t size;    /**< [in] sizeof(esy_xpt2046_calibration_blob_t). */
  esy_xpt2046_calibration_t calibration; /**< [in] Stored calibration. */
  uint32_t crc32; /**< [in] CRC32 over all previous fields. */
} esy_xpt2046_calibration_blob_t;

typedef struct {
  esy_xpt2046_touch_detect_mode_t
      mode; /**< [in] Touch validity detection mode. */
  uint16_t
      x_plate_ohms; /**< [in] X plate resistance for pressure estimation. */
  uint16_t pressure_max_ohms; /**< [in] Pressed when estimated resistance is <=
                                 this. */
} esy_xpt2046_touch_detect_t;

/**
 * @brief XPT2046 initialization configuration.
 */
typedef struct {
  esy_gpio_t *cs_gpio;  /**< [in] Initialized software CS resource. */
  esy_gpio_t *irq_gpio; /**< [in] Optional initialized IRQ resource. */
  struct esy_device_s
      *spi_device; /**< [in] SPI graph dependency used when PM is registered. */
  uint16_t width;  /**< [in] Logical screen width in pixels. */
  uint16_t height; /**< [in] Logical screen height in pixels. */
  esy_lcd_dir_t
      dir; /**< [in] Screen orientation. Must match LCD orientation. */
  esy_xpt2046_calibration_t calibration; /**< [in] Calibration for @ref dir. */
  esy_xpt2046_touch_detect_t
      touch_detect; /**< [in] Pressed-state detection policy. */

  esy_touch_spi_get_write_buffer
      spi_get_write_buffer_cb; /**< [in] Required sized full-duplex reservation.
                                * Every ADC request needs three writable bytes
                                * in both buffers before any command is written.
                                */
  void *spi_get_write_buffer_cb_arg;  /**< [in] User argument. */
  /** @brief Required status-bearing queued SPI provider.
   * Uses spi_submit_cb_arg and the configured write-buffer provider. Every
   * accepted transfer completes exactly once; failures never publish a sample.
   */
  esy_spi_submit_result_t spi_submit_result_cb;
  void *spi_submit_cb_arg; /**< [in] Borrowed SPI provider context. */
} esy_xpt2046_config_t;

/**
 * @brief Populate config with defaults (callbacks remain NULL).
 *
 * @param[out] config Configuration pointer (must not be NULL).
 */
void esy_xpt2046_default_config(esy_xpt2046_config_t *config);

esy_err_t esy_xpt2046_calibration_export(
    const esy_xpt2046_calibration_t *calibration,
    esy_xpt2046_calibration_blob_t *blob);

esy_err_t esy_xpt2046_calibration_import(
    const esy_xpt2046_calibration_blob_t *blob,
    esy_xpt2046_calibration_t *calibration);

/**
 * @brief Initialize XPT2046 driver and bind it to a touch handle.
 *
 * The driver uses @ref esy_touch_t internal opaque storage to hold its private
 * context. Initialize the handle with `{0}` before its first use. Repeated init
 * returns ESY_ERR_STATE without changing an attached handle, including a
 * registered handle or one with accepted SPI callbacks pending. Initialization
 * failures leave the handle unchanged.
 *
 * Stop polling, drain accepted transfers and provider callbacks, unregister any
 * logical device, then call @ref esy_touch_detach before initializing again.
 * Serialize all handle access and provider callbacks; init is task-context only.
 * Sampling rejects SPI and CS/IRQ GPIO errors without publishing new data.
 * get_sample returns the pending error without changing its output. Poll again
 * to retry failed CS release before detaching or reusing the shared SPI bus.
 *
 * @param[in,out] touch Zero-initialized or successfully detached handle.
 * @param[in]  config Config pointer (must not be NULL).
 *
 * @retval ESY_OK Success.
 * @retval ESY_ERR_INVALID_ARG Invalid handle or configuration.
 * @retval ESY_ERR_STATE Handle is already attached or still bound.
 */
esy_err_t esy_xpt2046_init(esy_touch_t *touch,
                           const esy_xpt2046_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* ESY_XPT2046_H */
