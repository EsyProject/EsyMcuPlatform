/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_dma.h"
#include "esy_hb_internal.h"
#include "esy_hb_resource_assembly_internal.h"
#include <stdatomic.h>
#include "hardware/sync.h"
#include "pico/platform.h"

#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/structs/dma_debug.h"

#define MAX_DMA_CHANNELS 12
#define DMA_IRQ_COUNT 2u
#define DMA_CHANNEL_MASK_LIMIT ((1u << MAX_DMA_CHANNELS) - 1u)

enum { DMA_IRQ_UNREGISTERED, DMA_IRQ_INSTALLING, DMA_IRQ_READY };

typedef struct {
  _Atomic(const void *) owners[MAX_DMA_CHANNELS];
  esy_dma_callback_t callbacks[MAX_DMA_CHANNELS];
  void *callback_args[MAX_DMA_CHANNELS];
  esy_dma_callback_t error_callbacks[MAX_DMA_CHANNELS];
  void *error_callback_args[MAX_DMA_CHANNELS];
  uint32_t dummy[MAX_DMA_CHANNELS];
  atomic_bool configured[MAX_DMA_CHANNELS];
  uint32_t dreq[MAX_DMA_CHANNELS];
  size_t unit_bytes[MAX_DMA_CHANNELS];
  atomic_flag dispatch_guard;
  uint8_t irq_state[DMA_IRQ_COUNT];
  uint8_t irq_core[DMA_IRQ_COUNT];
  uint8_t irq_priority[DMA_IRQ_COUNT];
} esy_hb_dma_ctx_t;

static esy_hb_dma_ctx_t s_ctx = {
    .dispatch_guard = ATOMIC_FLAG_INIT,
    .dummy = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
              0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
              0xFFFFFFFF, 0xFFFFFFFF}};

/* Task/IRQ exclusion is for callback retirement, not DMA data movement.
 * Shared DMA IRQs on one core use one priority so they cannot preempt an IRQ
 * holding dispatch_guard. ISR never waits. Callbacks must be bounded and must
 * not configure/abort DMA. Task callers mask local IRQs while waiting for an
 * already executing peer ISR. */
static uint32_t dma_callbacks_lock(void) {
  uint32_t key = save_and_disable_interrupts();
  while (atomic_flag_test_and_set_explicit(&s_ctx.dispatch_guard, memory_order_acquire))
    tight_loop_contents();
  return key;
}
static void dma_callbacks_unlock(uint32_t key) {
  atomic_flag_clear_explicit(&s_ctx.dispatch_guard, memory_order_release);
  restore_interrupts(key);
}
static void dma_dispatch(uint32_t status) {
  for (uint8_t ch = 0; ch < MAX_DMA_CHANNELS; ++ch) {
    if (!(status & (1u << ch))) continue;
    bool failed = (dma_hw->ch[ch].ctrl_trig & DMA_CH0_CTRL_TRIG_AHB_ERROR_BITS) != 0u;
    esy_dma_callback_t cb = failed ? s_ctx.error_callbacks[ch] : s_ctx.callbacks[ch];
    void *arg = failed ? s_ctx.error_callback_args[ch] : s_ctx.callback_args[ch];
    if (cb) cb(arg);
  }
}
static uint32_t dma_owned_irq_mask(void) {
  uint32_t mask = 0u;
  for (uint8_t ch = 0u; ch < MAX_DMA_CHANNELS; ++ch) {
    if (s_ctx.configured[ch] && atomic_load_explicit(&s_ctx.owners[ch],
                                                   memory_order_acquire) != NULL)
      mask |= 1u << ch;
  }
  return mask;
}
static void esy_hb_dma_irq0_handler(void) {
  if (atomic_flag_test_and_set_explicit(&s_ctx.dispatch_guard, memory_order_acquire)) return;
  uint32_t status = dma_hw->ints0 & dma_owned_irq_mask();
  dma_hw->ints0 = status;
  dma_dispatch(status);
  atomic_flag_clear_explicit(&s_ctx.dispatch_guard, memory_order_release);
}
static void esy_hb_dma_irq1_handler(void) {
  if (atomic_flag_test_and_set_explicit(&s_ctx.dispatch_guard, memory_order_acquire)) return;
  uint32_t status = dma_hw->ints1 & dma_owned_irq_mask();
  dma_hw->ints1 = status;
  dma_dispatch(status);
  atomic_flag_clear_explicit(&s_ctx.dispatch_guard, memory_order_release);
}

