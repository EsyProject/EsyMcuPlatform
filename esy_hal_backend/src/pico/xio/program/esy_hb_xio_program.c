/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/xio/program/esy_hb_xio_program.h"
#include "esy_hal_backend/esy_hb_xio.h"
#include "esy_hal_backend/esy_hb_dma.h"

#include <string.h>
#include "hardware/pio.h"
#include "pico/platform.h"

#define PROGRAM_SM_COUNT 4u
#define PROGRAM_IRQ_FLAG_COUNT 8u
#define PROGRAM_DMA_CHANNEL_COUNT 12u
#define PROGRAM_DMA_IRQ_COUNT 2u
#define PROGRAM_DMA_MAX_WORDS UINT32_C(0x0fffffff)
#define PROGRAM_INSTRUCTION_MAJOR_MASK UINT16_C(0xe000)
#define PROGRAM_JMP_TARGET_MASK UINT16_C(0x001f)
#define PROGRAM_GPIO_COUNT 32u

typedef struct {
  esy_hb_xio_program_t native;
  esy_xio_program_config_t config;
  uint8_t references;
  bool initialized;
} program_runtime_t;

typedef struct {
  uint32_t dreq;
  uint8_t channel;
  bool owned;
  esy_xio_program_dma_cb_t callback;
  void *user;
} program_dma_runtime_t;

typedef struct {
  esy_hb_xio_sm_t native;
  program_runtime_t *program;
  program_dma_runtime_t dma[2];
  uint8_t instance;
  uint8_t sm_index;
  uint8_t offset;
  bool initialized;
} program_sm_runtime_t;

_Static_assert(sizeof(program_runtime_t) <= ESY_XIO_PROGRAM_STORAGE_BYTES,
               "Program storage cannot hold the Pico provider");
_Static_assert(sizeof(program_sm_runtime_t) <= ESY_XIO_PROGRAM_SM_STORAGE_BYTES,
               "State-machine storage cannot hold the Pico provider");

static program_runtime_t *program_runtime(esy_xio_program_t *program) {
  return (program_runtime_t *)(void *)program->bytes;
}

static program_sm_runtime_t *sm_runtime(esy_xio_program_sm_t *sm) {
  return (program_sm_runtime_t *)(void *)sm->bytes;
}

static PIO sm_pio(const program_sm_runtime_t *sm) {
  return sm->instance == 0u ? pio0 : pio1;
}

static bool in_interrupt(void) {
  return __get_current_exception() != 0u;
}

static esy_err_t checked_sm(esy_xio_program_sm_t *sm,
                             program_sm_runtime_t **runtime) {
  if (sm == NULL) return ESY_ERR_INVALID_ARG;
  if (in_interrupt()) return ESY_ERR_NOT_SUPPORTED;
  program_sm_runtime_t *value = sm_runtime(sm);
  if (!value->initialized) return ESY_ERR_STATE;
  *runtime = value;
  return ESY_OK;
}

static bool native_enabled(const program_sm_runtime_t *sm) {
  return (sm_pio(sm)->ctrl & (1u << sm->sm_index)) != 0u;
}

static bool dma_owned(const program_sm_runtime_t *sm) {
  return sm->dma[ESY_XIO_PROGRAM_DMA_TX].owned ||
         sm->dma[ESY_XIO_PROGRAM_DMA_RX].owned;
}

static bool dma_active(const program_sm_runtime_t *sm) {
  for (size_t i = 0u; i < 2u; ++i) {
    if (sm->dma[i].owned &&
        esy_hb_dma_is_busy(sm->dma[i].dreq, sm->dma[i].channel))
      return true;
  }
  return false;
}

