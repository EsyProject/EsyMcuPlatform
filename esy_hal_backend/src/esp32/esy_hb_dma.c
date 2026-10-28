/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_dma.h"

esy_err_t esy_hb_dma_claim(const void *owner, uint32_t dreq, uint8_t channel) {
    (void)dreq;
    (void)channel;
    return owner == NULL ? ESY_ERR_INVALID_ARG : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_dma_release(const void *owner, uint32_t dreq, uint8_t channel) {
    (void)dreq;
    (void)channel;
    return owner == NULL ? ESY_ERR_INVALID_ARG : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_dma_get_resource_id(uint32_t dreq, uint8_t channel,
                                   uint8_t *out_id) {
    (void)dreq;
    (void)channel;
    return out_id == NULL ? ESY_ERR_INVALID_ARG : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_dma_prepare(const void *owner, const esy_dma_config_t *transfer) {
    (void)owner;
    (void)transfer;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_dma_start(const void *owner, uint32_t channel_mask) {
    (void)owner;
    (void)channel_mask;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_dma_start_single(const void *owner, const esy_dma_config_t *transfer) {
    (void)owner;
    (void)transfer;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_dma_abort(const void *owner, uint32_t dreq, uint8_t channel) {
    (void)owner;
    (void)dreq;
    (void)channel;
    return ESY_ERR_NOT_SUPPORTED;
}

bool esy_hb_dma_is_busy(uint32_t dreq, uint8_t channel) {
    (void)dreq;
    (void)channel;
    return false;
}

esy_err_t esy_hb_dma_bind_irq_to_core(uint32_t irq_index, uint32_t core_id) {
    (void)irq_index;
    (void)core_id;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_dma_set_read_addr(const void *owner, uint32_t dreq, uint8_t channel,
                                  const void *addr, bool trigger_now) {
    (void)owner;
    (void)dreq;
    (void)channel;
    (void)addr;
    (void)trigger_now;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_dma_set_write_addr(const void *owner, uint32_t dreq, uint8_t channel, void *addr,
                                   bool trigger_now) {
    (void)owner;
    (void)dreq;
    (void)channel;
    (void)addr;
    (void)trigger_now;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_dma_set_trans_count(const void *owner, uint32_t dreq, uint8_t channel,
                                    size_t count, bool trigger_now) {
    (void)owner;
    (void)dreq;
    (void)channel;
    (void)count;
    (void)trigger_now;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_dma_get_trans_count_remaining(uint32_t dreq,
                                                   uint8_t channel,
                                                   size_t *out_remaining) {
    (void)dreq;
    (void)channel;
    if (!out_remaining) {
        return ESY_ERR_INVALID_ARG;
    }
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_dma_chain_to(const void *owner, uint32_t dreq, uint8_t channel,
                             uint8_t chain_channel) {
    (void)owner;
    (void)dreq;
    (void)channel;
    (void)chain_channel;
    return ESY_ERR_NOT_SUPPORTED;
}

void esy_hb_dma_handle_irq(uint32_t dreq, uint8_t channel) {
    (void)dreq;
    (void)channel;
}
