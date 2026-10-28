/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_WIFI_MANAGER_DEVICE_H
#define ESY_WIFI_MANAGER_DEVICE_H

#include "esy_common/device/esy_device.h"
#include "esy_middleware/wifi/esy_wifi_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Caller-owned logical Wi-Fi manager graph binding.
 * Initialize with {0} before first registration. Keep this object, the manager,
 * registration name and dependency array alive until successful unregister.
 * Fields are read-only to callers. Never clear a live binding to reuse it.
 */
typedef struct {
  esy_device_t device;
  esy_wifi_manager_t *manager;
  bool suspended;
} esy_wifi_manager_device_binding_t;

/**
 * @brief Bind the exclusive manager before registry start.
 * @param[in,out] binding Zero-initialized, unbound storage.
 * @param[in,out] manager Initialized backend owner, not already bound.
 * @param[in,out] registry Configuring device graph.
 * @param[in] device_config Name, dependencies and supported power states.
 * @retval ESY_OK Bound; graph PM callbacks now own suspend/resume.
 * @retval ESY_ERR_INVALID_ARG NULL or invalid device configuration.
 * @retval ESY_ERR_STATE Manager/binding already bound or wrong lifecycle state.
 * @retval <0 Device registration error; existing objects remain unchanged.
 * @note Quiescent lifecycle operation, serialized with all manager/graph access.
 */
esy_err_t esy_wifi_manager_register_device(
    esy_wifi_manager_device_binding_t *binding, esy_wifi_manager_t *manager,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config);

/**
 * @brief Detach before registry start, or after successful registry deinit.
 * @param[in,out] binding Registered binding; retained on failure, cleared on success.
 * @retval ESY_OK Association detached; Wi-Fi backend remains manager-owned.
 * @retval ESY_ERR_INVALID_ARG NULL or unbound/mismatched binding.
 * @retval ESY_ERR_STATE Registry is still started or transitioning.
 * @retval ESY_ERR_BUSY Outstanding usage or dependencies prevent removal.
 * @note Stop graph and manager endpoints before detaching. For a started graph,
 * quiesce users, deinit the registry, detach this binding, then deinit manager.
 * Quiesce network clients before releasing Wi-Fi backend resources.
 */
esy_err_t esy_wifi_manager_unregister_device(
    esy_wifi_manager_device_binding_t *binding);

#ifdef __cplusplus
}
#endif

#endif /* ESY_WIFI_MANAGER_DEVICE_H */
