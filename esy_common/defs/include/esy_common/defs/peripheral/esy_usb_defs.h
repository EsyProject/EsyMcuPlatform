/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_usb_defs.h
 * @brief Generic USB types and **Device-mode** vtable/handle definitions
 * (platform-agnostic).
 *
 * Design constraints:
 * - Types/enums/callbacks/vtables/handles only; **no HAL backend implementation**
 * here.
 * - Host-mode types may be added later, but the HAL backend API remains separated
 * from Device.
 * - The handle provides a fixed-size in-place private storage to avoid dynamic
 * allocation.
 * - Chip-specific limits/alignment/cache rules are **NOT** defined here; the
 * HAL backend implementation manages those details internally.
 */

#ifndef ESY_USB_DEFS_H
#define ESY_USB_DEFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h" /* esy_err_t */

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Configurable: private context placeholder */
/* -------------------------------------------------------------------------- */

/**
 * @def ESY_USB_PRIV_BYTES
 * @brief Size (in bytes) of the in-handle private storage placeholder
 * (compile-time overridable).
 *
 * @note This value is decoupled from any specific SoC. It exists to reduce heap
 * usage:
 *       - A HAL backend may store its private context in @ref
 * esy_usb_dev_t::backend.bytes, or
 *       - It may keep the real context elsewhere and put the pointer in @ref
 * esy_usb_dev_t::ctx.
 */
#ifndef ESY_USB_PRIV_BYTES
#define ESY_USB_PRIV_BYTES 256
#endif

/* -------------------------------------------------------------------------- */
/* Basic enums and types */
/* -------------------------------------------------------------------------- */

/**
 * @brief USB speed.
 */
typedef enum {
  ESY_USB_SPEED_LOW = 0,  /**< 1.5 Mbps (rarely used for Device) */
  ESY_USB_SPEED_FULL = 1, /**< 12 Mbps */
  ESY_USB_SPEED_HIGH = 2  /**< 480 Mbps (if supported by hardware) */
} esy_usb_speed_t;

/**
 * @brief Endpoint direction.
 */
typedef enum {
  ESY_USB_DIR_OUT = 0, /**< Host -> Device */
  ESY_USB_DIR_IN = 1   /**< Device -> Host */
} esy_usb_dir_t;

/**
 * @brief Endpoint transfer type.
 */
typedef enum {
  ESY_USB_EP_CONTROL = 0, /**< Control endpoint (EP0 is always CONTROL) */
  ESY_USB_EP_ISO = 1,     /**< Isochronous */
  ESY_USB_EP_BULK = 2,    /**< Bulk */
  ESY_USB_EP_INT = 3      /**< Interrupt */
} esy_usb_ep_type_t;

/**
 * @brief SETUP packet structure (matches the USB 2.0 standard layout).
 */
typedef struct {
  uint8_t bmRequestType; /**< Request type bitmap */
  uint8_t bRequest;      /**< Request code */
  uint16_t wValue;       /**< Value field */
  uint16_t wIndex;       /**< Index field */
  uint16_t wLength;      /**< Data stage length */
} esy_usb_setup_pkt_t;

/**
 * @brief Device endpoint configuration (**excluding EP0**).
 */
typedef struct {
  uint8_t ep_num;         /**< Endpoint number: 1..15; 0 is reserved for EP0 */
  esy_usb_dir_t dir;      /**< Direction: IN/OUT */
  esy_usb_ep_type_t type; /**< Type: BULK/INT/ISO (EP0 is fixed CONTROL) */
  uint16_t mps;           /**< Max Packet Size */
  uint8_t interval;       /**< Polling interval (meaningful for INT/ISO) */
} esy_usb_ep_cfg_t;

/**
 * @brief Transfer flags (extensible).
 */
typedef enum {
  ESY_USB_XFER_NONE = 0, /**< No special flags */
  ESY_USB_XFER_ALLOW_SHORT =
      1u << 0, /**< A short packet may be treated as completion */
  ESY_USB_XFER_ZLP =
      1u << 1 /**< Append a ZLP when required (common for IN BULK) */
} esy_usb_xfer_flags_t;

/**
 * @brief Transfer request (asynchronous semantics).
 *
 * The transfer is submitted and returns immediately; completion is reported via
 * events.
 */
typedef struct {
  uint8_t* buf; /**< Buffer pointer; OUT: provided by upper layer; IN: consumed
                   by controller */
  uint32_t
      len; /**< Requested transfer length (HAL backend may split internally) */
  uint32_t flags;  /**< @ref esy_usb_xfer_flags_t */
  void* user_data; /**< Opaque user context passed back in completion events */
  volatile uint32_t actual; /**< Actual bytes transferred (filled by HAL backend on
                               completion) */
} esy_usb_xfer_t;