static enum dma_channel_transfer_size to_sdk_transfer_size(
    esy_dma_transfer_size_t size) {
  switch (size) {
    case ESY_DMA_TRANSFER_SIZE_8:
      return DMA_SIZE_8;
    case ESY_DMA_TRANSFER_SIZE_16:
      return DMA_SIZE_16;
    case ESY_DMA_TRANSFER_SIZE_32:
      return DMA_SIZE_32;
    default:
      return DMA_SIZE_8;
  }
}

static bool is_valid_transfer_size(esy_dma_transfer_size_t size) {
  return (size == ESY_DMA_TRANSFER_SIZE_8) ||
         (size == ESY_DMA_TRANSFER_SIZE_16) ||
         (size == ESY_DMA_TRANSFER_SIZE_32);
}

esy_err_t esy_hb_dma_bind_irq_to_core(uint32_t irq_index,
                                          uint32_t core_id) {
  if (irq_index >= DMA_IRQ_COUNT || core_id > 1u) return ESY_ERR_INVALID_ARG;
  uint32_t key = dma_callbacks_lock();
  if (s_ctx.irq_state[irq_index] != DMA_IRQ_UNREGISTERED) {
    dma_callbacks_unlock(key);
    return ESY_ERR_BUSY;
  }
  s_ctx.irq_core[irq_index] = (uint8_t)core_id;
  dma_callbacks_unlock(key);
  return ESY_OK;
}

