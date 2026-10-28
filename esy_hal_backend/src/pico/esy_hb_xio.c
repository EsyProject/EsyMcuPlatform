/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_xio.h"
#include "esy_hb_resource_assembly_internal.h"

#include <string.h>
#include <stdbool.h>
#include <stddef.h>

#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"

#define XIO_PIN_MASK_BITS 32u
#define XIO_SET_PIN_LIMIT 5u
#define XIO_CLKDIV_MAX 65536u
#define XIO_CLKDIV_FRAC_BITS 8u

typedef struct {
    const struct pio_program* native_prog;
    struct pio_program copy_prog;
    uint8_t use_copy;
    int offset[2];
    uint8_t refcnt[2];
    uint32_t length_words;
    int32_t origin;
} pio_prog_rt_t;

typedef struct {
    uint8_t  pio_index;
    uint8_t  sm_index;
    uint16_t _pad;
    uint32_t entry_offset;
} pio_sm_rt_t;

_Static_assert(sizeof(pio_prog_rt_t) <= ESY_XIO_PROG_PRIV_BYTES,
               "ESY_XIO_PROG_PRIV_BYTES too small for program runtime");
_Static_assert(sizeof(pio_sm_rt_t)  <= ESY_XIO_SM_PRIV_BYTES,
               "ESY_XIO_SM_PRIV_BYTES too small for SM runtime");

static PIO pio_from_index(uint8_t idx) { return (idx == 0) ? pio0 : pio1; }
static int pio_index_of(PIO pio)       { return (pio == pio0) ? 0 : 1; }

static pio_prog_rt_t* prog_rt(esy_hb_xio_program_t* prog) {
    return (pio_prog_rt_t*)&prog->backend.bytes[0];
}
static pio_sm_rt_t* sm_rt(esy_hb_xio_sm_t* sm) {
    return (pio_sm_rt_t*)&sm->backend.bytes[0];
}

static const struct pio_program* prog_ptr(pio_prog_rt_t* rt) {
    return rt->use_copy ? &rt->copy_prog : rt->native_prog;
}

static bool claim_sm(PIO pio, uint8_t sm_idx) {
    /* Caller holds allocation assembly against platform Wi-Fi. Raw SDK users
     * require consumer serialization; fixed-SM claim panics on a race. */
    if (pio_sm_is_claimed(pio, sm_idx)) return false;
    pio_sm_claim(pio, sm_idx);
    return true;
}

static void release_sm(uint8_t pio_idx, uint8_t sm_idx) {
    pio_sm_unclaim(pio_from_index(pio_idx), sm_idx);
}

static esy_err_t ensure_prog_loaded(PIO pio, pio_prog_rt_t* rt, uint* out_off) {
    if (!rt || !out_off) return ESY_ERR_INVALID_ARG;
    int idx = pio_index_of(pio);
    if (rt->offset[idx] < 0) {
        const struct pio_program* pr = prog_ptr(rt);
        if (!pr || !pr->instructions || pr->length == 0) return ESY_ERR_INVALID_ARG;

        /* SDK add atomically reserves instruction memory. A separate can-add
         * query could go stale when a native provider loads on another core. */
        int off = rt->origin >= 0
            ? pio_add_program_at_offset(pio, pr, (uint)rt->origin)
            : pio_add_program(pio, pr);
        if (off < 0) return ESY_ERR_NO_MEM;
        rt->offset[idx] = off;
    }
    rt->refcnt[idx] += 1;
    *out_off = (uint)rt->offset[idx];
    return ESY_OK;
}

static void maybe_unload_prog(PIO pio, pio_prog_rt_t* rt) {
    int idx = pio_index_of(pio);
    if (rt->offset[idx] >= 0 && rt->refcnt[idx] == 0) {
        pio_remove_program(pio, prog_ptr(rt), (uint)rt->offset[idx]);
        rt->offset[idx] = -1;
    }
}