static bool program_config_valid(const esy_xio_program_config_t *config) {
  if (config == NULL || config->instructions == NULL ||
      config->instruction_count == 0u ||
      config->instruction_count > ESY_XIO_PROGRAM_MAX_INSTRUCTIONS ||
      config->entry >= config->instruction_count ||
      config->wrap_bottom > config->wrap_top ||
      config->wrap_top >= config->instruction_count ||
      ((uintptr_t)config->instructions % _Alignof(uint16_t)) != 0u)
    return false;
  for (uint8_t i = 0u; i < config->instruction_count; ++i) {
    uint16_t word = config->instructions[i];
    if ((word & PROGRAM_INSTRUCTION_MAJOR_MASK) == 0u &&
        (word & PROGRAM_JMP_TARGET_MASK) >= config->instruction_count)
      return false;
  }
  return true;
}

static bool mapping_valid(uint8_t base, uint8_t count, uint8_t maximum) {
  return count <= maximum && (count == 0u ||
      (base < PROGRAM_GPIO_COUNT && count <= PROGRAM_GPIO_COUNT - base &&
       base < NUM_BANK0_GPIOS && count <= NUM_BANK0_GPIOS - base));
}

static bool sm_config_valid(const esy_xio_program_sm_config_t *config) {
  if (config == NULL || config->instance > 1u || config->sm_index >= PROGRAM_SM_COUNT ||
      !mapping_valid(config->out_pin_base, config->out_pin_count, 32u) ||
      !mapping_valid(config->in_pin_base, config->in_pin_count, 32u) ||
      !mapping_valid(config->set_pin_base, config->set_pin_count, 5u) ||
      !mapping_valid(config->sideset_pin_base, config->sideset_pin_count,
                       config->sideset_optional ? 4u : 5u) ||
      (config->sideset_optional && config->sideset_pin_count == 0u) ||
      config->out_pull_thresh > 32u || config->in_push_thresh > 32u)
    return false;
  uint32_t numerator = config->clk_div_div != 0u ? config->clk_div_div : 1u;
  uint32_t denominator = config->clk_div_mul != 0u ? config->clk_div_mul : 1u;
  return numerator >= denominator &&
      (uint64_t)numerator <= (uint64_t)denominator * 65536u;
}

esy_err_t esy_hb_xio_program_exec_init(esy_xio_program_t *program,
                                  const esy_xio_program_config_t *config) {
  if (program == NULL || !program_config_valid(config)) return ESY_ERR_INVALID_ARG;
  if (in_interrupt()) return ESY_ERR_NOT_SUPPORTED;
  program_runtime_t *runtime = program_runtime(program);
  if (runtime->initialized) return ESY_ERR_STATE;
  const esy_hb_xio_program_desc_t description = {
      .instructions = config->instructions,
      .length = config->instruction_count,
      .origin = -1,
      .name = NULL,
      .native_prog = NULL};
  esy_err_t result = esy_hb_xio_program_init(&runtime->native, &description);
  if (result != ESY_OK) return result;
  runtime->config = *config;
  runtime->references = 0u;
  runtime->initialized = true;
  return ESY_OK;
}

esy_err_t esy_hb_xio_program_exec_deinit(esy_xio_program_t *program) {
  if (program == NULL) return ESY_ERR_INVALID_ARG;
  if (in_interrupt()) return ESY_ERR_NOT_SUPPORTED;
  program_runtime_t *runtime = program_runtime(program);
  if (!runtime->initialized) return ESY_OK;
  if (runtime->references != 0u) return ESY_ERR_BUSY;
  esy_hb_xio_program_deinit(&runtime->native);
  memset(program, 0, sizeof(*program));
  return ESY_OK;
}

/* The owner has disabled this SM and retired DMA. Injected MOV/JMP operations
 * temporarily have no side-set mapping, so clearing registers preserves every
 * output latch, including a mandatory side-set clock's idle level. */
