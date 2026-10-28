/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdint.h>
#include <stdalign.h>

#include "pico/time.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/xio/esy_xio_spi_defs.h"
#include "esy_common/defs/peripheral/esy_spi_defs.h"
#include "esy_common/defs/peripheral/esy_dma_defs.h"
#include "esy_hal_backend/esy_hb_xio.h"
#include "esy_hal_backend/esy_hb_dma.h"
#include "esy_hb_resource_assembly_internal.h"

#include "esy_hb_xio_spi.pio.h"

#define XIO_SPI_PIN_MASK_BITS 32u

typedef struct {
  esy_hb_xio_program_t prog;
  esy_hb_xio_sm_t sm;

  uint8_t inst_index;
  uint8_t sm_index;
  uint8_t pin_sck;
  uint8_t pin_mosi;
  uint8_t pin_miso;
  uint8_t pin_cs;
  uint8_t pin_trigger;

  esy_spi_role_t role;
  uint32_t sck_hz;
  uint8_t data_bits;
  bool is_msb_first;
  bool is_three_wire;
  uint8_t cpol;
  uint8_t cpha;

  bool cs_active_low;
  uint8_t cs_monitor_sm_index;
  uint8_t cs_monitor_prog_offs;
  uint32_t cs_setup_ns;
  uint32_t cs_hold_ns;
  uint32_t cs_inactive_ns;

  uint8_t out_shift_right;
  uint8_t in_shift_right;
  uint8_t prog_offs;

  esy_spi_dma_complete_cb_t pending_cb;
  void *pending_arg;
  size_t tx_length;
  size_t rx_length;
  esy_spi_t *bound_spi;
  uint32_t tx_dreq;
  uint32_t rx_dreq;
  uint8_t tx_channel;
  uint8_t rx_channel;

  /* Only the serialized owner accesses these flags; DMA IRQs do not write
   * this context. Keep future ISR publication separate from this flag byte. */
  bool dma_active : 1;
  bool tx_owned : 1;
  bool rx_owned : 1;
  bool dma_failed : 1;
  bool wire_check_started : 1;
  bool cs_monitor_claimed : 1;
  bool cs_monitor_loaded : 1;
  bool initialized : 1;
  uint8_t tx_unit_bytes;
  uint8_t rx_unit_bytes;
} esy_xio_spi_ctx_t;

_Static_assert(ESY_SPI_PRIV_BYTES >= sizeof(esy_xio_spi_ctx_t),
               "spi->backend.bytes is too small for esy_xio_spi_ctx_t");

static inline esy_xio_spi_ctx_t *get_ctx(esy_spi_t *spi) {
  return (esy_xio_spi_ctx_t *)(void *)(spi->backend.bytes);
}
static inline PIO ctx_pio(const esy_xio_spi_ctx_t *c) {
  return c->inst_index ? pio1 : pio0;
}

static inline void busy_wait_ns(uint32_t ns) {
  if (!ns) return;
  uint32_t hz = clock_get_hz(clk_sys);
  uint64_t cycles = ((uint64_t)ns * hz + 999999999ull) / 1000000000ull;
  if (cycles) busy_wait_at_least_cycles((uint32_t)cycles);
}
static inline uint32_t effective_setup_ns(const esy_xio_spi_ctx_t *c) {
  if (c->cs_setup_ns) return c->cs_setup_ns;
  if (!c->sck_hz) return 0;
  uint32_t ns = (uint32_t)((500000000ull + (c->sck_hz - 1)) / c->sck_hz);
  return ns ? ns : 1;
}
static inline bool is_master(const esy_xio_spi_ctx_t *c) {
  return (c->role == ESY_SPI_ROLE_MASTER ||
          c->role == ESY_SPI_ROLE_MASTER_NO_CS);
}

static inline bool is_three_wire_config(const esy_xio_spi_config_t *c) {
  return c->pin_mosi != ESY_PIN_UNUSED && c->pin_mosi == c->pin_miso;
}

static inline void set_three_wire_direction(esy_xio_spi_ctx_t *c,
                                            bool is_output) {
  if (!c->is_three_wire) return;
  pio_sm_set_consecutive_pindirs(ctx_pio(c), c->sm_index, c->pin_mosi, 1,
                                 is_output);
}

static inline void cs_assert(esy_xio_spi_ctx_t *c) {
  if (!is_master(c) || (c->pin_cs == ESY_PIN_UNUSED)) return;
  if (c->role == ESY_SPI_ROLE_MASTER_NO_CS) return;
  gpio_put(c->pin_cs, c->cs_active_low ? 0 : 1);
  busy_wait_ns(effective_setup_ns(c));
}
static inline void cs_release(esy_xio_spi_ctx_t *c) {
  if (!is_master(c) || (c->pin_cs == ESY_PIN_UNUSED)) return;
  if (c->role == ESY_SPI_ROLE_MASTER_NO_CS) return;
  busy_wait_ns(c->cs_hold_ns);
  gpio_put(c->pin_cs, c->cs_active_low ? 1 : 0);
  busy_wait_ns(c->cs_inactive_ns);
}

static inline uint32_t frame_mask(uint8_t data_bits) {
  return (data_bits >= 32u) ? UINT32_MAX : ((1u << data_bits) - 1u);
}

static inline uint32_t pack_tx_frame(const esy_xio_spi_ctx_t *c,
                                     uint32_t value) {
  const uint32_t frame = value & frame_mask(c->data_bits);
  return c->out_shift_right ? frame : (frame << (32u - c->data_bits));
}

static inline uint32_t unpack_rx_frame(const esy_xio_spi_ctx_t *c,
                                       uint32_t value) {
  const uint32_t frame =
      c->in_shift_right ? (value >> (32u - c->data_bits)) : value;
  return frame & frame_mask(c->data_bits);
}

static inline uint32_t pack_tx8(const esy_xio_spi_ctx_t *c, uint8_t value) {
  return pack_tx_frame(c, value);
}

static inline uint8_t unpack_rx8(const esy_xio_spi_ctx_t *c, uint32_t value) {
  return (uint8_t)unpack_rx_frame(c, value);
}

static inline uint32_t pack_tx16(const esy_xio_spi_ctx_t *c, uint16_t value) {
  return pack_tx_frame(c, value);
}

static inline uint16_t unpack_rx16(const esy_xio_spi_ctx_t *c, uint32_t value) {
  return (uint16_t)unpack_rx_frame(c, value);
}

static inline bool is_sety31_placeholder(uint16_t insn) {
  return insn == pio_encode_set(pio_y, 31);
}

static inline uint16_t encode_wait_gpio_for_pio(PIO pio, bool level,
                                                uint gpio) {
  uint16_t insn = pio_encode_wait_gpio(level, gpio);
#ifdef PICO_PIO_USE_GPIO_BASE
  uint base = pio_get_gpio_base(pio);
  if (base) insn ^= (uint16_t)base;
#else
  (void)pio;
#endif
  return insn;
}
static inline bool is_wait0_placeholder(uint16_t insn) {
  return insn == pio_encode_wait_pin(false, 0) ||
         insn == pio_encode_wait_gpio(false, 0);
}
static inline bool is_wait1_placeholder(uint16_t insn) {
  return insn == pio_encode_wait_pin(true, 0) ||
         insn == pio_encode_wait_gpio(true, 0);
}

