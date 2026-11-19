/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/xio/esy_hal_xio_program.h"
#include "esy_hal_backend/xio/program/esy_hb_xio_program.h"

esy_err_t esy_hal_xio_program_init(esy_xio_program_t *program,
                                  const esy_xio_program_config_t *config) {
  return esy_hb_xio_program_exec_init(program, config);
}

esy_err_t esy_hal_xio_program_deinit(esy_xio_program_t *program) {
  return esy_hb_xio_program_exec_deinit(program);
}

esy_err_t esy_hal_xio_program_sm_init(esy_xio_program_sm_t *sm,
                                     esy_xio_program_t *program,
                                     const esy_xio_program_sm_config_t *config) {
  return esy_hb_xio_program_exec_sm_init(sm, program, config);
}

esy_err_t esy_hal_xio_program_sm_deinit(esy_xio_program_sm_t *sm) {
  return esy_hb_xio_program_exec_sm_deinit(sm);
}

esy_err_t esy_hal_xio_program_sm_reset(esy_xio_program_sm_t *sm) {
  return esy_hb_xio_program_exec_sm_reset(sm);
}

esy_err_t esy_hal_xio_program_start_group(esy_xio_program_sm_t *const *sms,
                                         size_t count) {
  return esy_hb_xio_program_exec_start_group(sms, count);
}

esy_err_t esy_hal_xio_program_sm_stop(esy_xio_program_sm_t *sm) {
  return esy_hb_xio_program_exec_sm_stop(sm);
}

esy_err_t esy_hal_xio_program_tx_try_put(esy_xio_program_sm_t *sm,
                                        uint32_t word, bool *written) {
  return esy_hb_xio_program_exec_tx_try_put(sm, word, written);
}

esy_err_t esy_hal_xio_program_set_pindirs(esy_xio_program_sm_t *sm,
                                        uint8_t base, uint8_t count, bool output) {
  return esy_hb_xio_program_exec_set_pindirs(sm, base, count, output);
}

esy_err_t esy_hal_xio_program_write_pins(esy_xio_program_sm_t *sm,
                                       uint8_t base, uint8_t count, uint32_t bits) {
  return esy_hb_xio_program_exec_write_pins(sm, base, count, bits);
}

esy_err_t esy_hal_xio_program_irq_clear(esy_xio_program_sm_t *sm, uint8_t flag) {
  return esy_hb_xio_program_exec_irq_clear(sm, flag);
}

esy_err_t esy_hal_xio_program_get_status(esy_xio_program_sm_t *sm,
                                       esy_xio_program_status_t *status) {
  return esy_hb_xio_program_exec_get_status(sm, status);
}

esy_err_t esy_hal_xio_program_clear_stalls(esy_xio_program_sm_t *sm,
                                         bool tx, bool rx) {
  return esy_hb_xio_program_exec_clear_stalls(sm, tx, rx);
}

esy_err_t esy_hal_xio_program_dma_tx_start(esy_xio_program_sm_t *sm,
                                          const uint32_t *words, size_t count,
                                          const esy_xio_program_dma_config_t *config) {
  return esy_hb_xio_program_exec_dma_tx_start(sm, words, count, config);
}

esy_err_t esy_hal_xio_program_dma_rx_start(esy_xio_program_sm_t *sm,
                                          uint32_t *words, size_t count,
                                          const esy_xio_program_dma_config_t *config) {
  return esy_hb_xio_program_exec_dma_rx_start(sm, words, count, config);
}

esy_err_t esy_hal_xio_program_dma_abort(esy_xio_program_sm_t *sm,
                                       esy_xio_program_dma_direction_t direction) {
  return esy_hb_xio_program_exec_dma_abort(sm, direction);
}