/* -------------------------------------------------------------------------- */
/* Event model */
/* -------------------------------------------------------------------------- */

/**
 * @brief Device-side event types.
 */
typedef enum {
  /* Link / power management */
  ESY_USB_EVT_CONNECT = 0, /**< Connected (may carry speed information) */
  ESY_USB_EVT_DISCONNECT,  /**< Disconnected */
  ESY_USB_EVT_RESET,       /**< Bus reset */
  ESY_USB_EVT_SUSPEND,     /**< Suspended */
  ESY_USB_EVT_RESUME,      /**< Resumed */
  ESY_USB_EVT_SOF,         /**< Start Of Frame */

  /* Control / endpoint related */
  ESY_USB_EVT_SETUP,            /**< EP0 SETUP packet received */
  ESY_USB_EVT_EP_XFER_COMPLETE, /**< A transfer on an endpoint completed */
  ESY_USB_EVT_EP_STALL          /**< Endpoint entered STALL */
} esy_usb_evt_type_t;

/**
 * @brief Event payload.
 */
typedef struct {
  esy_usb_evt_type_t type; /**< Event type */
  union {
    struct {
      esy_usb_speed_t speed; /**< Negotiated speed (if known) */
    } connect;               /**< Connect event payload */

    struct {
      esy_usb_setup_pkt_t setup; /**< SETUP packet snapshot */
    } setup;                     /**< Setup event payload */

    struct {
      uint8_t ep_addr;  /**< Endpoint address (bit7=dir, bit3..0=number) */
      esy_err_t status; /**< Transfer result */
      uint32_t actual;  /**< Actual bytes transferred */
      void* user_data;  /**< Passthrough from @ref esy_usb_xfer_t::user_data */
    } xfer;             /**< Endpoint transfer completion payload */
  } u;
} esy_usb_event_t;

/**
 * @brief Event callback prototype.
 *
 * @param[in] cb_arg  User argument provided at registration time
 * @param[in] ev      Event pointer (read-only)
 *
 * @note The callback may be invoked from an ISR context. It **must return
 * quickly**. Defer heavy work to a task/thread/worker.
 */
typedef void (*esy_usb_event_cb_t)(void* cb_arg, const esy_usb_event_t* ev);

/* -------------------------------------------------------------------------- */
/* Device initialization configuration */
/* -------------------------------------------------------------------------- */

/**
 * @brief Optional internal configuration (HAL backend may ignore).
 */
typedef struct {
  uint8_t irq_priority; /**< Interrupt priority (if the HAL backend supports it) */
} esy_usb_inner_config_t;

/**
 * @brief USB Device initialization configuration.
 */
typedef struct {
  uint8_t phys_index; /**< Physical controller index (platform-defined) */
  esy_usb_speed_t
      speed_hint; /**< Desired speed hint (best-effort; HAL backend may ignore) */
  uint16_t ep0_mps;            /**< EP0 Max Packet Size (typical FS=64) */
  esy_usb_event_cb_t event_cb; /**< Event callback */
  void* event_arg;             /**< Callback passthrough argument */
  esy_usb_inner_config_t
      inner; /**< HAL backend internal configuration (optional) */
} esy_usb_dev_config_t;

/* -------------------------------------------------------------------------- */
/* Capability flags (boolean capabilities only; numeric limits stay
 * backend-specific) */
/* -------------------------------------------------------------------------- */

/** @brief Supports Device mode. */
#define ESY_USB_CAP_DEVICE     (1u << 0)
/** @brief Supports High-Speed operation. */
#define ESY_USB_CAP_HIGH_SPEED (1u << 1)
/** @brief Supports isochronous endpoints. */
#define ESY_USB_CAP_ISO        (1u << 2)
/** @brief Has DMA capability. */
#define ESY_USB_CAP_DMA        (1u << 3)

/* -------------------------------------------------------------------------- */
/* Vtable and handle (Device) */
/* -------------------------------------------------------------------------- */

/* Forward declaration for vtable prototypes. */
struct esy_usb_dev_s;
struct esy_device_s;
typedef struct esy_usb_dev_s esy_usb_dev_t;

/**
 * @brief USB Device function table (bound by the HAL wrapper).
 *
 * Semantic notes:
 * - **Asynchronous I/O**: @ref ep_xfer submits and returns immediately;
 * completion is reported via the event callback.
 * - EP0 may use dedicated @ref ep0_* helpers, or the generic @ref ep_xfer with
 * EP0 address.
 * - Unimplemented features must return @ref ESY_ERR_NOT_SUPPORTED.
 */
