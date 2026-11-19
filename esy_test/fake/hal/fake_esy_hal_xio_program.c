/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Provider mode reuses this deterministic fake behind the real HAL forwarder.
 * Normal consumer tests leave it undefined and replace public HAL directly. */
#ifdef FAKE_ESY_XIO_PROGRAM_PROVIDER
#define esy_hal_xio_program_init esy_hb_xio_program_exec_init
#define esy_hal_xio_program_deinit esy_hb_xio_program_exec_deinit
#define esy_hal_xio_program_sm_init esy_hb_xio_program_exec_sm_init
#define esy_hal_xio_program_sm_deinit esy_hb_xio_program_exec_sm_deinit
#define esy_hal_xio_program_sm_reset esy_hb_xio_program_exec_sm_reset
#define esy_hal_xio_program_start_group esy_hb_xio_program_exec_start_group
#define esy_hal_xio_program_sm_stop esy_hb_xio_program_exec_sm_stop
#define esy_hal_xio_program_tx_try_put esy_hb_xio_program_exec_tx_try_put
#define esy_hal_xio_program_set_pindirs esy_hb_xio_program_exec_set_pindirs
#define esy_hal_xio_program_write_pins esy_hb_xio_program_exec_write_pins
#define esy_hal_xio_program_irq_clear esy_hb_xio_program_exec_irq_clear
#define esy_hal_xio_program_get_status esy_hb_xio_program_exec_get_status
#define esy_hal_xio_program_clear_stalls esy_hb_xio_program_exec_clear_stalls
#define esy_hal_xio_program_dma_tx_start esy_hb_xio_program_exec_dma_tx_start
#define esy_hal_xio_program_dma_rx_start esy_hb_xio_program_exec_dma_rx_start
#define esy_hal_xio_program_dma_abort esy_hb_xio_program_exec_dma_abort
#endif

#include "fake_esy_hal_xio_program.h"
#include <string.h>

#define FAKE_PROGRAM_COUNT 8u
#define FAKE_SM_COUNT 8u
#define FAKE_FIFO_WORDS 4u
#define FAKE_GPIO_COUNT 32u

typedef struct {
  esy_xio_program_t *handle;
  esy_xio_program_config_t config;
} fake_program_t;

typedef struct {
  const uint32_t *words;
  size_t count;
  esy_xio_program_dma_config_t config;
  bool owned;
  bool active;
} fake_dma_t;

typedef struct {
  esy_xio_program_sm_t *handle;
  fake_program_t *program;
  esy_xio_program_sm_config_t config;
  esy_xio_program_status_t status;
  fake_dma_t dma[2];
} fake_sm_t;

typedef struct {
  fake_program_t programs[FAKE_PROGRAM_COUNT];
  fake_sm_t sms[FAKE_SM_COUNT];
  esy_err_t failures[FAKE_XIO_PROGRAM_OPERATION_COUNT];
  size_t calls[FAKE_XIO_PROGRAM_OPERATION_COUNT];
  uint32_t outputs;
  uint32_t directions;
} fake_esy_hal_xio_program_ctx_t;

static fake_esy_hal_xio_program_ctx_t s_ctx;

static bool valid_operation(fake_xio_program_operation_t operation) {
  return operation >= FAKE_XIO_PROGRAM_INIT &&
         operation < FAKE_XIO_PROGRAM_OPERATION_COUNT;
}

static esy_err_t operation_result(fake_xio_program_operation_t operation) {
  ++s_ctx.calls[operation];
  esy_err_t result = s_ctx.failures[operation];
  s_ctx.failures[operation] = ESY_OK;
  return result;
}

static fake_program_t *find_program(esy_xio_program_t *program) {
  if (program == NULL) return NULL;
  for (size_t i = 0u; i < FAKE_PROGRAM_COUNT; ++i)
    if (s_ctx.programs[i].handle == program) return &s_ctx.programs[i];
  return NULL;
}

static fake_sm_t *find_sm(esy_xio_program_sm_t *sm) {
  if (sm == NULL) return NULL;
  for (size_t i = 0u; i < FAKE_SM_COUNT; ++i)
    if (s_ctx.sms[i].handle == sm) return &s_ctx.sms[i];
  return NULL;
}

static bool valid_direction(esy_xio_program_dma_direction_t direction) {
  return direction == ESY_XIO_PROGRAM_DMA_TX || direction == ESY_XIO_PROGRAM_DMA_RX;
}