static void cs_monitor_release(esy_xio_spi_ctx_t *c) {
  if (!c->cs_monitor_claimed) return;
  PIO pio = ctx_pio(c);
  const uint smi = c->cs_monitor_sm_index;
  pio_sm_set_enabled(pio, smi, false);
  if (c->cs_monitor_loaded) {
    pio_sm_set_consecutive_pindirs(pio, smi, c->pin_miso, 1u, false);
    pio_remove_program(pio, &xio_spi_slave_cs_program,
                       c->cs_monitor_prog_offs);
    c->cs_monitor_loaded = false;
  }
  pio_sm_unclaim(pio, smi);
  c->cs_monitor_claimed = false;
}

static esy_err_t cs_monitor_init(esy_xio_spi_ctx_t *c,
                                 const esy_xio_spi_config_t *config) {
  if (c->pin_miso == ESY_PIN_UNUSED) return ESY_OK;
  PIO pio = ctx_pio(c);
  const uint smi = config->slave_cs_sm_index;
  /* As with generic XIO claims, serialize fixed-ID native claims with the
   * built-in provider; external raw SDK allocators require caller exclusion. */
  if (!esy_hb_resource_assembly_try_acquire()) return ESY_ERR_BUSY;
  const bool claimed = pio_sm_is_claimed(pio, smi);
  if (!claimed) pio_sm_claim(pio, smi);
  esy_hb_resource_assembly_release();
  if (claimed) return ESY_ERR_BUSY;
  c->cs_monitor_sm_index = (uint8_t)smi;
  c->cs_monitor_claimed = true;
  const int offset = pio_add_program(pio, &xio_spi_slave_cs_program);
  if (offset < 0) {
    cs_monitor_release(c);
    return ESY_ERR_NO_MEM;
  }
  c->cs_monitor_prog_offs = (uint8_t)offset;
  pio->instr_mem[offset] =
      encode_wait_gpio_for_pio(pio, !c->cs_active_low, c->pin_cs) |
      pio_encode_sideset(1u, 0u);
  pio->instr_mem[offset + 1] =
      encode_wait_gpio_for_pio(pio, c->cs_active_low, c->pin_cs) |
      pio_encode_sideset(1u, 1u);
  pio_sm_config sc = pio_get_default_sm_config();
  sm_config_set_clkdiv_int_frac(&sc, 1u, 0u);
  sm_config_set_wrap(&sc, (uint)offset, (uint)offset + 1u);
  sm_config_set_sideset(&sc, 1u, false, true);
  sm_config_set_sideset_pins(&sc, c->pin_miso);
  if (pio_sm_init(pio, smi, (uint)offset, &sc) < 0) {
    pio_remove_program(pio, &xio_spi_slave_cs_program, (uint)offset);
    cs_monitor_release(c);
    return ESY_ERR_INVALID_ARG;
  }
  c->cs_monitor_loaded = true;
  pio_sm_set_consecutive_pindirs(pio, smi, c->pin_miso, 1u, false);
  return ESY_OK;
}

