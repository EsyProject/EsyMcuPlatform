/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_XIO_DEFS_H
#define ESY_XIO_DEFS_H

/**
 * @file esy_xio_defs.h
 * @brief Common internal binding types for the XIO/PIO subsystem
 *
 * This header provides common XIO internal binding types, such as instance
 * index and state machine (SM) index.
 *
 * Public XIO peripheral configurations expose caller-selected resource fields
 * directly. The internal binding type in this header is used when the HAL
 * passes that selection to board hooks without exposing board implementation
 * details in the peripheral configuration.
 *
 * This file contains type and macro definitions only and does not include
 * any backend-specific implementation.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @def ESY_XIO_PIN_UNUSED
 * @brief Placeholder value indicating an unused XIO pin.
 *
 * Some XIO adapters (e.g. XIO-SPI or XIO-UART) allow certain pins to be
 * unconnected or managed externally. In such cases, this value should be
 * assigned to the corresponding pin field.
 */
#ifndef ESY_XIO_PIN_UNUSED
#define ESY_XIO_PIN_UNUSED 0xFFu
#endif

/* -------------------------------------------------------------------------- */
/* Configuration Types                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief XIO internal binding configuration
 *
 * Carries the selected XIO instance and state machine (SM) index across the
 * HAL-to-board boundary. Public peripheral callers select these values
 * through direct fields in the corresponding peripheral configuration.
 */
typedef struct {
    uint8_t instance;  /**< XIO instance index (platform-defined, e.g. 0/1/...) */
    uint8_t sm_index;  /**< XIO state machine index (platform-defined) */
} esy_xio_inner_config_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_XIO_DEFS_H */
