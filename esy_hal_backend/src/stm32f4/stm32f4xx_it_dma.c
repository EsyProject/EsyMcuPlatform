/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_dma.h"

/* Backend request IDs enumerate DMA1 streams first, followed by DMA2. */
void DMA1_Stream0_IRQHandler(void) { esy_hb_dma_handle_irq(0u, 0u); }
void DMA1_Stream1_IRQHandler(void) { esy_hb_dma_handle_irq(1u, 0u); }
void DMA1_Stream2_IRQHandler(void) { esy_hb_dma_handle_irq(2u, 0u); }
void DMA1_Stream3_IRQHandler(void) { esy_hb_dma_handle_irq(3u, 0u); }
void DMA1_Stream4_IRQHandler(void) { esy_hb_dma_handle_irq(4u, 0u); }
void DMA1_Stream5_IRQHandler(void) { esy_hb_dma_handle_irq(5u, 0u); }
void DMA1_Stream6_IRQHandler(void) { esy_hb_dma_handle_irq(6u, 0u); }
void DMA1_Stream7_IRQHandler(void) { esy_hb_dma_handle_irq(7u, 0u); }
void DMA2_Stream0_IRQHandler(void) { esy_hb_dma_handle_irq(8u, 0u); }
void DMA2_Stream1_IRQHandler(void) { esy_hb_dma_handle_irq(9u, 0u); }
void DMA2_Stream2_IRQHandler(void) { esy_hb_dma_handle_irq(10u, 0u); }
void DMA2_Stream3_IRQHandler(void) { esy_hb_dma_handle_irq(11u, 0u); }
void DMA2_Stream4_IRQHandler(void) { esy_hb_dma_handle_irq(12u, 0u); }
void DMA2_Stream5_IRQHandler(void) { esy_hb_dma_handle_irq(13u, 0u); }
void DMA2_Stream6_IRQHandler(void) { esy_hb_dma_handle_irq(14u, 0u); }
void DMA2_Stream7_IRQHandler(void) { esy_hb_dma_handle_irq(15u, 0u); }
