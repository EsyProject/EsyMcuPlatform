/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_TOUCH_H
#define ESY_TOUCH_H

/**
 * @file esy_touch.h
 * @brief Generic touch input facade.
 *
 * Upper layers should prefer these driver-agnostic APIs over calling
 * @ref esy_touch_t function pointers directly.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/display/esy_touch_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

struct esy_device_s;

/**
 * @brief Opaque storage size (bytes) inside @ref esy_touch_t.
 *
 * Touch controller drivers place their private context into this storage.
 * You may override this macro at build time if needed.
 *
 * The driver implementation must validate its context size with _Static_assert.
 */
#ifndef ESY_TOUCH_PRIV_BYTES
#define ESY_TOUCH_PRIV_BYTES 384u
#endif

/**
 * @brief Unified touch device handle.
 *
 * - @ref poll starts or advances sampling.
 * - @ref get_sample returns the latest completed sample.
 * - @ref ctx points to the driver private context located inside @ref
 * priv.bytes.
 *
 * Scheme C:
 * - The handle owns an internal opaque buffer @ref priv.bytes.
 * - Driver initializes @ref ctx to point into this buffer.
 * - Users must not access bytes directly.
 *
 * Upper layers should call the touch facade APIs rather than invoking backend
 * hooks directly.
 *
 * Initialize the handle with `{0}` before first use. An attached handle must
 * drain its accepted transfers and provider callbacks, unregister its logical
 * device, and successfully detach before another backend initialization. Never
 * clear a live handle or its opaque storage to bypass this lifecycle. Handle
 * operations and callbacks require one execution owner or external serialization.
 */
typedef struct {
  /**
   * @brief Advance or start touch sampling.
   *
   * Mandatory backend hook used by the facade. Backends should start or advance
   * sampling here without requiring callers to consume the latest completed
   * sample at the same time.
   *
   * @param[in] arg Driver/user context (typically the @ref esy_touch_t handle).
   *
   * @retval ESY_OK Poll accepted or no new work was needed.
   * @retval <0     Failure reported by the concrete driver.
   */
  esy_err_t (*poll)(void *arg);

  /**
   * @brief Return the latest completed touch sample.
   *
   * Mandatory backend hook used by the facade. This function must not start a
   * new hardware transfer; it only reports the backend's current sample cache.
   *
   * @param[out] sample Latest completed sample.
   * @param[in]  arg    Driver/user context (typically the @ref esy_touch_t
   * handle).
   *
   * @retval ESY_OK Sample returned.
   * @retval <0     Failure reported by the concrete driver.
   */
  esy_err_t (*get_sample)(esy_touch_data_t *sample, void *arg);

  void *
      ctx; /**< Driver private context pointer (points into @ref priv.bytes). */
  struct esy_device_s *device; /**< Optional registered logical PM device. */

  union {
    max_align_t _align;                  /**< Alignment guarantee. */
    uint8_t bytes[ESY_TOUCH_PRIV_BYTES]; /**< Opaque private storage. */
  } priv;
} esy_touch_t;

/* -------------------------------------------------------------------------- */
/* SPI scheduler callback prototypes                                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief Reserve writable full-duplex SPI buffers of at least the requested size.
 *
 * The returned buffers are owned by the SPI provider. The touch backend may
 * write command bytes to @p tx_buf and may clear @p rx_buf before submit, but
 * it must not retain the buffer pointers after the corresponding submit call
 * except through the provider-managed transfer callbacks.
 *
 * The provider must validate both capacities before reserving or exposing a
 * block. On success, both buffers contain at least @p minimum_size writable
 * bytes. On failure, reserve no block, set both outputs to NULL and leave
 * existing reservations unchanged. The backend must not write or submit after
 * a failed reserve. Serialize reservation, writes and submission with the
 * provider's producer endpoint; this callback is not ISR-safe.
 *
 * @param[out] tx_buf TX buffer pointer.
 * @param[out] rx_buf RX buffer pointer.
 * @param[in]  minimum_size Required writable bytes in each buffer; must be nonzero.
 * @param[in]  arg    User context pointer.
 *
 * @retval ESY_OK Buffers returned successfully.
 * @retval ESY_ERR_NO_MEM Either buffer is smaller than the requested size.
 * @retval <0     Failure reported by the SPI owner.
 */
typedef esy_err_t (*esy_touch_spi_get_write_buffer)(uint8_t **tx_buf,
                                                    uint8_t **rx_buf,
                                                    size_t minimum_size,
                                                    void *arg);


/**
 * @brief Check whether a touch handle is attached to a concrete backend.
 *
 * @param[in] touch Touch handle pointer.
 *
 * @retval true  Handle has a backend context and read operation.
 * @retval false Handle is NULL or not attached.
 */
bool esy_touch_is_attached(const esy_touch_t *touch);

/**
 * @brief Clear an unregistered touch handle after its transfers have drained.
 *
 * The lifecycle owner must stop polling/input producers, wait until every
 * accepted SPI transfer and its callbacks have finished, and successfully
 * unregister any logical device before calling this function. Serialize detach
 * with all other handle access, including provider callbacks. This function
 * neither cancels transfers nor waits for their completion.
 *
 * A registered handle is left unchanged. Already detached handles succeed.
 * After successful detachment the handle may be initialized again.
 *
 * @param[in,out] touch Touch handle pointer.
 * @retval ESY_OK Handle cleared, including an already detached handle.
 * @retval ESY_ERR_INVALID_ARG NULL handle.
 * @retval ESY_ERR_STATE Handle still owns a registered logical device.
 */
esy_err_t esy_touch_detach(esy_touch_t *touch);

/**
 * @brief Advance or start touch sampling.
 *
 * The attached backend must provide a poll hook.
 *
 * @param[in,out] touch Touch handle pointer.
 *
 * @retval ESY_OK Poll accepted or no new work was needed.
 * @retval ESY_ERR_INVALID_ARG Invalid handle pointer.
 * @retval ESY_ERR_STATE Handle is not attached to a backend.
 * @retval <0 Backend-specific failure.
 */
esy_err_t esy_touch_poll(esy_touch_t *touch);

/**
 * @brief Get the latest completed touch sample without starting new hardware I/O.
 *
 * @param[in,out] touch  Touch handle pointer.
 * @param[out]    sample Latest sample.
 *
 * @retval ESY_OK Sample returned.
 * @retval ESY_ERR_INVALID_ARG Invalid pointer.
 * @retval ESY_ERR_STATE Handle is not attached or backend has no cached sample API.
 * @retval <0 Backend-specific failure.
 */
esy_err_t esy_touch_get_sample(esy_touch_t *touch, esy_touch_data_t *sample);

/**
 * @brief Read touch input through the attached backend.
 *
 * This API polls the backend and then returns the latest completed sample
 * through the backend get-sample hook.
 *
 * @param[in,out] touch   Touch handle pointer.
 * @param[out]    x       X coordinate in pixels.
 * @param[out]    y       Y coordinate in pixels.
 * @param[out]    pressed Pressed state.
 *
 * @retval ESY_OK Touch state read or latest completed sample returned.
 * @retval ESY_ERR_INVALID_ARG Invalid handle or output pointer.
 * @retval ESY_ERR_STATE Handle is not attached to a backend.
 * @retval <0 Backend-specific failure.
 */
esy_err_t esy_touch_read_input(esy_touch_t *touch,
                               uint16_t *x,
                               uint16_t *y,
                               bool *pressed);

#ifdef __cplusplus
}
#endif

#endif /* ESY_TOUCH_H */