static esy_err_t xio_spi_load_and_config_master(
    esy_xio_spi_ctx_t *c, const esy_xio_spi_config_t *xcfg) {
  const bool use_trig = (xcfg->pin_trigger != ESY_PIN_UNUSED);
  const bool is_three_wire = is_three_wire_config(xcfg);
  const uint32_t cycles_per_bit = use_trig ? 5u : 4u;
  const uint32_t sck_hz = xcfg->speed_hz ? xcfg->speed_hz : 1000000u;
  if (sck_hz > UINT32_MAX / cycles_per_bit) return ESY_ERR_INVALID_ARG;
  const uint8_t input_pin = is_three_wire ? xcfg->pin_mosi : xcfg->pin_miso;
  const pio_program_t *prog =
      (!use_trig)
          ? ((xcfg->mode == ESY_SPI_MODE_0 || xcfg->mode == ESY_SPI_MODE_2)
                 ? &xio_spi_cpha0_program
                 : &xio_spi_cpha1_program)
          : ((xcfg->mode == ESY_SPI_MODE_0 || xcfg->mode == ESY_SPI_MODE_2)
                 ? &xio_spi_cpha0_trig_program
                 : &xio_spi_cpha1_trig_program);

  esy_hb_xio_program_desc_t d = {
      .instructions = NULL,
      .length = 0,
      .origin = -1,
      .name =
          (!use_trig
               ? ((xcfg->mode == ESY_SPI_MODE_0 || xcfg->mode == ESY_SPI_MODE_2)
                      ? "xio_spi_cpha0"
                      : "xio_spi_cpha1")
               : ((xcfg->mode == ESY_SPI_MODE_0 || xcfg->mode == ESY_SPI_MODE_2)
                      ? "xio_spi_cpha0_trig"
                      : "xio_spi_cpha1_trig")),
      .native_prog = prog,
  };
  esy_err_t rc = esy_hb_xio_program_init(&c->prog, &d);
  if (rc != ESY_OK) return rc;

  rc = esy_hb_xio_sm_claim_manual(&c->prog, &c->sm, xcfg->instance,
                                      xcfg->sm_index);
  if (rc != ESY_OK) {
    (void)esy_hb_xio_program_deinit(&c->prog);
    return rc;
  }
  c->inst_index = xcfg->instance;
  c->sm_index = xcfg->sm_index;
  uint32_t entry_offset;
  rc = esy_hb_xio_sm_get_entry_offset(&c->sm, &entry_offset);
  if (rc != ESY_OK) {
    esy_hb_xio_sm_release(&c->sm);
    esy_hb_xio_program_deinit(&c->prog);
    return rc;
  }
  c->prog_offs = (uint8_t)entry_offset;

  esy_hb_xio_sm_config_t sc = (esy_hb_xio_sm_config_t){0};

  sc.out_pin_base = xcfg->pin_mosi;
  sc.out_pin_count = (xcfg->pin_mosi != ESY_PIN_UNUSED) ? 1 : 0;
  sc.set_pin_base = ESY_PIN_UNUSED;
  sc.set_pin_count = 0;
  sc.sideset_pin_base = xcfg->pin_sck;
  sc.sideset_pin_count = 1;
  sc.sideset_optional = use_trig ? 1u : 0u;
  sc.jmp_pin = ESY_PIN_UNUSED;
  sc.in_pin_base = input_pin;
  sc.in_pin_count = (input_pin != ESY_PIN_UNUSED) ? 1 : 0;

  c->is_msb_first = xcfg->is_msb_first;
  c->out_shift_right = c->is_msb_first ? 0 : 1;
  c->in_shift_right = c->is_msb_first ? 0 : 1;

  sc.in_shift_right = c->in_shift_right;
  sc.in_autopush = 1;
  sc.in_push_thresh = xcfg->data_bits;
  sc.out_shift_right = c->out_shift_right;
  sc.out_autopull = 1;
  sc.out_pull_thresh = xcfg->data_bits;
  sc.enable_on_init = 0;

  uint32_t clk_sys_hz = clock_get_hz(clk_sys);
  /* Triggered programs execute an extra loop-counter jump for every bit. */
  sc.clk_div_mul = sck_hz * cycles_per_bit;
  sc.clk_div_div = clk_sys_hz;

  rc = esy_hb_xio_sm_init(&c->sm, &sc);
  if (rc != ESY_OK) {
    esy_hb_xio_sm_release(&c->sm);
    esy_hb_xio_program_deinit(&c->prog);
    return rc;
  }

  PIO pio = ctx_pio(c);
  uint smi = c->sm_index;
  if (xcfg->pin_sck != ESY_PIN_UNUSED) {
    pio_gpio_init(pio, xcfg->pin_sck);
    pio_sm_set_consecutive_pindirs(pio, smi, xcfg->pin_sck, 1, true);
    pio_sm_set_sideset_pins(pio, smi, xcfg->pin_sck);
    bool cpol = (xcfg->mode == ESY_SPI_MODE_2 || xcfg->mode == ESY_SPI_MODE_3);
    gpio_set_outover(xcfg->pin_sck,
                     cpol ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);
    hw_set_bits(&pio->input_sync_bypass, 1u << xcfg->pin_sck);
  }
  if (xcfg->pin_mosi != ESY_PIN_UNUSED) {
    pio_gpio_init(pio, xcfg->pin_mosi);
    pio_sm_set_consecutive_pindirs(pio, smi, xcfg->pin_mosi, 1, true);
    pio_sm_set_out_pins(pio, smi, xcfg->pin_mosi, 1);
  }
  if (input_pin != ESY_PIN_UNUSED) {
    if (!is_three_wire) {
      pio_gpio_init(pio, input_pin);
      pio_sm_set_consecutive_pindirs(pio, smi, input_pin, 1, false);
    }
    pio_sm_set_in_pins(pio, smi, input_pin);
    hw_set_bits(&pio->input_sync_bypass, 1u << input_pin);
  }
  if (xcfg->role == ESY_SPI_ROLE_MASTER && xcfg->pin_cs != ESY_PIN_UNUSED) {
    gpio_init(xcfg->pin_cs);
    gpio_set_dir(xcfg->pin_cs, GPIO_OUT);
    gpio_put(xcfg->pin_cs, xcfg->cs_active_low ? 1 : 0);
  }

  if (use_trig) {
    pio_gpio_init(pio, xcfg->pin_trigger);
    pio_sm_set_consecutive_pindirs(pio, smi, xcfg->pin_trigger, 1, false);
    hw_set_bits(&pio->input_sync_bypass, 1u << xcfg->pin_trigger);

    pio_sm_clear_fifos(pio, smi);
    pio_sm_restart(pio, smi);
    pio_sm_set_enabled(pio, smi, false);

    int idx_wait0 = -1, idx_wait1 = -1, idx_sety = -1;
    for (uint i = 0; i < prog->length; ++i) {
      uint16_t insn = prog->instructions[i];
      if (is_wait0_placeholder(insn) && idx_wait0 < 0)
        idx_wait0 = (int)i;
      else if (is_wait1_placeholder(insn) && idx_wait1 < 0)
        idx_wait1 = (int)i;
      else if (is_sety31_placeholder(insn) && idx_sety < 0)
        idx_sety = (int)i;
    }
    if (idx_wait0 < 0 || idx_wait1 < 0 || idx_sety < 0) {
      esy_hb_xio_sm_release(&c->sm);
      esy_hb_xio_program_deinit(&c->prog);
      return ESY_ERR_STATE;
    }

    const uint32_t base = c->prog_offs;

    uint16_t w0 = encode_wait_gpio_for_pio(pio, false, xcfg->pin_trigger);
    uint16_t w1 = encode_wait_gpio_for_pio(pio, true, xcfg->pin_trigger);
    pio->instr_mem[base + (uint)idx_wait0] = w0;
    pio->instr_mem[base + (uint)idx_wait1] = w1;

    uint8_t yimm = (uint8_t)((xcfg->data_bits - 1) & 0x1F);
    pio->instr_mem[base + (uint)idx_sety] = pio_encode_set(pio_y, yimm);
  } else {
    pio_sm_clear_fifos(pio, smi);
    pio_sm_restart(pio, smi);
  }

  esy_err_t rc2 = esy_hb_xio_sm_start(&c->sm);
  if (rc2 != ESY_OK) {
    esy_hb_xio_sm_release(&c->sm);
    esy_hb_xio_program_deinit(&c->prog);
    return rc2;
  }

  c->pin_sck = xcfg->pin_sck;
  c->pin_mosi = xcfg->pin_mosi;
  c->pin_miso = xcfg->pin_miso;
  c->pin_cs = xcfg->pin_cs;
  c->pin_trigger = xcfg->pin_trigger;
  c->role = xcfg->role;
  c->sck_hz = sck_hz;
  c->data_bits = xcfg->data_bits;
  c->is_three_wire = is_three_wire;
  c->cpol =
      (xcfg->mode == ESY_SPI_MODE_2 || xcfg->mode == ESY_SPI_MODE_3) ? 1 : 0;
  c->cpha =
      (xcfg->mode == ESY_SPI_MODE_1 || xcfg->mode == ESY_SPI_MODE_3) ? 1 : 0;
  c->cs_active_low = xcfg->cs_active_low;
  c->cs_setup_ns = xcfg->cs_setup_ns;
  c->cs_hold_ns = xcfg->cs_hold_ns;
  c->cs_inactive_ns = xcfg->cs_inactive_ns;
  return ESY_OK;
}

