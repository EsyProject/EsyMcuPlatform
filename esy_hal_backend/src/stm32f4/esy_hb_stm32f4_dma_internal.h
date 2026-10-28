/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HB_STM32F4_DMA_INTERNAL_H
#define ESY_HB_STM32F4_DMA_INTERNAL_H

#include "stm32f4xx_hal.h"

/**
 * @brief Borrow the generic DMA owner's already prepared STM32 stream handle.
 *
 * Backend-module implementation only. The ADC native provider must link this
 * exact handle, rather than allocate another stream owner or start it twice.
 * Task-owned prepare/start/abort must be serialized; no ownership is transferred.
 * NULL means the owner token, prepared route, or request channel does not match.
 * @param[in] owner Stable token used to prepare the stream.
 */
DMA_HandleTypeDef *esy_hb_stm32f4_dma_get_handle(const void *owner, uint32_t dreq, uint8_t channel);

#endif /* ESY_HB_STM32F4_DMA_INTERNAL_H */
