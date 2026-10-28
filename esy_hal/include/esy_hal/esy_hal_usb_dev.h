/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hal_usb_dev.h
 * @brief HAL USB Device interface (handle-based).
 *
 * Design notes:
 * - The caller provides storage for the device handle (@ref esy_usb_dev_t);
 *   the HAL assembles backend context and operation tables.
 * - All operations are performed via @ref esy_usb_dev_t* handles;
 *   no controller index or instance ID is exposed in the API.
 * - APIs return @ref esy_err_t or other explicit types;
 *   unsupported features return @ref ESY_ERR_NOT_SUPPORTED.
 * - Chip-specific limits, alignment requirements, and cache coherency
 *   are handled internally by the HAL backend.
 */

#ifndef ESY_HAL_USB_DEV_H
#define ESY_HAL_USB_DEV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_usb_defs.h"

/**
 * @note Handle lifecycle: initialize caller-owned handle storage with {0} before
 * its first init. Serialize lifecycle and data operations on one owner context.
 * Repeated init, including init while registered, returns ESY_ERR_STATE without
 * changing the live handle. Stop asynchronous work before unregister/deinit.
 * The built-in Pico port provides controller index 0 only. Another handle for
 * that controller returns ESY_ERR_BUSY and leaves the current callback owner
 * unchanged. Serialize TinyUSB dispatch, callbacks and lifecycle on the same
 * owner context; callbacks may not overlap deinit or reinitialize the device.
 * Pico init/deinit own only this HAL event binding. The separate TinyUSB stack
 * owner initializes, dispatches and stops the shared controller and class users.
 * Handle deinit does not disconnect USB or disable the stack, IRQs or clocks.
 * Failed init dismantles partial HAL backend state and permits retry.
 * Registered handles cannot be deinitialized directly. Successful deinit returns
 * an unregistered handle with no retained event callback to the zero state,
 * ready for another init. Controller shutdown is a separate owner operation.
 * The Pico event binding forwards only TinyUSB connect/disconnect, suspend,
 * resume and SOF callbacks dispatched by that shared stack. Generic endpoint,
 * device-address and EP0 control operations return ESY_ERR_NOT_SUPPORTED; this
 * port does not publish SETUP or endpoint-transfer-complete events. Class
 * traffic, including CDC, belongs to its separate TinyUSB class adapter.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** @name Configuration & Lifecycle
 *  @{
 */

/**
 * @note This initializer replaces every field. Apply caller overrides afterward.
 * @brief Fill a USB Device configuration structure with default values.
 *
 * Provides a safe and usable default configuration (e.g. EP0 MPS,
 * default speed hints). Callers may modify the fields before passing
 * the configuration to @ref esy_hal_usb_dev_init.
 *
 * @param[out] cfg Configuration structure to initialize; must not be NULL.
 */
extern void esy_hal_usb_dev_default_cfg(esy_usb_dev_config_t *cfg);

/**
 * @brief Perform board-level USB initialization.
 *
 * On Pico this initializes the shared board/TinyUSB stack. Its unique startup
 * owner calls it once before binding a HAL device and owns TinyUSB dispatch,
 * class-driver lifetimes and final controller shutdown. Handle deinit does not
 * undo this initialization or establish hardware quiet for sleep or shutdown.
 */
extern void esy_hal_usb_dev_board_init(void);

/**
 * @brief Bind a USB Device handle and its event callback.
 *
 * The HAL binds @ref esy_usb_dev_t::ops and backend context. On Pico this only
 * acquires the exclusive event-callback binding for controller zero; it does
 * not initialize, reconnect or acquire ownership of the shared TinyUSB stack.
 * Its unique stack owner must already have completed board initialization.
 * Serialize callback dispatch and handle lifecycle on the same owner context.
 *
 * @param[in,out] dev Device handle (storage provided by the caller).
 * @param[in]     cfg Device initialization configuration.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code (invalid arguments, resource exhaustion,
 *                hardware initialization failure, etc.).
 */
extern esy_err_t esy_hal_usb_dev_init(esy_usb_dev_t *dev,
                                           const esy_usb_dev_config_t *cfg);

/**
 * @brief Detach the USB Device handle and release its event binding.
 *
 * Unregister the handle and finish its callback dispatch first. Successful
 * detachment clears the handle and permits releasing its callback argument.
 * On Pico it leaves the shared TinyUSB stack, controller, pull-ups, interrupts
 * and clocks running. The stack owner separately quiesces class users and stops
 * USB when hardware quiet is needed. Deinit of a zero handle is harmless;
 * registered handles are left unchanged until they have been unregistered.
 *
 * @param[in,out] dev Device handle.
 */
