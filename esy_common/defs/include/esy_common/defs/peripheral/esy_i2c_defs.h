/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_I2C_DEFS_H
#define ESY_I2C_DEFS_H

/**
 * @file esy_i2c_defs.h
 * @brief Common I2C type definitions and backend vtable interface.
 *
 * This header defines the platform-independent I2C abstraction:
 * - unified handle type and fixed-size private backend storage (no dynamic
 * allocation),
 * - configuration structures for master and slave roles,
 * - combined transfer descriptor (write/read, optional repeated-start),
 * - callback prototypes for slave mode,
 * - backend function table (vtable) bound by HAL implementations.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_dma_defs.h"

/**
 * @brief Size of the private backend storage embedded in the I2C handle.
 *
 * The HAL backend can store its private context in this buffer to avoid
 * dynamic memory allocation.
 */
#define ESY_I2C_PRIV_BYTES 96

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Forward Declarations */
/* -------------------------------------------------------------------------- */

/**
 * @brief Forward declaration of the unified I2C handle type.
 *
 * Kept consistent with the definition in the HAL backend so it can be
 * referenced by callbacks and the vtable.
 */
struct esy_i2c_s;
struct esy_device_s;
typedef struct esy_i2c_s esy_i2c_t;

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
  ESY_I2C_CAP_MASTER_TRANSFER = 1u << 0, /**< Master blocking write/read/transfer entry points. */
  ESY_I2C_CAP_REPEATED_START = 1u << 1, /**< Combined master write/read without an intervening STOP. */
  ESY_I2C_CAP_DMA_TRANSFER = 1u << 2, /**< Master write_dma/read_dma entry points; may complete synchronously. */
  ESY_I2C_CAP_RECOVERY = 1u << 3, /**< Master recover_bus entry point; may reset only the controller/driver. */
} esy_i2c_cap_t;

/* -------------------------------------------------------------------------- */
/* Enumerations */
/* -------------------------------------------------------------------------- */

/**
 * @brief I2C operating role.
 */
typedef enum {
  ESY_I2C_ROLE_MASTER, /**< Master role */
  ESY_I2C_ROLE_SLAVE   /**< Slave role */
} esy_i2c_role_t;

/**
 * @brief I2C addressing mode.
 */
typedef enum {
  ESY_I2C_ADDR_7BIT, /**< 7-bit address (not left-shifted) */
  ESY_I2C_ADDR_10BIT /**< 10-bit address */
} esy_i2c_addr_mode_t;

/** @brief Largest raw address accepted in 7-bit mode. */
#define ESY_I2C_ADDR_7BIT_MAX 0x7fu
/** @brief Largest raw address accepted in supported 10-bit mode. */
#define ESY_I2C_ADDR_10BIT_MAX 0x3ffu
/** @brief Finite default total transaction budget used by built-in ports. */
#define ESY_I2C_DEFAULT_TIMEOUT_MS 1000u

/* -------------------------------------------------------------------------- */
/* Callback Types                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief Generic callback type for master-mode asynchronous operations.
 *
 * @param[in] arg User-provided argument.
 */
typedef void (*esy_i2c_callback_t)(void *arg);

/**
 * @brief Slave-mode receive callback.
 *
 * Called when the master writes data to the slave.
 * Runs in the native IRQ context on Pico/STM32. It must be bounded,
 * non-blocking and must not log, change controller lifecycle or restart I/O.
 * Copy required data before returning; the supplied bytes are borrowed.
 * Retain callback context and handle storage until successful deinit.
 *
 * @param[in] i2c  I2C handle
 * @param[in] data Pointer to received data
 * @param[in] len  Number of received bytes
 */
typedef void (*esy_i2c_slave_receive_cb)(esy_i2c_t *i2c, const uint8_t *data,
                                         size_t len);

/**
 * @brief Slave-mode request callback.
 *
 * Called when the master requests data (master reads). The slave must return
 * one byte to be provided to the master.
 * Runs in IRQ context on Pico/STM32. It must be bounded and non-blocking,
 * without logging, controller lifecycle changes or I/O restart.
 *
 * @param[in] i2c I2C handle
 * @return One byte to be returned to the master
 */
typedef uint8_t (*esy_i2c_slave_request_cb)(esy_i2c_t *i2c);

/**
 * @brief Slave-mode transfer-finish callback.
 *
 * Called after a STOP or RESTART condition is observed.
 * Runs in IRQ context on Pico/STM32. Only publish bounded notifications;
 * do not change controller lifecycle, restart I/O, block or log.
 *
 * @param[in] i2c I2C handle
 */