static esy_err_t dma_prepare_irq(const esy_dma_inner_config_t *config) {
  uint32_t key = dma_callbacks_lock();
  uint8_t state = s_ctx.irq_state[config->irq_index];
  /* SDK IRQ installation and NVIC writes affect only the executing core.
   * The selected core must perform first assembly; later submitters may use
   * the established shared IRQ from either core. Never access a peer PPB. */
  if (state == DMA_IRQ_UNREGISTERED &&
      get_core_num() != s_ctx.irq_core[config->irq_index]) {
    dma_callbacks_unlock(key);
    return ESY_ERR_STATE;
  }
  /* Handlers remain installed until reset. Reserve their common per-core
   * priority during assembly, including before SDK installation completes. */
  for (uint8_t index = 0u; index < DMA_IRQ_COUNT; ++index) {
    if (s_ctx.irq_state[index] != DMA_IRQ_UNREGISTERED &&
        s_ctx.irq_core[index] == s_ctx.irq_core[config->irq_index] &&
        s_ctx.irq_priority[index] != config->irq_priority) {
      dma_callbacks_unlock(key);
      return ESY_ERR_BUSY;
    }
  }
  if (state == DMA_IRQ_INSTALLING) {
    dma_callbacks_unlock(key);
    return ESY_ERR_BUSY;
  }
  if (state == DMA_IRQ_READY) {
    dma_callbacks_unlock(key);
    return ESY_OK;
  }
  s_ctx.irq_priority[config->irq_index] = config->irq_priority;
  s_ctx.irq_state[config->irq_index] = DMA_IRQ_INSTALLING;
  dma_callbacks_unlock(key);

  /* Distinct task/core owners share IRQ assembly. Reserve under metadata
   * exclusion, then call the SDK outside it: irq_add_shared_handler takes its
   * own SDK IRQ spinlock. Contenders retry BUSY instead of nesting those locks. */
  const uint32_t irq = config->irq_index == 0u ? DMA_IRQ_0 : DMA_IRQ_1;
  irq_set_priority(irq, config->irq_priority);
  irq_add_shared_handler(irq,
      config->irq_index == 0u ? esy_hb_dma_irq0_handler : esy_hb_dma_irq1_handler,
      PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
  irq_set_enabled(irq, true);

  key = dma_callbacks_lock();
  s_ctx.irq_state[config->irq_index] = DMA_IRQ_READY;
  dma_callbacks_unlock(key);
  return ESY_OK;
}

esy_err_t esy_hb_dma_get_resource_id(uint32_t dreq, uint8_t channel,
                                   uint8_t *out_id) {
  if (out_id == NULL || channel >= MAX_DMA_CHANNELS ||
      dreq > DREQ_FORCE) return ESY_ERR_INVALID_ARG;
  *out_id = channel;
  return ESY_OK;
}

esy_err_t esy_hb_dma_claim(const void *owner, uint32_t dreq, uint8_t channel) {
  uint8_t resource;
  if (owner == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t result = esy_hb_dma_get_resource_id(dreq, channel, &resource);
  if (result != ESY_OK) return result;
  const void *expected = NULL;
  if (!atomic_compare_exchange_strong_explicit(&s_ctx.owners[resource],
      &expected, owner, memory_order_acq_rel, memory_order_acquire))
    return ESY_ERR_BUSY;
  /* Fixed-ID SDK claim has no non-panicking try operation. Reserve allocation
   * assembly against platform Wi-Fi's lazy SDK allocation. Foreign raw SDK
   * allocators still require consumer serialization. Never wait or mask IRQs
   * across an SDK call; an interrupted reservation makes contenders retry. */
  if (!esy_hb_resource_assembly_try_acquire()) {
    atomic_store_explicit(&s_ctx.owners[resource], NULL, memory_order_release);
    return ESY_ERR_BUSY;
  }
  if (dma_channel_is_claimed(resource) || dma_channel_is_busy(resource)) {
    esy_hb_resource_assembly_release();
    atomic_store_explicit(&s_ctx.owners[resource], NULL, memory_order_release);
    return ESY_ERR_BUSY;
  }
  dma_channel_claim(resource);
  esy_hb_resource_assembly_release();
  return ESY_OK;
}

esy_err_t esy_hb_dma_release(const void *owner, uint32_t dreq, uint8_t channel) {
  uint8_t resource;
  if (owner == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t result = esy_hb_dma_get_resource_id(dreq, channel, &resource);
  if (result != ESY_OK) return result;
  if (esy_is_in_isr()) return ESY_ERR_NOT_SUPPORTED;
  const void *current = atomic_load_explicit(&s_ctx.owners[resource], memory_order_acquire);
  if (current != NULL && current != owner) return ESY_ERR_BUSY;
  if (dma_channel_is_busy(resource)) return ESY_ERR_BUSY;
  if (current == NULL)
    return dma_channel_is_claimed(resource) ? ESY_ERR_BUSY : ESY_OK;
  /* Task/IRQ metadata exclusion spans native unclaim and owner publication:
   * a peer-channel dispatcher must not carry an old channel mask into a new
   * SDK owner's transaction. Hardware is already idle; SDK unclaim only
   * clears its claim bit under the SDK's short allocation lock. */
  uint32_t key = dma_callbacks_lock();
  s_ctx.configured[resource] = false;
  s_ctx.callbacks[resource] = NULL;
  s_ctx.callback_args[resource] = NULL;
  s_ctx.error_callbacks[resource] = NULL;
  s_ctx.error_callback_args[resource] = NULL;
  dma_channel_unclaim(resource);
  atomic_store_explicit(&s_ctx.owners[resource], NULL, memory_order_release);
  dma_callbacks_unlock(key);
  return ESY_OK;
}

static bool dma_count_valid(size_t count) {
#ifdef DMA_CH0_TRANS_COUNT_COUNT_BITS
  return count != 0u && count <=
      (DMA_CH0_TRANS_COUNT_COUNT_BITS >> DMA_CH0_TRANS_COUNT_COUNT_LSB);
#else
  return count != 0u && count <= UINT32_MAX;
#endif
}

esy_err_t esy_hb_dma_prepare(const void *owner, const esy_dma_config_t *dma_config) {
  if (!dma_config || dma_config->inner.channel >= MAX_DMA_CHANNELS) {
    return ESY_ERR_INVALID_ARG;
  }
  if (dma_config->inner.irq_index >= DMA_IRQ_COUNT) return ESY_ERR_INVALID_ARG;
  if (dma_config->length == 0u || dma_config->inner.dreq > DREQ_FORCE)
    return ESY_ERR_INVALID_ARG;
  if (!is_valid_transfer_size(dma_config->transfer_size)) {
    return ESY_ERR_INVALID_ARG;
  }
  const size_t unit_bytes = (size_t)1u << dma_config->transfer_size;
  if (!dma_count_valid(dma_config->length / unit_bytes) ||
      (dma_config->length % unit_bytes) != 0u ||
      ((uintptr_t)dma_config->src % unit_bytes) != 0u ||
      ((uintptr_t)dma_config->dst % unit_bytes) != 0u) {
    return ESY_ERR_INVALID_ARG;
  }

  if (dma_config->circular) return ESY_ERR_NOT_SUPPORTED;
  if ((dma_config->inner.direction != ESY_DMA_MEM_TO_MEM &&
       dma_config->inner.direction != ESY_DMA_MEM_TO_PERIPH &&
       dma_config->inner.direction != ESY_DMA_PERIPH_TO_MEM) ||
      (dma_config->inner.direction == ESY_DMA_MEM_TO_MEM &&
       (dma_config->src == NULL || dma_config->dst == NULL)) ||
      (dma_config->inner.direction == ESY_DMA_MEM_TO_PERIPH && dma_config->dst == NULL) ||
      (dma_config->inner.direction == ESY_DMA_PERIPH_TO_MEM && dma_config->src == NULL))
    return ESY_ERR_INVALID_ARG;
  esy_err_t claim = esy_hb_dma_claim(owner, dma_config->inner.dreq,
                                    dma_config->inner.channel);
  if (claim != ESY_OK) return claim;
  esy_err_t irq_result = dma_prepare_irq(&dma_config->inner);
  if (irq_result != ESY_OK) {
    (void)esy_hb_dma_release(owner, dma_config->inner.dreq,
                            dma_config->inner.channel);
    return irq_result;
  }
  dma_channel_set_irq0_enabled(dma_config->inner.channel, false);
  dma_channel_set_irq1_enabled(dma_config->inner.channel, false);
  dma_hw->ch[dma_config->inner.channel].ctrl_trig =
      DMA_CH0_CTRL_TRIG_READ_ERROR_BITS | DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS;

  dma_channel_config c =
      dma_channel_get_default_config(dma_config->inner.channel);
  channel_config_set_transfer_data_size(
      &c, to_sdk_transfer_size(dma_config->transfer_size));

  switch (dma_config->inner.direction) {
    case ESY_DMA_MEM_TO_MEM:
      channel_config_set_read_increment(&c, true);
      channel_config_set_write_increment(&c, true);
      break;
    case ESY_DMA_MEM_TO_PERIPH:
      channel_config_set_read_increment(&c, dma_config->src != NULL);
      channel_config_set_write_increment(&c, false);
      break;
    case ESY_DMA_PERIPH_TO_MEM:
      channel_config_set_read_increment(&c, false);
      channel_config_set_write_increment(&c, dma_config->dst != NULL);
      break;
    default:
      (void)esy_hb_dma_release(owner, dma_config->inner.dreq,
                                dma_config->inner.channel);
      return ESY_ERR_INVALID_ARG;
  }

  channel_config_set_dreq(&c, dma_config->inner.dreq);

  dma_channel_configure(
      dma_config->inner.channel, &c,
      dma_config->dst ? dma_config->dst
                      : (void *)&s_ctx.dummy[dma_config->inner.channel],
      dma_config->src ? dma_config->src
                      : (const void *)&s_ctx.dummy[dma_config->inner.channel],
      dma_config->length / unit_bytes, false);

  uint32_t key = dma_callbacks_lock();
  s_ctx.callbacks[dma_config->inner.channel] = dma_config->callback;
  s_ctx.callback_args[dma_config->inner.channel] = dma_config->callback_arg;
  s_ctx.error_callbacks[dma_config->inner.channel] = dma_config->error_callback;
  s_ctx.error_callback_args[dma_config->inner.channel] = dma_config->error_callback_arg;
  s_ctx.dreq[dma_config->inner.channel] = dma_config->inner.dreq;
  s_ctx.unit_bytes[dma_config->inner.channel] = unit_bytes;
  s_ctx.configured[dma_config->inner.channel] = true;
  dma_callbacks_unlock(key);
  if (dma_config->inner.irq_index == 0) {
    dma_channel_set_irq0_enabled(dma_config->inner.channel, true);
  } else {
    dma_channel_set_irq1_enabled(dma_config->inner.channel, true);
  }

  return ESY_OK;
}

esy_err_t esy_hb_dma_start(const void *owner, uint32_t channel_mask) {
  if (owner == NULL || channel_mask == 0u || (channel_mask & ~DMA_CHANNEL_MASK_LIMIT) != 0u) {
    return ESY_ERR_INVALID_ARG;
  }

  for (uint8_t channel = 0u; channel < MAX_DMA_CHANNELS; ++channel) {
    if ((channel_mask & (1u << channel)) == 0u) continue;
    if (atomic_load_explicit(&s_ctx.owners[channel], memory_order_acquire) != owner)
      return ESY_ERR_BUSY;
    /* Prepare writes the reload value; the live count can stay zero until trigger. */
    if (!s_ctx.configured[channel] ||
        dma_debug_hw->ch[channel].dbg_tcr == 0u) return ESY_ERR_STATE;
    if (dma_channel_is_busy(channel)) return ESY_ERR_BUSY;
  }
  dma_start_channel_mask(channel_mask);
  return ESY_OK;
}

esy_err_t esy_hb_dma_start_single(const void *owner, const esy_dma_config_t *dma_config) {
  esy_err_t ret = esy_hb_dma_prepare(owner, dma_config);
  if (ret != ESY_OK) return ret;
  return esy_hb_dma_start(owner, 1u << dma_config->inner.channel);
}

esy_err_t esy_hb_dma_abort(const void *owner, uint32_t dreq, uint8_t channel) {
  uint8_t resource;
  if (owner == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t result = esy_hb_dma_get_resource_id(dreq, channel, &resource);
  if (result != ESY_OK) return result;
  const void *current = atomic_load_explicit(&s_ctx.owners[resource], memory_order_acquire);
  if (current != owner) return current == NULL &&
      !dma_channel_is_claimed(resource) && !dma_channel_is_busy(resource)
      ? ESY_OK : ESY_ERR_BUSY;
  dma_channel_set_irq0_enabled(channel, false);
  dma_channel_set_irq1_enabled(channel, false);
  dma_channel_abort(channel);
  uint32_t key = dma_callbacks_lock();
  dma_hw->ints0 = 1u << channel;
  dma_hw->ints1 = 1u << channel;
  s_ctx.configured[channel] = false;
  s_ctx.callbacks[channel] = NULL;
  s_ctx.callback_args[channel] = NULL;
  s_ctx.error_callbacks[channel] = NULL;
  s_ctx.error_callback_args[channel] = NULL;
  dma_callbacks_unlock(key);
  return esy_hb_dma_release(owner, dreq, channel);
}

bool esy_hb_dma_is_busy(uint32_t dreq, uint8_t channel) {
  (void)dreq;
  if (channel >= MAX_DMA_CHANNELS) return false;
  return dma_channel_is_busy(channel);
}

static esy_err_t dma_validate_control(const void *owner, uint32_t dreq, uint8_t channel) {
  if (owner == NULL || channel >= MAX_DMA_CHANNELS || dreq > DREQ_FORCE) return ESY_ERR_INVALID_ARG;
  if (atomic_load_explicit(&s_ctx.owners[channel], memory_order_acquire) != owner)
    return ESY_ERR_BUSY;
  if (!s_ctx.configured[channel] || s_ctx.dreq[channel] != dreq)
    return ESY_ERR_STATE;
  if (dma_channel_is_busy(channel)) return ESY_ERR_BUSY;
  return ESY_OK;
}

esy_err_t esy_hb_dma_set_read_addr(const void *owner, uint32_t dreq, uint8_t channel,
                                  const void *addr, bool trigger_now) {
  if (addr == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t result = dma_validate_control(owner, dreq, channel);
  if (result != ESY_OK) return result;
  if ((uintptr_t)addr % s_ctx.unit_bytes[channel] != 0u)
    return ESY_ERR_INVALID_ARG;
  if (trigger_now && dma_debug_hw->ch[channel].dbg_tcr == 0u)
    return ESY_ERR_STATE;
  dma_channel_set_read_addr(channel, addr, trigger_now);
  return ESY_OK;
}

esy_err_t esy_hb_dma_set_write_addr(const void *owner, uint32_t dreq, uint8_t channel, void *addr,
                                   bool trigger_now) {
  if (addr == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t result = dma_validate_control(owner, dreq, channel);
  if (result != ESY_OK) return result;
  if ((uintptr_t)addr % s_ctx.unit_bytes[channel] != 0u)
    return ESY_ERR_INVALID_ARG;
  if (trigger_now && dma_debug_hw->ch[channel].dbg_tcr == 0u)
    return ESY_ERR_STATE;
  dma_channel_set_write_addr(channel, addr, trigger_now);
  return ESY_OK;
}

esy_err_t esy_hb_dma_set_trans_count(const void *owner, uint32_t dreq, uint8_t channel,
                                    size_t count, bool trigger_now) {
  if (!dma_count_valid(count)) return ESY_ERR_INVALID_ARG;
  esy_err_t result = dma_validate_control(owner, dreq, channel);
  if (result != ESY_OK) return result;
  dma_channel_set_trans_count(channel, count, trigger_now);
  return ESY_OK;
}

esy_err_t esy_hb_dma_get_trans_count_remaining(uint32_t dreq,
                                                   uint8_t channel,
                                                   size_t *out_remaining) {
  (void)dreq;
  if (out_remaining == NULL) return ESY_ERR_INVALID_ARG;
  if (channel >= MAX_DMA_CHANNELS) return ESY_ERR_INVALID_ARG;

  *out_remaining = (size_t)dma_channel_hw_addr(channel)->transfer_count;
  return ESY_OK;
}

esy_err_t esy_hb_dma_chain_to(const void *owner, uint32_t dreq, uint8_t channel,
                             uint8_t chain_channel) {
  if (chain_channel >= MAX_DMA_CHANNELS) return ESY_ERR_INVALID_ARG;
  esy_err_t result = dma_validate_control(owner, dreq, channel);
  if (result != ESY_OK) return result;
  if (!s_ctx.configured[chain_channel]) return ESY_ERR_STATE;
  if (atomic_load_explicit(&s_ctx.owners[chain_channel], memory_order_acquire) != owner)
    return ESY_ERR_BUSY;
  if (dma_channel_is_busy(chain_channel)) return ESY_ERR_BUSY;
  dma_channel_hw_t *hw = dma_channel_hw_addr(channel);
  hw->al1_ctrl = (hw->al1_ctrl & ~DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS) |
                 (chain_channel << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB);
  return ESY_OK;
}

extern void esy_hb_dma_handle_irq(uint32_t dreq, uint8_t channel) {
  (void)dreq;
  (void)channel;
}
