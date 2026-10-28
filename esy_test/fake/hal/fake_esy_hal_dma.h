/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file fake_esy_hal_dma.h
 * @brief Fake HAL DMA contract for unit tests.
 *
 * Single-threaded, SDK-independent fake. Identity is the caller-owned controller
 * plus its configured (dreq, channel) pair; this fake does not infer hardware
 * stream aliases or move data. Up to 32 configured routes across live handles
 * share bounded static counter storage. Handle runtime holds lifecycle/callback
 * state. Deinit releases its slots only after transfers are aborted/completed.
 * IRQ entry publishes completion for active matching pairs; owner poll retires
 * terminal state before task-context callbacks. Generic chaining is unsupported. Keep owners alive while fake operations can
 * inspect them. Fixture reset discards registrations without dereferencing owners
 * and invalidates all counter state.
 */

#ifndef FAKE_ESY_HAL_DMA_H
#define FAKE_ESY_HAL_DMA_H

#include <stddef.h>
#include <stdint.h>

#include "esy_hal/esy_hal_dma.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reset the fake DMA state.
 */
void fake_esy_hal_dma_reset(void);

/**
 * @brief Set the result returned by prepare APIs.
 */
void fake_esy_hal_dma_set_prepare_result(esy_err_t ret);

/**
 * @brief Set the result returned by start APIs.
 */
void fake_esy_hal_dma_set_start_result(esy_err_t ret);

/**
 * @brief Set the result returned by start_single APIs.
 */
void fake_esy_hal_dma_set_start_single_result(esy_err_t ret);

/** @brief Inject task-side retirement failure; success permits poll/abort retry. */
void fake_esy_hal_dma_set_abort_result(esy_err_t result);

/** @brief Inject a control error without changing prepared/started state. */
void fake_esy_hal_dma_set_control_result(esy_err_t result);

/**
 * @brief Publish a transfer error for an active owner/pair, without notification.
 * @return ESY_OK, INVALID_ARG for unknown pair, or STATE for an idle route.
 */
esy_err_t fake_esy_hal_dma_signal_error(esy_dma_t *dma, uint32_t dreq,
                                       uint8_t channel);

/**
 * @brief Set remaining beat count for one owner's configured routing pair.
 * @param[in] dma Initialized caller-owned controller.
 * @param[in] dreq Configured request/routing key.
 * @param[in] channel Configured request/channel selector.
 * @param[in] remaining Remaining native transfer units, independent of byte width.
 * @retval ESY_OK Counter updated without changing prepared/busy state.
 * @retval ESY_ERR_INVALID_ARG Owner or pair is not configured.
 */
esy_err_t fake_esy_hal_dma_set_remaining(esy_dma_t *dma, uint32_t dreq,
                                         uint8_t channel, size_t remaining);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESY_HAL_DMA_H */
