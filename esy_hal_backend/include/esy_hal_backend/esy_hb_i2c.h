/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hb_i2c.h
 * @brief I2C HAL backend interface with caller-owned platform state.
 *
 * Design rules:
 * - The caller provides storage for the I2C handle and backend context.
 * - Native SDK initialization may create SDK-owned controller resources;
 *   deinitialization releases them. Transfer paths do not allocate memory.
 * - Physical controller indices are defined in @ref esy_i2c_config_t and
 *   @ref esy_i2c_slave_config_t.
 * - Unsupported features must return @ref ESY_ERR_NOT_SUPPORTED.
 * - All APIs are synchronous unless explicitly stated otherwise.
 */

#ifndef ESY_HB_I2C_H
#define ESY_HB_I2C_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_i2c_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @name HAL backend top-level APIs (handle-based, explicit extern) */
/** @{ */

/**
 * @brief Validate slave support before claiming resources or configuring pins.
 * @param[in] config Requested slave configuration; not retained.
 * @return ESY_OK, ESY_ERR_INVALID_ARG, or ESY_ERR_NOT_SUPPORTED.
 * This synchronous query changes no handle, board, IRQ or native state.
 * Pico supports 7-bit slaves; STM32F4 and ESP32 reject slave mode.
 */
extern esy_err_t esy_hb_i2c_validate_slave_config(
    const esy_i2c_slave_config_t *config);

/**
 * @brief Reserve a physical controller before changing board pins.
 * @param[in,out] i2c Zero-initialized handle or this controller's suspended owner.
 * @param[in] phys_index Physical controller index.
 * @return ESY_OK on reservation, ESY_ERR_BUSY for a different owner, or an error.
 * Lifecycle and I/O on one handle must be serialized outside ISR context.
 * Reservation remains held through suspend and failed cleanup until deinit
 * succeeds; another handle cannot configure the reserved controller's pins.
 */
extern esy_err_t esy_hb_i2c_claim(esy_i2c_t *i2c, uint8_t phys_index);

/**
 * @brief Initialize an I2C controller in master mode.
 * @note ESP32 uses one native master bus and one addressless device, allocated
 * by the SDK during initialization. One owner serializes lifecycle and I/O;
 * master transfers do not allocate SDK command links or per-address devices.
 * Direct backend init also claims the controller. Failure may retain its
 * reservation and partially initialized native resources: call deinit and
 * require ESY_OK before retry or storage reuse. HAL performs that cleanup and
 * propagates cleanup errors to its caller.
 *
 * @param[in,out] i2c     I2C handle storage provided by the caller
 * @param[in]     config  Master configuration (includes phys_index, speed, etc.)
 *
 * @retval ESY_OK  Success
 * @retval <0      Error code
 */
extern esy_err_t esy_hb_i2c_init_master(esy_i2c_t *i2c,
                                            const esy_i2c_config_t *config);

/**
 * @brief Initialize an I2C controller in slave mode.
 * @note STM32F4 and ESP32 return ESY_ERR_NOT_SUPPORTED without creating a native
 * driver, enabling IRQs or borrowing callback storage. HAL rejects this mode
 * before configuring board pins on those ports.
 * Direct backend initialization failures may retain controller ownership or
 * partial native state. Require successful deinit before retry or handle reuse.
 *
 * @param[in,out] i2c     I2C handle storage provided by the caller
 * @param[in]     config  Slave configuration (includes phys_index, address, callbacks)
 *
 * @retval ESY_OK  Success
 * @retval <0      Error code
 */
extern esy_err_t esy_hb_i2c_init_slave(esy_i2c_t *i2c,
                                           const esy_i2c_slave_config_t *config);

