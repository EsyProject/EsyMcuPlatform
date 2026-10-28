/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/freq_synth/esy_freq_synth.h"
#include "esy_freq_synth_internal.h"

#include <string.h>

static bool esy_freq_synth_has_core_ops(const esy_freq_synth_t *synth) {
  if (synth == NULL) {
    return false;
  }

  /* Core capability check: the backend can set frequency. */
  return (synth->ctx != NULL) && (synth->ops != NULL) &&
         (synth->ops->set_frequency_hz != NULL);
}

static esy_err_t esy_freq_synth_require_attached(
    const esy_freq_synth_t *synth) {
  if (synth == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!esy_freq_synth_has_core_ops(synth) ||
      esy_freq_synth_runtime_const(synth)->faulted) {
    return ESY_ERR_STATE;
  }
  return ESY_OK;
}

static esy_err_t esy_freq_synth_acquire(esy_freq_synth_t *synth) {
  esy_device_t *device = esy_freq_synth_runtime_const(synth)->device;
  return device == NULL ? ESY_OK : esy_device_get(device);
}

static void esy_freq_synth_release(esy_freq_synth_t *synth) {
  esy_device_t *device = esy_freq_synth_runtime_const(synth)->device;
  if (device != NULL) (void)esy_device_put(device);
}

void esy_freq_synth_detach(esy_freq_synth_t *synth) {
  if (synth == NULL) return;
  const esy_freq_synth_runtime_t *runtime = esy_freq_synth_runtime_const(synth);
  if (runtime->device != NULL || runtime->faulted) return;
  memset(synth, 0, sizeof(*synth));
}

bool esy_freq_synth_is_attached(const esy_freq_synth_t *synth) {
  return esy_freq_synth_has_core_ops(synth);
}

esy_err_t esy_freq_synth_set_frequency_hz(esy_freq_synth_t *synth,
                                          uint64_t freq_hz) {
  esy_err_t err = esy_freq_synth_require_attached(synth);
  if (err != ESY_OK) return err;
  err = esy_freq_synth_acquire(synth);
  if (err != ESY_OK) return err;
  err = synth->ops->set_frequency_hz(synth->ctx, freq_hz);
  esy_freq_synth_release(synth);
  return err;
}

esy_err_t esy_freq_synth_set_output_enable(esy_freq_synth_t *synth,
                                           esy_freq_synth_port_t port,
                                           bool enable) {
  esy_err_t err = esy_freq_synth_require_attached(synth);
  if (err != ESY_OK) return err;
  if (synth->ops->set_output_enable == NULL) {
    return ESY_ERR_NOT_SUPPORTED;
  }
  err = esy_freq_synth_acquire(synth);
  if (err != ESY_OK) return err;
  err = synth->ops->set_output_enable(synth->ctx, port, enable);
  esy_freq_synth_release(synth);
  return err;
}

esy_err_t esy_freq_synth_set_power_level(esy_freq_synth_t *synth,
                                         esy_freq_synth_port_t port,
                                         uint8_t level) {
  esy_err_t err = esy_freq_synth_require_attached(synth);
  if (err != ESY_OK) return err;
  if (synth->ops->set_power_level == NULL) {
    return ESY_ERR_NOT_SUPPORTED;
  }
  err = esy_freq_synth_acquire(synth);
  if (err != ESY_OK) return err;
  err = synth->ops->set_power_level(synth->ctx, port, level);
  esy_freq_synth_release(synth);
  return err;
}

esy_err_t esy_freq_synth_get_info(esy_freq_synth_t *synth,
                                  esy_freq_synth_info_id_t id,
                                  esy_freq_synth_info_value_t *out) {
  esy_err_t err = esy_freq_synth_require_attached(synth);
  if (err != ESY_OK) return err;
  if (out == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  if (synth->ops->get_info == NULL) {
    return ESY_ERR_NOT_SUPPORTED;
  }

  /* Give the caller a clean value; drivers only need to fill supported fields.
   */
  memset(out, 0, sizeof(*out));
  err = esy_freq_synth_acquire(synth);
  if (err != ESY_OK) return err;
  err = synth->ops->get_info(synth->ctx, id, out);
  esy_freq_synth_release(synth);
  return err;
}