static bool clkdiv_from_ratio(uint32_t mul, uint32_t div,
                              uint16_t* out_integer, uint8_t* out_fraction) {
    uint32_t numerator = div ? div : 1u;
    uint32_t denominator = mul ? mul : 1u;
    if (numerator < denominator ||
        (uint64_t)numerator > (uint64_t)denominator * XIO_CLKDIV_MAX)
        return false;
    /* Quantize exactly to 16:8 without floating-point boundary rounding.
     * Hardware encodes the valid integer divider 65536 as zero. */
    uint64_t fixed = ((uint64_t)numerator << XIO_CLKDIV_FRAC_BITS) / denominator;
    *out_integer = (uint16_t)(fixed >> XIO_CLKDIV_FRAC_BITS);
    *out_fraction = (uint8_t)fixed;
    return true;
}

static bool pin_range_valid(uint8_t pin_base, uint8_t count) {
    return count > 0u && pin_base < XIO_PIN_MASK_BITS &&
           count <= XIO_PIN_MASK_BITS - pin_base &&
           pin_base < NUM_BANK0_GPIOS && count <= NUM_BANK0_GPIOS - pin_base;
}

static bool pin_mapping_valid(uint8_t pin_base, uint8_t count,
                              uint8_t maximum_count) {
    if (pin_base == ESY_PIN_UNUSED || count == 0u) return true;
    return count <= maximum_count && pin_range_valid(pin_base, count);
}

esy_err_t esy_hb_xio_program_init(esy_hb_xio_program_t* prog,
                                      const esy_hb_xio_program_desc_t* desc) {
    if (!prog || !desc) return ESY_ERR_INVALID_ARG;
    if (prog->initialized) return ESY_ERR_STATE;

    memset(prog, 0, sizeof(*prog));
    prog->desc.instructions = desc->instructions;
    prog->desc.length       = desc->length;
    prog->desc.origin       = desc->origin;
    prog->desc.name         = desc->name;
    prog->desc.native_prog  = desc->native_prog;

    pio_prog_rt_t* rt = prog_rt(prog);
    rt->native_prog   = (const struct pio_program*)prog->desc.native_prog;
    rt->use_copy      = (rt->native_prog == NULL) ? 1 : 0;
    rt->origin        = prog->desc.origin;

    if (rt->use_copy) {
        if (!prog->desc.instructions || prog->desc.length == 0) return ESY_ERR_INVALID_ARG;
        rt->copy_prog.instructions = prog->desc.instructions;
        rt->copy_prog.length       = (uint)prog->desc.length;
        rt->copy_prog.origin       = -1;
        rt->length_words           = prog->desc.length;
    } else {
        rt->length_words = rt->native_prog ? rt->native_prog->length : 0u;
    }

    rt->offset[0] = rt->offset[1] = -1;
    rt->refcnt[0] = rt->refcnt[1] = 0;

    prog->initialized = 1;
    return ESY_OK;
}

void esy_hb_xio_program_deinit(esy_hb_xio_program_t* prog) {
    if (!prog || !prog->initialized) return;

    pio_prog_rt_t* rt = prog_rt(prog);

    if (rt->refcnt[0] || rt->refcnt[1]) return;

    if (rt->offset[0] >= 0) {
        pio_remove_program(pio0, prog_ptr(rt), (uint)rt->offset[0]);
        rt->offset[0] = -1;
    }
    if (rt->offset[1] >= 0) {
        pio_remove_program(pio1, prog_ptr(rt), (uint)rt->offset[1]);
        rt->offset[1] = -1;
    }

    memset(prog, 0, sizeof(*prog));
}

