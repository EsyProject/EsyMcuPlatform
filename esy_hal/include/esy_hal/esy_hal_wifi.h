/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_WIFI_H
#define ESY_HAL_WIFI_H

/**
 * @file esy_hal_wifi.h
 * @brief HAL Wi-Fi interface.
 *
 * One global station backend is available. Direct callers must serialize every
 * operation, including queries and deinit, in task context on an SDK-supported
 * execution core; none is ISR-safe. Do not mix direct station-control calls with
 * an initialized Wi-Fi manager. The process owner must poll at a product-selected
 * interval. The network-runtime descriptor query is independent of station control.
 *
 * Configuration is copied shallowly: SSID/password remain caller-owned and
 * immutable until deinit, including start/init failures and retry. A failed
 * connection may retain initialized backend resources; call deinit before
 * reclaiming strings or handing ownership to a different client. Repeated start
 * replaces the current attempt and deadline. Deinit stops the backend, is safe
 * when already stopped, and permits subsequent start/init. Network clients must
 * be quiescent before deinit. The SDK's init-failure cleanup and hardware
 * shutdown behavior require port smoke validation.
 */

#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_wifi_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fill a Wi-Fi configuration structure with platform defaults.
 *
 * @param[out] config Configuration structure to initialize; ignored when NULL.
 */
extern void esy_hal_wifi_default_cfg(esy_wifi_config_t *config);

/**
 * @brief Start an asynchronous Wi-Fi station connection attempt.
 *
 * Initial SDK setup and submission may perform synchronous I/O; this does not
 * wait for station association or DHCP and provides no hard execution bound.
 * On Pico, the shared DMA/PIO assembly guard can defer native allocation with
 * ESY_ERR_BUSY before changing configuration or the current connection attempt.
 * Retry the same start from the serialized owner after contention ends; queries
 * continue to describe the existing attempt and are not forced disconnected.
 *
 * The connection is advanced by @ref esy_hal_wifi_process_sta.
 *
 * @param[in] config Station connection configuration.
 *
 * @retval ESY_OK Success.
 * @retval ESY_ERR_BUSY Native resource assembly was deferred; retry start.
 * @retval <0 Error code.
 */
extern esy_err_t esy_hal_wifi_start_sta(
    const esy_wifi_config_t *config);

/**
 * @brief Drive one non-blocking station connection processing iteration.
 *
 * @retval ESY_OK Connected.
 * @retval ESY_ERR_BUSY Connection is still in progress, or Pico native retry
 * allocation was deferred. The existing attempt and deadline remain in effect.
 * @retval <0 Error code.
 */
extern esy_err_t esy_hal_wifi_process_sta(void);

/**
 * @brief Initialize Wi-Fi and wait for connection or the configured deadline.
 *
 * Blocking task-context convenience API; use start/process for incremental work.
 * Pico allocation contention may return ESY_ERR_BUSY before waiting begins;
 * retry the same init without clearing the current configuration or backend.
 *
 * @param[in] config Station connection configuration.
 *
 * @retval ESY_OK Success.
 * @retval ESY_ERR_BUSY Initial native resource assembly was deferred.
 * @retval <0 Error code.
 */
extern esy_err_t esy_hal_wifi_init_sta(
    const esy_wifi_config_t *config);

/**
 * @brief Query whether the Wi-Fi station is connected.
 *
 * @return true when connected; false otherwise.
 */
extern bool esy_hal_wifi_is_connected(void);

/**
 * @brief Read the IPv4 address assigned to the Wi-Fi station interface.
 *
 * The returned address is only valid after station connection and DHCP address
 * assignment have completed.
 *
 * @param[out] out_addr Destination for the IPv4 address.
 *
 * @retval ESY_OK Address was copied to @p out_addr.
 * @retval ESY_ERR_INVALID_ARG @p out_addr is NULL.
 * @retval ESY_ERR_STATE Wi-Fi is not connected or no IPv4 address is assigned.
 */
extern esy_err_t esy_hal_wifi_get_ipv4_addr(
    esy_wifi_ipv4_addr_t *out_addr);

/**
 * @brief Obtain the port's borrowed native network execution hooks.
 *
 * May be called before station/native initialization. This query initializes
 * nothing, allocates no resources and does not change station configuration,
 * connection deadlines or retry state. Success supplies all three callbacks;
 * context may be NULL. The descriptor may be copied into a network provider's
 * configuration without introducing a dependency on its implementation types.
 *
 * Invoke the callbacks only on the single serialized, SDK-supported owner while
 * the native provider is alive, never in an ISR. Progress drives native network
 * input/timers without advancing the station manager's connection bookkeeping
 * or retry policy; native callbacks may run synchronously. Enter/leave must be
 * paired on that owner and do not make network handles safe for multiple tasks.
 * Do not reenter Wi-Fi lifecycle operations from these callbacks. Stop and
 * detach all network borrowers before Wi-Fi stop, deinit or provider replacement.
 * The Pico polling port uses a NULL context and defensively ignores callbacks
 * before native init or after deinit; that does not extend borrower lifetime.
 *
 * @param[out] out_runtime Caller-owned descriptor, written only on success.
 * @retval ESY_OK Descriptor supplied without native initialization.
 * @retval ESY_ERR_INVALID_ARG NULL output pointer.
 * @retval ESY_ERR_NOT_SUPPORTED The selected port has no execution hooks.
 * @retval <0 Provider error; output remains unchanged.
 */
extern esy_err_t esy_hal_wifi_get_network_runtime(
    esy_wifi_network_runtime_t *out_runtime);

/**
 * @brief Deinitialize the Wi-Fi backend.
 */
extern void esy_hal_wifi_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_WIFI_H */
