/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hb_usb_dev.h
 * @brief HAL backend USB Device interface (handle-based, no internal allocation).
 *
 * Key points:
 * - The caller provides storage for @ref esy_usb_dev_t; the HAL backend must not
 *   perform any heap allocation.
 * - This header declares the HAL backend API. Hardware-specific
 *   implementations live under esy_hal_backend/src/<platform>/.
 * - Unsupported capabilities must return @ref ESY_ERR_NOT_SUPPORTED.
 * - Chip-specific limits/alignment/buffer management are intentionally not
 *   declared here and must be handled inside the HAL backend implementation.
 * - The Pico provider binds TinyUSB connect/disconnect, suspend, resume and SOF
 *   events only. Generic endpoint/address/EP0 operations are unsupported and
 *   it does not emit SETUP or endpoint-transfer-complete events. The shared
 *   stack owner supplies dispatch and any class-driver adapters independently.
 */

#ifndef ESY_HB_USB_DEV_H
#define ESY_HB_USB_DEV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_usb_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @name Lifecycle */
/** @{ */

/**
 * @brief Perform board-level USB initialization for Device mode.
 *
 * On Pico this initializes the shared board/TinyUSB stack. One startup owner
 * calls it once and owns dispatch, class-driver lifetimes and final hardware
 * shutdown. Handle init/deinit only acquire/release the event callback binding;
 * they do not undo stack initialization or disconnect the shared controller.
 *
 * @note This function is board/platform specific.
 */
extern void esy_hb_usb_dev_board_init(void);

/**
 * @brief Acquire the device event-callback binding.
 *
 * Initialize caller-owned handle storage with {0} before first use. The HAL
 * backend populates its private context; the HAL wrapper binds the ops table.
 * Pico acquires only the callback binding for controller zero, after its stack
 * owner initializes TinyUSB. It does not bring up or reconnect hardware.
 * Serialize all handle lifecycle and TinyUSB callback dispatch on one owner
 * context; callbacks must not overlap deinit or reenter lifecycle calls. Keep
 * the live handle at a stable address and its callback argument alive until
 * deinit. Repeated init returns STATE unchanged; a second handle returns BUSY
 * without changing the live owner. Failed init retains no new binding and
 * permits retry. Never clear live storage to bypass deinit.
 *
 * @param[in,out] dev Device handle storage provided by the caller
 * @param[in]     cfg Device initialization config (event callback, EP0 MPS,
 *                    phys_index, etc.)
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (invalid args, missing implementation, out of
 *                resources, etc.)
 */
extern esy_err_t esy_hb_usb_dev_init(esy_usb_dev_t *dev,
                                        const esy_usb_dev_config_t *cfg);

/**
 * @brief Release this device's event binding and clear its backend context.
 *
 * Finish callback dispatch on the serialized owner first. Pico detaches the
 * callback and clears dev->ctx, permitting callback-argument reuse and another
 * init. It does not stop TinyUSB, disconnect pull-ups or disable controller
 * IRQs/clocks; their unique stack owner performs any hardware shutdown after
 * quiescing all class users. A zero or non-owning handle is unchanged.
 *
 * @param[in,out] dev Device handle
 */
extern void esy_hb_usb_dev_deinit(esy_usb_dev_t *dev);

/** @} */

/** @name Capability / Info */
/** @{ */

/**
 * @brief Query boolean capability flags.
 *
 * @param[in]  dev      Device handle
 * @param[out] out_caps Capability bitmask (e.g. @ref ESY_USB_CAP_HIGH_SPEED)
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (@ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_usb_dev_get_caps(const esy_usb_dev_t *dev,
                                            uint32_t *out_caps);

/**
 * @brief Query the physical controller index (for diagnostics).
 *
 * @param[in]  dev       Device handle
 * @param[out] out_index Physical controller index
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (@ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_usb_dev_get_phys_index(const esy_usb_dev_t *dev,
                                                  uint8_t *out_index);

/** @} */

/** @name Control & Endpoints */
/** @{ */

/**
 * @brief Set the USB device address.
 *
 * Typically takes effect after the status stage of the standard SET_ADDRESS request.
 *
 * @param[in] dev  Device handle
 * @param[in] addr Device address (0..127)
 *
 * @retval ESY_OK Success
 * @retval <0     Error code
 */
extern esy_err_t esy_hb_usb_dev_set_address(esy_usb_dev_t *dev, uint8_t addr);

/**
 * @brief Initialize (enable) a non-control endpoint.
 *
 * @param[in] dev Device handle
 * @param[in] cfg Endpoint configuration (excludes EP0; includes number, direction,
 *                type, MPS, interval)
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (@ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_usb_dev_ep_init(esy_usb_dev_t *dev,
                                           const esy_usb_ep_cfg_t *cfg);

/**
 * @brief Deinitialize (disable) a non-control endpoint.
 *
 * @param[in] dev     Device handle
 * @param[in] ep_addr Endpoint address (bit7=direction, bit3..0=endpoint number)
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (@ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_usb_dev_ep_deinit(esy_usb_dev_t *dev, uint8_t ep_addr);

/**
 * @brief Set or clear STALL on an endpoint.
 *
 * @param[in] dev     Device handle
 * @param[in] ep_addr Endpoint address
 * @param[in] stall   true to STALL; false to clear STALL
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (@ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_usb_dev_ep_stall(esy_usb_dev_t *dev,
                                            uint8_t ep_addr,
                                            bool stall);

/**
 * @brief Submit an endpoint transfer (asynchronous).
 *
 * Completion is reported through the event callback via @ref esy_usb_event_t,
 * with event type @ref ESY_USB_EVT_EP_XFER_COMPLETE.
 *
 * @param[in] dev     Device handle
 * @param[in] ep_addr Endpoint address
 * @param[in] xfer    Transfer request (buffer, length, flags, user_data, etc.)
 *
 * @retval ESY_OK Success (queued/submitted)
 * @retval <0     Error code (@ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_usb_dev_ep_xfer(esy_usb_dev_t *dev,
                                           uint8_t ep_addr,
                                           esy_usb_xfer_t *xfer);

/**
 * @brief Cancel an in-flight transfer on an endpoint (if supported).
 *
 * @param[in] dev     Device handle
 * @param[in] ep_addr Endpoint address
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (@ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_usb_dev_ep_cancel(esy_usb_dev_t *dev, uint8_t ep_addr);

/** @} */

/** @name EP0 helpers (optional; a HAL backend may choose not to implement) */
/** @{ */

/**
 * @brief Read the EP0 data stage (OUT).
 *
 * @param[in]  dev Device handle
 * @param[out] buf Receive buffer
 * @param[in]  len Requested length
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (@ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_usb_dev_ep0_read(esy_usb_dev_t *dev,
                                            uint8_t *buf,
                                            uint16_t len);

/**
 * @brief Write the EP0 data stage (IN).
 *
 * @param[in] dev Device handle
 * @param[in] buf Transmit buffer (may be NULL when len==0)
 * @param[in] len Transmit length
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (@ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_usb_dev_ep0_write(esy_usb_dev_t *dev,
                                             const uint8_t *buf,
                                             uint16_t len);

/**
 * @brief Perform the EP0 status stage handshake.
 *
 * @param[in] dev Device handle
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (@ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_usb_dev_ep0_status(esy_usb_dev_t *dev);

/** @} */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_HB_USB_DEV_H */