esy_err_t esy_hb_xio_sm_claim_manual(esy_hb_xio_program_t* prog,
                                         esy_hb_xio_sm_t* sm,
                                         uint8_t instance,
                                         uint8_t sm_index) {
    if (!prog || !sm) return ESY_ERR_INVALID_ARG;
    if (!prog->initialized) return ESY_ERR_STATE;
    if (sm->claimed) return ESY_ERR_STATE;
    if (!((instance == 0) || (instance == 1))) return ESY_ERR_INVALID_ARG;
    if (!(sm_index < 4)) return ESY_ERR_INVALID_ARG;

    pio_prog_rt_t* prt = prog_rt(prog);
    PIO pio = pio_from_index(instance);
    if (!esy_hb_resource_assembly_try_acquire()) return ESY_ERR_BUSY;
    bool claimed = claim_sm(pio, sm_index);
    esy_hb_resource_assembly_release();
    if (!claimed) return ESY_ERR_BUSY;

    uint off = 0;
    esy_err_t rc = ensure_prog_loaded(pio, prt, &off);
    if (rc != ESY_OK) {
        release_sm(instance, sm_index);
        return rc;
    }

    memset(sm, 0, sizeof(*sm));
    sm->program = prog;
    pio_sm_rt_t* srt = sm_rt(sm);
    srt->pio_index    = instance;
    srt->sm_index     = sm_index;
    srt->entry_offset = off;

    sm->claimed = 1;
    sm->started = 0;

    return ESY_OK;
}

esy_err_t esy_hb_xio_sm_get_location(esy_hb_xio_sm_t* sm,
                                         uint8_t* out_instance,
                                         uint8_t* out_sm_index) {
    if (!sm || !sm->claimed || !out_instance || !out_sm_index) return ESY_ERR_INVALID_ARG;
    pio_sm_rt_t* srt = sm_rt(sm);
    *out_instance = srt->pio_index;
    *out_sm_index = srt->sm_index;
    return ESY_OK;
}

esy_err_t esy_hb_xio_sm_get_entry_offset(esy_hb_xio_sm_t* sm,
                                         uint32_t *entry_offset) {
    if (!sm || !sm->claimed || !entry_offset) return ESY_ERR_INVALID_ARG;
    pio_sm_rt_t* srt = sm_rt(sm);
    *entry_offset = srt->entry_offset;
    return ESY_OK;
}

void esy_hb_xio_sm_release(esy_hb_xio_sm_t* sm) {
    if (!sm || !sm->claimed) return;

    pio_sm_rt_t* srt = sm_rt(sm);
    PIO  pio = pio_from_index(srt->pio_index);
    uint sm_index = srt->sm_index;

    pio_sm_set_enabled(pio, sm_index, false);

    pio_prog_rt_t* prt = prog_rt(sm->program);
    int idx = srt->pio_index;
    if (prt->refcnt[idx] > 0) {
        prt->refcnt[idx]--;
        maybe_unload_prog(pio, prt);
    }

    release_sm(srt->pio_index, sm_index);

    memset(sm, 0, sizeof(*sm));
}

