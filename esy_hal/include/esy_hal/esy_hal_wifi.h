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
 * an initialized Wi-Fi manager. The owner must call process at a product-selected interval. The network-runtime descriptor query is independent of station control.
 *
 * Configuration is copied shallowly: SSID/password remain caller-owned and
 * immutable until deinit, including start/init failures and retry. A failed
 * connection may retain initialized backend resources; call deinit before
 * reclaiming strings or handing ownership to a different client. Repeated start
 * replaces the current attempt and deadline. Deinit stops the backend, is safe
 * when already stopped, and permits subsequent start/init. Network clients must
 * be quiescent before repeated start or deinit. Cleanup failure retains the
 * backend and its remaining native resources; retry deinit before another start.
 * ESP32 borrows a consumer-initialized esp-netif stack and default event loop.
 * It exclusively owns its native Wi-Fi driver/default STA netif and uses RAM
 * credential storage with native Wi-Fi NVS disabled. Native tasks, event
 * handlers, buffers and netif allocations belong to ESP-IDF; start/deinit are
 * serialized lifecycle operations and provide no hard execution-time bound.
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
 * ESP32 requires a 1..32-byte SSID; OPEN requires an empty password, while PSK
 * requires 8..63 passphrase bytes or exactly 64 hexadecimal digits. WPA2 AES
 * accepts WPA2-capable APs that also support WPA and requires CCMP after
 * association. Its timeout uses monotonic
 * signed 64-bit microseconds from ESP-IDF, includes native start latency, and
 * accepts the full uint32_t millisecond range if the deadline fits INT64_MAX.
 * Light sleep advances this clock; deep sleep starts a new native boot/session.
 * An expired or cancelled attempt stays inactive until explicit start; a later
 * process call cannot revive it. Owner execution determines deadline-observation
 * latency.
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
 * @brief Drive one incremental station connection processing iteration.
 * ESP32 samples association without waiting for it or DHCP. Native status and
 * terminal stop operations still contribute synchronous provider latency.
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
 * ESP32 polls with a one-millisecond hardware busy delay and does not actively
 * yield the scheduler; prefer start/process from the existing task owner.
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
 * ESP32 supplies a no-op progress hook because native TCP/IP tasks own progress.
 * Its enter/leave hooks acquire/release the native TCP/IP core lock and require
 * CONFIG_LWIP_TCPIP_CORE_LOCKING and CONFIG_LWIP_CHECK_THREAD_SAFETY. Without both,
 * this query returns NOT_SUPPORTED; station control remains available. These
 * hooks may block and require the consumer-initialized TCP/IP stack to stay alive.
 * The native core mutex is not recursive. Enter only from the ordinary app
 * owner task without an already-held core lock; never call endpoints using
 * these hooks from native TCP/DNS callbacks, tcpip_callback context, or inside
 * an outer LOCK_TCPIP_CORE region. Leave must match that owner's single enter.
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
 * @brief Stop the station and release only backend-owned native resources.
 *
 * Quiesce network borrowers first. Success cancels the active deadline and
 * permits a new start; an already stopped backend succeeds. Failure retains
 * ownership and any remaining native resources for another deinit attempt.
 * Keep credentials and owner storage alive until success; do not restart or
 * transfer ownership while cleanup is incomplete. ESP32 leaves the borrowed
 * esp-netif stack/default event loop and NVS untouched.
 * @retval ESY_OK Native resources released, including repeated deinit.
 * @retval <0 Cleanup failed; retain ownership and retry.
 */
extern esy_err_t esy_hal_wifi_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_WIFI_H */
