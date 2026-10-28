/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file fake_esy_hal_pwm.h
 * @brief Fake HAL PWM contract for unit tests.
 * @note Test-thread only; serialize all use. Storage starts at {0}; reset the
 * fake only after retiring live handles. DMA is unsupported by default. Opt-in
 * simulation models owner polling, finite output retention and failed cleanup;
 * it does not model hardware timing, route arbitration or PM graph references.
 */

#ifndef FAKE_ESY_HAL_PWM_H
#define FAKE_ESY_HAL_PWM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esy_hal/esy_hal_pwm.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reset all fake PWM instances.
 */
void fake_esy_hal_pwm_reset(void);

/**
 * @brief Select the DMA submission result; default is NOT_SUPPORTED.
 * @param result ESY_OK enables explicit queued-event DMA simulation. Other
 * errors except NOT_SUPPORTED model a partial submission followed by cleanup.
 */
void fake_esy_hal_pwm_set_dma_result(esy_err_t result);

/**
 * @brief Inject retirement and hardware-deinit results, both default ESY_OK.
 * @param abort_result Failure retains active DMA and pending notifications.
 * @param deinit_result Failure retains initialized handle and slot ownership.
 * @note Idle abort remains idempotent, independent of the injected DMA result.
 */
void fake_esy_hal_pwm_set_cleanup_results(esy_err_t abort_result,
                                          esy_err_t deinit_result);

/**
 * @brief Queue a full-transfer notification or terminal error for owner poll.
 * @param pwm Initialized PWM with an active simulated DMA borrow.
 * @param error true queues terminal error; false queues full/cycle completion.
 * @return ESY_OK, INVALID_ARG for NULL, STATE if inactive, BUSY in a callback.
 * @note Notifications coalesce and error wins. This never invokes a callback;
 * poll retires terminal work before notification. Circular completion retains
 * the borrow. Half-transfer timing is not simulated.
 */
esy_err_t fake_esy_hal_pwm_queue_dma_event(esy_pwm_t *pwm, bool error);

/**
 * @brief Query whether a PWM instance has been initialized.
 */
bool fake_esy_hal_pwm_is_initialized(esy_pwm_t *pwm);

/**
 * @brief Get the last configured duty cycle.
 * @return Last duty cycle, or 0.0f when unset.
 */
float fake_esy_hal_pwm_get_duty(esy_pwm_t *pwm);

/**
 * @brief Get the last configured wrap frequency.
 * @return Last frequency, or 0 when unset.
 */
uint32_t fake_esy_hal_pwm_get_freq(esy_pwm_t *pwm);

/**
 * @brief Query whether the PWM instance is enabled.
 */
bool fake_esy_hal_pwm_is_enabled(esy_pwm_t *pwm);

/**
 * @brief Manually trigger the wrap callback for unit test assertions.
 */
void fake_esy_hal_pwm_invoke_wrap_handler(esy_pwm_t *pwm);

struct esy_device_s;

/** @brief Observe whether the fake still owns an active DMA notification/borrow. */
bool fake_esy_hal_pwm_is_dma_active(const esy_pwm_t *pwm);
/** @brief Set a borrowed graph association in the fake provider; NULL clears it. */
void fake_esy_hal_pwm_set_device(esy_pwm_t *pwm, const struct esy_device_s *device);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESY_HAL_PWM_H */