static esy_err_t xio_spi_load_and_config_slave(
    esy_xio_spi_ctx_t *c, const esy_xio_spi_config_t *xcfg) {
  const pio_program_t *prog =
      (xcfg->mode == ESY_SPI_MODE_0 || xcfg->mode == ESY_SPI_MODE_2)
          ? &xio_spi_slave_cpha0_program
          : &xio_spi_slave_cpha1_program;

  esy_hb_xio_program_desc_t d = {
      .instructions = NULL,
      .length = 0,
      .origin = -1,
      .name = (xcfg->mode == ESY_SPI_MODE_0 || xcfg->mode == ESY_SPI_MODE_2)
                  ? "xio_spi_slave_cpha0"
                  : "xio_spi_slave_cpha1",
      .native_prog = prog,
  };
  esy_err_t rc = esy_hb_xio_program_init(&c->prog, &d);
  if (rc != ESY_OK) return rc;

  rc = esy_hb_xio_sm_claim_manual(&c->prog, &c->sm, xcfg->instance,
                                      xcfg->sm_index);
  if (rc != ESY_OK) {
    esy_hb_xio_program_deinit(&c->prog);
    return rc;
  }

  c->inst_index = xcfg->instance;
  c->sm_index = xcfg->sm_index;
  uint32_t entry_offset;
  rc = esy_hb_xio_sm_get_entry_offset(&c->sm, &entry_offset);
  if (rc != ESY_OK) {
    esy_hb_xio_sm_release(&c->sm);
    esy_hb_xio_program_deinit(&c->prog);
    return rc;
  }
  c->prog_offs = (uint8_t)entry_offset;

  c->pin_sck = xcfg->pin_sck;
  c->pin_mosi = xcfg->pin_mosi;
  c->pin_miso = xcfg->pin_miso;
  c->pin_cs = xcfg->pin_cs;
  c->pin_trigger = ESY_PIN_UNUSED;
  c->role = ESY_SPI_ROLE_SLAVE;
  c->cpol =
      (xcfg->mode == ESY_SPI_MODE_2 || xcfg->mode == ESY_SPI_MODE_3) ? 1 : 0;
  c->cpha =
      (xcfg->mode == ESY_SPI_MODE_1 || xcfg->mode == ESY_SPI_MODE_3) ? 1 : 0;
  c->cs_active_low = xcfg->cs_active_low;
  c->data_bits = xcfg->data_bits;
  c->is_msb_first = xcfg->is_msb_first;

  esy_hb_xio_sm_config_t sc = (esy_hb_xio_sm_config_t){0};

  /* Configure data-value mapping below without generic SM init enabling OE.
   * Only the independent CS monitor may write the MISO direction. */
  sc.out_pin_base = ESY_PIN_UNUSED;
  sc.out_pin_count = 0;
  sc.set_pin_base = ESY_PIN_UNUSED;
  sc.set_pin_count = 0;

  sc.sideset_pin_base = ESY_PIN_UNUSED;
  sc.sideset_pin_count = 0;
  sc.sideset_optional = 0;

  sc.jmp_pin = xcfg->pin_sck;

  sc.in_pin_base = xcfg->pin_mosi;
  sc.in_pin_count = (xcfg->pin_mosi != ESY_PIN_UNUSED) ? 1 : 0;

  c->out_shift_right = c->is_msb_first ? 0 : 1;
  c->in_shift_right = c->is_msb_first ? 0 : 1;

  sc.in_shift_right = c->in_shift_right;
  sc.in_autopush = 1;
  sc.in_push_thresh = xcfg->data_bits;

  sc.out_shift_right = c->out_shift_right;
  sc.out_autopull = 1;
  sc.out_pull_thresh = xcfg->data_bits;

  sc.enable_on_init = 0;
  sc.clk_div_mul = 1;
  sc.clk_div_div = 1;

  rc = cs_monitor_init(c, xcfg);
  if (rc != ESY_OK) {
    esy_hb_xio_sm_release(&c->sm);
    esy_hb_xio_program_deinit(&c->prog);
    return rc;
  }
  rc = esy_hb_xio_sm_init(&c->sm, &sc);
  if (rc != ESY_OK) {
    cs_monitor_release(c);
    esy_hb_xio_sm_release(&c->sm);
    esy_hb_xio_program_deinit(&c->prog);
    return rc;
  }

  PIO pio = c->inst_index ? pio1 : pio0;
  uint smi = c->sm_index;

  pio_sm_set_jmp_pin(pio, smi, xcfg->pin_sck);

  if (xcfg->pin_miso != ESY_PIN_UNUSED) {
    pio_gpio_init(pio, xcfg->pin_miso);
    pio_sm_set_out_pins(pio, smi, xcfg->pin_miso, 1);
    pio_sm_set_set_pins(pio, smi, xcfg->pin_miso, 1);
    pio_sm_exec(pio, smi, pio_encode_set(pio_pins, 0));
  }
  if (xcfg->pin_mosi != ESY_PIN_UNUSED) {
    pio_gpio_init(pio, xcfg->pin_mosi);
    pio_sm_set_consecutive_pindirs(pio, smi, xcfg->pin_mosi, 1, false);
    pio_sm_set_in_pins(pio, smi, xcfg->pin_mosi);
    hw_set_bits(&pio->input_sync_bypass, 1u << xcfg->pin_mosi);
  }
  if (xcfg->pin_sck != ESY_PIN_UNUSED) {
    pio_gpio_init(pio, xcfg->pin_sck);
    pio_sm_set_consecutive_pindirs(pio, smi, xcfg->pin_sck, 1, false);
    hw_set_bits(&pio->input_sync_bypass, 1u << xcfg->pin_sck);
    if (c->cpol) gpio_set_inover(xcfg->pin_sck, GPIO_OVERRIDE_INVERT);
  }
  if (xcfg->pin_cs != ESY_PIN_UNUSED) {
    pio_gpio_init(pio, xcfg->pin_cs);
    pio_sm_set_consecutive_pindirs(pio, smi, xcfg->pin_cs, 1, false);
    if (c->cs_active_low)
      gpio_pull_up(xcfg->pin_cs);
    else
      gpio_pull_down(xcfg->pin_cs);
    /* CS is asynchronous to clk_sys. The monitor uses the synchronizer and
     * always runs at clk_sys, independent of the data SM and SCK. */
    hw_clear_bits(&pio->input_sync_bypass, 1u << xcfg->pin_cs);
  }

  pio_sm_clear_fifos(pio, smi);
  pio_sm_restart(pio, smi);
  pio_sm_set_enabled(pio, smi, false);

  const uint32_t base = c->prog_offs;

  if (xcfg->pin_cs != ESY_PIN_UNUSED) {
    int idx0a = -1, idx0b = -1, idx1 = -1;
    for (uint i = 0; i < prog->length; ++i) {
      uint16_t insn = prog->instructions[i];
      if (is_wait0_placeholder(insn)) {
        if (idx0a < 0)
          idx0a = (int)i;
        else if (idx0b < 0)
          idx0b = (int)i;
      } else if (is_wait1_placeholder(insn)) {
        idx1 = (int)i;
      }
    }
    if (idx0a < 0 || idx0b < 0 || idx1 < 0) {
      cs_monitor_release(c);
      esy_hb_xio_sm_release(&c->sm);
      esy_hb_xio_program_deinit(&c->prog);
      return ESY_ERR_STATE;
    }
    const bool cs_active = c->cs_active_low ? 0 : 1;
    const bool cs_inactive = !cs_active;

    uint16_t w_act = encode_wait_gpio_for_pio(pio, cs_active, xcfg->pin_cs);
    uint16_t w_inact = encode_wait_gpio_for_pio(pio, cs_inactive, xcfg->pin_cs);

    pio->instr_mem[base + (uint)idx0a] = w_act;
    pio->instr_mem[base + (uint)idx0b] = w_act;
    pio->instr_mem[base + (uint)idx1] = w_inact;
  }

  pio_sm_exec(pio, smi, pio_encode_set(pio_y, (xcfg->data_bits - 1) & 0x1F));

  esy_err_t rc2 = esy_hb_xio_sm_start(&c->sm);
  if (rc2 != ESY_OK) {
    cs_monitor_release(c);
    esy_hb_xio_sm_release(&c->sm);
    esy_hb_xio_program_deinit(&c->prog);
    return rc2;
  }
  if (c->cs_monitor_claimed)
    pio_sm_set_enabled(pio, c->cs_monitor_sm_index, true);

  return ESY_OK;
}

static esy_err_t xio_spi_load_and_config(esy_xio_spi_ctx_t *c,
                                         const esy_xio_spi_config_t *xcfg) {
  if (xcfg->role == ESY_SPI_ROLE_SLAVE)
    return xio_spi_load_and_config_slave(c, xcfg);
  else
    return xio_spi_load_and_config_master(c, xcfg);
}