esy_err_t esy_hb_xio_sm_init(esy_hb_xio_sm_t* sm,
                                 const esy_hb_xio_sm_config_t* cfg) {
    if (!sm || !cfg) return ESY_ERR_INVALID_ARG;
    if (!sm->claimed || !sm->program || !sm->program->initialized) return ESY_ERR_STATE;
    uint16_t divider_integer;
    uint8_t divider_fraction;
    if (!clkdiv_from_ratio(cfg->clk_div_mul, cfg->clk_div_div,
                           &divider_integer, &divider_fraction))
        return ESY_ERR_INVALID_ARG;
    if (cfg->sideset_optional > 1u || cfg->sideset_pin_count > 5u ||
        (cfg->sideset_optional &&
         (cfg->sideset_pin_count == 0u || cfg->sideset_pin_count > 4u)))
        return ESY_ERR_INVALID_ARG;
    if (!pin_mapping_valid(cfg->out_pin_base, cfg->out_pin_count,
                           XIO_PIN_MASK_BITS) ||
        !pin_mapping_valid(cfg->in_pin_base, cfg->in_pin_count,
                           XIO_PIN_MASK_BITS) ||
        !pin_mapping_valid(cfg->set_pin_base, cfg->set_pin_count,
                           XIO_SET_PIN_LIMIT) ||
        !pin_mapping_valid(cfg->sideset_pin_base, cfg->sideset_pin_count,
                           XIO_SET_PIN_LIMIT) ||
        (cfg->jmp_pin != ESY_PIN_UNUSED && !pin_range_valid(cfg->jmp_pin, 1u)) ||
        cfg->out_pull_thresh > XIO_PIN_MASK_BITS ||
        cfg->in_push_thresh > XIO_PIN_MASK_BITS)
        return ESY_ERR_INVALID_ARG;

    pio_sm_rt_t* srt = sm_rt(sm);
    PIO  pio = pio_from_index(srt->pio_index);
    uint sm_index = srt->sm_index;

    pio_prog_rt_t* prt = prog_rt(sm->program);
    const struct pio_program* pr = prog_ptr(prt);

    pio_sm_config c = pio_get_default_sm_config();

    sm_config_set_clkdiv_int_frac(&c, divider_integer, divider_fraction);

    {
        uint8_t out_th = cfg->out_pull_thresh ? cfg->out_pull_thresh : 32;
        sm_config_set_out_shift(&c, cfg->out_shift_right ? true : false,
                                   cfg->out_autopull    ? true : false, out_th);
        uint8_t in_th  = cfg->in_push_thresh  ? cfg->in_push_thresh  : 32;
        sm_config_set_in_shift(&c, cfg->in_shift_right ? true : false,
                                  cfg->in_autopush   ? true : false, in_th);
    }

    if (cfg->out_pin_base != ESY_PIN_UNUSED && cfg->out_pin_count) {
        sm_config_set_out_pins(&c, cfg->out_pin_base, cfg->out_pin_count);
        pio_sm_set_consecutive_pindirs(pio, sm_index, cfg->out_pin_base, cfg->out_pin_count, true);
        for (uint i = 0; i < cfg->out_pin_count; ++i) pio_gpio_init(pio, cfg->out_pin_base + i);
    }
    if (cfg->in_pin_base != ESY_PIN_UNUSED && cfg->in_pin_count) {
        sm_config_set_in_pins(&c, cfg->in_pin_base);
        pio_sm_set_consecutive_pindirs(pio, sm_index, cfg->in_pin_base, cfg->in_pin_count, false);
        for (uint i = 0; i < cfg->in_pin_count; ++i) pio_gpio_init(pio, cfg->in_pin_base + i);
    }
    if (cfg->set_pin_base != ESY_PIN_UNUSED && cfg->set_pin_count) {
        sm_config_set_set_pins(&c, cfg->set_pin_base, cfg->set_pin_count);
        pio_sm_set_consecutive_pindirs(pio, sm_index, cfg->set_pin_base, cfg->set_pin_count, true);
        for (uint i = 0; i < cfg->set_pin_count; ++i) pio_gpio_init(pio, cfg->set_pin_base + i);
    }
    if (cfg->sideset_pin_base != ESY_PIN_UNUSED && cfg->sideset_pin_count) {
        /* Public count names physical pins; the native field also includes
         * the optional instruction enable bit, which is not a GPIO. */
        sm_config_set_sideset(&c, cfg->sideset_pin_count + cfg->sideset_optional,
                                 cfg->sideset_optional ? true : false,
                                 false);
        sm_config_set_sideset_pins(&c, cfg->sideset_pin_base);
        pio_sm_set_consecutive_pindirs(pio, sm_index, cfg->sideset_pin_base, cfg->sideset_pin_count, true);
        for (uint i = 0; i < cfg->sideset_pin_count; ++i) pio_gpio_init(pio, cfg->sideset_pin_base + i);
    }
    if (cfg->jmp_pin != ESY_PIN_UNUSED) {
        sm_config_set_jmp_pin(&c, cfg->jmp_pin);
        pio_gpio_init(pio, cfg->jmp_pin);
        pio_sm_set_consecutive_pindirs(pio, sm_index, cfg->jmp_pin, 1, false);
    }

    uint wrap_bottom = srt->entry_offset;
    uint wrap_top    = srt->entry_offset + (pr ? pr->length : prt->length_words);
    if (wrap_top) wrap_top -= 1;
    sm_config_set_wrap(&c, wrap_bottom, wrap_top);

    pio_sm_init(pio, sm_index, srt->entry_offset, &c);

    pio_sm_set_enabled(pio, sm_index, cfg->enable_on_init ? true : false);
    sm->started = cfg->enable_on_init ? 1 : 0;

    return ESY_OK;
}

