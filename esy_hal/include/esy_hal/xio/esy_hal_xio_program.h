/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_XIO_PROGRAM_H
#define ESY_HAL_XIO_PROGRAM_H

#include "esy_common/defs/peripheral/xio/esy_xio_program_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_hal_xio_program.h
 * @brief Caller-owned RP PIO v0 program execution, synchronized SMs and FIFO DMA.
 *
 * Feature ENABLE_ESY_XIO_PROGRAM requires ENABLE_ESY_XIO and a Pico-family
 * target. Link esy_hal. No OS, heap, SDK header or native resource pointer is
 * required. One task/main-loop owner serializes every API and cooperating SMs;
 * only DMA callbacks run in ISR context. No API in this header is ISR-safe.
 * Initialize all storage with {0}; never copy or clear live objects. Programs
 * outlive their SMs, and SMs outlive DMA callbacks. GPIO/IRQ assignment and the
 * bytecode's electrical/cycle correctness belong to the configuration owner.
 */

/** @brief Borrow valid bytecode; repeated init returns STATE unchanged. */
esy_err_t esy_hal_xio_program_init(esy_xio_program_t *program,
                                  const esy_xio_program_config_t *config);
/** @brief BUSY with live SMs; success returns storage to {0}; idempotent. */
esy_err_t esy_hal_xio_program_deinit(esy_xio_program_t *program);
/** @brief Claim/configure a disabled SM; failure releases temporary resources.
 * @note Register and shift state match @ref esy_hal_xio_program_sm_reset.
 */
esy_err_t esy_hal_xio_program_sm_init(esy_xio_program_sm_t *sm,
                                     esy_xio_program_t *program,
                                     const esy_xio_program_sm_config_t *config);
/**
 * @brief Stop SM and abort both DMA routes before release; idempotent.
 * Cleanup failure retains the handle, program reference, callbacks and buffers.
 * Establish electrically safe idle pins before deinit when required: stopping
 * and releasing an SM preserves its output latches, not an assumed idle level.
 */
esy_err_t esy_hal_xio_program_sm_deinit(esy_xio_program_sm_t *sm);
/**
 * @brief Reset a disabled SM to entry, empty FIFOs and clear shift/register state.
 * BUSY if enabled or either DMA route still claimed, including completed DMA.
 * X/Y/ISR/OSR values are zero; the ISR shift count is zero and the OSR shift
 * count is 32 (empty), so autopull fetches TX data before the first OUT.
 * Does not change GPIO output levels or clear shared PIO IRQ flags.
 */
esy_err_t esy_hal_xio_program_sm_reset(esy_xio_program_sm_t *sm);
/**
 * @brief Atomically restart dividers and enable 1..4 disabled SMs in one PIO.
 * All entries must be distinct and initialized; validation failure starts none.
 * Does not reset PCs, FIFOs, shift state or IRQ flags. Preload/arm DMA first.
 * Same divider configuration gives synchronized PIO cycles; the owner proves
 * instruction-level phase relationships. Calling again on an enabled SM is BUSY.
 */
esy_err_t esy_hal_xio_program_start_group(esy_xio_program_sm_t *const *sms,
                                         size_t count);
/** @brief Disable this SM only; DMA and borrowed buffers remain owned. */
esy_err_t esy_hal_xio_program_sm_stop(esy_xio_program_sm_t *sm);
/** @brief Nonblocking FIFO put; written is false when full; output required. */
esy_err_t esy_hal_xio_program_tx_try_put(esy_xio_program_sm_t *sm,
                                        uint32_t word, bool *written);
/** @brief Set a GPIO range's direction while disabled; no DMA may be active. */
esy_err_t esy_hal_xio_program_set_pindirs(esy_xio_program_sm_t *sm,
                                        uint8_t base, uint8_t count, bool output);
/** @brief Set GPIO output latches while disabled; other GPIOs are preserved. */
esy_err_t esy_hal_xio_program_write_pins(esy_xio_program_sm_t *sm,
                                       uint8_t base, uint8_t count, uint32_t bits);
/** @brief Clear an owned PIO IRQ flag 0..7; serialize every program using it. */
esy_err_t esy_hal_xio_program_irq_clear(esy_xio_program_sm_t *sm, uint8_t flag);
/** @brief Read a hardware snapshot; failure preserves status. */
esy_err_t esy_hal_xio_program_get_status(esy_xio_program_sm_t *sm,
                                       esy_xio_program_status_t *status);
/** @brief Explicitly clear selected sticky stall flags without restarting. */
esy_err_t esy_hal_xio_program_clear_stalls(esy_xio_program_sm_t *sm,
                                         bool tx, bool rx);
/**
 * @brief Start finite 32-bit TX DMA; buffer is borrowed through completion/error.
 * Nonempty aligned words/count are required. Another claimed TX route is BUSY,
 * including completed transfers until abort. Init/configure the SM first; it
 * may remain disabled while the TX FIFO is prefilled. Failure before prepare
 * borrows nothing; failed native start is retired before returning when possible.
 * A cleanup failure retains the route and storage; call dma_abort before reuse.
 */
esy_err_t esy_hal_xio_program_dma_tx_start(esy_xio_program_sm_t *sm,
                                          const uint32_t *words, size_t count,
                                          const esy_xio_program_dma_config_t *config);
/** @brief Start finite 32-bit RX DMA; same lifetime/errors as TX DMA. */
esy_err_t esy_hal_xio_program_dma_rx_start(esy_xio_program_sm_t *sm,
                                          uint32_t *words, size_t count,
                                          const esy_xio_program_dma_config_t *config);
/**
 * @brief Retire DMA and callbacks synchronously from the owner task/main loop.
 * Stop the associated SM first when cancellation must prevent further edges.
 * Success releases the DMA route and all borrowed buffer/callback references;
 * failure retains ownership for retry. No cancellation callback is generated.
 * Already unclaimed directions return OK. No timeout/wall-clock wait is added;
 * native task-context DMA abort may block until hardware/callback retirement.
 */
esy_err_t esy_hal_xio_program_dma_abort(esy_xio_program_sm_t *sm,
                                       esy_xio_program_dma_direction_t direction);

#ifdef __cplusplus
}
#endif
#endif