esy_err_t esy_hb_xio_spi_init(esy_spi_t *spi,
                                  const esy_xio_spi_config_t *cfg) {
  if (!spi || !cfg) return ESY_ERR_INVALID_ARG;
  if (spi->ctx != NULL) return ESY_ERR_STATE;
  if (cfg->role == ESY_SPI_ROLE_SLAVE && cfg->pin_cs == ESY_PIN_UNUSED)
    return ESY_ERR_NOT_SUPPORTED;
  if (cfg->role == ESY_SPI_ROLE_SLAVE && cfg->pin_miso != ESY_PIN_UNUSED &&
      (cfg->slave_cs_sm_index >= NUM_PIO_STATE_MACHINES ||
       cfg->slave_cs_sm_index == cfg->sm_index ||
       cfg->pin_miso == cfg->pin_sck || cfg->pin_miso == cfg->pin_cs ||
       cfg->pin_miso == cfg->pin_mosi))
    return ESY_ERR_INVALID_ARG;
  const uint8_t pins[] = {cfg->pin_sck, cfg->pin_mosi, cfg->pin_miso,
                          cfg->pin_cs, cfg->pin_trigger};
  for (size_t i = 0u; i < sizeof(pins) / sizeof(pins[0]); ++i) {
    if (pins[i] != ESY_PIN_UNUSED &&
        (pins[i] >= XIO_SPI_PIN_MASK_BITS || pins[i] >= NUM_BANK0_GPIOS))
      return ESY_ERR_INVALID_ARG;
  }
  if (cfg->pin_sck == ESY_PIN_UNUSED) return ESY_ERR_INVALID_ARG;
  if (cfg->pin_miso == ESY_PIN_UNUSED && cfg->pin_mosi == ESY_PIN_UNUSED)
    return ESY_ERR_INVALID_ARG;
  if (is_three_wire_config(cfg) && cfg->pin_trigger != ESY_PIN_UNUSED)
    return ESY_ERR_NOT_SUPPORTED;
  if (is_three_wire_config(cfg) && cfg->role == ESY_SPI_ROLE_SLAVE)
    return ESY_ERR_INVALID_ARG;
  if (cfg->data_bits < 2 || cfg->data_bits > 32) return ESY_ERR_INVALID_ARG;

  esy_xio_spi_ctx_t *ctx = get_ctx(spi);
  memset(ctx, 0, sizeof(*ctx));

  esy_err_t rc = xio_spi_load_and_config(ctx, cfg);
  if (rc == ESY_OK) {
    ctx->bound_spi = spi;
    ctx->initialized = true;
    spi->ctx = ctx;
  } else {
    memset(ctx, 0, sizeof(*ctx));
  }
  return rc;
}

void esy_hb_xio_spi_deinit(esy_spi_t *spi) {
  if (!spi) return;
  esy_xio_spi_ctx_t *c = get_ctx(spi);
  if (c->dma_active) return;
  if (!c->initialized) return;

  esy_hb_xio_sm_stop(&c->sm);
  cs_monitor_release(c);
  esy_hb_xio_sm_release(&c->sm);
  esy_hb_xio_program_deinit(&c->prog);

  if (c->pin_sck != ESY_PIN_UNUSED) {
    gpio_set_outover(c->pin_sck, GPIO_OVERRIDE_NORMAL);
    gpio_set_inover(c->pin_sck, GPIO_OVERRIDE_NORMAL);
  }
  if (is_master(c) && c->pin_cs != ESY_PIN_UNUSED) {
    gpio_put(c->pin_cs, c->cs_active_low ? 1 : 0);
  }
  memset(c, 0, sizeof(*c));
  spi->ctx = NULL;
}

static inline void wait_tx_done_blocking_master(esy_xio_spi_ctx_t *c) {
  PIO pio = ctx_pio(c);
  uint sm = c->sm_index;

  while (!pio_sm_is_tx_fifo_empty(pio, sm)) {
    tight_loop_contents();
  }

  if (c->pin_trigger != ESY_PIN_UNUSED) {
    const uint32_t base = c->prog_offs;
    for (;;) {
      uint pc = pio_sm_get_pc(pio, sm);
      if (pc == base || pc == base + 1) break;
      tight_loop_contents();
    }
    return;
  }

  uint32_t mask = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);
  pio->fdebug = mask;
  while (!(pio->fdebug & mask)) {
    tight_loop_contents();
  }
  pio->fdebug = mask;
}

static uint32_t three_wire_half_period_ns(const esy_xio_spi_ctx_t *c) {
  return c->sck_hz ? (uint32_t)((500000000ull + c->sck_hz - 1u) / c->sck_hz)
                   : 500u;
}

static void three_wire_sio_begin(esy_xio_spi_ctx_t *c) {
  PIO pio = ctx_pio(c);
  const uint smi = c->sm_index;

  pio_sm_set_enabled(pio, smi, false);
  pio_sm_clear_fifos(pio, smi);
  pio_sm_restart(pio, smi);

  /* Preload the SIO output state before changing the pin mux. The external
   * chip select may already be active for MASTER_NO_CS transfers, so exposing
   * a stale SIO clock level here would create an extra serial clock edge. */
  gpio_put(c->pin_sck, false);
  gpio_put(c->pin_mosi, false);
  gpio_set_dir(c->pin_sck, GPIO_OUT);
  gpio_set_dir(c->pin_mosi, GPIO_OUT);
  gpio_set_function(c->pin_sck, GPIO_FUNC_SIO);
  gpio_set_function(c->pin_mosi, GPIO_FUNC_SIO);
  gpio_set_pulls(c->pin_mosi, false, false);
}

static void three_wire_sio_write(esy_xio_spi_ctx_t *c, const uint8_t *tx_buf,
                                 size_t tx_len, uint32_t half_period_ns,
                                 bool release_after_last_bit) {
  for (size_t frame_index = 0u; frame_index < tx_len; frame_index++) {
    for (uint8_t bit_index = 0u; bit_index < c->data_bits; bit_index++) {
      const uint8_t shift = c->is_msb_first
                                ? (uint8_t)(c->data_bits - 1u - bit_index)
                                : bit_index;
      const bool bit = ((tx_buf[frame_index] >> shift) & 1u) != 0u;
      const bool is_last =
          (frame_index == (tx_len - 1u)) && (bit_index == (c->data_bits - 1u));
      gpio_put(c->pin_mosi, bit);
      busy_wait_ns(half_period_ns);
      gpio_put(c->pin_sck, true);
      busy_wait_ns(half_period_ns);
      if (is_last && release_after_last_bit) gpio_set_dir(c->pin_mosi, GPIO_IN);
      gpio_put(c->pin_sck, false);
    }
  }
}

static void three_wire_sio_end(esy_xio_spi_ctx_t *c) {
  PIO pio = ctx_pio(c);
  const uint smi = c->sm_index;

  pio_gpio_init(pio, c->pin_sck);
  pio_gpio_init(pio, c->pin_mosi);
  pio_sm_set_consecutive_pindirs(pio, smi, c->pin_sck, 1u, true);
  pio_sm_set_consecutive_pindirs(pio, smi, c->pin_mosi, 1u, false);
  pio_sm_clear_fifos(pio, smi);
  pio_sm_restart(pio, smi);
  pio_sm_clkdiv_restart(pio, smi);
  pio_sm_set_enabled(pio, smi, true);
}

