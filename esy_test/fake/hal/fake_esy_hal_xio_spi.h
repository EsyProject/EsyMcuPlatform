/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file fake_esy_hal_xio_spi.h
 * @brief Fake HAL XIO SPI contract for unit tests.
 */

#ifndef FAKE_ESY_HAL_XIO_SPI_H
#define FAKE_ESY_HAL_XIO_SPI_H

#include "esy_hal/xio/esy_hal_xio_spi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reset the fake XIO SPI state.
 */
void fake_esy_hal_xio_spi_reset(void);

/**
 * @brief Set the result returned by init.
 */
void fake_esy_hal_xio_spi_set_init_result(esy_err_t ret);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESY_HAL_XIO_SPI_H */
