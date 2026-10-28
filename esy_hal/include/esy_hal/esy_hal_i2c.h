/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hal_i2c.h
 * @brief HAL I2C interface (handle-oriented).
 *
 * Design notes:
 * - The caller provides storage for the I2C handle; the HAL assembles
 *   backend function pointers and performs dispatch.
 * - No controller indices or numeric IDs are exposed; all operations target
 *   an @ref esy_i2c_t* handle.
 * - Return values are either @ref esy_err_t or a non-negative byte count;
 *   negative values indicate errors.
 */

#ifndef ESY_HAL_I2C_H
#define ESY_HAL_I2C_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_i2c_defs.h"
#include "esy_common/defs/peripheral/esy_dma_defs.h"

/**
 * @note Handle lifecycle: initialize caller-owned handle storage with {0} before
 * its first init. Serialize lifecycle and data operations on one owner context.
 * Repeated init, including init while registered, returns ESY_ERR_STATE without
 * changing the live handle. Stop asynchronous work before unregister/deinit.
 * Failed init dismantles partial HAL backend state when cleanup succeeds.
 * If cleanup fails, retain handle storage and retry deinit before another init.
 * Registered handles cannot be deinitialized directly. Successful deinit returns
 * an unregistered, quiescent handle to the zero state, ready for another init.
 * Pin routing is board-owned: master/slave init copies the caller config and
 * obtains SDA/SCL through the board hook, including after PM resume. ESP32
 * supports master mode only and requires two distinct, output-capable GPIOs;
 * unused/equal/invalid pins return
 * ESY_ERR_INVALID_ARG before calling the SDK. Caller pin fields do not override
 * board routing. ESP32 bus recovery resets its existing native bus while
 * retaining the accepted mapping and initialization frequency. Runtime
 * set_speed is unsupported; select another frequency in a new master init.
 * A physical controller has one handle owner. A different handle returns
 * ESY_ERR_BUSY before board pins change, including while the owner is suspended.
 */

/** @note DMA descriptors are copied at submission. Circular I2C transactions
 * are unsupported and return ESY_ERR_NOT_SUPPORTED without retaining buffers.
 * Pico ordinary DMA read/write is unsupported: valid submissions return
 * ESY_ERR_NOT_SUPPORTED without starting a bus transaction or retaining buffers
 * or callbacks. Use blocking or combined transfers on that port; its capability
 * mask does not include ESY_I2C_CAP_DMA_TRANSFER. */
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @note This initializer replaces every field. Apply caller overrides afterward.
 * @brief Fill default configuration for I2C master mode.
 *
 * Writes platform-recommended safe defaults into @p cfg. Callers may override
 * specific fields after this call.
 *
 * @param[out] cfg Configuration structure pointer; must not be NULL.
 */
extern void esy_hal_i2c_default_cfg(esy_i2c_config_t *cfg);

/**
 * @brief Initialize I2C in master mode.
 *
 * @param[in,out] i2c I2C handle provided by the caller.
 * @param[in]     cfg Master configuration.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hal_i2c_init(esy_i2c_t *i2c,
                                       const esy_i2c_config_t *cfg);

/**
 * @brief Initialize I2C in slave mode.
 * @note STM32F4 and ESP32 return ESY_ERR_NOT_SUPPORTED before claiming a
 * controller, changing board pins, enabling native IRQs or borrowing callbacks.
 *
 * @param[in,out] i2c I2C handle provided by the caller.
 * @param[in]     cfg Slave configuration.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hal_i2c_slave_init(esy_i2c_t *i2c,
                                             const esy_i2c_slave_config_t *cfg);

/**
 * @brief Deinitialize I2C.
 *
 * Releases platform resources associated with the handle.
 * A zero handle succeeds. Registered handles return ESY_ERR_STATE. Active
 * transfers/callbacks return ESY_ERR_BUSY. Native cleanup errors retain the
 * handle and its exclusive controller reservation; keep storage alive and
 * retry deinit after resolving the error. Board pins reset only after native
 * cleanup succeeds and before the reservation is released. Success zeroes the
 * handle and permits reuse. Call from the serialized owner outside ISR context.
 *
 * @param[in,out] i2c I2C handle.
 * @return ESY_OK on cleanup, ESY_ERR_INVALID_ARG for NULL, or a lifecycle error.
 */
extern esy_err_t esy_hal_i2c_deinit(esy_i2c_t *i2c);

/**
 * @brief Blocking write transaction.
 *
 * @param[in] i2c  I2C handle.
 * @param[in] addr Raw 7-bit (0..0x7f) or supported 10-bit (0..0x3ff) address.
 * @param[in] data Transmit buffer.
 * @param[in] len  Number of bytes to write.
 *
 * @return >=0 Number of bytes written; <0 error code.
 */
extern int32_t esy_hal_i2c_write_blocking(esy_i2c_t *i2c,
                                               uint16_t addr,
                                               const uint8_t *data,
                                               size_t len);

