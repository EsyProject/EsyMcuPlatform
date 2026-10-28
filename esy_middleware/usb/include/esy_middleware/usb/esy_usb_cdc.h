/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_MIDDLEWARE_USB_CDC_H
#define ESY_MIDDLEWARE_USB_CDC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_middleware_usb_cdc.h
 * @brief USB CDC middleware wrapper (backend-backed CDC adapter; no extra buffering).
 *
 * ## Overview
 * This module provides a thin wrapper around the linked USB CDC backend.
 * The default backend is TinyUSB CDC.
 *
 * Key properties:
 * - Requires the selected backend CDC class to be enabled.
 * - No dynamic allocation: both handle and configuration are caller-provided.
 * - Multi-interface support: instances bind by CDC interface index `itf`
 *   in range [0 .. CFG_TUD_CDC-1].
 * - No intermediate ring buffer: read/write operations directly forward to the
 *   backend CDC APIs (may short-read/short-write depending on backend FIFOs).
 *
 * ## Handle memory model (Scheme C)
 * - @ref esy_usb_cdc_t contains an internal private storage `backend`.
 * - The implementation places its internal context into `backend.bytes` and
 *   sets `cdc->ctx` to point to it.
 * - The public header does not expose internal context fields.
 *
 * ## Concurrency and callbacks
 * - RX/line-state callbacks are invoked from the USB stack callback context.
 *   Keep them short and non-blocking. They may call @ref esy_usb_cdc_read for
 *   the same interface when the linked backend allows CDC FIFO reads from that
 *   callback context.
 * - The module-level task function @ref esy_usb_cdc_task should be called
 *   periodically to drive the backend USB device stack.
 * - The module does not serialize concurrent API calls. Init/deinit and callback
 *   registration are single-owner lifecycle operations. Applications must keep
 *   read/write/flush/task calls compatible with the selected backend threading
 *   model and provide external serialization if multiple execution contexts use
 *   the same CDC interface.
 */

/* -------------------------------------------------------------------------- */
/* Private storage size                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Private bytes reserved inside @ref esy_usb_cdc_t.
 *
 * The implementation must ensure its internal context fits into this buffer.
 * You may override this macro at build time if needed.
 */
#ifndef ESY_USB_CDC_PRIV_BYTES
#define ESY_USB_CDC_PRIV_BYTES 64u
#endif

/**
 * @brief Maximum number of CDC interfaces tracked by the middleware registry.
 *
 * The effective interface count is the smaller of this value and the linked
 * backend interface count. Override this at build time when a product exposes
 * more than four CDC interfaces. Valid values are 1..255.
 */
#ifndef ESY_USB_CDC_MAX_IFACES
#define ESY_USB_CDC_MAX_IFACES 4u
#endif

#if ESY_USB_CDC_MAX_IFACES < 1 || ESY_USB_CDC_MAX_IFACES > UINT8_MAX
#error "ESY_USB_CDC_MAX_IFACES must be in 1..255"
#endif

/* -------------------------------------------------------------------------- */
/* Callbacks                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief CDC RX event callback.
 *
 * Called when interface `itf` receives data. The user may call
 * @ref esy_usb_cdc_read within this callback to fetch data.
 *
 * @note The callback should return quickly and must not perform heavy or
 * blocking operations.
 *
 * @param[in] arg User argument passed from configuration.
 * @param[in] itf CDC interface index.
 */
typedef void (*esy_usb_cdc_rx_cb)(void *arg, uint8_t itf);

/**
 * @brief CDC line state change callback (DTR/RTS).
 *
 * @param[in] arg User argument passed from configuration.
 * @param[in] itf CDC interface index.
 * @param[in] dtr DTR state.
 * @param[in] rts RTS state.
 */
typedef void (*esy_usb_cdc_line_state_cb)(void *arg, uint8_t itf, bool dtr, bool rts);

/* -------------------------------------------------------------------------- */
/* Configuration                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief CDC instance initialization configuration.
 */
