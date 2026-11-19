/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAKE_ESY_HAL_XIO_PROGRAM_H
#define FAKE_ESY_HAL_XIO_PROGRAM_H

#include "esy_hal/xio/esy_hal_xio_program.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Deterministic API operation selected for failure/count observation. */
typedef enum {
  FAKE_XIO_PROGRAM_INIT,
  FAKE_XIO_PROGRAM_DEINIT,
  FAKE_XIO_PROGRAM_SM_INIT,
  FAKE_XIO_PROGRAM_SM_DEINIT,
  FAKE_XIO_PROGRAM_SM_RESET,
  FAKE_XIO_PROGRAM_START_GROUP,
  FAKE_XIO_PROGRAM_SM_STOP,
  FAKE_XIO_PROGRAM_TX_PUT,
  FAKE_XIO_PROGRAM_SET_DIRS,
  FAKE_XIO_PROGRAM_WRITE_PINS,
  FAKE_XIO_PROGRAM_IRQ_CLEAR,
  FAKE_XIO_PROGRAM_STATUS,
  FAKE_XIO_PROGRAM_CLEAR_STALLS,
  FAKE_XIO_PROGRAM_DMA_TX_START,
  FAKE_XIO_PROGRAM_DMA_RX_START,
  FAKE_XIO_PROGRAM_DMA_ABORT,
  FAKE_XIO_PROGRAM_OPERATION_COUNT
} fake_xio_program_operation_t;

/** @brief Clear all fake resources, failures and counters between test cases. */
void fake_esy_hal_xio_program_reset(void);
/** @brief Fail the next selected operation once; ESY_OK cancels that injection. */
void fake_esy_hal_xio_program_fail_next(fake_xio_program_operation_t operation,
                                       esy_err_t result);
/** @brief Number of attempts for an operation since reset. */
size_t fake_esy_hal_xio_program_call_count(fake_xio_program_operation_t operation);
/** @brief Find an initialized fake SM by explicit hardware resource. */
esy_xio_program_sm_t *fake_esy_hal_xio_program_find_sm(uint8_t instance,
                                                     uint8_t sm_index);
/** @brief Inspect the SM's copied configuration; NULL if not initialized. */
const esy_xio_program_sm_config_t *fake_esy_hal_xio_program_sm_config(
    esy_xio_program_sm_t *sm);
/** @brief Inspect its program's borrowed bytecode configuration. */
const esy_xio_program_config_t *fake_esy_hal_xio_program_config(
    esy_xio_program_sm_t *sm);
/** @brief Set deterministic PC/FIFO/stall/enabled hardware state. */
esy_err_t fake_esy_hal_xio_program_set_status(esy_xio_program_sm_t *sm,
                                             const esy_xio_program_status_t *status);
/** @brief Inspect all fake output latches; bit positions are GPIO numbers. */
uint32_t fake_esy_hal_xio_program_output_bits(esy_xio_program_sm_t *sm);
/** @brief Get the borrowed DMA memory and its word count; NULL if unclaimed.
 * Tests may fill RX memory before completion; TX memory remains read-only.
 */
const uint32_t *fake_esy_hal_xio_program_dma_words(
    esy_xio_program_sm_t *sm, esy_xio_program_dma_direction_t direction,
    size_t *count);
/** @brief Fill an active RX buffer without completing it; exact count required. */
esy_err_t fake_esy_hal_xio_program_fill_rx(esy_xio_program_sm_t *sm,
                                         const uint32_t *words, size_t count);
/**
 * @brief Synchronously invoke the retained ISR-style notification once.
 * Does not execute bytecode, advance PC, model GPIO edges or release the route.
 * Tests set status and RX memory explicitly. Repeated completion returns STATE.
 */
esy_err_t fake_esy_hal_xio_program_complete_dma(
    esy_xio_program_sm_t *sm, esy_xio_program_dma_direction_t direction,
    esy_err_t status);

#ifdef __cplusplus
}
#endif
#endif
