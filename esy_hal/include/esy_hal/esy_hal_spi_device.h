/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_SPI_DEVICE_H
#define ESY_HAL_SPI_DEVICE_H

#include "esy_common/device/esy_device.h"
#include "esy_hal/esy_hal_spi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESY_SPI_DEVICE_BACKEND_BYTES 128u

struct esy_spi_device_binding_s;

/** @brief SPI backend PM transition implemented by a HAL assembly. */
typedef esy_err_t (*esy_spi_device_transition_fn_t)(
    struct esy_spi_device_binding_s *binding, esy_device_pm_action_t action,
    esy_power_state_t system_state);

/**
 * @brief Caller-owned adapter that makes one SPI handle a graph device.
 *
 * @note Initialize this binding with {0} before its first registration. Keep it
 * alive and unchanged until successful unregister, including after registry
 * deinit. Reusing an attached binding for another handle returns ESY_ERR_STATE
 * without changing its original handle or graph node. Registration failures
 * preserve an existing binding; never clear it to bypass unregister.
 */
typedef struct esy_spi_device_binding_s {
  esy_device_t device;
  esy_spi_t *spi;
  esy_spi_device_transition_fn_t transition;
  esy_spi_dma_complete_cb_t async_complete_cb;
  void *async_user_data;
  bool async_ref_held;
  ESY_ALIGNED_STORAGE(backend, ESY_SPI_DEVICE_BACKEND_BYTES);
} esy_spi_device_binding_t;

/**
 * @brief Borrow the HAL-owned PM graph binding without acquiring a reference.
 * @param spi Initialized hardware or XIO SPI handle; NULL is permitted.
 * @return The read-only device binding, or NULL when spi is NULL/unbound.
 * The binding remains caller-owned and borrowed until successful unregister.
 * The serialized lifecycle owner must prevent concurrent unregister/deinit.
 */
const esy_device_t *esy_hal_spi_get_device(const esy_spi_t *spi);

/**
 * @brief Write synchronously from a dependent device's PM transition callback.
 *
 * This restricted operation acquires no usage reference. The requester must be
 * TRANSITIONING in the same live registry as spi's ACTIVE graph binding and
 * list that SPI device as a direct dependency. The transition owner serializes
 * the call with all I/O and keeps both devices, data and provider context alive.
 * Use only during PM callback execution, never ordinary task work or ISR. The
 * provider's blocking/time bounds remain those of its normal blocking write.
 *
 * @return Bytes written; INVALID_ARG for NULL arguments or zero length, STATE
 * for invalid/unbound/mismatched device state, BUSY for an outstanding async
 * borrow or busy provider, NOT_SUPPORTED if the provider has no write hook.
 * Failure preserves handles, reservations and usage counts.
 */
int32_t esy_hal_spi_write_blocking_for_pm(esy_spi_t *spi,
                                         const esy_device_t *requester,
                                         const uint8_t *data, size_t len);

/**
 * @brief Register an initialized hardware SPI controller as a graph device.
 *
 * The binding and dependency array remain caller-owned until unregister. A
 * PM-enabled system calls this before @ref esy_device_registry_start. Systems
 * without power management omit this call. The hardware controller reservation
 * remains attached through runtime/system suspend. Suspend stops native hardware
 * before changing board pad state; failed board transitions restore the active
 * controller when possible. Partial native cleanup or failed resume/rollback
 * returns STATE so the registry faults the graph. A faulted graph retains the
 * handle/binding and controller reservation for system recovery/reset; ordinary
 * registry teardown and unregister are unavailable in that state. No error
 * permits clearing the handle or handing its physical controller to a new owner.
 */
esy_err_t esy_hal_spi_register_device(
    esy_spi_device_binding_t *binding, esy_spi_t *spi,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config,
    const esy_spi_config_t *resume_config);

/**
 * @brief Unbind a hardware or XIO SPI adapter before start or after registry deinit.
 * Finish or abort DMA and drain callbacks before this serialized owner call.
 * Registry deinit leaves the binding attached; unbind before hardware deinit.
 * Failure retains ownership and permits retry after its cause is resolved.
 */
esy_err_t esy_hal_spi_unregister_device(esy_spi_device_binding_t *binding);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_SPI_DEVICE_H */