static bool valid_range(uint8_t base, uint8_t count, uint8_t maximum) {
  return count <= maximum &&
      (count == 0u || (base < FAKE_GPIO_COUNT && count <= FAKE_GPIO_COUNT - base));
}

static uint32_t range_mask(uint8_t base, uint8_t count) {
  return (count == FAKE_GPIO_COUNT ? UINT32_MAX : (UINT32_C(1) << count) - 1u)
      << base;
}

void fake_esy_hal_xio_program_reset(void) { memset(&s_ctx, 0, sizeof(s_ctx)); }

void fake_esy_hal_xio_program_fail_next(fake_xio_program_operation_t operation,
                                       esy_err_t result) {
  if (valid_operation(operation)) s_ctx.failures[operation] = result;
}

size_t fake_esy_hal_xio_program_call_count(fake_xio_program_operation_t operation) {
  return valid_operation(operation) ? s_ctx.calls[operation] : 0u;
}

esy_xio_program_sm_t *fake_esy_hal_xio_program_find_sm(uint8_t instance,
                                                     uint8_t sm_index) {
  for (size_t i = 0u; i < FAKE_SM_COUNT; ++i)
    if (s_ctx.sms[i].handle != NULL && s_ctx.sms[i].config.instance == instance &&
        s_ctx.sms[i].config.sm_index == sm_index)
      return s_ctx.sms[i].handle;
  return NULL;
}

const esy_xio_program_sm_config_t *fake_esy_hal_xio_program_sm_config(
    esy_xio_program_sm_t *sm) {
  fake_sm_t *state = find_sm(sm);
  return state != NULL ? &state->config : NULL;
}

const esy_xio_program_config_t *fake_esy_hal_xio_program_config(
    esy_xio_program_sm_t *sm) {
  fake_sm_t *state = find_sm(sm);
  return state != NULL ? &state->program->config : NULL;
}

esy_err_t fake_esy_hal_xio_program_set_status(esy_xio_program_sm_t *sm,
                                             const esy_xio_program_status_t *status) {
  fake_sm_t *state = find_sm(sm);
  if (state == NULL || status == NULL) return ESY_ERR_INVALID_ARG;
  state->status = *status;
  return ESY_OK;
}

uint32_t fake_esy_hal_xio_program_output_bits(esy_xio_program_sm_t *sm) {
  return find_sm(sm) != NULL ? s_ctx.outputs : 0u;
}

const uint32_t *fake_esy_hal_xio_program_dma_words(
    esy_xio_program_sm_t *sm, esy_xio_program_dma_direction_t direction,
    size_t *count) {
  fake_sm_t *state = find_sm(sm);
  if (state == NULL || !valid_direction(direction) || !state->dma[direction].owned)
    return NULL;
  if (count != NULL) *count = state->dma[direction].count;
  return state->dma[direction].words;
}

esy_err_t fake_esy_hal_xio_program_fill_rx(esy_xio_program_sm_t *sm,
                                         const uint32_t *words, size_t count) {
  fake_sm_t *state = find_sm(sm);
  if (state == NULL || words == NULL) return ESY_ERR_INVALID_ARG;
  fake_dma_t *dma = &state->dma[ESY_XIO_PROGRAM_DMA_RX];
  if (!dma->active) return ESY_ERR_STATE;
  if (count != dma->count) return ESY_ERR_INVALID_ARG;
  memcpy((void *)dma->words, words, count * sizeof(uint32_t));
  return ESY_OK;
}

esy_err_t fake_esy_hal_xio_program_complete_dma(
    esy_xio_program_sm_t *sm, esy_xio_program_dma_direction_t direction,
    esy_err_t status) {
  fake_sm_t *state = find_sm(sm);
  if (state == NULL || !valid_direction(direction)) return ESY_ERR_INVALID_ARG;
  fake_dma_t *dma = &state->dma[direction];
  if (!dma->active) return ESY_ERR_STATE;
  dma->active = false;
  if (dma->config.callback != NULL) dma->config.callback(dma->config.user, status);
  return ESY_OK;
}

