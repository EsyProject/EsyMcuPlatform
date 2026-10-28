/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_POWER_TICKLESS_ADAPTER_H
#define ESY_POWER_TICKLESS_ADAPTER_H

#include "esy_middleware/os/esy_os.h"
#include "esy_middleware/power_manager/esy_power_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Tickless adapter initialization policy. */
typedef struct esy_power_tickless_adapter_config_s {
  esy_power_manager_t *manager;
  uint64_t maximum_exit_latency_us;
  uint64_t late_resume_guard_us;
  uint32_t allowed_state_mask;
  uint32_t required_wake_source_mask;
} esy_power_tickless_adapter_config_t;

/** @brief Caller-owned bridge between the OS and one power manager.
 * Initialize with `{0}` before first use. One serialized lifecycle owner must
 * retain this object and its manager until OS unregistration and successful
 * deinit. Fields are private implementation state, not caller configuration.
 */
typedef struct esy_power_tickless_adapter_s {
  esy_power_manager_t *manager;
  esy_power_transaction_t transaction;
  esy_power_result_t result;
  uint64_t maximum_exit_latency_us;
  uint64_t late_resume_guard_us;
  uint32_t allowed_state_mask;
  uint32_t required_wake_source_mask;
  uint32_t magic;
  bool prepared;
} esy_power_tickless_adapter_t;

/** @brief Initialize an idle adapter before installing its OS operations.
 * @retval ESY_OK Initialized; the manager is borrowed and must be ready before
 * callback dispatch begins.
 * @retval ESY_ERR_INVALID_ARG Invalid pointers or configuration.
 * @retval ESY_ERR_STATE Already initialized.
 * Failure preserves the adapter. Call from the serialized task owner, before
 * installing the operations with esy_os_set_tickless_ops().
 */
esy_err_t esy_power_tickless_adapter_init(
    esy_power_tickless_adapter_t *adapter,
    const esy_power_tickless_adapter_config_t *config);

/** @brief Deinitialize an adapter after clearing its OS registration.
 * The owner must stop callback dispatch before calling this task-only API.
 * @retval ESY_OK Storage may be reused or initialized again.
 * @retval ESY_ERR_INVALID_ARG NULL or uninitialized adapter.
 * @retval ESY_ERR_BUSY Transaction still owns resources, including failed
 * recovery. Retain storage; a fatal recovery requires system reset.
 */
esy_err_t esy_power_tickless_adapter_deinit(
    esy_power_tickless_adapter_t *adapter);

/** @brief Return immutable operations for @ref esy_os_set_tickless_ops. */
const esy_os_tickless_ops_t *esy_power_tickless_adapter_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* ESY_POWER_TICKLESS_ADAPTER_H */