static void reset_sm(program_sm_runtime_t *runtime) {
  PIO pio = sm_pio(runtime);
  uint sm = runtime->sm_index;
  pio_sm_clear_fifos(pio, sm);
  /* Cancel any stalled instruction before injecting the register reset. */
  pio_sm_restart(pio, sm);
  pio_sm_clkdiv_restart(pio, sm);
  uint32_t saved_pinctrl = pio->sm[sm].pinctrl;
  pio->sm[sm].pinctrl = saved_pinctrl & ~PIO_SM0_PINCTRL_SIDESET_COUNT_BITS;
  pio_sm_exec(pio, sm, pio_encode_mov(pio_isr, pio_null));
  pio_sm_exec(pio, sm, pio_encode_mov(pio_osr, pio_null));
  pio_sm_exec(pio, sm, pio_encode_set(pio_x, 0u));
  pio_sm_exec(pio, sm, pio_encode_set(pio_y, 0u));
  pio_sm_exec(pio, sm, pio_encode_jmp(runtime->offset + runtime->program->config.entry));
  /* MOV OSR marks it full. Restore the empty shift count so the first autopull
   * consumes FIFO data; restart preserves PC, X/Y and the zeroed OSR value. */
  pio_sm_restart(pio, sm);
  pio->sm[sm].pinctrl = saved_pinctrl;
}

esy_err_t esy_hb_xio_program_exec_sm_init(esy_xio_program_sm_t *sm,
                                     esy_xio_program_t *program,
                                     const esy_xio_program_sm_config_t *config) {
  if (sm == NULL || program == NULL || !sm_config_valid(config))
    return ESY_ERR_INVALID_ARG;
  if (in_interrupt()) return ESY_ERR_NOT_SUPPORTED;
  program_sm_runtime_t *runtime = sm_runtime(sm);
  program_runtime_t *prog = program_runtime(program);
  if (runtime->initialized || !prog->initialized) return ESY_ERR_STATE;
  PIO pio = config->instance == 0u ? pio0 : pio1;
  if (pio_get_gpio_base(pio) != 0u) return ESY_ERR_NOT_SUPPORTED;
  esy_hb_xio_sm_config_t native_config = {
      .clk_div_mul = config->clk_div_mul,
      .clk_div_div = config->clk_div_div,
      .out_pin_base = config->out_pin_base,
      .out_pin_count = config->out_pin_count,
      .in_pin_base = config->in_pin_base,
      .in_pin_count = config->in_pin_count,
      .set_pin_base = config->set_pin_base,
      .set_pin_count = config->set_pin_count,
      .sideset_pin_base = config->sideset_pin_base,
      .sideset_pin_count = config->sideset_pin_count,
      .sideset_optional = config->sideset_optional,
      .jmp_pin = ESY_PIN_UNUSED,
      .out_shift_right = config->out_shift_right,
      .out_autopull = config->out_autopull,
      .out_pull_thresh = config->out_pull_thresh,
      .in_shift_right = config->in_shift_right,
      .in_autopush = config->in_autopush,
      .in_push_thresh = config->in_push_thresh,
      .enable_on_init = 0u};
  esy_err_t result = esy_hb_xio_sm_claim_manual(&prog->native, &runtime->native,
                                               config->instance, config->sm_index);
  if (result != ESY_OK) return result;
  result = esy_hb_xio_sm_init(&runtime->native, &native_config);
  if (result != ESY_OK) {
    esy_hb_xio_sm_release(&runtime->native);
    return result;
  }
  uint32_t offset = 0u;
  result = esy_hb_xio_sm_get_entry_offset(&runtime->native, &offset);
  if (result != ESY_OK) {
    esy_hb_xio_sm_release(&runtime->native);
    return result;
  }
  runtime->program = prog;
  runtime->instance = config->instance;
  runtime->sm_index = config->sm_index;
  runtime->offset = (uint8_t)offset;
  pio_sm_set_wrap(pio, config->sm_index, offset + prog->config.wrap_bottom,
                  offset + prog->config.wrap_top);
  if (config->in_pin_count != 0u) {
    uint32_t mask = config->in_pin_count == PROGRAM_GPIO_COUNT
        ? UINT32_MAX : (UINT32_C(1) << config->in_pin_count) - 1u;
    hw_clear_bits(&pio->input_sync_bypass, mask << config->in_pin_base);
  }
  reset_sm(runtime);
  runtime->initialized = true;
  ++prog->references;
  return ESY_OK;
}

