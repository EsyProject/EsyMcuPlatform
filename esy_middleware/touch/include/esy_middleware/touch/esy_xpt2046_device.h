/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_XPT2046_DEVICE_H
#define ESY_XPT2046_DEVICE_H

#include "esy_middleware/touch/esy_touch_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register an initialized XPT2046 with its SPI dependency.
 *
 * Link esy_touch_device for this optional integration.
 *
 * Uses the initialization, ownership and cleanup contract of
 * @ref esy_touch_register_device. The SPI dependency configured during init must
 * occur in device_config->registration.dependencies. The binding must be `{0}`
 * on first use or have completed a successful unregister.
 *
 * @param[in,out] binding Caller-owned logical touch binding.
 * @param[in,out] touch Handle attached to the XPT2046 backend.
 * @param[in,out] registry Configuring device registry.
 * @param[in] device_config Registration and optional auxiliary GPIO metadata.
 * @retval ESY_OK Registered successfully.
 * @retval ESY_ERR_INVALID_ARG Invalid pointer, wrong backend or missing SPI
 * dependency.
 * @retval <0 Registration, ownership or cleanup failure.
 */
esy_err_t esy_xpt2046_register_device(
    esy_touch_device_binding_t *binding, esy_touch_t *touch,
    esy_device_registry_t *registry,
    const esy_touch_device_config_t *device_config);

#ifdef __cplusplus
}
#endif

#endif /* ESY_XPT2046_DEVICE_H */