typedef struct {
  uint8_t                   itf;       /**< CDC interface index to bind (0..CFG_TUD_CDC-1). */
  esy_usb_cdc_rx_cb         rx_cb;     /**< Optional notification; NULL preserves data for polling. */
  esy_usb_cdc_line_state_cb line_cb;   /**< Line state callback (may be NULL). */
  void                     *user_arg;  /**< User argument passed to callbacks. */
  /** Opt in to discarding closed-session FIFOs; see get_session_id(). */
  bool discard_on_disconnect;
} esy_usb_cdc_config_t;

/* -------------------------------------------------------------------------- */
/* Handle                                                                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief CDC handle.
 *
 * Treat this as an opaque handle:
 * - Do not modify fields directly.
 * - Use the APIs in this header.
 * - First init accepts uninitialized storage; no zero initializer is required.
 * - Keep the handle at a stable address and callback arguments alive until deinit
 *   returns. Do not copy or clear a bound handle to bypass deinit.
 * - Before first successful init, only init/deinit may use uninitialized storage.
 *   A zero-initialized or successfully deinitialized handle is detached and may
 *   also be passed to the query and I/O APIs.
 */
typedef struct esy_usb_cdc_s {
  void *ctx;  /**< Internal context pointer (read-only for users). */

  union {
    max_align_t _a;                        /**< Alignment guarantee. */
    uint8_t     bytes[ESY_USB_CDC_PRIV_BYTES]; /**< Private storage for implementation. */
  } backend;
} esy_usb_cdc_t;

/* -------------------------------------------------------------------------- */
/* API                                                                         */
/* -------------------------------------------------------------------------- */

/**
 * @brief Fill configuration structure with default values.
 *
 * Defaults:
 * - itf = 0
 * - rx_cb = NULL
 * - line_cb = NULL
 * - user_arg = NULL
 * - discard_on_disconnect = false
 *
 * @param[out] cfg Configuration object to fill (must not be NULL).
 */
void esy_usb_cdc_default_config(esy_usb_cdc_config_t *cfg);

/**
 * @brief Initialize a CDC instance and bind to an interface index.
 *
 * No initial contents are required for a first-use handle. Failures leave both
 * the handle and registry unchanged; a bound handle remains usable. A failed
 * first init may be retried or passed to deinit before other APIs are used.
 * The configuration is copied; cfg need not outlive this call. Callbacks and
 * user_arg must remain valid until deinit returns. Serialize lifecycle calls
 * with all callbacks and I/O; this API is not ISR-safe.
 *
 * @param[in,out] cdc CDC handle storage provided by caller (must not be NULL).
 * @param[in]  cfg Initialization config (must not be NULL).
 *
 * @retval ESY_OK              Success.
 * @retval ESY_ERR_INVALID_ARG Invalid arguments (NULL pointers, itf out of range).
 * @retval ESY_ERR_STATE       The specified itf or handle is already bound.
 */
esy_err_t esy_usb_cdc_init(esy_usb_cdc_t *cdc, const esy_usb_cdc_config_t *cfg);

/**
 * @brief Deinitialize a CDC instance and unbind the interface.
 *
 * After this call, non-NULL storage is cleared to the detached state, including
 * first-use storage and storage from a failed init. This synchronous operation
 * cannot fail and releases only the registry binding: it does not stop the USB
 * stack or clear backend FIFOs. Stop concurrent callbacks and I/O before calling;
 * no callback for this handle is dispatched after return. This API is not
 * ISR-safe. Repeated deinit is harmless and init may be called again.
 *
 * @param[in,out] cdc Caller-owned handle storage; NULL is ignored.
 */
void esy_usb_cdc_deinit(esy_usb_cdc_t *cdc);

/**
 * @brief Check whether the CDC interface is connected and DTR is asserted.
 *
 * @param[in] cdc CDC handle.
 * @retval true  Connected and host terminal opened (DTR=1).
 * @retval false Not connected or DTR=0.
 */