esy_err_t esy_hb_xio_sm_set_enabled(esy_hb_xio_sm_t* sm, bool enable) {
    if (!sm || !sm->claimed) return ESY_ERR_INVALID_ARG;
    pio_sm_rt_t* srt = sm_rt(sm);
    pio_sm_set_enabled(pio_from_index(srt->pio_index), srt->sm_index, enable);
    sm->started = enable ? 1 : 0;
    return ESY_OK;
}

esy_err_t esy_hb_xio_sm_start(esy_hb_xio_sm_t* sm) {
    if (!sm || !sm->claimed) return ESY_ERR_INVALID_ARG;
    pio_sm_rt_t* srt = sm_rt(sm);
    PIO pio = pio_from_index(srt->pio_index);
    uint si = srt->sm_index;

    pio_sm_clear_fifos(pio, si);
    pio_sm_restart(pio, si);
    pio_sm_clkdiv_restart(pio, si);
    pio_sm_exec(pio, si, pio_encode_jmp(srt->entry_offset));
    pio_sm_set_enabled(pio, si, true);
    sm->started = 1;
    return ESY_OK;
}

void esy_hb_xio_sm_stop(esy_hb_xio_sm_t* sm) {
    if (!sm || !sm->claimed) return;
    pio_sm_rt_t* srt = sm_rt(sm);
    pio_sm_set_enabled(pio_from_index(srt->pio_index), srt->sm_index, false);
    sm->started = 0;
}

esy_err_t esy_hb_xio_tx_try_put32(esy_hb_xio_sm_t* sm, uint32_t value, bool* out_ok) {
    if (!sm || !sm->claimed) return ESY_ERR_INVALID_ARG;
    pio_sm_rt_t* srt = sm_rt(sm);
    PIO pio = pio_from_index(srt->pio_index);
    uint si = srt->sm_index;

    bool ok = !pio_sm_is_tx_fifo_full(pio, si);
    if (ok) pio_sm_put(pio, si, value);
    if (out_ok) *out_ok = ok;
    return ESY_OK;
}

esy_err_t esy_hb_xio_tx_put32_blocking(esy_hb_xio_sm_t* sm, uint32_t value) {
    if (!sm || !sm->claimed) return ESY_ERR_INVALID_ARG;
    pio_sm_rt_t* srt = sm_rt(sm);
    PIO pio = pio_from_index(srt->pio_index);
    uint si = srt->sm_index;

    pio_sm_put_blocking(pio, si, value);

    if (sm->cb && pio_sm_is_tx_fifo_empty(pio, si)) {
        sm->cb(sm->user, ESY_XIO_EVT_TX_EMPTY);
    }
    return ESY_OK;
}

esy_err_t esy_hb_xio_rx_try_get32(esy_hb_xio_sm_t* sm, uint32_t* out_value, bool* out_ok) {
    if (!sm || !sm->claimed) return ESY_ERR_INVALID_ARG;
    pio_sm_rt_t* srt = sm_rt(sm);
    PIO pio = pio_from_index(srt->pio_index);
    uint si = srt->sm_index;

    bool ok = !pio_sm_is_rx_fifo_empty(pio, si);
    if (ok && out_value) *out_value = pio_sm_get(pio, si);
    if (out_ok) *out_ok = ok;

    if (ok && sm->cb) sm->cb(sm->user, ESY_XIO_EVT_RX_READY);
    return ESY_OK;
}