/**
 * @brief Deinitialize an I2C controller.
 *
 * Success releases the reservation and clears i2c->ctx. A zero handle is an
 * idempotent success. Failure retains context, callbacks and native resources
 * still owned by the handle; retain its storage and retry deinit. Do not resume
 * I/O or reinitialize after failed cleanup.
 *
 * @param[in,out] i2c I2C handle
 * @return ESY_OK on release, ESY_ERR_BUSY while active, or a native cleanup error.
 */
extern esy_err_t esy_hb_i2c_deinit(esy_i2c_t *i2c);

/**
 * @brief Stop native hardware while retaining the controller reservation.
 * @param[in,out] i2c Initialized owner, serialized with lifecycle and I/O.
 * @return ESY_OK on suspend, ESY_ERR_BUSY before cleanup, or a cleanup error.
 * Success permits init by the same owner to resume. Other errors may indicate
 * partial native cleanup; the PM adapter must report ESY_ERR_STATE to fault the
 * graph rather than claim the old active hardware state remains usable.
 */
extern esy_err_t esy_hb_i2c_suspend(esy_i2c_t *i2c);

/**
 * @brief Report this handle's native transfer or callback activity.
 * @param[in] i2c Controller owner, serialized with task lifecycle operations.
 * @return true while its own native I/O/callback is active; false when quiescent.
 * This query never inspects or claims another peripheral's physical DMA route.
 */
extern bool esy_hb_i2c_is_busy(const esy_i2c_t *i2c);

/**
 * @brief Blocking write transaction (master mode).
 * @note Uses the finite default transaction budget of
 * ESY_I2C_DEFAULT_TIMEOUT_MS. No buffers are retained after return.
 *
 * @param[in] i2c   I2C handle
 * @param[in] addr  Unshifted target address: 0..0x7f in 7-bit mode or
 *                 0..0x3ff in supported 10-bit mode (ESP32 only).
 * @param[in] data  Data buffer to write
 * @param[in] len   Number of bytes to write
 *
 * @return >=0  Number of bytes written on success
 * @return <0   Error code on failure
 */
extern int32_t esy_hb_i2c_write_blocking(esy_i2c_t *i2c,
                                             uint16_t addr,
                                             const uint8_t *data,
                                             size_t len);

/**
 * @brief Blocking read transaction (master mode).
 * @note Uses the finite default transaction budget of
 * ESY_I2C_DEFAULT_TIMEOUT_MS. No buffers are retained after return.
 *
 * @param[in]  i2c   I2C handle
 * @param[in]  addr  Unshifted target address: 0..0x7f in 7-bit mode or
 *                  0..0x3ff in supported 10-bit mode (ESP32 only).
 * @param[out] data  Receive buffer
 * @param[in]  len   Number of bytes to read
 *
 * @return >=0  Number of bytes read on success
 * @return <0   Error code on failure
 */
extern int32_t esy_hb_i2c_read_blocking(esy_i2c_t *i2c,
                                            uint16_t addr,
                                            uint8_t *data,
                                            size_t len);

/**
 * @brief Write data using DMA (master mode, optional).
 * @note Pico and STM32F4 return ESY_ERR_NOT_SUPPORTED before starting a
 * transaction or borrowing buffers/callbacks. Use blocking/combined transfers.
 *
 * @param[in] i2c         I2C handle
 * @param[in] addr        Unshifted 7-bit or supported 10-bit target address
 * @param[in] dma_config  DMA configuration
 *
 * @retval ESY_OK  Success
 * @retval <0      Error code (@ref ESY_ERR_NOT_SUPPORTED if not supported)
 */
extern esy_err_t esy_hb_i2c_write_dma(esy_i2c_t *i2c,
                                          uint16_t addr,
                                          esy_dma_config_t *dma_config);

/**
 * @brief Read data using DMA (master mode, optional).
 * @note Pico and STM32F4 return ESY_ERR_NOT_SUPPORTED before starting a
 * transaction or borrowing buffers/callbacks. Use blocking/combined transfers.
 *
 * @param[in] i2c         I2C handle
 * @param[in] addr        Unshifted 7-bit or supported 10-bit target address
 * @param[in] dma_config  DMA configuration
 *
 * @retval ESY_OK  Success
 * @retval <0      Error code (@ref ESY_ERR_NOT_SUPPORTED if not supported)
 */