/**
 * @brief Blocking read transaction.
 *
 * @param[in]  i2c  I2C handle.
 * @param[in]  addr Raw 7-bit (0..0x7f) or supported 10-bit (0..0x3ff) address.
 * @param[out] data Receive buffer.
 * @param[in]  len  Number of bytes to read.
 *
 * @return >=0 Number of bytes read; <0 error code.
 */
extern int32_t esy_hal_i2c_read_blocking(esy_i2c_t *i2c,
                                              uint16_t addr,
                                              uint8_t *data,
                                              size_t len);

/**
 * @brief DMA-based write transaction (optional).
 *
 * @param[in] i2c     I2C handle.
 * @param[in] addr    Raw 7-bit (0..0x7f) or supported 10-bit (0..0x3ff) address.
 * @param[in] dma_cfg DMA configuration.
 *
 * @retval ESY_OK Success.
 * @retval <0    Error code.
 */
extern esy_err_t esy_hal_i2c_write_dma(esy_i2c_t *i2c,
                                            uint16_t addr,
                                            esy_dma_config_t *dma_cfg);

/**
 * @brief DMA-based read transaction (optional).
 *
 * @param[in] i2c     I2C handle.
 * @param[in] addr    Raw 7-bit (0..0x7f) or supported 10-bit (0..0x3ff) address.
 * @param[in] dma_cfg DMA configuration.
 *
 * @retval ESY_OK Success.
 * @retval <0    Error code.
 */
extern esy_err_t esy_hal_i2c_read_dma(esy_i2c_t *i2c,
                                           uint16_t addr,
                                           esy_dma_config_t *dma_cfg);

/**
 * @brief Combined transfer (optional).
 *
 * Supports compound operations such as write-then-read, repeated start,
 * and one total TX/RX timeout budget as described by @p xfer. A zero timeout
 * selects the finite 1000 ms built-in default; values above INT32_MAX are invalid.
 * Expiry returns ESY_ERR_TIMEOUT. All address entry points preserve uint16_t
 * raw addresses; the initialized mode determines the accepted address range.
 *
 * @param[in] i2c  I2C handle.
 * @param[in] xfer Transfer descriptor.
 *
 * @return >=0 Total number of bytes transferred; <0 error code.
 */
extern int32_t esy_hal_i2c_transfer(esy_i2c_t *i2c,
                                         const esy_i2c_xfer_t *xfer);

/**
 * @brief Change I2C bus speed at runtime (optional).
 * @note ESP32 returns ESY_ERR_NOT_SUPPORTED. Select a different frequency in
 * a new master initialization; the live native device is not replaced.
 *
 * @param[in] i2c      I2C handle.
 * @param[in] speed_hz Target bus speed in Hz.
 *
 * @retval ESY_OK Success.
 * @retval <0    Error code.
 */
extern esy_err_t esy_hal_i2c_set_speed(esy_i2c_t *i2c,
                                            uint32_t speed_hz);

/**
 * @brief Abort the current I2C transaction (optional).
 *
 * @param[in] i2c I2C handle.
 *
 * @retval ESY_OK Success.
 * @retval <0    Error code.
 */
extern esy_err_t esy_hal_i2c_abort(esy_i2c_t *i2c);

/**
 * @brief Attempt I2C bus recovery (optional).
 * @note ESP32 resets the existing native master bus without replacing its bus
 * or device resources. The accepted board pins and initialization frequency
 * remain in effect; recovery does not enable slave mode or change speed.
 *
 * Typically toggles SCL/SDA to recover from a stuck bus.
 *
 * @param[in] i2c I2C handle.
 *
 * @retval ESY_OK Success.
 * @retval <0    Error code.
 */
extern esy_err_t esy_hal_i2c_recover_bus(esy_i2c_t *i2c);

/**
 * @brief Query public I2C capabilities for an initialized handle.
 *
 * Uses @ref esy_i2c_cap_t, with identical bit meanings on every port.
 * Serialize with handle lifecycle. Board resources and configuration still
 * constrain each operation; DMA bits do not promise asynchronous completion.
 * Zero is valid. Ignore unknown bits; output is written only on success.
 *
 * @param[in]  i2c      I2C handle.
 * @param[out] out_caps Capability bitmask.
 *
 * @retval ESY_OK Success.
 * @retval <0    Error code.
 */
extern esy_err_t esy_hal_i2c_get_caps(const esy_i2c_t *i2c,
                                           uint32_t *out_caps);

/**
 * @brief Query physical controller index (optional).
 *
 * @param[in]  i2c       I2C handle.
 * @param[out] out_index Physical controller index.
 *
 * @retval ESY_OK Success.
 * @retval <0    Error code.
 */
extern esy_err_t esy_hal_i2c_get_phys_index(const esy_i2c_t *i2c,
                                                 uint8_t *out_index);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_I2C_H */