int32_t esy_hb_xio_spi_transfer_blocking(esy_spi_t *spi,
                                             const uint8_t *tx_buf,
                                             uint8_t *rx_buf, size_t len) {
  if (!spi || (!tx_buf && !rx_buf) || !len) return ESY_ERR_INVALID_ARG;
  esy_xio_spi_ctx_t *c = get_ctx(spi);
  if (!c || !c->initialized) return ESY_ERR_STATE;
  if (c->dma_active) return ESY_ERR_BUSY;
  if (c->data_bits < 2 || c->data_bits > 8) return ESY_ERR_INVALID_ARG;
  if (c->is_three_wire && tx_buf && rx_buf) return ESY_ERR_NOT_SUPPORTED;

  PIO pio = ctx_pio(c);
  uint smi = c->sm_index;

  if (is_master(c)) {
    set_three_wire_direction(c, tx_buf != NULL);
    while (!pio_sm_is_rx_fifo_empty(pio, smi)) {
      (void)pio->rxf[smi];
    }

    cs_assert(c);
    for (size_t i = 0; i < len; ++i) {
      uint8_t t = tx_buf ? tx_buf[i] : 0xFFu;
      while (pio_sm_is_tx_fifo_full(pio, smi)) {
        tight_loop_contents();
      }
      pio->txf[smi] = pack_tx8(c, t);
      while (pio_sm_is_rx_fifo_empty(pio, smi)) {
        tight_loop_contents();
      }
      uint32_t wrx = pio->rxf[smi];
      if (rx_buf) rx_buf[i] = unpack_rx8(c, wrx);
    }
    wait_tx_done_blocking_master(c);
    cs_release(c);
  } else {
    if (c->pin_cs != ESY_PIN_UNUSED) {
      const int cs_inactive = c->cs_active_low ? 1 : 0;
      if (gpio_get(c->pin_cs) == cs_inactive) {
        while (!pio_sm_is_rx_fifo_empty(pio, smi)) {
          (void)pio->rxf[smi];
        }
      }
    }

    /* Every completed frame pushes one RX word, even for transmit-only.
     * Retire exactly len words instead of clearing a possibly final TXSTALL
     * and waiting for another CS assertion after owner preemption. Keep one
     * frame queued ahead so TX-only and full duplex share the same accounting. */
    while (pio_sm_is_tx_fifo_full(pio, smi)) {
      tight_loop_contents();
    }
    pio->txf[smi] = pack_tx8(c, tx_buf ? tx_buf[0] : 0xFFu);
    for (size_t i = 0; i < len; ++i) {
      if (i + 1u < len) {
        while (pio_sm_is_tx_fifo_full(pio, smi)) {
          tight_loop_contents();
        }
        pio->txf[smi] = pack_tx8(c, tx_buf ? tx_buf[i + 1u] : 0xFFu);
      }
      while (pio_sm_is_rx_fifo_empty(pio, smi)) {
        tight_loop_contents();
      }
      const uint32_t wrx = pio->rxf[smi];
      if (rx_buf) rx_buf[i] = unpack_rx8(c, wrx);
    }
  }
  return (int32_t)len;
}

esy_err_t esy_hb_xio_spi_write_then_read_blocking(esy_spi_t *spi,
                                                      const uint8_t *tx_buf,
                                                      size_t tx_len,
                                                      uint8_t *rx_buf,
                                                      size_t rx_len) {
  if (!spi || !tx_buf || !tx_len || !rx_buf || !rx_len)
    return ESY_ERR_INVALID_ARG;
  esy_xio_spi_ctx_t *c = get_ctx(spi);
  if (!c || !c->initialized) return ESY_ERR_STATE;
  if (c->dma_active) return ESY_ERR_BUSY;
  if (!is_master(c) || !c->is_three_wire || c->data_bits < 2u ||
      c->data_bits > 8u || c->cpha || c->cpol)
    return ESY_ERR_NOT_SUPPORTED;

  const uint32_t half_period_ns = three_wire_half_period_ns(c);

  three_wire_sio_begin(c);
  cs_assert(c);
  three_wire_sio_write(c, tx_buf, tx_len, half_period_ns, true);

  busy_wait_ns(half_period_ns);
  for (size_t frame_index = 0u; frame_index < rx_len; frame_index++) {
    uint8_t value = 0u;
    for (uint8_t bit_index = 0u; bit_index < c->data_bits; bit_index++) {
      gpio_put(c->pin_sck, true);
      busy_wait_ns(half_period_ns);
      if (c->is_msb_first)
        value = (uint8_t)((value << 1u) | (gpio_get(c->pin_miso) ? 1u : 0u));
      else
        value |= (uint8_t)((gpio_get(c->pin_miso) ? 1u : 0u) << bit_index);
      gpio_put(c->pin_sck, false);
      busy_wait_ns(half_period_ns);
    }
    rx_buf[frame_index] = value;
  }
  cs_release(c);
  three_wire_sio_end(c);
  return ESY_OK;
}

int32_t esy_hb_xio_spi_write_blocking(esy_spi_t *spi, const uint8_t *tx_buf,
                                          size_t len) {
  return esy_hb_xio_spi_transfer_blocking(spi, tx_buf, NULL, len);
}

int32_t esy_hb_xio_spi_read_blocking(esy_spi_t *spi, uint8_t *rx_buf,
                                         size_t len) {
  return esy_hb_xio_spi_transfer_blocking(spi, NULL, rx_buf, len);
}

int32_t esy_hb_xio_spi_transfer16_blocking(esy_spi_t *spi,
                                               const uint16_t *tx_buf,
                                               uint16_t *rx_buf, size_t len) {
  if (!spi || (!tx_buf && !rx_buf) || !len) return ESY_ERR_INVALID_ARG;
  esy_xio_spi_ctx_t *c = get_ctx(spi);
  if (!c || !c->initialized) return ESY_ERR_STATE;
  if (c->dma_active) return ESY_ERR_BUSY;
  if (c->data_bits < 9 || c->data_bits > 16) return ESY_ERR_INVALID_ARG;
  if (c->is_three_wire && tx_buf && rx_buf) return ESY_ERR_NOT_SUPPORTED;

  PIO pio = ctx_pio(c);
  uint smi = c->sm_index;

  if (is_master(c)) {
    set_three_wire_direction(c, tx_buf != NULL);
    while (!pio_sm_is_rx_fifo_empty(pio, smi)) {
      (void)pio->rxf[smi];
    }

    cs_assert(c);
    for (size_t i = 0; i < len; ++i) {
      uint16_t t = tx_buf ? tx_buf[i] : 0xFFFFu;
      while (pio_sm_is_tx_fifo_full(pio, smi)) {
        tight_loop_contents();
      }
      pio->txf[smi] = pack_tx16(c, t);
      while (pio_sm_is_rx_fifo_empty(pio, smi)) {
        tight_loop_contents();
      }
      uint32_t wrx = pio->rxf[smi];
      if (rx_buf) rx_buf[i] = unpack_rx16(c, wrx);
    }
    wait_tx_done_blocking_master(c);
    cs_release(c);
  } else {
    if (c->pin_cs != ESY_PIN_UNUSED) {
      const int cs_inactive = c->cs_active_low ? 1 : 0;
      if (gpio_get(c->pin_cs) == cs_inactive) {
        while (!pio_sm_is_rx_fifo_empty(pio, smi)) {
          (void)pio->rxf[smi];
        }
      }
    }

    /* Every completed frame pushes one RX word, even for transmit-only.
     * Retire exactly len words instead of clearing a possibly final TXSTALL
     * and waiting for another CS assertion after owner preemption. Keep one
     * frame queued ahead so TX-only and full duplex share the same accounting. */
    while (pio_sm_is_tx_fifo_full(pio, smi)) {
      tight_loop_contents();
    }
    pio->txf[smi] = pack_tx16(c, tx_buf ? tx_buf[0] : 0xFFFFu);
    for (size_t i = 0; i < len; ++i) {
      if (i + 1u < len) {
        while (pio_sm_is_tx_fifo_full(pio, smi)) {
          tight_loop_contents();
        }
        pio->txf[smi] = pack_tx16(c, tx_buf ? tx_buf[i + 1u] : 0xFFFFu);
      }
      while (pio_sm_is_rx_fifo_empty(pio, smi)) {
        tight_loop_contents();
      }
      const uint32_t wrx = pio->rxf[smi];
      if (rx_buf) rx_buf[i] = unpack_rx16(c, wrx);
    }
  }
  return (int32_t)len;
}