esy_err_t esy_hb_xio_program_exec_sm_reset(esy_xio_program_sm_t *sm) {
  program_sm_runtime_t *runtime;
  esy_err_t result = checked_sm(sm, &runtime);
  if (result != ESY_OK) return result;
  if (native_enabled(runtime) || dma_owned(runtime)) return ESY_ERR_BUSY;
  reset_sm(runtime);
  return ESY_OK;
}

esy_err_t esy_hb_xio_program_exec_start_group(esy_xio_program_sm_t *const *sms,
                                         size_t count) {
  if (sms == NULL || count == 0u || count > PROGRAM_SM_COUNT)
    return ESY_ERR_INVALID_ARG;
  program_sm_runtime_t *runtimes[PROGRAM_SM_COUNT];
  uint32_t mask = 0u;
  for (size_t i = 0u; i < count; ++i) {
    esy_err_t result = checked_sm(sms[i], &runtimes[i]);
    if (result != ESY_OK) return result;
    if ((i > 0u && runtimes[i]->instance != runtimes[0]->instance) ||
        (mask & (1u << runtimes[i]->sm_index)) != 0u)
      return ESY_ERR_INVALID_ARG;
    if (native_enabled(runtimes[i])) return ESY_ERR_BUSY;
    mask |= 1u << runtimes[i]->sm_index;
  }
  pio_enable_sm_mask_in_sync(sm_pio(runtimes[0]), mask);
  for (size_t i = 0u; i < count; ++i) runtimes[i]->native.started = 1u;
  return ESY_OK;
}

esy_err_t esy_hb_xio_program_exec_sm_stop(esy_xio_program_sm_t *sm) {
  program_sm_runtime_t *runtime;
  esy_err_t result = checked_sm(sm, &runtime);
  if (result != ESY_OK) return result;
  esy_hb_xio_sm_stop(&runtime->native);
  return ESY_OK;
}

esy_err_t esy_hb_xio_program_exec_tx_try_put(esy_xio_program_sm_t *sm,
                                        uint32_t word, bool *written) {
  if (written == NULL) return ESY_ERR_INVALID_ARG;
  program_sm_runtime_t *runtime;
  esy_err_t result = checked_sm(sm, &runtime);
  if (result != ESY_OK) return result;
  if (runtime->dma[ESY_XIO_PROGRAM_DMA_TX].owned) return ESY_ERR_BUSY;
  return esy_hb_xio_tx_try_put32(&runtime->native, word, written);
}

static bool pin_range_valid(uint8_t base, uint8_t count) {
  return count > 0u && base < PROGRAM_GPIO_COUNT &&
         count <= PROGRAM_GPIO_COUNT - base && base < NUM_BANK0_GPIOS &&
         count <= NUM_BANK0_GPIOS - base;
}

esy_err_t esy_hb_xio_program_exec_set_pindirs(esy_xio_program_sm_t *sm,
                                        uint8_t base, uint8_t count, bool output) {
  if (!pin_range_valid(base, count)) return ESY_ERR_INVALID_ARG;
  program_sm_runtime_t *runtime;
  esy_err_t result = checked_sm(sm, &runtime);
  if (result != ESY_OK) return result;
  if (native_enabled(runtime) || dma_active(runtime)) return ESY_ERR_BUSY;
  return esy_hb_xio_set_consecutive_pindirs(&runtime->native, base, count, output);
}

esy_err_t esy_hb_xio_program_exec_write_pins(esy_xio_program_sm_t *sm,
                                       uint8_t base, uint8_t count, uint32_t bits) {
  if (!pin_range_valid(base, count)) return ESY_ERR_INVALID_ARG;
  program_sm_runtime_t *runtime;
  esy_err_t result = checked_sm(sm, &runtime);
  if (result != ESY_OK) return result;
  if (native_enabled(runtime)) return ESY_ERR_BUSY;
  return esy_hb_xio_write_pins_mask(&runtime->native, base, count, bits);
}

