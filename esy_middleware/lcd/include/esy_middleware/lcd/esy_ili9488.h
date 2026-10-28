/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_ILI9488_H
#define ESY_ILI9488_H

/**
 * @file esy_ili9488.h
 * @brief ILI9488 LCD controller driver interface.
 *
 * The driver stores its private context inside @ref esy_lcd_t opaque storage
 * (Scheme C). The context type is hidden from users.
 */

#include <stdint.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_gpio_defs.h"
#include "esy_middleware/lcd/esy_lcd.h"
#include "esy_common/defs/peripheral/esy_spi_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LVGL flush done callback type.
 *
 * The callback releases an accepted flush, including failed transfers; it does
 * not report transfer success. Rejected
 * flush requests are reported through the flush return value and do not invoke
 * this callback. The callback may call back into the LCD facade.
 *
 * @param[in] arg Callback argument.
 */
typedef void (*esy_lvgl_flush_done_cb_t)(void *arg);

/**
 * @brief ILI9488 command sequence entry.
 *
 * The data pointer must remain valid for the lifetime of any profile that
 * references it. Entries are sent synchronously during initialization.
 */
typedef struct {
  uint8_t cmd;         /**< [in] Controller command byte. */
  const uint8_t *data; /**< [in] Optional command payload. */
  uint8_t data_len;    /**< [in] Payload length in bytes. */
  uint16_t delay_ms;   /**< [in] Delay after the command and payload. */
} esy_ili9488_command_t;

/**
 * @brief ILI9488 panel profile.
 *
 * A profile captures panel-specific initialization commands. Passing NULL in
 * @ref esy_ili9488_config_t::panel_profile selects the built-in default
 * profile.
 */
typedef struct {
  const esy_ili9488_command_t *init_sequence; /**< [in] Init command entries. */
  uint32_t init_sequence_count; /**< [in] Number of init entries. */
} esy_ili9488_panel_profile_t;

/**
 * @brief Backlight control callback.
 *
 * @param[in] enabled Backlight target state.
 * @param[in] arg     User callback argument.
 *
 * @retval ESY_OK Backlight state accepted.
 * @retval <0     Backend-specific failure; leave the previous state unchanged.
 *
 * @note Device system resume may invoke this callback after interrupts and the
 * OS tick are restored while the RTOS scheduler is still suspended. The
 * callback must be bounded and must not wait on an RTOS object or delay.
 */
typedef esy_err_t (*esy_ili9488_backlight_set_cb_t)(bool enabled, void *arg);

/** @brief Wire encoding for command parameters, independent of pixel data. */
typedef enum {
  ESY_ILI9488_PARAMETER_U8 = 0, /**< Default: one byte per parameter. */
  ESY_ILI9488_PARAMETER_U16_BE = 1 /**< Each parameter is sent as 00, value,
                                   * with a separate CS assertion. */
} esy_ili9488_parameter_format_t;

/**
 * @brief ILI9488 initialization configuration.
 *
 * The driver uses blocking HAL SPI writes for controller initialization
 * and the caller-provided submit callback for pixel flushes. Submitted flush
 * buffers must remain valid until the flush done callback runs.
 */