bool esy_usb_cdc_connected(const esy_usb_cdc_t *cdc);

/**
 * @brief Read the generation of a CDC instance using session isolation.
 *
 * Set config.discard_on_disconnect before init to enable this policy. DTR
 * deassertion and a disconnect observed by task() discard the backend software
 * RX/TX FIFOs and advance this generation before dispatching line callbacks.
 * Task pumping and RX notifications also discard FIFO bytes while closed.
 * Opening a port does not discard bytes already received from the new peer.
 * The generation is nonzero, wraps while skipping zero, and is valid only until
 * deinit. Compare for inequality; it is not a monotonically ordered timestamp.
 * Changes wholly between task calls are detected through DTR-close callbacks;
 * physical unplug without such a callback must be observed by task() before
 * reconnect. USB bus reset must discard provider endpoint/FIFO state as well.
 * Bytes already submitted to an endpoint cannot be recalled by this policy.
 * Use the same serialized owner as task(), I/O and callbacks; not ISR-safe.
 *
 * @param[in] cdc Initialized or detached CDC handle.
 * @return Session generation, or zero for NULL/detached handles or disabled
 * session isolation.
 */
uint32_t esy_usb_cdc_get_session_id(const esy_usb_cdc_t *cdc);

/* ------------------------------ RX (read) --------------------------------- */

/**
 * @brief Get number of bytes currently available for reading.
 *
 * @param[in] cdc CDC handle.
 * @return Number of bytes available in the backend CDC RX FIFO.
 */
uint32_t esy_usb_cdc_available(const esy_usb_cdc_t *cdc);

/**
 * @brief Read bytes from CDC.
 *
 * This reads from the backend CDC RX FIFO and may return fewer bytes than requested.
 * A NULL RX callback preserves received data for this polling API. The caller
 * must drain the FIFO regularly; otherwise backend flow control may stall RX.
 *
 * @param[in,out] cdc CDC handle.
 * @param[out]    buf Output buffer (must not be NULL).
 * @param[in]     len Requested length in bytes.
 * @return Actual number of bytes read.
 */
uint32_t esy_usb_cdc_read(esy_usb_cdc_t *cdc, void *buf, uint32_t len);

/* ------------------------------ TX (write) -------------------------------- */

/**
 * @brief Get the number of bytes currently writable without a short write.
 *
 * @param[in] cdc CDC handle.
 * @return Writable byte count in the backend TX FIFO.
 */
uint32_t esy_usb_cdc_write_available(const esy_usb_cdc_t *cdc);

/**
 * @brief Write bytes to CDC (non-blocking).
 *
 * This writes into the backend CDC TX FIFO and may short-write if FIFO space is limited.
 *
 * @param[in,out] cdc CDC handle.
 * @param[in]     buf Input data (must not be NULL).
 * @param[in]     len Number of bytes to write.
 * @return Actual number of bytes written.
 */
uint32_t esy_usb_cdc_write(esy_usb_cdc_t *cdc, const void *buf, uint32_t len);

/**
 * @brief Flush backend CDC TX FIFO (trigger immediate send).
 *
 * @param[in,out] cdc CDC handle.
 */
void esy_usb_cdc_flush(esy_usb_cdc_t *cdc);

/* -------------------------------------------------------------------------- */
/* Pump / scheduling                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief USB backend service function.
 *
 * Bare-metal usage: call every 1~2 ms (typical).
 * RTOS usage: run in a dedicated task loop or equivalent scheduler hook.
 */
void esy_usb_cdc_task(void);

/* -------------------------------------------------------------------------- */
/* USB stack hooks                                                             */
/* -------------------------------------------------------------------------- */
/*
 * Implemented by this module and linked to backend CDC callbacks in the .c:
 *   void tud_cdc_rx_cb(uint8_t itf);
 *   void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts);
 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_MIDDLEWARE_USB_CDC_H */
