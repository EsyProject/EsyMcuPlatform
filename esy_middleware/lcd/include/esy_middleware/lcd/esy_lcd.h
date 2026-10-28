/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_LCD_H
#define ESY_LCD_H

/**
 * @file esy_lcd.h
 * @brief Generic LCD facade.
 *
 * Upper layers should prefer these driver-agnostic APIs over calling
 * @ref esy_lcd_t function pointers directly.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/display/esy_lcd_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

struct esy_device_s;

/**
 * @brief Opaque storage size (bytes) inside @ref esy_lcd_t.
 *
 * LCD controller drivers place their private context into this storage.
 * You may override this macro at build time if needed.
 *
 * The driver implementation must validate its context size with _Static_assert.
 */
#ifndef ESY_LCD_PRIV_BYTES
#define ESY_LCD_PRIV_BYTES 384u
#endif

/**
 * @brief Unified LCD handle.
 *
 * - @ref flush is the region flush function (LVGL style).
 * - @ref deinit detaches the concrete backend, if provided.
 * - @ref ctx points to the driver private context located inside @ref
 * priv.bytes.
 *
 * Scheme C:
 * - The handle owns an internal opaque buffer @ref priv.bytes.
 * - Driver initializes @ref ctx to point into this buffer.
 * - Users must not access bytes directly.
 * - Initialize the handle with {0} before first use. Never reset or copy a live
 *   handle; detach must succeed before releasing or reusing its storage.
 */
typedef struct {
  /**
   * @brief Flush a rectangular region.
   *
   * Implemented by a specific LCD controller driver.
   * `ESY_OK` means the backend accepted the request and owns completion
   * notification through @p done_cb. A negative return value means the request
   * was rejected and @p done_cb must not be called for that request.
   *
   * @param[in] x0        Left-top X.
   * @param[in] y0        Left-top Y.
   * @param[in] x1        Right-bottom X (inclusive).
   * @param[in] y1        Right-bottom Y (inclusive).
   * @param[in] color_buf Pointer to pixel data.
   * @param[in] done_cb   Flush done callback (may be NULL).
   * @param[in] cb_arg    Callback argument for done_cb.
   * @param[in] arg       User/driver context (typically the @ref esy_lcd_t
   * handle).
   *
   * @retval ESY_OK Request accepted.
   * @retval <0 Request rejected.
   */
  esy_err_t (*flush)(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                     const uint8_t *color_buf, void (*done_cb)(void *),
                     void *cb_arg, void *arg);

  /**
   * @brief Detach the concrete backend from this handle.
   *
   * Return ESY_ERR_BUSY while callbacks or transfers still own the handle,
   * preserving all fields for retry. ESY_OK releases the backend. Registered
   * device bindings must be unregistered first. The argument is the handle.
   *
   * @param[in,out] arg User/driver context.
   */
  esy_err_t (*deinit)(void *arg);

  void *
      ctx; /**< Driver private context pointer (points into @ref priv.bytes). */
  struct esy_device_s *device; /**< Optional registered logical PM device. */

  union {
    max_align_t _align;                /**< Alignment guarantee. */
    uint8_t bytes[ESY_LCD_PRIV_BYTES]; /**< Opaque private storage. */
  } priv;
} esy_lcd_t;


/**
 * @brief Check whether an LCD handle is attached to a concrete backend.
 *
 * @param[in] lcd LCD handle pointer.
 *
 * @retval true  Handle has a backend context and flush operation.
 * @retval false Handle is NULL or not attached.
 */
bool esy_lcd_is_attached(const esy_lcd_t *lcd);

/**
 * @brief Detach an LCD handle from its concrete backend.
 *
 * Calls the backend deinit hook and propagates its status. A busy backend must
 * retain its state and callbacks for retry after completion. Without a backend
 * hook, clears an unregistered handle directly. Calls must be serialized with
 * other public operations and user completion callbacks.
 *
 * @retval ESY_OK Detached, already detached, or NULL.
 * @retval ESY_ERR_BUSY Logical device remains registered or backend is busy.
 * @retval <0 Backend cleanup failed; caller must retain the handle.
 *
 * @param[in,out] lcd LCD handle pointer. NULL is allowed.
 */
esy_err_t esy_lcd_detach(esy_lcd_t *lcd);

/**
 * @brief Flush a rectangular region through the attached backend.
 *
 * This facade validates only backend-independent arguments. Backend-specific
 * bounds, format, and busy-state checks remain the concrete driver's
 * responsibility. A successful return means the backend accepted the request
 * and owns completion notification through @p done_cb. A negative return value
 * means the request was rejected and @p done_cb will not be called for that
 * request. Calls are task-context and externally serialized. Completion may
 * run in the provider task or inline; done_cb may submit the next flush. Stop
 * producers and coordinate callbacks before lifecycle changes. Completion
 * releases the pixel buffer even on transport failure; it does not report
 * whether pixels reached the display.
 *
 * @param[in,out] lcd       LCD handle pointer.
 * @param[in]     x0        Left-top X.
 * @param[in]     y0        Left-top Y.
 * @param[in]     x1        Right-bottom X, inclusive.
 * @param[in]     y1        Right-bottom Y, inclusive.
 * @param[in]     color_buf Packed row-major pixels in the configured panel's
 *                         wire byte order. The facade does not convert native
 *                         uint16_t data. Retain until done_cb.
 * @param[in]     done_cb   Flush done callback, may be NULL.
 * @param[in]     cb_arg    Callback argument.
 *
 * @retval ESY_OK Request accepted by the attached backend.
 * @retval ESY_ERR_INVALID_ARG Invalid handle, region ordering, or buffer.
 * @retval ESY_ERR_STATE Handle is not attached to a backend.
 * @retval <0 Backend-specific rejection.
 */
esy_err_t esy_lcd_flush(esy_lcd_t *lcd,
                        uint16_t x0,
                        uint16_t y0,
                        uint16_t x1,
                        uint16_t y1,
                        const uint8_t *color_buf,
                        void (*done_cb)(void *),
                        void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif /* ESY_LCD_H */
