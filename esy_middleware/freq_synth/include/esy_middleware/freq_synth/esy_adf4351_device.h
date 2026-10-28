/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_ADF4351_DEVICE_H
#define ESY_ADF4351_DEVICE_H

#include "esy_middleware/freq_synth/esy_adf4351.h"
#include "esy_middleware/freq_synth/esy_freq_synth_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register an initialized ADF4351 as a logical graph device.
 *
 * The dependency list must include the registered SPI controller used by the
 * ADF4351. Suspend saves the output state, selects optional auxiliary GPIO PM
 * overrides, and relies on graph ordering to suspend the SPI parent. Resume
 * restores each referenced GPIO configured state, inactive CS/LE and active CE.
 * The SPI backend must provide bounded blocking writes for PM transitions.
 * First use requires a zeroed binding; use the matching initialized backend.
 * Link esy_freq_synth_device with ENABLE_ESY_GPIO and ENABLE_ESY_ADF4351.
 * @return ESY_ERR_INVALID_ARG for a wrong backend, malformed dependency list or
 * missing SPI parent; otherwise see esy_freq_synth_register_device().
 */
esy_err_t esy_adf4351_register_device(
    esy_freq_synth_device_binding_t *binding, esy_freq_synth_t *synth,
    esy_device_registry_t *registry,
    const esy_freq_synth_device_config_t *device_config);

#ifdef __cplusplus
}
#endif

#endif /* ESY_ADF4351_DEVICE_H */
