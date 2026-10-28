/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAKE_ESY_HAL_I2C_H
#define FAKE_ESY_HAL_I2C_H

#include "esy_hal/esy_hal_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file fake_esy_hal_i2c.h
 * @brief Serialized, fixed-capacity scripted I2C provider for host tests.
 *
 * Reset between tests before initializing handles. Expectations copy TX/RX
 * bytes, are consumed in FIFO order across initialized handles, and compare
 * address, lengths and TX bytes. Matching reads copy data only for nonnegative
 * scripted results. An unexpected/mismatched call returns ESY_ERR_IO and
 * increments the mismatch counter. This fake owns no IRQ, DMA or allocation.
 */

/** @brief Clear handles, expectations and observations; test-owner context only. */
void fake_esy_hal_i2c_reset(void);
/**
 * @brief Append a copied transaction expectation with its exact transfer result.
 * @return OK, INVALID_ARG for invalid buffers/address/lengths, or NO_MEM when full.
 * Capacity is 128 expectations, with up to 64 bytes in either direction.
 */
esy_err_t fake_esy_hal_i2c_expect_transfer(uint16_t addr,
    const uint8_t *tx, size_t tx_len, const uint8_t *rx, size_t rx_len,
    int32_t result);
/** @brief Require this timeout on the last queued expectation; no queued entry is a no-op. */
void fake_esy_hal_i2c_expect_timeout(uint32_t timeout_ms);
/** @brief Require this repeated-START value on the last queued expectation. */
void fake_esy_hal_i2c_expect_repeated_start(bool repeated_start);
/** @brief Return unconsumed expectations; reset clears the queue. */
size_t fake_esy_hal_i2c_pending_count(void);
/** @brief Return unexpected or mismatched transfer calls since reset. */
size_t fake_esy_hal_i2c_mismatch_count(void);
/** @brief Return structurally valid transfer calls since reset, including mismatches. */
size_t fake_esy_hal_i2c_transfer_count(void);
/** @brief Return the last valid transfer timeout, or zero after reset. */
uint32_t fake_esy_hal_i2c_last_timeout(void);

#ifdef __cplusplus
}
#endif
#endif /* FAKE_ESY_HAL_I2C_H */
