/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_wifi_manager.h
 * @brief Task-free, single-backend Wi-Fi station manager.
 *
 * One initialized manager exclusively owns the Wi-Fi HAL backend. Do not
 * call HAL/backend Wi-Fi APIs directly while it is owned by a manager.
 * init/deinit/register/unregister are externally serialized lifecycle operations
 * across all instances, with request/query/process and device-graph endpoints
 * quiescent. Do not copy, move, or modify an initialized manager.
 *
 * One task owns process/stop. With a registered device, PM transitions may run
 * in the graph owner's task; device references exclude backend operations from
 * transitions. request_connect and all queries support concurrent task callers
 * while initialized. Queries are snapshots, not guarantees of continued link
 * availability. No API is ISR-safe. The module creates no task or allocation.
 */
#ifndef ESY_WIFI_MANAGER_H
#define ESY_WIFI_MANAGER_H

#include <stdbool.h>
#if !defined(__cplusplus)
#include <stdatomic.h>
#endif
#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_wifi_defs.h"

#ifdef __cplusplus
extern "C" {
/** @brief C++ layout representation; fields are not directly accessible. */
typedef uint32_t esy_wifi_atomic_u32_t;
/** @brief C++ layout representation of an atomic error snapshot. */
typedef int32_t esy_wifi_atomic_i32_t;
#else
/** @brief C11 atomic snapshot/request storage; accessed only by the library.
 * The selected backend determines lock-free or IRQ-spinlock semantics. */
typedef _Atomic uint32_t esy_wifi_atomic_u32_t;
/** @brief C11 error snapshot storage; accessed only by the library. */
typedef _Atomic int32_t esy_wifi_atomic_i32_t;
#endif

struct esy_device_s;

/** @brief Manager connection state. Numeric values remain stable. */
typedef enum {
  ESY_WIFI_MANAGER_STATE_IDLE = 0,
  ESY_WIFI_MANAGER_STATE_DISCONNECTED,
  ESY_WIFI_MANAGER_STATE_CONNECTING,
  ESY_WIFI_MANAGER_STATE_CONNECTED,
  ESY_WIFI_MANAGER_STATE_FAILED
} esy_wifi_manager_state_t;

/** @brief Configuration copied by init, including the credential strings. */
typedef struct {
  esy_wifi_config_t sta_config; /**< Station settings. */
} esy_wifi_manager_config_t;

/**
 * @brief Caller-owned manager storage, retained until successful deinit.
 *
 * A fresh object may contain uninitialized storage: successful init establishes
 * every field without reading its prior contents. An init failure leaves the
 * object unchanged; only retry init is valid for a never-initialized object.
 * `{0}` is recommended and permits pre-init queries returning STATE/FAILED.
 * Reinitializing the active owner returns STATE without changing it. A second
 * instance returns BUSY. Fields are exposed for allocation only, not caller use.
 * Credential arrays are fixed storage owned by this object, not borrowed memory.
 * Rebuild C and C++ callers if this concrete handle layout changes.
 */
typedef struct {
  esy_wifi_manager_config_t config;
  esy_wifi_atomic_u32_t state;
  esy_wifi_atomic_u32_t ipv4_snapshot;
  esy_wifi_atomic_i32_t last_error;
  esy_wifi_atomic_u32_t connect_requested;
  esy_wifi_atomic_u32_t has_ipv4_addr;
  bool connect_started;
  bool initialized;
  struct esy_device_s *device;
  char ssid[ESY_WIFI_SSID_MAX_LENGTH + 1u];
  char password[ESY_WIFI_PASSWORD_MAX_LENGTH + 1u];
} esy_wifi_manager_t;

/** @brief Fill defaults; NULL is ignored. No backend resources are acquired.
 * @param[out] config Configuration to fill. Credentials must be supplied later.
 */
void esy_wifi_manager_default_cfg(esy_wifi_manager_config_t *config);

/**
 * @brief Reserve the single backend and initialize a manager without hardware I/O.
 * @param[out] manager Fresh storage, or the current owner (rejected unchanged).
 * @param[in] config Readable configuration; strings need survive only this call.
 * @retval ESY_OK Initialized; object must live until successful deinit.
 * @retval ESY_ERR_INVALID_ARG NULL, empty SSID, or oversized/nonterminated strings.
 * @retval ESY_ERR_NOT_SUPPORTED Unsupported authentication mode.
 * @retval ESY_ERR_STATE This manager already owns the backend.
 * @retval ESY_ERR_BUSY Another manager owns the backend.
 * @note Lifecycle operation; serialize as described in the file contract.
 */
esy_err_t esy_wifi_manager_init(esy_wifi_manager_t *manager,
                                const esy_wifi_manager_config_t *config);

/**
 * @brief Publish persistent connection intent from any task.
 * @param[in,out] manager Initialized manager.
 * @retval ESY_OK Request retained until stop, including while PM-suspended.
 * @retval ESY_ERR_INVALID_ARG NULL manager.
 * @retval ESY_ERR_STATE Manager not initialized.
 * @note Requests coalesce; repeated requests do not reset an active timeout.
 * process drives connection and reconnect attempts. No hardware work runs here.
 */
esy_err_t esy_wifi_manager_request_connect(esy_wifi_manager_t *manager);

/**
 * @brief Advance connection/reconnection and refresh IPv4 once in the owner task.
 * @param[in,out] manager Initialized manager.
 * @retval ESY_OK Idle, still connecting, or connected. Pending DHCP is also OK;
 * get_ipv4_addr returns STATE and get_last_error retains the address error.
 * Backend start contention keeps connection intent and retries on a later call.
 * @retval ESY_ERR_INVALID_ARG NULL manager.
 * @retval ESY_ERR_STATE Uninitialized manager or unavailable device/backend.
 * @retval ESY_ERR_BUSY Device graph is transitioning/not started; retry later.
 * @retval <0 Other backend/device error, also stored by get_last_error.
 * @note Does not wait for the entire connection attempt. Backend initialization
 * and a polling iteration can perform synchronous SDK I/O; this is not a hard
 * real-time execution bound. Do not overlap process and stop calls.
 */
esy_err_t esy_wifi_manager_process(esy_wifi_manager_t *manager);

/**
 * @brief Cancel earlier requests and shut down Wi-Fi in the process owner task.
 * @param[in,out] manager Initialized manager; remains initialized on success.
 * @retval ESY_OK Stopped; a concurrent request after cancellation remains pending.
 * @retval ESY_ERR_INVALID_ARG NULL manager.
 * @retval ESY_ERR_STATE Uninitialized manager or invalid device.
 * @retval <0 Device acquisition/release failure, recorded as last_error.
 * @note Stop network consumers before calling. A failed acquisition leaves
 * requests/backend unchanged. A registered manager requires a started registry.
 */
esy_err_t esy_wifi_manager_stop(esy_wifi_manager_t *manager);

/**
 * @brief Shut down the backend and release the exclusive reservation.
 * @param[in,out] manager Current owner; endpoints must be quiescent.
 * @retval ESY_OK Backend stopped, object cleared and reusable by init.
 * @retval ESY_ERR_INVALID_ARG NULL manager.
 * @retval ESY_ERR_STATE Not the owner, including repeated deinit.
 * @retval ESY_ERR_BUSY Still bound to a device; detach it first.
 * @note Failure leaves ownership/storage intact. Deinit is valid after a failed
 * connection attempt. Before reclaiming storage, unregister the device (or
 * deinit its quiescent registry then unregister the surviving binding).
 */
esy_err_t esy_wifi_manager_deinit(esy_wifi_manager_t *manager);

/** @brief Read an atomic state snapshot from any task.
 * @param[in] manager Initialized or zero-initialized object.
 * @return State, or FAILED for NULL/uninitialized storage.
 */
esy_wifi_manager_state_t esy_wifi_manager_get_state(
    const esy_wifi_manager_t *manager);

/** @brief Read the connection-state snapshot from any task.
 * @param[in] manager Initialized or zero-initialized object.
 * @return True for CONNECTED; false otherwise, including NULL.
 */
bool esy_wifi_manager_is_connected(const esy_wifi_manager_t *manager);

/** @brief Read the last IPv4 snapshot; may become stale immediately after return.
 * @param[in] manager Initialized or zero-initialized object.
 * @param[out] out_addr Address destination; unchanged on error.
 * @retval ESY_OK An untorn address snapshot was copied.
 * @retval ESY_ERR_INVALID_ARG NULL argument.
 * @retval ESY_ERR_STATE Uninitialized or no address currently published.
 * @note Task-safe; process refreshes or invalidates the snapshot on each poll.
 */
esy_err_t esy_wifi_manager_get_ipv4_addr(const esy_wifi_manager_t *manager,
                                         esy_wifi_ipv4_addr_t *out_addr);

/** @brief Read the last process/stop result, including pending-address errors.
 * @param[in] manager Initialized or zero-initialized object.
 * @return Snapshot, INVALID_ARG for NULL, or STATE when uninitialized.
 * @note Task-safe. Init sets OK; request/query/lifecycle rejection does not
 * overwrite it. Connection-in-progress BUSY is normalized to OK; device BUSY
 * is retained. A later successful process/stop clears an earlier error.
 */
esy_err_t esy_wifi_manager_get_last_error(const esy_wifi_manager_t *manager);

#ifdef __cplusplus
}
#endif
#endif /* ESY_WIFI_MANAGER_H */
