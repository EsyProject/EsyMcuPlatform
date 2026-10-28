/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_XIO_UART_DEFS_H
#define ESY_XIO_UART_DEFS_H

/**
 * @file esy_xio_uart_defs.h
 * @brief Configuration type definitions for UART over XIO/PIO
 *
 * This file defines the configuration structure for XIO (PIO-based) UART.
 *
 * The configuration is typically paired with a specific PIO program
 * (TX-only, RX-only, or full-duplex). Therefore, the semantic role of
 * the @ref pin field (TX or RX) is determined by the backend PIO program
 * that is loaded, not by this type definition itself.
 *
 * This header contains type definitions only and does not include
 * any backend-specific implementation.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/xio/esy_xio_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Configuration Types                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief XIO (PIO-based) UART configuration
 *
 * @details
 * - @ref pin represents the UART signal pin bound to the PIO program.
 *   Whether this pin functions as TX or RX is defined entirely by the
 *   backend PIO program in use (TX-only, RX-only, or full-duplex).
 *   If a direction is unused and the selected program allows it,
 *   the pin may be set to @ref ESY_XIO_PIN_UNUSED.
 *
 * - @ref baudrate specifies the target baud rate in bits per second.
 *   The actual achievable baud rate depends on the PIO clock source
 *   and divider resolution.
 *
 * - @ref instance and @ref sm_index explicitly select the XIO instance and
 *   state machine used by the caller.
 */
typedef struct {
    uint8_t  pin;      /**< UART signal pin (TX or RX as defined by backend PIO program;
                            may be ESY_XIO_PIN_UNUSED if unused and supported) */
    uint32_t baudrate; /**< Target baud rate in bits per second (bit/s) */
    uint8_t  instance; /**< Caller-selected XIO instance index (platform-defined) */
    uint8_t  sm_index; /**< Caller-selected state machine index (platform-defined) */
} esy_xio_uart_config_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_XIO_UART_DEFS_H */