typedef void (*esy_i2c_slave_finish_cb)(esy_i2c_t *i2c);

/* -------------------------------------------------------------------------- */
/* Configuration Structures */
/* -------------------------------------------------------------------------- */

/**
 * @brief Internal controller configuration (platform/board specific).
 * @note HAL initialization replaces the pin fields with ESY_PIN_UNUSED before
 * the board hook supplies physical routing. ESP32 requires distinct SDA/SCL
 * GPIOs that support output; ports with fixed board routing may ignore them.
 * These are library GPIO numbers, never native SDK enum/register types.
 */
typedef struct {
  uint8_t irq_priority; /**< IRQ priority (platform-defined; smaller value =
                           higher priority) */
  uint8_t sda_pin; /**< Board-completed GPIO number; ESY_PIN_UNUSED if absent. */
  uint8_t scl_pin; /**< Board-completed GPIO number; ESY_PIN_UNUSED if absent. */
} esy_i2c_inner_config_t;

/**
 * @brief I2C master configuration.
 *
 * @note @ref phys_index selects the physical I2C controller instance.
 */
typedef struct {
  uint8_t phys_index;            /**< Physical controller index */
  uint32_t speed_hz;             /**< Bus speed in Hz */
  esy_i2c_addr_mode_t addr_mode; /**< Address mode (7-bit / 10-bit) */
  esy_i2c_inner_config_t inner;  /**< Internal controller parameters */
} esy_i2c_config_t;

/**
 * @brief I2C slave configuration.
 *
 * @note @ref phys_index selects the physical I2C controller instance.
 *       @ref address and @ref addr_mode define the slave address.
 */
typedef struct {
  uint8_t phys_index; /**< Physical controller index */
  uint16_t
      address; /**< Slave address (7-bit not shifted, or 10-bit raw value) */
  esy_i2c_addr_mode_t addr_mode; /**< Address mode (7-bit / 10-bit) */
  esy_i2c_slave_receive_cb
      receive_cb; /**< Called when master writes to slave */
  esy_i2c_slave_request_cb
      request_cb;                    /**< Called when master reads from slave */
  esy_i2c_slave_finish_cb finish_cb; /**< Called after STOP/RESTART */
  esy_i2c_inner_config_t inner;      /**< Internal controller parameters */
} esy_i2c_slave_config_t;

/**
 * @brief Combined I2C transfer descriptor.
 *
 * Used by a backend transfer function (e.g. write-then-read).
 * - TX/RX lengths may be 0.
 * - Addresses are raw, unshifted values: 0..0x7f in 7-bit mode or
 *   0..0x3ff in supported 10-bit mode. Every transfer entry preserves all bits.
 * - timeout_ms is one total transaction budget, including both phases. Built-in
 *   ports use a finite 1000 ms budget when it is zero. Values above INT32_MAX
 *   are invalid. A provider must report TIMEOUT when the budget expires; the
 *   deadline does not make this synchronous API ISR-safe.
 * - If @ref repeated_start is true, a repeated START is used between write and
 * read (no STOP in between).
 */
typedef struct {
  uint16_t addr;     /**< Target slave address (7-bit not shifted, or 10-bit raw
                        value) */
  const uint8_t *tx; /**< TX buffer (may be NULL) */
  size_t tx_len;     /**< TX length in bytes */
  uint8_t *rx;       /**< RX buffer (may be NULL) */
  size_t rx_len;     /**< RX length in bytes */
  bool repeated_start; /**< true to use repeated START between TX and RX */
  uint32_t timeout_ms; /**< Total budget in ms; 0 selects the finite 1000 ms default. */
  bool prefer_dma;     /**< Hint to prefer DMA if available (implementation may
                          ignore) */
} esy_i2c_xfer_t;

/* -------------------------------------------------------------------------- */
/* Backend Vtable */
/* -------------------------------------------------------------------------- */

/**
 * @brief I2C backend function table (vtable).
 *
 * - All functions take @ref esy_i2c_t* as the first argument.
 * - Every address argument is a raw 7-bit or supported 10-bit value in uint16_t.
 *   Providers reject addresses outside the configured mode before bus activity.
 * - Unimplemented functions should return @ref ESY_ERR_NOT_IMPLEMENTED
 *   or @ref ESY_ERR_NOT_SUPPORTED as appropriate.
 */