int32_t esy_hb_xio_spi_write16_blocking(esy_spi_t *spi,
                                            const uint16_t *tx_buf,
                                            size_t len) {
  return esy_hb_xio_spi_transfer16_blocking(spi, tx_buf, NULL, len);
}

int32_t esy_hb_xio_spi_read16_blocking(esy_spi_t *spi, uint16_t *rx_buf,
                                           size_t len) {
  return esy_hb_xio_spi_transfer16_blocking(spi, NULL, rx_buf, len);
}

/* DMA completion is retired by the serialized owner task. No DMA ISR waits
 * for PIO or invokes a user callback. Both channels must retire before buffers
 * and the HAL device reference can be released. */
static esy_err_t retire_dma(esy_xio_spi_ctx_t *c, bool reset_sm) {
  if (reset_sm) pio_sm_set_enabled(ctx_pio(c), c->sm_index, false);
  esy_err_t tx = ESY_OK, rx = ESY_OK;
  if (c->tx_owned) {
    size_t remaining = SIZE_MAX;
    (void)esy_hb_dma_get_trans_count_remaining(c->tx_dreq, c->tx_channel, &remaining);
    tx = esy_hb_dma_abort(c, c->tx_dreq, c->tx_channel);
    if (tx == ESY_OK) {
      c->tx_owned = false;
      c->tx_length = remaining <= c->tx_length / c->tx_unit_bytes
          ? c->tx_length - remaining * c->tx_unit_bytes : 0u;
    }
  }
  if (c->rx_owned) {
    size_t remaining = SIZE_MAX;
    (void)esy_hb_dma_get_trans_count_remaining(c->rx_dreq, c->rx_channel, &remaining);
    rx = esy_hb_dma_abort(c, c->rx_dreq, c->rx_channel);
    if (rx == ESY_OK) {
      c->rx_owned = false;
      c->rx_length = remaining <= c->rx_length / c->rx_unit_bytes
          ? c->rx_length - remaining * c->rx_unit_bytes : 0u;
    }
  }
  if (tx != ESY_OK) return tx;
  if (rx != ESY_OK) return rx;
  if (reset_sm) {
    pio_sm_clear_fifos(ctx_pio(c), c->sm_index);
    pio_sm_restart(ctx_pio(c), c->sm_index);
    pio_sm_exec(ctx_pio(c), c->sm_index, pio_encode_jmp(c->prog_offs));
    pio_sm_set_enabled(ctx_pio(c), c->sm_index, true);
  }
  cs_release(c);
  c->dma_active = false;
  return ESY_OK;
}

bool esy_hb_xio_spi_is_busy(esy_spi_t *spi) {
  return spi != NULL && get_ctx(spi)->dma_active;
}

esy_err_t esy_hb_xio_spi_abort(esy_spi_t *spi) {
  if (spi == NULL) return ESY_ERR_INVALID_ARG;
  esy_xio_spi_ctx_t *c = get_ctx(spi);
  if (!c->initialized) return ESY_ERR_STATE;
  if (!c->dma_active) return ESY_OK;
  esy_err_t ret = retire_dma(c, true);
  if (ret == ESY_OK) {
    c->pending_cb = NULL;
    c->pending_arg = NULL;
  }
  return ret;
}

esy_err_t esy_hb_xio_spi_poll(esy_spi_t *spi) {
  if (spi == NULL) return ESY_ERR_INVALID_ARG;
  esy_xio_spi_ctx_t *c = get_ctx(spi);
  if (!c->initialized) return ESY_ERR_STATE;
  if (!c->dma_active) return ESY_OK;
  uint32_t errors = ((c->tx_owned ? dma_channel_hw_addr(c->tx_channel)->ctrl_trig : 0u) |
                     (c->rx_owned ? dma_channel_hw_addr(c->rx_channel)->ctrl_trig : 0u)) &
                    (DMA_CH0_CTRL_TRIG_READ_ERROR_BITS |
                     DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS);
  c->dma_failed = c->dma_failed || errors != 0u;
  if (!c->dma_failed && ((c->tx_owned && esy_hb_dma_is_busy(c->tx_dreq, c->tx_channel)) ||
                  (c->rx_owned && esy_hb_dma_is_busy(c->rx_dreq, c->rx_channel))))
    return ESY_ERR_BUSY;
  size_t tx_remaining = 0u, rx_remaining = 0u;
  if ((c->tx_owned && esy_hb_dma_get_trans_count_remaining(c->tx_dreq, c->tx_channel,
                                                &tx_remaining) != ESY_OK) ||
      (c->rx_owned && esy_hb_dma_get_trans_count_remaining(c->rx_dreq, c->rx_channel,
                                                &rx_remaining) != ESY_OK))
    return ESY_ERR_FAIL;
  bool failed = c->dma_failed || tx_remaining || rx_remaining;
  if (!failed) {
    PIO pio = ctx_pio(c);
    if (!pio_sm_is_tx_fifo_empty(pio, c->sm_index)) return ESY_ERR_BUSY;
    /* Slave RX DMA counts every sampled frame, including dummy RX for TX-only.
     * Unlike a master, it may already be waiting for the next CS assertion;
     * no future stall/clock event is needed to certify this transaction. */
    if (is_master(c) && c->pin_trigger != ESY_PIN_UNUSED) {
      uint pc = pio_sm_get_pc(pio, c->sm_index);
      if (pc != c->prog_offs && pc != c->prog_offs + 1u) return ESY_ERR_BUSY;
    } else if (is_master(c)) {
      uint32_t mask = 1u << (PIO_FDEBUG_TXSTALL_LSB + c->sm_index);
      if (!c->wire_check_started) {
        pio->fdebug = mask;
        c->wire_check_started = true;
        return ESY_ERR_BUSY;
      }
      if (!(pio->fdebug & mask)) return ESY_ERR_BUSY;
    }
  }
  esy_spi_dma_transfer_result_t result = {
      .status = c->dma_failed ? ESY_SPI_DMA_TRANSFER_DMA_ERROR :
                failed ? ESY_SPI_DMA_TRANSFER_SHORT : ESY_SPI_DMA_TRANSFER_OK,
      .tx_transferred = c->tx_owned ? c->tx_length - tx_remaining * c->tx_unit_bytes : c->tx_length,
      .rx_transferred = c->rx_owned ? c->rx_length - rx_remaining * c->rx_unit_bytes : c->rx_length,
  };
  esy_err_t ret = retire_dma(c, failed);
  if (ret != ESY_OK) return ret;
  esy_spi_dma_complete_cb_t cb = c->pending_cb;
  void *arg = c->pending_arg;
  c->pending_cb = NULL;
  c->pending_arg = NULL;
  if (cb != NULL) cb(spi, &result, arg);
  return ESY_OK;
}