extern esy_err_t esy_hb_i2c_read_dma(esy_i2c_t *i2c,
                                         uint16_t addr,
                                         esy_dma_config_t *dma_config);

/**
 * @brief Combined I2C transaction (master mode, optional).
 *
 * Supports write-only, read-only, or write-then-read with repeated START.
 * @note timeout_ms is one total transaction budget, including both phases when
 * a STOP separates write/read. Zero selects ESY_I2C_DEFAULT_TIMEOUT_MS;
 * values above INT32_MAX are invalid. Expiry returns ESY_ERR_TIMEOUT.
 * Native clock/tick granularity and bounded native
 * cleanup contribute to return latency; keep the native timebase running.
 * All ports complete synchronously and retain no buffers after return.
 *
 * @param[in] i2c   I2C handle
 * @param[in] xfer  Transaction descriptor
 *
 * @return >=0  Total number of bytes transferred
 * @return <0   Error code
 */
extern int32_t esy_hb_i2c_transfer(esy_i2c_t *i2c,
                                       const esy_i2c_xfer_t *xfer);

/**
 * @brief Set I2C bus speed at runtime (optional).
 * @note ESP32 returns ESY_ERR_NOT_SUPPORTED. Select a different frequency by
 * deinitializing and initializing the master again, avoiding runtime SDK
 * device replacement and allocation.
 *
 * @param[in] i2c       I2C handle
 * @param[in] speed_hz  Target bus speed in Hz
 *
 * @retval ESY_OK  Success
 * @retval <0      Error code (@ref ESY_ERR_NOT_SUPPORTED if not supported)
 */
extern esy_err_t esy_hb_i2c_set_speed(esy_i2c_t *i2c, uint32_t speed_hz);

/**
 * @brief Abort the current I2C transaction (optional).
 *
 * @param[in] i2c I2C handle
 *
 * @retval ESY_OK  Success
 * @retval <0      Error code
 */
extern esy_err_t esy_hb_i2c_abort(esy_i2c_t *i2c);

/**
 * @brief Attempt to recover a stuck I2C bus (optional).
 *
 * Typical use case: SDA held low by a misbehaving slave.
 *
 * @param[in] i2c I2C handle
 *
 * @retval ESY_OK  Success
 * @retval <0      Error code (@ref ESY_ERR_NOT_SUPPORTED if not supported)
 */
extern esy_err_t esy_hb_i2c_recover_bus(esy_i2c_t *i2c);

/**
 * @brief Query public I2C capabilities for an initialized handle.
 *
 * Uses @ref esy_i2c_cap_t, with identical bit meanings on every port.
 * Serialize with handle lifecycle. Board resources and configuration still
 * constrain each operation; DMA bits do not promise asynchronous completion.
 * Zero is valid. Ignore unknown bits; output is written only on success.
 *
 * @param[in]  i2c       I2C handle
 * @param[out] out_caps  Public capability bitmask
 *
 * @retval ESY_OK  Success
 * @retval <0      Error code (@ref ESY_ERR_NOT_SUPPORTED if not supported)
 */
extern esy_err_t esy_hb_i2c_get_caps(const esy_i2c_t *i2c, uint32_t *out_caps);

/**
 * @brief Query physical controller index (optional).
 *
 * @param[in]  i2c        I2C handle
 * @param[out] out_index  Physical controller index
 *
 * @retval ESY_OK  Success
 * @retval <0      Error code (@ref ESY_ERR_NOT_SUPPORTED if not supported)
 */
extern esy_err_t esy_hb_i2c_get_phys_index(const esy_i2c_t *i2c,
                                               uint8_t *out_index);

/** @} */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_HB_I2C_H */