typedef struct {
  /**
   * @brief Set USB device address (typically takes effect after the status
   * stage).
   */
  esy_err_t (*set_address)(esy_usb_dev_t* dev, uint8_t addr);

  /**
   * @brief Initialize (enable) a non-control endpoint.
   *
   * @param[in] dev Device handle
   * @param[in] cfg Endpoint configuration
   */
  esy_err_t (*ep_init)(esy_usb_dev_t* dev, const esy_usb_ep_cfg_t* cfg);

  /**
   * @brief Deinitialize (disable) a non-control endpoint.
   *
   * @param[in] dev     Device handle
   * @param[in] ep_addr Endpoint address (bit7=dir, bit3..0=number)
   */
  esy_err_t (*ep_deinit)(esy_usb_dev_t* dev, uint8_t ep_addr);

  /**
   * @brief Set or clear STALL on an endpoint.
   */
  esy_err_t (*ep_stall)(esy_usb_dev_t* dev, uint8_t ep_addr, bool stall);

  /**
   * @brief Submit a transfer on an endpoint (asynchronous).
   *
   * @param[in] dev     Device handle
   * @param[in] ep_addr Endpoint address
   * @param[in] xfer    Transfer descriptor
   *
   * @note Completion is reported as @ref ESY_USB_EVT_EP_XFER_COMPLETE.
   */
  esy_err_t (*ep_xfer)(esy_usb_dev_t* dev, uint8_t ep_addr,
                       esy_usb_xfer_t* xfer);

  /**
   * @brief Cancel an ongoing transfer on an endpoint (if supported).
   */
  esy_err_t (*ep_cancel)(esy_usb_dev_t* dev, uint8_t ep_addr);

  /* ---- EP0 helpers (optional) ---- */

  /**
   * @brief EP0 data stage read (OUT).
   */
  esy_err_t (*ep0_read)(esy_usb_dev_t* dev, uint8_t* buf, uint16_t len);

  /**
   * @brief EP0 data stage write (IN).
   */
  esy_err_t (*ep0_write)(esy_usb_dev_t* dev, const uint8_t* buf, uint16_t len);

  /**
   * @brief EP0 status stage handshake.
   */
  esy_err_t (*ep0_status)(esy_usb_dev_t* dev);

  /* ---- Queries ---- */

  /**
   * @brief Get boolean capability flags.
   */
  esy_err_t (*get_caps)(const esy_usb_dev_t* dev, uint32_t* out_caps);

  /**
   * @brief Get physical controller index (debugging).
   */
  esy_err_t (*get_phys_index)(const esy_usb_dev_t* dev, uint8_t* out_index);
} esy_usb_vtable_dev_t;

/** @brief Opaque HAL capacity for the optional registered PM binding. */
#define ESY_USB_HAL_STORAGE_BYTES (sizeof(void *))

/**
 * @brief USB Device handle (caller-provided storage; no heap allocation).
 *
 * - @ref ops: shared immutable provider table, bound by HAL during init,
 * - @ref ctx: backend private context pointer (read-only; must not be
 * interpreted/modified),
 * - @ref backend: fixed-size storage area that the HAL backend may use as a
 * private context pool.
 */
typedef struct esy_usb_dev_s {
  const esy_usb_vtable_dev_t *ops; /**< Shared provider table; HAL-owned binding. */
  void* ctx;                   /**< HAL backend private context pointer */
  union {
    void *alignment; /**< Private HAL state alignment. */
    uint8_t bytes[ESY_USB_HAL_STORAGE_BYTES]; /**< Opaque PM binding storage. */
  } hal; /**< HAL-owned state; never inspect, copy or modify while live. */

  union {
    max_align_t _a;                    /**< Alignment guarantee */
    uint8_t bytes[ESY_USB_PRIV_BYTES]; /**< Private storage placeholder */
  } backend;
} esy_usb_dev_t;

/* -------------------------------------------------------------------------- */
/* Helpers */
/* -------------------------------------------------------------------------- */

/**
 * @brief Build an endpoint address.
 *
 * @param[in] ep_num Endpoint number (0..15; 0 is EP0)
 * @param[in] dir    Direction (IN/OUT)
 * @return Endpoint address (bit7=dir, bit3..0=endpoint number)
 */
static inline uint8_t esy_usb_ep_addr(uint8_t ep_num, esy_usb_dir_t dir) {
  return (uint8_t)((ep_num & 0x0Fu) | (dir == ESY_USB_DIR_IN ? 0x80u : 0x00u));
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_USB_DEFS_H */