typedef struct {
  /**
   * @brief Blocking write.
   * @param[in] i2c  I2C handle
   * @param[in] addr Slave address
   * @param[in] data TX buffer
   * @param[in] len  Number of bytes to write
   * @return Number of bytes written on success, negative error code on failure
   */
  int32_t (*write_blocking)(esy_i2c_t *i2c, uint16_t addr, const uint8_t *data,
                            size_t len);

  /**
   * @brief Blocking read.
   * @param[in]  i2c  I2C handle
   * @param[in]  addr Slave address
   * @param[out] data RX buffer
   * @param[in]  len  Number of bytes to read
   * @return Number of bytes read on success, negative error code on failure
   */
  int32_t (*read_blocking)(esy_i2c_t *i2c, uint16_t addr, uint8_t *data,
                           size_t len);

  /**
   * @brief DMA write (optional).
   * @param[in] i2c     I2C handle
   * @param[in] addr    Slave address
   * @param[in] dma_cfg DMA configuration
   * @return Result code
   */
  esy_err_t (*write_dma)(esy_i2c_t *i2c, uint16_t addr,
                         esy_dma_config_t *dma_cfg);

  /**
   * @brief DMA read (optional).
   * @param[in] i2c     I2C handle
   * @param[in] addr    Slave address
   * @param[in] dma_cfg DMA configuration
   * @return Result code
   */
  esy_err_t (*read_dma)(esy_i2c_t *i2c, uint16_t addr,
                        esy_dma_config_t *dma_cfg);

  /**
   * @brief Combined transfer (optional).
   * @param[in] i2c  I2C handle
   * @param[in] xfer Transfer descriptor
   * @return Total bytes transferred on success, negative error code on failure
   */
  int32_t (*transfer)(esy_i2c_t *i2c, const esy_i2c_xfer_t *xfer);

  /**
   * @brief Set bus speed (optional).
   * @param[in] i2c      I2C handle
   * @param[in] speed_hz Target bus speed in Hz
   * @return Result code
   */
  esy_err_t (*set_speed)(esy_i2c_t *i2c, uint32_t speed_hz);

  /**
   * @brief Abort the current transaction (optional).
   * @param[in] i2c I2C handle
   * @return Result code
   */
  esy_err_t (*abort)(esy_i2c_t *i2c);

  /**
   * @brief Bus recovery (optional).
   *
   * Typical scenario: SDA/SCL held low by a misbehaving slave; implementation
   * may attempt clock pulses on SCL and re-init the controller.
   *
   * @param[in] i2c I2C handle
   * @return Result code
   */
  esy_err_t (*recover_bus)(esy_i2c_t *i2c);

  /**
   * @brief Get capability flags (optional).
   * @param[in]  i2c      I2C handle
   * @param[out] out_caps Capability bitmask
   * @return Result code
   */
  esy_err_t (*get_caps)(const esy_i2c_t *i2c, uint32_t *out_caps);

  /**
   * @brief Get physical controller index (optional).
   * @param[in]  i2c       I2C handle
   * @param[out] out_index Physical controller index
   * @return Result code
   */
  esy_err_t (*get_phys_index)(const esy_i2c_t *i2c, uint8_t *out_index);
} esy_i2c_vtable_t;

/* -------------------------------------------------------------------------- */
/* Handle Definition */
/* -------------------------------------------------------------------------- */

/** @brief Private HAL storage capacity; rebuild all consumers after layout changes. */
#define ESY_I2C_HAL_STORAGE_BYTES (1u * sizeof(void *))

/**
 * @brief Caller-owned I2C handle; initialize with {0} before first use.
 *
 * The HAL selects one shared immutable ops table. Callers must not replace it,
 * copy a live handle, inspect private state or clear storage to bypass cleanup.
 * ctx and backend remain the port's opaque storage contract; hal is owned only
 * by the HAL lifecycle and I/O implementation. Keep the handle and every borrowed
 * resource alive until successful deinit, including failed cleanup retries.
 */
typedef struct esy_i2c_s {
  const esy_i2c_vtable_t *ops; /**< Shared immutable HAL/provider dispatch. */
  void *ctx;                   /**< Backend private context pointer */
  union {
    void *alignment; /**< Natural pointer alignment for private HAL state. */
    uint8_t bytes[ESY_I2C_HAL_STORAGE_BYTES]; /**< Do not inspect or modify. */
  } hal; /**< Opaque HAL-owned lifecycle state. */

  union {
    max_align_t _a;                    /**< Alignment helper */
    uint8_t bytes[ESY_I2C_PRIV_BYTES]; /**< Backend private storage */
  } backend;
} esy_i2c_t;

#ifdef __cplusplus
}
#endif

#endif /* ESY_I2C_DEFS_H */
