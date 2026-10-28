/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_DMA_DEFS_H
#define ESY_DMA_DEFS_H

/**
 * @file esy_dma_defs.h
 * @brief Common DMA definitions: direction, transfer width, config structs, and
 * callbacks.
 *
 * This header provides platform-agnostic DMA types and configuration
 * structures:
 * - transfer direction (memory ↔ peripheral) and element width;
 * - completion / half-completion callback types;
 * - board/HAL-provided DMA binding info (channel, DREQ, IRQ, priority);
 * - the generic transfer configuration structure @ref esy_dma_config_t.
 *
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Enumerations                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief DMA transfer direction.
 */
typedef enum {
  ESY_DMA_MEM_TO_MEM,     /**< Memory to memory */
  ESY_DMA_MEM_TO_PERIPH,  /**< Memory to peripheral */
  ESY_DMA_PERIPH_TO_MEM   /**< Peripheral to memory */
} esy_dma_direction_t;

/**
 * @brief DMA element (beat) width for a single transfer step.
 *
 * The meaning of @ref esy_dma_config_t::length is still bytes; the
 * HAL backend will derive the element count based on this width as needed.
 */
typedef enum {
  ESY_DMA_TRANSFER_SIZE_8,   /**< 8-bit */
  ESY_DMA_TRANSFER_SIZE_16,  /**< 16-bit */
  ESY_DMA_TRANSFER_SIZE_32   /**< 32-bit */
} esy_dma_transfer_size_t;

/* -------------------------------------------------------------------------- */
/* Callback Types                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief DMA transfer callback prototype.
 *
 * @param[in] arg User callback argument provided by
 *                @ref esy_dma_config_t::callback_arg or
 *                @ref esy_dma_config_t::half_callback_arg.
 *
 * @note Public generic HAL callbacks execute from the owner task in
 * esy_hal_dma_poll(). Backend/peripheral-internal callback context follows
 * its own API and may be ISR context; keep handlers bounded and nonblocking.
 * Do not reenter DMA lifecycle/control APIs from a DMA callback.
 */
typedef void (*esy_dma_callback_t)(void *arg);

/* -------------------------------------------------------------------------- */
/* Configuration Structures                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief DMA internal binding info (filled by board/HAL code).
 *
 * This structure describes DMA routing/assignment and interrupt attributes for
 * a specific peripheral instance. It is typically produced by board-layer query
 * functions (e.g. `esy_board_*_get_dma_info()`).
 */
typedef struct {
  esy_dma_direction_t
      direction;        /**< Bound direction for this peripheral path */
  uint8_t  channel;               /**< DMA channel index (platform-specific) */
  uint32_t dreq;                  /**< Platform-defined DREQ/routing key */
  uint8_t  irq_index;             /**< DMA IRQ index (platform-specific) */
  uint8_t irq_priority; /**< DMA IRQ priority (smaller value = higher priority
                           by convention) */
} esy_dma_inner_config_t;

/**
 * @brief DMA transfer configuration.
 *
 * Built by the caller before starting a DMA transfer:
 * - @ref src / @ref dst / @ref length / @ref transfer_size are provided by the
 * caller;
 * - callbacks are optional and may be NULL;
 * - @ref inner is usually filled by a board query function, or injected during
 * HAL resource assembly/binding.
 */
typedef struct {
  /* Basic transfer parameters ---------------------------------------------- */
  const void *src;                        /**< Source address */
  void *dst;                              /**< Destination address */
  size_t length;                          /**< Transfer length in bytes */
  esy_dma_transfer_size_t transfer_size;  /**< Element width per beat */

  /* Completion / half-completion callbacks -------------------------------- */
  esy_dma_callback_t callback;            /**< Completion callback (optional) */
  void *callback_arg;                     /**< Completion callback argument */

  esy_dma_callback_t half_callback; /**< Half-transfer callback (optional;
                                       common for circular/double-buffer) */
  void *half_callback_arg;                /**< Half-transfer callback argument */
  esy_dma_callback_t error_callback;      /**< Transfer error callback */
  void *error_callback_arg;               /**< Transfer error callback argument */
  bool circular; /**< Restart automatically after the configured transfer length
                  */

  /* Board/HAL binding info ------------------------------------------------ */
  esy_dma_inner_config_t
      inner; /**< Internal DMA binding (filled by board/HAL) */
} esy_dma_config_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_DMA_DEFS_H */
