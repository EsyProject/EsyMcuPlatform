/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_GPIO_PM_H
#define ESY_HAL_GPIO_PM_H

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/system/esy_gpio_pm_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Claim every physical pin in a logical device GPIO PM profile.
 *
 * @param[in] profile Optional caller-owned immutable profile. NULL is a no-op.
 * @param[in] owner Stable non-NULL adapter identity retained until release.
 * @retval ESY_OK Profile claimed or @p profile is NULL.
 * @retval ESY_ERR_BUSY A physical pin is already owned by another profile.
 * @retval ESY_ERR_NO_MEM The fixed physical-pin claim table is full.
 * @retval ESY_ERR_INVALID_ARG Invalid profile, owner, handle, or duplicate pin.
 */
esy_err_t esy_hal_gpio_pm_claim(
    const esy_device_gpio_pm_profile_t *profile, const void *owner);

/**
 * @brief Release a profile previously claimed by the same owner.
 *
 * The profile must be in default state. NULL is a no-op.
 *
 * @retval ESY_OK Profile released or @p profile is NULL.
 * @retval ESY_ERR_STATE Owner, profile, handle, or current state mismatch.
 * @retval ESY_ERR_INVALID_ARG Invalid profile or owner.
 */
esy_err_t esy_hal_gpio_pm_release(
    const esy_device_gpio_pm_profile_t *profile, const void *owner);

/**
 * @brief Select a GPIO-only power state for a claimed profile.
 *
 * Default restores each GPIO initialization state. Idle and sleep apply the
 * optional overrides. On an apply failure, the function attempts to restore
 * the whole profile to default before returning the original error.
 *
 * @retval ESY_OK State selected or @p profile is NULL.
 * @retval ESY_ERR_STATE The profile is not claimed by @p owner.
 * @retval ESY_ERR_INVALID_ARG Invalid profile, owner, or state.
 */
esy_err_t esy_hal_gpio_pm_select(
    const esy_device_gpio_pm_profile_t *profile, const void *owner,
    esy_gpio_pm_state_id_t state_id);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_GPIO_PM_H */
