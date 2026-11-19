/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HB_XIO_PROGRAM_H
#define ESY_HB_XIO_PROGRAM_H

#include "esy_common/defs/peripheral/xio/esy_xio_program_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @file esy_hb_xio_program.h
 * @brief Provider boundary for public RP PIO v0 execution contracts.
 * Parameters, ownership and errors match the public HAL execution interface.
 * Native SDK objects and FIFO addresses stay inside the Pico implementation.
 */

esy_err_t esy_hb_xio_program_exec_init(esy_xio_program_t *program,
                                  const esy_xio_program_config_t *config);

esy_err_t esy_hb_xio_program_exec_deinit(esy_xio_program_t *program);

esy_err_t esy_hb_xio_program_exec_sm_init(esy_xio_program_sm_t *sm,
                                     esy_xio_program_t *program,
                                     const esy_xio_program_sm_config_t *config);

esy_err_t esy_hb_xio_program_exec_sm_deinit(esy_xio_program_sm_t *sm);

esy_err_t esy_hb_xio_program_exec_sm_reset(esy_xio_program_sm_t *sm);

esy_err_t esy_hb_xio_program_exec_start_group(esy_xio_program_sm_t *const *sms,
                                         size_t count);

esy_err_t esy_hb_xio_program_exec_sm_stop(esy_xio_program_sm_t *sm);

esy_err_t esy_hb_xio_program_exec_tx_try_put(esy_xio_program_sm_t *sm,
                                        uint32_t word, bool *written);

esy_err_t esy_hb_xio_program_exec_set_pindirs(esy_xio_program_sm_t *sm,
                                        uint8_t base, uint8_t count, bool output);

esy_err_t esy_hb_xio_program_exec_write_pins(esy_xio_program_sm_t *sm,
                                       uint8_t base, uint8_t count, uint32_t bits);

esy_err_t esy_hb_xio_program_exec_irq_clear(esy_xio_program_sm_t *sm, uint8_t flag);

esy_err_t esy_hb_xio_program_exec_get_status(esy_xio_program_sm_t *sm,
                                       esy_xio_program_status_t *status);

esy_err_t esy_hb_xio_program_exec_clear_stalls(esy_xio_program_sm_t *sm,
                                         bool tx, bool rx);

esy_err_t esy_hb_xio_program_exec_dma_tx_start(esy_xio_program_sm_t *sm,
                                          const uint32_t *words, size_t count,
                                          const esy_xio_program_dma_config_t *config);

esy_err_t esy_hb_xio_program_exec_dma_rx_start(esy_xio_program_sm_t *sm,
                                          uint32_t *words, size_t count,
                                          const esy_xio_program_dma_config_t *config);

esy_err_t esy_hb_xio_program_exec_dma_abort(esy_xio_program_sm_t *sm,
                                       esy_xio_program_dma_direction_t direction);

#ifdef __cplusplus
}
#endif
#endif