typedef struct {
  esy_spi_t *spi;       /**< [in] SPI controller handle. */
  esy_gpio_t *cs_gpio;  /**< [in] Initialized active-low CS resource. */
  esy_gpio_t *dc_gpio;  /**< [in] Initialized Data/Command resource. */
  esy_gpio_t *rst_gpio; /**< [in] Initialized active-low reset resource. */
  bool bgr;             /**< [in] Use BGR order if true. */
  uint16_t width;       /**< [in] Physical width at 0 degrees, in pixels. */
  uint16_t height;      /**< [in] Physical height at 0 degrees, in pixels. */
  esy_color_format_t
      color_format;  /**< [in] Pixel format. Currently RGB565 only. */
  esy_lcd_dir_t dir; /**< [in] Initial direction. */
  const esy_ili9488_panel_profile_t
      *panel_profile; /**< [in] Optional panel init profile. */
  esy_ili9488_backlight_set_cb_t
      backlight_set_cb;       /**< [in] Optional backlight callback. */
  void *backlight_set_cb_arg; /**< [in] Backlight callback argument. */
  bool backlight_initially_enabled; /**< [in] Actual initial backlight state.
                                     * Default false; init does not change it. */

  esy_spi_submit_prepared_t
      spi_submit_cb;       /**< [in] Required atomic prepare/payload provider. */
  void *spi_submit_cb_arg; /**< [in] Caller-owned provider instance. */
  esy_ili9488_parameter_format_t parameter_format; /**< [in] Init, direction
      * and window parameter encoding. Commands remain single bytes; pixel
      * payloads pass through unchanged. Invalid values fail init. */
} esy_ili9488_config_t;

/**
 * @brief Populate configuration with defaults (callbacks set to NULL).
 *
 * @param[out] config Configuration pointer (must not be NULL).
 */
void esy_ili9488_default_config(esy_ili9488_config_t *config);

/**
 * @brief Initialize ILI9488 LCD driver and bind it to a LCD handle.
 *
 * The driver uses @ref esy_lcd_t internal opaque storage to hold its private
 * context. The current backend supports @ref ESY_COLOR_FORMAT_RGB565 only.
 * Initialize lcd with {0} before its first use. Repeated init of an attached
 * handle returns ESY_ERR_STATE without changing it. Init failure normally
 * leaves a detached handle that may be retried. If CS cleanup also fails, the
 * handle remains attached in fault state: retain resources and deinit before
 * retrying init. Successful deinit permits reinit.
 *
 * Public calls are externally serialized task-context operations. The provider
 * may complete a flush from another task; the backend publishes its idle state
 * atomically. Lifecycle operations must also be serialized with user callbacks.
 * Blocking control calls and init require exclusive access to the SPI bus;
 * drain other scheduler users before invoking them. Flush preparation executes
 * under the provider's bus ownership. ISR callbacks are not supported.
 *
 * @param[in,out] lcd    LCD handle to initialize (must not be NULL).
 * @param[in]     config Driver configuration (must not be NULL).
 *
 * @retval ESY_OK Success.
 * @retval ESY_ERR_INVALID_ARG Invalid pointer, pin, dimension, direction, or
 *                             missing flush submit callback.
 * @retval ESY_ERR_NOT_SUPPORTED Unsupported color format.
 * @retval ESY_ERR_STATE Handle is already attached or registered.
 * @retval <0 GPIO or SPI initialization failure.
 */
esy_err_t esy_ili9488_init(esy_lcd_t *lcd, const esy_ili9488_config_t *config);

/**
 * @brief Detach an ILI9488 driver from a LCD handle.
 *
 * The caller must stop submitting new flush requests before calling this
 * function. A queued or active flush returns ESY_ERR_BUSY and leaves the handle
 * intact. Wait for completion, release its logical binding according to
 * esy_lcd_unregister_device(), and retry. With a latched CS cleanup fault,
 * exclusively hold the bus before calling: deinit retries CS release and keeps
 * the handle on failure. ESY_OK means callback arguments may be released.
 * Repeated deinit of a detached handle succeeds; NULL is a no-op.
 *
 * @retval ESY_OK Detached, or already detached.
 * @retval ESY_ERR_BUSY A registered binding or accepted flush still owns it.
 * @retval <0 CS recovery failed; retain resources and retry under bus exclusion.
 *
 * @param[in,out] lcd LCD handle to detach (may be NULL).
 */
esy_err_t esy_ili9488_deinit(esy_lcd_t *lcd);