esy_err_t esy_hb_xio_program_exec_irq_clear(esy_xio_program_sm_t *sm, uint8_t flag) {
  if (flag >= PROGRAM_IRQ_FLAG_COUNT) return ESY_ERR_INVALID_ARG;
  program_sm_runtime_t *runtime;
  esy_err_t result = checked_sm(sm, &runtime);
  if (result != ESY_OK) return result;
  pio_interrupt_clear(sm_pio(runtime), flag);
  return ESY_OK;
}

esy_err_t esy_hb_xio_program_exec_get_status(esy_xio_program_sm_t *sm,
                                       esy_xio_program_status_t *status) {
  if (status == NULL) return ESY_ERR_INVALID_ARG;
  program_sm_runtime_t *runtime;
  esy_err_t result = checked_sm(sm, &runtime);
  if (result != ESY_OK) return result;
  PIO pio = sm_pio(runtime);
  uint index = runtime->sm_index;
  uint32_t debug = pio->fdebug;
  const esy_xio_program_status_t value = {
      .pc = (uint8_t)((pio_sm_get_pc(pio, index) - runtime->offset) & 31u),
      .tx_level = (uint8_t)pio_sm_get_tx_fifo_level(pio, index),
      .rx_level = (uint8_t)pio_sm_get_rx_fifo_level(pio, index),
      .enabled = native_enabled(runtime),
      .tx_stall = (debug & (1u << (PIO_FDEBUG_TXSTALL_LSB + index))) != 0u,
      .rx_stall = (debug & (1u << (PIO_FDEBUG_RXSTALL_LSB + index))) != 0u};
  *status = value;
  return ESY_OK;
}

esy_err_t esy_hb_xio_program_exec_clear_stalls(esy_xio_program_sm_t *sm,
                                         bool tx, bool rx) {
  program_sm_runtime_t *runtime;
  esy_err_t result = checked_sm(sm, &runtime);
  if (result != ESY_OK) return result;
  uint32_t mask = (tx ? 1u << (PIO_FDEBUG_TXSTALL_LSB + runtime->sm_index) : 0u) |
                  (rx ? 1u << (PIO_FDEBUG_RXSTALL_LSB + runtime->sm_index) : 0u);
  sm_pio(runtime)->fdebug = mask;
  return ESY_OK;
}

static void dma_complete(void *user) {
  program_dma_runtime_t *runtime = user;
  if (runtime->callback != NULL) runtime->callback(runtime->user, ESY_OK);
}

static void dma_error(void *user) {
  program_dma_runtime_t *runtime = user;
  if (runtime->callback != NULL) runtime->callback(runtime->user, ESY_ERR_IO);
}

