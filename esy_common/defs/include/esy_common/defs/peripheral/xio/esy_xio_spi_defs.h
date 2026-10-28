/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_XIO_SPI_DEFS_H
#define ESY_XIO_SPI_DEFS_H

/**
 * @file esy_xio_spi_defs.h
 * @brief SPI configuration types for XIO/PIO backend
 *
 * This file defines configuration structures for SPI instances implemented
 * on top of XIO (PIO-based) backends.
 *
 * It reuses common SPI enums (such as @ref esy_spi_mode_t and
 * @ref esy_spi_role_t) to maintain cross-backend consistency, while keeping
 * the configuration strictly backend-specific.
 *
 * This header contains type definitions only and does not include
 * any HAL backend or platform-specific implementation.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_spi_defs.h"
#include "esy_common/defs/peripheral/xio/esy_xio_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Configuration Types                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief XIO (PIO-based) SPI configuration structure
 *
 * @details
 * - This configuration does NOT reuse @ref esy_spi_config_t directly, but
 *   reuses its common enums (e.g. @ref esy_spi_mode_t and
 *   @ref esy_spi_role_t) to ensure semantic consistency across backends.
 * - Any unused pin fields must be set to @ref ESY_XIO_PIN_UNUSED.
 * - The Pico XIO blocking APIs support 2-bit to 8-bit frames through the byte
 *   APIs and 9-bit to 16-bit frames through the half-word APIs. Frame values
 *   occupy the least-significant `data_bits` of each buffer element;
 *   `is_msb_first` controls only the order in which those bits appear on the
 *   wire.
 * - Pico XIO DMA supports exactly 8-bit frames with 8-bit DMA elements or
 *   16-bit frames with 16-bit DMA elements, for either bit order. Each memory
 *   element is one frame with its numeric value in the least-significant bits.
 *   Other frame/element combinations return NOT_SUPPORTED before DMA or FIFO
 *   changes. Slave mode requires a physical pin_cs input.
 * - Pico slave MISO additionally requires a distinct caller-selected
 *   slave_cs_sm_index in the same instance. This SM controls MISO output enable
 *   independently of stalled clock/FIFO work; master and MISO-less slave modes
 *   ignore the field. Slave MISO must not alias SCK, MOSI or CS. All translation
 *   units must use the same configuration layout.
 */
typedef struct {
    /* ---------- XIO resource selection ------------------------------------ */
    uint8_t instance; /**< Caller-selected XIO instance index (platform-defined) */
    uint8_t sm_index; /**< Caller-selected state machine index (platform-defined) */

    /* ---------- Pin mapping ------------------------------------------------ */
    uint8_t pin_sck;       /**< SCK pin (typically mapped to PIO sideset); set to @ref ESY_XIO_PIN_UNUSED if unused */
    uint8_t pin_mosi;      /**< MOSI pin (master out, slave in); matching a valid pin_miso selects master SDIO mode */
    uint8_t pin_miso;      /**< MISO pin (master in, slave out); matching a valid pin_mosi selects master SDIO mode */
    uint8_t pin_cs;        /**< Chip-select pin; required in slave mode; master may use @ref ESY_XIO_PIN_UNUSED for external CS */
    uint8_t pin_trigger;   /**< Optional trigger pin; set to @ref ESY_XIO_PIN_UNUSED if unused;
                               unsupported when pin_mosi and pin_miso select three-wire mode */

    /* ---------- Protocol parameters (reuse common SPI enums) --------------- */
    esy_spi_role_t role;         /**< SPI role: @ref ESY_SPI_ROLE_MASTER /
                                      @ref ESY_SPI_ROLE_MASTER_NO_CS /
                                      @ref ESY_SPI_ROLE_SLAVE */
    esy_spi_mode_t mode;         /**< SPI mode: @ref ESY_SPI_MODE_0 / 1 / 2 / 3 (CPOL/CPHA) */
    bool           is_msb_first; /**< Bit order: true = MSB first, false = LSB first */
    uint8_t        data_bits;    /**< Bits per data frame; blocking APIs support 2 to 16 */
    uint32_t       speed_hz;     /**< SPI clock frequency in Hz (derived from PIO program and divider) */

    /* ---------- CS timing (effective only when CS is driven by backend) ---- */
    bool     cs_active_low;  /**< CS active polarity; true means active-low */
    uint32_t cs_setup_ns;    /**< Minimum time from CS assertion to first SCK edge (nanoseconds) */
    uint32_t cs_hold_ns;     /**< Minimum time from last SCK edge to CS deassertion (nanoseconds) */
    uint32_t cs_inactive_ns; /**< Minimum CS inactive time between consecutive transfers (nanoseconds) */

    /** @brief Caller-selected CS monitor SM for Pico slave MISO, distinct from
     * sm_index. Set to 0xFF when unused. No monitor is claimed in master mode
     * or when pin_miso is unused. Missing/conflicting slave selection is
     * INVALID_ARG before board/native resource changes. */
    uint8_t slave_cs_sm_index;
} esy_xio_spi_config_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_XIO_SPI_DEFS_H */
