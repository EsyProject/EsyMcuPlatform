/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HB_WIFI_H
#define ESY_HB_WIFI_H

/**
 * @file esy_hb_wifi.h
 * @brief Wi-Fi HAL backend interface.
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
 * @brief Fill a Wi-Fi configuration structure with HAL backend defaults.
 *
 * @param[out] config Configuration structure to initialize; ignored when NULL.
 */
extern void esy_hb_wifi_default_cfg(esy_wifi_config_t *config);

/**
 * @brief Start an asynchronous Wi-Fi station connection attempt.
 *
 * Initial SDK setup and submission may perform synchronous I/O; this does not
 * wait for station association or DHCP and provides no hard execution bound.
 *
 * The connection is advanced by @ref esy_hb_wifi_process_sta.
 *
 * @param[in] config Station connection configuration.
 *
 * @retval ESY_OK Success.
 * @retval ESY_ERR_INVALID_ARG Invalid configuration.
 * @retval ESY_ERR_IO Backend initialization or connection start failed.
 * @retval ESY_ERR_NOT_SUPPORTED Unsupported authentication mode.
 * @retval ESY_ERR_BUSY Pico resource allocation is reserved by another
 * provider. Retry start with the same configuration; this rejection leaves
 * the current connection attempt/configuration unchanged.
 */
extern esy_err_t esy_hb_wifi_start_sta(const esy_wifi_config_t *config);

/**
 * @brief Drive one non-blocking station connection processing iteration.
 *
 * @retval ESY_OK Connected.
 * @retval ESY_ERR_BUSY Connection is still in progress or a native retry is
 * deferred by resource allocation contention. The original deadline remains.
 * @retval ESY_ERR_TIMEOUT Connection deadline expired.
 * @retval <0 Error code.
 */
extern esy_err_t esy_hb_wifi_process_sta(void);

/**
 * @brief Initialize Wi-Fi and wait for connection or the configured deadline.
 *
 * Blocking task-context convenience API; use start/process for incremental work.
 *
 * @param[in] config Station connection configuration.
 *
 * @retval ESY_OK Success.
 * @retval ESY_ERR_INVALID_ARG Invalid configuration.
 * @retval ESY_ERR_IO Backend initialization failed.
 * @retval ESY_ERR_TIMEOUT Connection deadline expired.
 * @retval ESY_ERR_NOT_SUPPORTED Unsupported authentication mode.
 * @retval ESY_ERR_BUSY Initial resource allocation was deferred before the
 * connection attempt changed; retry this call with the same configuration.
 */
extern esy_err_t esy_hb_wifi_init_sta(const esy_wifi_config_t *config);

/**
 * @brief Query whether the Wi-Fi station is connected.
 *
 * @return true when connected; false otherwise.
 */
extern bool esy_hb_wifi_is_connected(void);

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
extern esy_err_t esy_hb_wifi_get_ipv4_addr(esy_wifi_ipv4_addr_t *out_addr);

/**
 * @brief Obtain native network execution hooks without initializing the port.
 *
 * Allowed before station initialization; no allocation, polling, configuration,
 * deadline or retry mutation occurs. Success supplies all callbacks and a
 * borrowed context. Invoke them only on one serialized non-ISR owner while the
 * native provider is alive; enter/leave must be paired on that owner. Progress
 * may dispatch native callbacks but does not advance station connection/retry
 * bookkeeping. It does not make borrower handles safe for multiple tasks.
 * Quiesce and detach all borrowers before native shutdown or replacement;
 * callbacks must not reenter Wi-Fi lifecycle operations. The Pico polling port
 * uses NULL context and defensively ignores calls while not initialized.
 *
 * @param[out] out_runtime Written only on success.
 * @retval ESY_OK All three hooks supplied; native initialization is unchanged.
 * @retval ESY_ERR_INVALID_ARG NULL output pointer.
 * @retval ESY_ERR_NOT_SUPPORTED No hooks implemented by the selected port.
 */
extern esy_err_t esy_hb_wifi_get_network_runtime(
    esy_wifi_network_runtime_t *out_runtime);

/**
 * @brief Deinitialize the Wi-Fi backend.
 */
extern void esy_hb_wifi_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HB_WIFI_H */