esy_err_t esy_hb_xio_spi_transfer_dma(esy_spi_t *spi,
                                          esy_dma_config_t *tx_dma_config,
                                          esy_dma_config_t *rx_dma_config,
                                          esy_spi_dma_complete_cb_t complete_cb,
                                          void *user_data) {
  if (!spi || !tx_dma_config || !rx_dma_config) return ESY_ERR_INVALID_ARG;
  esy_xio_spi_ctx_t *c = get_ctx(spi);
  if (!c || !c->initialized) return ESY_ERR_STATE;
  if (c->dma_active) return ESY_ERR_BUSY;
  if (tx_dma_config->circular || rx_dma_config->circular)
    return ESY_ERR_NOT_SUPPORTED;
  if (tx_dma_config->inner.channel == rx_dma_config->inner.channel ||
      tx_dma_config->inner.channel >= NUM_DMA_CHANNELS ||
      rx_dma_config->inner.channel >= NUM_DMA_CHANNELS)
    return ESY_ERR_INVALID_ARG;
  if (tx_dma_config->length != rx_dma_config->length ||
      tx_dma_config->transfer_size != rx_dma_config->transfer_size)
    return ESY_ERR_INVALID_ARG;
  esy_dma_config_t tx = *tx_dma_config, rx = *rx_dma_config;
  tx_dma_config = &tx;
  rx_dma_config = &rx;
  if (c->is_three_wire) return ESY_ERR_NOT_SUPPORTED;

  if (tx_dma_config->transfer_size != ESY_DMA_TRANSFER_SIZE_8 &&
      tx_dma_config->transfer_size != ESY_DMA_TRANSFER_SIZE_16 &&
      tx_dma_config->transfer_size != ESY_DMA_TRANSFER_SIZE_32)
    return ESY_ERR_INVALID_ARG;
  /* Narrow FIFO accesses replicate TX lanes and select the matching RX lane.
   * Other frame widths require explicit packing and are not DMA formats. */
  if (!((c->data_bits == 8u && tx_dma_config->transfer_size == ESY_DMA_TRANSFER_SIZE_8) ||
        (c->data_bits == 16u && tx_dma_config->transfer_size == ESY_DMA_TRANSFER_SIZE_16)))
    return ESY_ERR_NOT_SUPPORTED;

  PIO pio = ctx_pio(c);
  uint smi = c->sm_index;

  while (!pio_sm_is_rx_fifo_empty(pio, smi)) {
    (void)pio->rxf[smi];
  }

  c->pending_cb = complete_cb;
  c->pending_arg = user_data;
  c->tx_length = tx_dma_config->length;
  c->rx_length = rx_dma_config->length;
  c->tx_unit_bytes = 1u << tx_dma_config->transfer_size;
  c->rx_unit_bytes = 1u << rx_dma_config->transfer_size;
  c->wire_check_started = false;
  c->tx_owned = false;
  c->rx_owned = false;
  c->dma_failed = false;
  c->tx_dreq = tx_dma_config->inner.dreq;
  c->rx_dreq = rx_dma_config->inner.dreq;
  c->tx_channel = tx_dma_config->inner.channel;
  c->rx_channel = rx_dma_config->inner.channel;

  rx_dma_config->inner.direction = ESY_DMA_PERIPH_TO_MEM;
  const size_t rx_lane = c->in_shift_right ? sizeof(uint32_t) - c->rx_unit_bytes : 0u;
  rx_dma_config->src = (uint8_t *)&pio->rxf[smi] + rx_lane;
  rx_dma_config->callback = NULL;
  rx_dma_config->callback_arg = NULL;
  rx_dma_config->error_callback = NULL;
  rx_dma_config->error_callback_arg = NULL;

  tx_dma_config->inner.direction = ESY_DMA_MEM_TO_PERIPH;
  const size_t tx_lane = c->out_shift_right ? 0u : sizeof(uint32_t) - c->tx_unit_bytes;
  tx_dma_config->dst = (uint8_t *)&pio->txf[smi] + tx_lane;
  tx_dma_config->callback = NULL;
  tx_dma_config->callback_arg = NULL;
  tx_dma_config->error_callback = NULL;
  tx_dma_config->error_callback_arg = NULL;

  esy_err_t err = esy_hb_dma_prepare(c, rx_dma_config);
  if (err != ESY_OK) {
    c->pending_cb = NULL;
    c->pending_arg = NULL;
    return err;
  }
  c->rx_owned = true;
  err = esy_hb_dma_prepare(c, tx_dma_config);
  if (err != ESY_OK) {
    /* The Pico DMA abort contract synchronously retires valid channels. */
    (void)esy_hb_dma_abort(c, c->rx_dreq, c->rx_channel);
    c->rx_owned = false;
    c->pending_cb = NULL;
    c->pending_arg = NULL;
    return err;
  }
  c->tx_owned = true;
  c->dma_active = true;
  cs_assert(c);
  uint32_t mask = (1u << tx_dma_config->inner.channel) |
                  (1u << rx_dma_config->inner.channel);
  err = esy_hb_dma_start(c, mask);
  if (err != ESY_OK) {
    c->pending_cb = NULL;
    c->pending_arg = NULL;
    (void)retire_dma(c, true);
  }
  return err;
}

uint8_t esy_hb_xio_spi_get_tx_pin(esy_spi_t *spi) {
  if (!spi) return ESY_PIN_UNUSED;
  esy_xio_spi_ctx_t *ctx = get_ctx(spi);
  return ctx ? (is_master(ctx) ? ctx->pin_mosi : ctx->pin_miso)
             : ESY_PIN_UNUSED;
}
uint8_t esy_hb_xio_spi_get_rx_pin(esy_spi_t *spi) {
  if (!spi) return ESY_PIN_UNUSED;
  esy_xio_spi_ctx_t *ctx = get_ctx(spi);
  return ctx ? (is_master(ctx)
                    ? (ctx->is_three_wire ? ctx->pin_mosi : ctx->pin_miso)
                    : ctx->pin_mosi)
             : ESY_PIN_UNUSED;
}
uint8_t esy_hb_xio_spi_get_pin_sck(esy_spi_t *spi) {
  if (!spi) return ESY_PIN_UNUSED;
  esy_xio_spi_ctx_t *ctx = get_ctx(spi);
  return ctx ? ctx->pin_sck : ESY_PIN_UNUSED;
}
