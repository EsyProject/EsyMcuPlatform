/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_ILI9488_DEVICE_H
#define ESY_ILI9488_DEVICE_H

#include "esy_middleware/lcd/esy_lcd_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register an initialized ILI9488 with its SPI dependency.
 *
 * Link esy_lcd_device for this optional integration.
 *
 * Binding initialization and cleanup follow esy_lcd_register_device(). PM
 * requires a powered panel that retains controller state. Profiles may contain
 * only the configured CS, DC and reset handles. Reset must remain output-high
 * in DEFAULT, IDLE and SLEEP; reset/power cycling requires detach and reinit
 * outside PM callbacks. Other auxiliary pins are not supported.
 * @retval ESY_ERR_INVALID_ARG Invalid handle or dependency array.
 * @retval ESY_ERR_NOT_SUPPORTED Profile could reset or power-cycle the panel.
 * @retval ESY_OK Registered.
 * @retval <0 Logical registration or resource claim failure.
 */
esy_err_t esy_ili9488_register_device(
    esy_lcd_device_binding_t *binding, esy_lcd_t *lcd,
    esy_device_registry_t *registry,
    const esy_lcd_device_config_t *device_config);

#ifdef __cplusplus
}
#endif

#endif /* ESY_ILI9488_DEVICE_H */