esy_err_t esy_hal_xio_program_init(esy_xio_program_t *program,
                                  const esy_xio_program_config_t *config) {
  esy_err_t result = operation_result(FAKE_XIO_PROGRAM_INIT);
  if (result != ESY_OK) return result;
  if (program == NULL || config == NULL || config->instructions == NULL ||
      config->instruction_count == 0u || config->instruction_count > 32u ||
      config->entry >= config->instruction_count ||
      config->wrap_bottom > config->wrap_top ||
      config->wrap_top >= config->instruction_count)
    return ESY_ERR_INVALID_ARG;
  if (find_program(program) != NULL) return ESY_ERR_STATE;
  for (size_t i = 0u; i < FAKE_PROGRAM_COUNT; ++i) {
    if (s_ctx.programs[i].handle == NULL) {
      s_ctx.programs[i].handle = program;
      s_ctx.programs[i].config = *config;
      return ESY_OK;
    }
  }
  return ESY_ERR_NO_MEM;
}

esy_err_t esy_hal_xio_program_deinit(esy_xio_program_t *program) {
  esy_err_t result = operation_result(FAKE_XIO_PROGRAM_DEINIT);
  if (result != ESY_OK) return result;
  if (program == NULL) return ESY_ERR_INVALID_ARG;
  fake_program_t *state = find_program(program);
  if (state == NULL) return ESY_OK;
  for (size_t i = 0u; i < FAKE_SM_COUNT; ++i)
    if (s_ctx.sms[i].handle != NULL && s_ctx.sms[i].program == state)
      return ESY_ERR_BUSY;
  memset(state, 0, sizeof(*state));
  memset(program, 0, sizeof(*program));
  return ESY_OK;
}

esy_err_t esy_hal_xio_program_sm_init(esy_xio_program_sm_t *sm,
                                     esy_xio_program_t *program,
                                     const esy_xio_program_sm_config_t *config) {
  esy_err_t result = operation_result(FAKE_XIO_PROGRAM_SM_INIT);
  if (result != ESY_OK) return result;
  if (sm == NULL || program == NULL || config == NULL || config->instance > 1u ||
      config->sm_index > 3u ||
      !valid_range(config->out_pin_base, config->out_pin_count, 32u) ||
      !valid_range(config->in_pin_base, config->in_pin_count, 32u) ||
      !valid_range(config->set_pin_base, config->set_pin_count, 5u) ||
      !valid_range(config->sideset_pin_base, config->sideset_pin_count,
                   config->sideset_optional ? 4u : 5u) ||
      (config->sideset_optional && config->sideset_pin_count == 0u) ||
      config->out_pull_thresh > 32u || config->in_push_thresh > 32u)
    return ESY_ERR_INVALID_ARG;
  if (find_sm(sm) != NULL) return ESY_ERR_STATE;
  fake_program_t *prog = find_program(program);
  if (prog == NULL) return ESY_ERR_STATE;
  if (fake_esy_hal_xio_program_find_sm(config->instance, config->sm_index) != NULL)
    return ESY_ERR_BUSY;
  for (size_t i = 0u; i < FAKE_SM_COUNT; ++i) {
    if (s_ctx.sms[i].handle == NULL) {
      s_ctx.sms[i].handle = sm;
      s_ctx.sms[i].program = prog;
      s_ctx.sms[i].config = *config;
      s_ctx.sms[i].status.pc = prog->config.entry;
      return ESY_OK;
    }
  }
  return ESY_ERR_NO_MEM;
}

esy_err_t esy_hal_xio_program_sm_reset(esy_xio_program_sm_t *sm) {
  esy_err_t result = operation_result(FAKE_XIO_PROGRAM_SM_RESET);
  if (result != ESY_OK) return result;
  if (sm == NULL) return ESY_ERR_INVALID_ARG;
  fake_sm_t *state = find_sm(sm);
  if (state == NULL) return ESY_ERR_STATE;
  if (state->status.enabled || state->dma[0].owned || state->dma[1].owned)
    return ESY_ERR_BUSY;
  state->status.pc = state->program->config.entry;
  state->status.tx_level = 0u;
  state->status.rx_level = 0u;
  return ESY_OK;
}

