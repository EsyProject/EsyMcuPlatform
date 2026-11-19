/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_XIO_PROGRAM_DEFS_H
#define ESY_XIO_PROGRAM_DEFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Caller-owned program storage size; rebuild all users if changed. */
#define ESY_XIO_PROGRAM_STORAGE_BYTES 192u
/** @brief Caller-owned state-machine storage size; not a hardware capacity. */
#define ESY_XIO_PROGRAM_SM_STORAGE_BYTES 768u
/** @brief Maximum RP PIO v0 instructions in one program. */
#define ESY_XIO_PROGRAM_MAX_INSTRUCTIONS 32u

/** @brief Program storage. Initialize with {0}; never copy a live object. */
typedef union {
  max_align_t alignment; /**< Internal alignment, not application state. */
  uint8_t bytes[ESY_XIO_PROGRAM_STORAGE_BYTES]; /**< Private implementation. */
} esy_xio_program_t;

/** @brief SM storage. Initialize with {0}; retain until successful deinit. */
typedef union {
  max_align_t alignment; /**< Internal alignment, not application state. */
  uint8_t bytes[ESY_XIO_PROGRAM_SM_STORAGE_BYTES]; /**< Private implementation. */
} esy_xio_program_sm_t;

/**
 * @brief Explicit RP PIO v0 bytecode contract, without native SDK objects.
 *
 * Words use the RP PIO v0 instruction encoding. JMP addresses are program
 * relative and relocated on load. Instructions remain borrowed through program
 * deinit. Entry and wrap indices are relative to this array. The owner must
 * validate the program's pin/IRQ effects and cycle budget; this is intentionally
 * a target-specific execution interface, not a portable bytecode interpreter.
 */
typedef struct {
  const uint16_t *instructions; /**< Immutable borrowed instruction array. */
  uint8_t instruction_count; /**< 1..32 instruction words. */
  uint8_t entry; /**< First executed instruction. */
  uint8_t wrap_bottom; /**< First instruction after hardware wrap. */
  uint8_t wrap_top; /**< Inclusive final instruction before hardware wrap. */
} esy_xio_program_config_t;

/**
 * @brief Explicit SM resources and pin/shift configuration; init stays disabled.
 *
 * Unused pin mappings have count zero. Pin ranges must fit GPIO 0..31 and the
 * selected chip. SET and side-set widths are at most five; optional side-set
 * reserves one encoding bit and allows at most four physical pins. The divider
 * is clk_div_div / clk_div_mul; zero independently means one. Pico quantizes
 * down to 1/256 and accepts ratios 1..65536. Cycle counts refer to this hardware
 * clock; fractional dividers may vary individual cycle durations. Init configures
 * mapped inputs with the native two-clk_sys input synchronizer
 * enabled; previous SPI input-bypass settings are cleared for these inputs.
 * This latency is in system-clock cycles, not divided PIO instruction cycles.
 * One owner reserves all GPIOs and IRQ flags used by cooperating programs and serializes
 * their lifecycle. Other users must not manipulate those resources.
 */
typedef struct {
  uint8_t instance; /**< Explicit PIO instance 0 or 1. */
  uint8_t sm_index; /**< Explicit state machine 0..3. */
  uint32_t clk_div_mul; /**< Divider denominator. */
  uint32_t clk_div_div; /**< Divider numerator. */
  uint8_t out_pin_base; /**< First OUT pin. */
  uint8_t out_pin_count; /**< OUT width, 0..32. */
  uint8_t in_pin_base; /**< First IN pin. */
  uint8_t in_pin_count; /**< IN width, 0..32. */
  uint8_t set_pin_base; /**< First SET pin. */
  uint8_t set_pin_count; /**< SET width, 0..5. */
  uint8_t sideset_pin_base; /**< First side-set pin. */
  uint8_t sideset_pin_count; /**< Physical side-set width. */
  bool sideset_optional; /**< Include side-set enable encoding bit. */
  bool out_shift_right; /**< Shift OUT toward bit zero. */
  bool out_autopull; /**< Pull after out_pull_thresh bits. */
  uint8_t out_pull_thresh; /**< 1..32; zero selects 32. */
  bool in_shift_right; /**< Shift IN toward bit zero. */
  bool in_autopush; /**< Push after in_push_thresh bits. */
  uint8_t in_push_thresh; /**< 1..32; zero selects 32. */
} esy_xio_program_sm_config_t;

/** @brief Snapshot; PC is relative to the loaded program's first instruction. */
typedef struct {
  uint8_t pc; /**< Relative program counter. */
  uint8_t tx_level; /**< Used TX words, 0..4. */
  uint8_t rx_level; /**< Available RX words, 0..4. */
  bool enabled; /**< SM enabled state. */
  bool tx_stall; /**< Sticky pull/autopull stall since last explicit clear. */
  bool rx_stall; /**< Sticky push/autopush stall since last explicit clear. */
} esy_xio_program_status_t;

/** @brief DMA direction for abort and ownership queries. */
typedef enum {
  ESY_XIO_PROGRAM_DMA_TX = 0, /**< Memory to TX FIFO. */
  ESY_XIO_PROGRAM_DMA_RX = 1 /**< RX FIFO to memory. */
} esy_xio_program_dma_direction_t;

/**
 * @brief ISR completion/error notification; never call lifecycle APIs here.
 * @param user Borrowed callback context, valid until successful DMA abort.
 * @param status ESY_OK on completion or ESY_ERR_IO on a DMA error.
 *
 * Publish a bounded notification only. Success means all requested memory words
 * transferred; TX completion does not mean the PIO program finished its edges.
 * The DMA route remains claimed until explicit task-context abort, including
 * after this callback. On error the buffer remains borrowed until abort.
 */
typedef void (*esy_xio_program_dma_cb_t)(void *user, esy_err_t status);

/** @brief One finite 32-bit FIFO DMA route; no half/circular transfers. */
typedef struct {
  uint8_t channel; /**< Explicit native DMA channel 0..11. */
  uint8_t irq_index; /**< DMA IRQ 0 or 1. */
  uint8_t irq_priority; /**< Same priority as other DMA users on this core. */
  esy_xio_program_dma_cb_t callback; /**< Optional ISR notification. */
  void *user; /**< Borrowed callback context. */
} esy_xio_program_dma_config_t;

#ifdef __cplusplus
}
#endif
#endif