extern void esy_hal_usb_dev_deinit(esy_usb_dev_t *dev);

/** @} */

/** @name Capability & Information
 *  @{
 */

/**
 * @brief Query device capability flags.
 *
 * @param[in]  dev      Device handle.
 * @param[out] out_caps Capability bitmask (e.g. @ref ESY_USB_CAP_HIGH_SPEED).
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported).
 */
extern esy_err_t esy_hal_usb_dev_get_caps(const esy_usb_dev_t *dev,
                                               uint32_t *out_caps);

/**
 * @brief Query physical controller index (for diagnostics).
 *
 * @param[in]  dev        Device handle.
 * @param[out] out_index  Physical controller index (platform-defined).
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported).
 */
extern esy_err_t esy_hal_usb_dev_get_phys_index(const esy_usb_dev_t *dev,
                                                     uint8_t *out_index);

/** @} */

/** @name Control Plane & Endpoints
 *  @{
 */

/**
 * @brief Set USB device address.
 *
 * Typically applied after the status stage of the standard
 * SET_ADDRESS request.
 *
 * @param[in] dev  Device handle.
 * @param[in] addr Device address (0..127).
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hal_usb_dev_set_address(esy_usb_dev_t *dev, uint8_t addr);

/**
 * @brief Initialize (enable) a non-control endpoint.
 *
 * @param[in] dev Device handle.
 * @param[in] cfg Endpoint configuration (excluding EP0; includes
 *                endpoint number, direction, type, MPS, interval).
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported).
 */
extern esy_err_t esy_hal_usb_dev_ep_init(esy_usb_dev_t *dev,
                                              const esy_usb_ep_cfg_t *cfg);

/**
 * @brief Deinitialize (disable) a non-control endpoint.
 *
 * @param[in] dev     Device handle.
 * @param[in] ep_addr Endpoint address (bit7 = direction, bit3..0 = endpoint number).
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported).
 */
extern esy_err_t esy_hal_usb_dev_ep_deinit(esy_usb_dev_t *dev, uint8_t ep_addr);

/**
 * @brief Set or clear the STALL condition on an endpoint.
 *
 * @param[in] dev     Device handle.
 * @param[in] ep_addr Endpoint address.
 * @param[in] stall   true to set STALL; false to clear STALL.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported).
 */
extern esy_err_t esy_hal_usb_dev_ep_stall(esy_usb_dev_t *dev,
                                               uint8_t ep_addr,
                                               bool stall);

/**
 * @brief Submit an asynchronous endpoint transfer.
 *
 * Completion shall be reported via the event callback using
 * @ref esy_usb_event_t with event type
 * @ref ESY_USB_EVT_EP_XFER_COMPLETE.
 *
 * @param[in] dev     Device handle.
 * @param[in] ep_addr Endpoint address.
 * @param[in] xfer    Transfer request (buffer, length, flags, user_data, etc.).
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported).
 */
extern esy_err_t esy_hal_usb_dev_ep_xfer(esy_usb_dev_t *dev,
                                              uint8_t ep_addr,
                                              esy_usb_xfer_t *xfer);

/**
 * @brief Cancel an in-progress endpoint transfer (if supported).
 *
 * @param[in] dev     Device handle.
 * @param[in] ep_addr Endpoint address.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported).
 */
extern esy_err_t esy_hal_usb_dev_ep_cancel(esy_usb_dev_t *dev, uint8_t ep_addr);

/** @} */

/** @name EP0 Helpers (Optional, platform-dependent)
 *  @{
 */

/**
 * @brief EP0 OUT data stage read.
 *
 * @param[in]  dev Device handle.
 * @param[out] buf Receive buffer.
 * @param[in]  len Number of bytes to read.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported).
 */
extern esy_err_t esy_hal_usb_dev_ep0_read(esy_usb_dev_t *dev,
                                               uint8_t *buf,
                                               uint16_t len);

/**
 * @brief EP0 IN data stage write.
 *
 * @param[in] dev Device handle.
 * @param[in] buf Transmit buffer (may be NULL if len == 0).
 * @param[in] len Number of bytes to transmit.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported).
 */
extern esy_err_t esy_hal_usb_dev_ep0_write(esy_usb_dev_t *dev,
                                                const uint8_t *buf,
                                                uint16_t len);

/**
 * @brief EP0 status stage handshake.
 *
 * @param[in] dev Device handle.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported).
 */
extern esy_err_t esy_hal_usb_dev_ep0_status(esy_usb_dev_t *dev);

/** @} */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_HAL_USB_DEV_H */