esy_err_t esy_hal_xio_program_start_group(esy_xio_program_sm_t *const *sms,
                                         size_t count) {
  esy_err_t result = operation_result(FAKE_XIO_PROGRAM_START_GROUP);
  if (result != ESY_OK) return result;
  if (sms == NULL || count == 0u || count > 4u) return ESY_ERR_INVALID_ARG;
  fake_sm_t *states[4];
  uint32_t mask = 0u;
  for (size_t i = 0u; i < count; ++i) {
    if (sms[i] == NULL) return ESY_ERR_INVALID_ARG;
    states[i] = find_sm(sms[i]);
    if (states[i] == NULL) return ESY_ERR_STATE;
    if ((i != 0u && states[i]->config.instance != states[0]->config.instance) ||
        (mask & (1u << states[i]->config.sm_index)) != 0u)
      return ESY_ERR_INVALID_ARG;
    if (states[i]->status.enabled) return ESY_ERR_BUSY;
    mask |= 1u << states[i]->config.sm_index;
  }
  for (size_t i = 0u; i < count; ++i) states[i]->status.enabled = true;
  return ESY_OK;
}

esy_err_t esy_hal_xio_program_sm_stop(esy_xio_program_sm_t *sm) {
  esy_err_t result = operation_result(FAKE_XIO_PROGRAM_SM_STOP);
  if (result != ESY_OK) return result;
  if (sm == NULL) return ESY_ERR_INVALID_ARG;
  fake_sm_t *state = find_sm(sm);
  if (state == NULL) return ESY_ERR_STATE;
  state->status.enabled = false;
  return ESY_OK;
}

esy_err_t esy_hal_xio_program_tx_try_put(esy_xio_program_sm_t *sm,
                                        uint32_t word, bool *written) {
  (void)word;
  esy_err_t result = operation_result(FAKE_XIO_PROGRAM_TX_PUT);
  if (result != ESY_OK) return result;
  if (sm == NULL || written == NULL) return ESY_ERR_INVALID_ARG;
  fake_sm_t *state = find_sm(sm);
  if (state == NULL) return ESY_ERR_STATE;
  if (state->dma[ESY_XIO_PROGRAM_DMA_TX].owned) return ESY_ERR_BUSY;
  *written = state->status.tx_level < FAKE_FIFO_WORDS;
  if (*written) ++state->status.tx_level;
  return ESY_OK;
}

esy_err_t esy_hal_xio_program_set_pindirs(esy_xio_program_sm_t *sm,
                                        uint8_t base, uint8_t count, bool output) {
  esy_err_t result = operation_result(FAKE_XIO_PROGRAM_SET_DIRS);
  if (result != ESY_OK) return result;
  if (sm == NULL || count == 0u || !valid_range(base, count, 32u))
    return ESY_ERR_INVALID_ARG;
  fake_sm_t *state = find_sm(sm);
  if (state == NULL) return ESY_ERR_STATE;
  if (state->status.enabled || state->dma[0].active || state->dma[1].active)
    return ESY_ERR_BUSY;
  uint32_t mask = range_mask(base, count);
  s_ctx.directions = output ? s_ctx.directions | mask : s_ctx.directions & ~mask;
  return ESY_OK;
}

esy_err_t esy_hal_xio_program_write_pins(esy_xio_program_sm_t *sm,
                                       uint8_t base, uint8_t count, uint32_t bits) {
  esy_err_t result = operation_result(FAKE_XIO_PROGRAM_WRITE_PINS);
  if (result != ESY_OK) return result;
  if (sm == NULL || count == 0u || !valid_range(base, count, 32u))
    return ESY_ERR_INVALID_ARG;
  fake_sm_t *state = find_sm(sm);
  if (state == NULL) return ESY_ERR_STATE;
  if (state->status.enabled) return ESY_ERR_BUSY;
  uint32_t mask = range_mask(base, count);
  s_ctx.outputs = (s_ctx.outputs & ~mask) | ((bits << base) & mask);
  return ESY_OK;
}

esy_err_t esy_hal_xio_program_irq_clear(esy_xio_program_sm_t *sm, uint8_t flag) {
  esy_err_t result = operation_result(FAKE_XIO_PROGRAM_IRQ_CLEAR);
  if (result != ESY_OK) return result;
  if (sm == NULL || flag > 7u) return ESY_ERR_INVALID_ARG;
  return find_sm(sm) != NULL ? ESY_OK : ESY_ERR_STATE;
}

esy_err_t esy_hal_xio_program_get_status(esy_xio_program_sm_t *sm,
                                       esy_xio_program_status_t *status) {
  esy_err_t result = operation_result(FAKE_XIO_PROGRAM_STATUS);
  if (result != ESY_OK) return result;
  if (sm == NULL || status == NULL) return ESY_ERR_INVALID_ARG;
  fake_sm_t *state = find_sm(sm);
  if (state == NULL) return ESY_ERR_STATE;
  *status = state->status;
  return ESY_OK;
}