static esy_err_t start_dma(esy_xio_program_sm_t *sm, const void *words, size_t count,
                            const esy_xio_program_dma_config_t *config,
                            esy_xio_program_dma_direction_t direction) {
  if (words == NULL || count == 0u || count > PROGRAM_DMA_MAX_WORDS ||
      ((uintptr_t)words % _Alignof(uint32_t)) != 0u || config == NULL ||
      config->channel >= PROGRAM_DMA_CHANNEL_COUNT ||
      config->irq_index >= PROGRAM_DMA_IRQ_COUNT ||
      count > (UINTPTR_MAX - (uintptr_t)words) / sizeof(uint32_t))
    return ESY_ERR_INVALID_ARG;
  program_sm_runtime_t *runtime;
  esy_err_t result = checked_sm(sm, &runtime);
  if (result != ESY_OK) return result;
  program_dma_runtime_t *dma = &runtime->dma[direction];
  if (dma->owned) return ESY_ERR_BUSY;
  PIO pio = sm_pio(runtime);
  bool tx = direction == ESY_XIO_PROGRAM_DMA_TX;
  uint32_t dreq = pio_get_dreq(pio, runtime->sm_index, tx);
  dma->dreq = dreq;
  dma->channel = config->channel;
  dma->callback = config->callback;
  dma->user = config->user;
  esy_dma_config_t transfer = {
      .src = tx ? words : (const void *)&pio->rxf[runtime->sm_index],
      .dst = tx ? (void *)&pio->txf[runtime->sm_index] : (void *)words,
      .length = count * sizeof(uint32_t),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_32,
      .callback = dma_complete,
      .callback_arg = dma,
      .error_callback = dma_error,
      .error_callback_arg = dma,
      .inner = {.direction = tx ? ESY_DMA_MEM_TO_PERIPH : ESY_DMA_PERIPH_TO_MEM,
                .channel = config->channel,
                .dreq = dreq,
                .irq_index = config->irq_index,
                .irq_priority = config->irq_priority}};
  result = esy_hb_dma_prepare(dma, &transfer);
  if (result != ESY_OK) {
    memset(dma, 0, sizeof(*dma));
    return result;
  }
  dma->owned = true;
  uint8_t resource;
  result = esy_hb_dma_get_resource_id(dreq, config->channel, &resource);
  if (result == ESY_OK) result = esy_hb_dma_start(dma, 1u << resource);
  if (result != ESY_OK) {
    esy_err_t cleanup = esy_hb_dma_abort(dma, dma->dreq, dma->channel);
    if (cleanup != ESY_OK) return cleanup;
    memset(dma, 0, sizeof(*dma));
  }
  return result;
}

esy_err_t esy_hb_xio_program_exec_dma_tx_start(esy_xio_program_sm_t *sm,
                                          const uint32_t *words, size_t count,
                                          const esy_xio_program_dma_config_t *config) {
  return start_dma(sm, words, count, config, ESY_XIO_PROGRAM_DMA_TX);
}

esy_err_t esy_hb_xio_program_exec_dma_rx_start(esy_xio_program_sm_t *sm,
                                          uint32_t *words, size_t count,
                                          const esy_xio_program_dma_config_t *config) {
  return start_dma(sm, words, count, config, ESY_XIO_PROGRAM_DMA_RX);
}

esy_err_t esy_hb_xio_program_exec_dma_abort(esy_xio_program_sm_t *sm,
                                       esy_xio_program_dma_direction_t direction) {
  if (direction != ESY_XIO_PROGRAM_DMA_TX && direction != ESY_XIO_PROGRAM_DMA_RX)
    return ESY_ERR_INVALID_ARG;
  program_sm_runtime_t *runtime;
  esy_err_t result = checked_sm(sm, &runtime);
  if (result != ESY_OK) return result;
  program_dma_runtime_t *dma = &runtime->dma[direction];
  if (!dma->owned) return ESY_OK;
  result = esy_hb_dma_abort(dma, dma->dreq, dma->channel);
  if (result == ESY_OK) memset(dma, 0, sizeof(*dma));
  return result;
}

esy_err_t esy_hb_xio_program_exec_sm_deinit(esy_xio_program_sm_t *sm) {
  if (sm == NULL) return ESY_ERR_INVALID_ARG;
  if (in_interrupt()) return ESY_ERR_NOT_SUPPORTED;
  program_sm_runtime_t *runtime = sm_runtime(sm);
  if (!runtime->initialized) return ESY_OK;
  esy_hb_xio_sm_stop(&runtime->native);
  esy_err_t tx_result = esy_hb_xio_program_exec_dma_abort(sm, ESY_XIO_PROGRAM_DMA_TX);
  esy_err_t rx_result = esy_hb_xio_program_exec_dma_abort(sm, ESY_XIO_PROGRAM_DMA_RX);
  if (tx_result != ESY_OK || rx_result != ESY_OK)
    return tx_result != ESY_OK ? tx_result : rx_result;
  program_runtime_t *program = runtime->program;
  esy_hb_xio_sm_release(&runtime->native);
  --program->references;
  memset(sm, 0, sizeof(*sm));
  return ESY_OK;
}