esy_err_t esy_hb_xio_rx_get32_blocking(esy_hb_xio_sm_t* sm, uint32_t* out_value) {
    if (!sm || !out_value || !sm->claimed) return ESY_ERR_INVALID_ARG;

    pio_sm_rt_t* srt = sm_rt(sm);
    PIO pio = pio_from_index(srt->pio_index);
    uint si = srt->sm_index;

    uint32_t v = pio_sm_get_blocking(pio, si);
    *out_value = v;

    return ESY_OK;
}


esy_err_t esy_hb_xio_tx_level(esy_hb_xio_sm_t* sm, uint32_t* out_level) {
    if (!sm || !sm->claimed || !out_level) return ESY_ERR_INVALID_ARG;
    pio_sm_rt_t* srt = sm_rt(sm);
    *out_level = (uint32_t)pio_sm_get_tx_fifo_level(pio_from_index(srt->pio_index), srt->sm_index);
    return ESY_OK;
}

esy_err_t esy_hb_xio_rx_level(esy_hb_xio_sm_t* sm, uint32_t* out_level) {
    if (!sm || !sm->claimed || !out_level) return ESY_ERR_INVALID_ARG;
    pio_sm_rt_t* srt = sm_rt(sm);
    *out_level = (uint32_t)pio_sm_get_rx_fifo_level(pio_from_index(srt->pio_index), srt->sm_index);
    return ESY_OK;
}

esy_err_t esy_hb_xio_set_event_cb(esy_hb_xio_sm_t* sm,
                                      esy_hb_xio_event_cb_t cb,
                                      void* user) {
    if (!sm || !sm->claimed) return ESY_ERR_INVALID_ARG;
    sm->cb = cb;
    sm->user = user;
    return ESY_OK;
}

esy_err_t esy_hb_xio_set_consecutive_pindirs(esy_hb_xio_sm_t* sm,
                                                 uint8_t pin_base, uint8_t count, bool out) {
    if (!sm || !sm->claimed) return ESY_ERR_INVALID_ARG;
    if (!pin_range_valid(pin_base, count)) return ESY_ERR_INVALID_ARG;
    pio_sm_rt_t* srt = sm_rt(sm);
    PIO pio = pio_from_index(srt->pio_index);
    pio_sm_set_consecutive_pindirs(pio, srt->sm_index, pin_base, count, out);
    for (uint i = 0; i < count; ++i) pio_gpio_init(pio, pin_base + i);
    return ESY_OK;
}

esy_err_t esy_hb_xio_write_pins_mask(esy_hb_xio_sm_t* sm,
                                         uint8_t pin_base, uint8_t count, uint32_t mask) {
    if (!sm || !sm->claimed) return ESY_ERR_INVALID_ARG;
    if (!pin_range_valid(pin_base, count)) return ESY_ERR_INVALID_ARG;
    pio_sm_rt_t* srt = sm_rt(sm);
    PIO pio = pio_from_index(srt->pio_index);

    uint32_t range_mask = count == XIO_PIN_MASK_BITS
        ? UINT32_MAX : (UINT32_C(1) << count) - 1u;
    uint32_t value_map = (mask & range_mask) << pin_base;
    uint32_t pin_mask = range_mask << pin_base;
    pio_sm_set_pins_with_mask(pio, srt->sm_index, value_map, pin_mask);
    return ESY_OK;
}

esy_err_t esy_hb_xio_restart(esy_hb_xio_sm_t* sm) {
    if (!sm || !sm->claimed) return ESY_ERR_INVALID_ARG;
    pio_sm_rt_t* srt = sm_rt(sm);
    PIO pio = pio_from_index(srt->pio_index);
    uint si = srt->sm_index;
    pio_sm_restart(pio, si);
    pio_sm_clkdiv_restart(pio, si);
    pio_sm_exec(pio, si, pio_encode_jmp(sm_rt(sm)->entry_offset));
    return ESY_OK;
}