esy_err_t esy_hal_xio_program_clear_stalls(esy_xio_program_sm_t *sm,
                                         bool tx, bool rx) {
  esy_err_t result = operation_result(FAKE_XIO_PROGRAM_CLEAR_STALLS);
  if (result != ESY_OK) return result;
  if (sm == NULL) return ESY_ERR_INVALID_ARG;
  fake_sm_t *state = find_sm(sm);
  if (state == NULL) return ESY_ERR_STATE;
  if (tx) state->status.tx_stall = false;
  if (rx) state->status.rx_stall = false;
  return ESY_OK;
}

static esy_err_t dma_start(esy_xio_program_sm_t *sm, const uint32_t *words,
                            size_t count, const esy_xio_program_dma_config_t *config,
                            esy_xio_program_dma_direction_t direction) {
  esy_err_t result = operation_result(direction == ESY_XIO_PROGRAM_DMA_TX
      ? FAKE_XIO_PROGRAM_DMA_TX_START : FAKE_XIO_PROGRAM_DMA_RX_START);
  if (result != ESY_OK) return result;
  if (sm == NULL || words == NULL || count == 0u || config == NULL ||
      config->channel >= 12u || config->irq_index > 1u ||
      ((uintptr_t)words % _Alignof(uint32_t)) != 0u ||
      count > UINT32_C(0x0fffffff) ||
      count > (UINTPTR_MAX - (uintptr_t)words) / sizeof(uint32_t))
    return ESY_ERR_INVALID_ARG;
  fake_sm_t *state = find_sm(sm);
  if (state == NULL) return ESY_ERR_STATE;
  if (state->dma[direction].owned) return ESY_ERR_BUSY;
  for (size_t i = 0u; i < FAKE_SM_COUNT; ++i)
    for (size_t j = 0u; j < 2u; ++j)
      if (s_ctx.sms[i].dma[j].owned &&
          s_ctx.sms[i].dma[j].config.channel == config->channel)
        return ESY_ERR_BUSY;
  state->dma[direction] = (fake_dma_t){
      .words = words, .count = count, .config = *config, .owned = true, .active = true};
  return ESY_OK;
}

esy_err_t esy_hal_xio_program_dma_tx_start(esy_xio_program_sm_t *sm,
                                          const uint32_t *words, size_t count,
                                          const esy_xio_program_dma_config_t *config) {
  return dma_start(sm, words, count, config, ESY_XIO_PROGRAM_DMA_TX);
}

esy_err_t esy_hal_xio_program_dma_rx_start(esy_xio_program_sm_t *sm,
                                          uint32_t *words, size_t count,
                                          const esy_xio_program_dma_config_t *config) {
  return dma_start(sm, words, count, config, ESY_XIO_PROGRAM_DMA_RX);
}

esy_err_t esy_hal_xio_program_dma_abort(esy_xio_program_sm_t *sm,
                                       esy_xio_program_dma_direction_t direction) {
  esy_err_t result = operation_result(FAKE_XIO_PROGRAM_DMA_ABORT);
  if (result != ESY_OK) return result;
  if (sm == NULL || !valid_direction(direction)) return ESY_ERR_INVALID_ARG;
  fake_sm_t *state = find_sm(sm);
  if (state == NULL) return ESY_ERR_STATE;
  memset(&state->dma[direction], 0, sizeof(state->dma[direction]));
  return ESY_OK;
}

esy_err_t esy_hal_xio_program_sm_deinit(esy_xio_program_sm_t *sm) {
  esy_err_t result = operation_result(FAKE_XIO_PROGRAM_SM_DEINIT);
  if (result != ESY_OK) return result;
  if (sm == NULL) return ESY_ERR_INVALID_ARG;
  fake_sm_t *state = find_sm(sm);
  if (state == NULL) return ESY_OK;
  state->status.enabled = false;
  esy_err_t tx = esy_hal_xio_program_dma_abort(sm, ESY_XIO_PROGRAM_DMA_TX);
  esy_err_t rx = esy_hal_xio_program_dma_abort(sm, ESY_XIO_PROGRAM_DMA_RX);
  if (tx != ESY_OK || rx != ESY_OK) return tx != ESY_OK ? tx : rx;
  memset(state, 0, sizeof(*state));
  memset(sm, 0, sizeof(*sm));
  return ESY_OK;
}
