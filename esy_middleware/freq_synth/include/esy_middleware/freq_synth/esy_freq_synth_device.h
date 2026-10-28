/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_FREQ_SYNTH_DEVICE_H
#define ESY_FREQ_SYNTH_DEVICE_H

#include "esy_common/device/esy_device.h"
#include "esy_common/defs/system/esy_gpio_pm_defs.h"
#include "esy_middleware/freq_synth/esy_freq_synth.h"

#ifdef __cplusplus
extern "C" {
#endif

struct esy_freq_synth_device_binding_s;

/** @brief Frequency-synthesizer-specific device registration configuration. */
typedef struct {
  esy_device_registration_config_t registration; /**< Graph metadata. */
  const esy_device_gpio_pm_profile_t *gpio_pm_profile; /**< Auxiliary GPIOs. */
} esy_freq_synth_device_config_t;

/** @brief Concrete synthesizer PM transition hook. */
typedef esy_err_t (*esy_freq_synth_device_transition_fn_t)(
    struct esy_freq_synth_device_binding_s *binding,
    esy_device_pm_action_t action, esy_power_state_t system_state);

/** @brief Concrete synthesizer persistent-activity query. */
typedef bool (*esy_freq_synth_device_is_runtime_idle_fn_t)(
    const struct esy_freq_synth_device_binding_s *binding);

/**
 * @brief Caller-owned logical frequency-synthesizer device adapter.
 *
 * Initialize with `{0}` before first registration. The binding, synth, GPIO
 * profile, dependency array, name, hooks and owner remain valid at stable
 * addresses until successful unregister. Never reuse or clear a live binding.
 * Registration is an externally serialized task operation before graph start.
 * Unregister also supports cleanup after quiescent registry deinit.
 * Failed registration leaves an empty binding unless GPIO release
 * also fails; then unregister must complete cleanup. Failed unregister
 * retains ownership and may be retried after restoring the GPIO default state.
 * Link the optional esy_freq_synth_device target, assembled with ENABLE_ESY_GPIO.
 * The facade and chip drivers do not link this GPIO PM registration adapter.
 */
typedef struct esy_freq_synth_device_binding_s {
  esy_device_t device;
  esy_freq_synth_t *synth;
  esy_freq_synth_device_transition_fn_t transition;
  esy_freq_synth_device_is_runtime_idle_fn_t is_runtime_idle;
  const esy_device_gpio_pm_profile_t *gpio_pm_profile;
  void *owner;
  bool registered; /**< Graph cleanup has not yet been acknowledged by unbind. */
} esy_freq_synth_device_binding_t;

/**
 * @brief Register one initialized logical synthesizer before graph start.
 * @return ESY_OK, ESY_ERR_INVALID_ARG for invalid arguments, ESY_ERR_STATE for
 * an occupied/faulted binding, or the GPIO/device registration error.
 * Hooks run synchronously in task context. A suspend hook must restore the
 * previous state on error; ESY_ERR_STATE reports unrecoverable rollback failure.
 */
esy_err_t esy_freq_synth_register_device(
    esy_freq_synth_device_binding_t *binding, esy_freq_synth_t *synth,
    esy_device_registry_t *registry,
    const esy_freq_synth_device_config_t *device_config,
    esy_freq_synth_device_transition_fn_t transition,
    esy_freq_synth_device_is_runtime_idle_fn_t is_runtime_idle, void *owner);

/**
 * @brief Unbind a synthesizer before graph start or after registry deinit.
 * Quiesce outputs/producers and serialize I/O/lifecycle first. Registry deinit
 * leaves the typed binding attached; unbind before concrete backend deinit.
 * @return ESY_OK (also for an empty binding), ESY_ERR_INVALID_ARG for NULL,
 * or the graph/GPIO cleanup error. Retain all resources until ESY_OK.
 */
esy_err_t esy_freq_synth_unregister_device(
    esy_freq_synth_device_binding_t *binding);

#ifdef __cplusplus
}
#endif

#endif /* ESY_FREQ_SYNTH_DEVICE_H */