/**
 * @brief Query transfer readiness from the serialized producer or done callback.
 *
 * This does not access hardware or acquire the logical device. Completion
 * releases the pixel buffer even when final CS cleanup fails. Such a failure
 * latches ESY_ERR_STATE and prevents new transfers. The owner must exclude all
 * other bus users and retry deinit to release CS; failed deinit retains state.
 * This query does not report pixel I/O errors or device-registry readiness.
 * @param[in] lcd Initialized ILI9488 handle; lifetime externally protected.
 * @retval ESY_OK Idle with no latched cleanup fault.
 * @retval ESY_ERR_BUSY A transfer is accepted and not yet completed.
 * @retval ESY_ERR_STATE Detached or CS cleanup failed.
 * @retval ESY_ERR_INVALID_ARG NULL handle.
 */
esy_err_t esy_ili9488_get_status(esy_lcd_t *lcd);

/**
 * @brief Send display-on command.
 *
 * @param[in,out] lcd Initialized and idle LCD handle.
 *
 * @retval ESY_OK Command sent.
 * @retval ESY_ERR_INVALID_ARG Invalid handle.
 * @retval ESY_ERR_STATE Handle is not initialized.
 * @retval ESY_ERR_BUSY A flush is in flight.
 * @retval <0 SPI write failure.
 */
esy_err_t esy_ili9488_display_on(esy_lcd_t *lcd);

/**
 * @brief Send display-off command.
 *
 * @param[in,out] lcd Initialized and idle LCD handle.
 *
 * @retval ESY_OK Command sent.
 * @retval ESY_ERR_INVALID_ARG Invalid handle.
 * @retval ESY_ERR_STATE Handle is not initialized.
 * @retval ESY_ERR_BUSY A flush is in flight.
 * @retval <0 SPI write failure.
 */
esy_err_t esy_ili9488_display_off(esy_lcd_t *lcd);

/**
 * @brief Send sleep-in and wait 120 ms before allowing subsequent commands.
 *
 * @param[in,out] lcd Initialized and idle LCD handle.
 *
 * @retval ESY_OK Command sent.
 * @retval ESY_ERR_INVALID_ARG Invalid handle.
 * @retval ESY_ERR_STATE Handle is not initialized.
 * @retval ESY_ERR_BUSY A flush is in flight.
 * @retval <0 SPI write failure.
 */
esy_err_t esy_ili9488_sleep_in(esy_lcd_t *lcd);

/**
 * @brief Send sleep-out command and wait the profile-independent wake delay.
 *
 * @param[in,out] lcd Initialized and idle LCD handle.
 *
 * @retval ESY_OK Command sent.
 * @retval ESY_ERR_INVALID_ARG Invalid handle.
 * @retval ESY_ERR_STATE Handle is not initialized.
 * @retval ESY_ERR_BUSY A flush is in flight.
 * @retval <0 SPI write failure.
 */
esy_err_t esy_ili9488_sleep_out(esy_lcd_t *lcd);

/**
 * @brief Set panel backlight state through the optional callback.
 *
 * @param[in,out] lcd     Initialized LCD handle.
 * @param[in]     enabled Backlight target state.
 *
 * @retval ESY_OK Backlight callback accepted the state.
 * @retval ESY_ERR_INVALID_ARG Invalid handle.
 * @retval ESY_ERR_NOT_SUPPORTED No backlight callback was configured.
 * @retval <0 Callback-specific failure.
 */
esy_err_t esy_ili9488_set_backlight(esy_lcd_t *lcd, bool enabled);

/**
 * @brief Set display direction.
 *
 * @param[in,out] lcd LCD handle. The handle must be initialized and idle.
 * @param[in]     dir New direction.
 * @retval ESY_OK Direction applied.
 * @retval ESY_ERR_INVALID_ARG Invalid handle or direction.
 * @retval ESY_ERR_STATE Detached handle.
 * @retval ESY_ERR_BUSY A flush is in flight.
 * @retval <0 GPIO, SPI, or device acquisition failure.
 */
esy_err_t esy_ili9488_set_direction(esy_lcd_t *lcd, esy_lcd_dir_t dir);

#ifdef __cplusplus
}
#endif

#endif /* ESY_ILI9488_H */
