/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_FREQ_SYNTH_INTERNAL_H
#define ESY_FREQ_SYNTH_INTERNAL_H

#include "esy_middleware/freq_synth/esy_freq_synth.h"
#include "esy_common/device/esy_device.h"

#if defined(__GNUC__) || defined(__clang__)
#define ESY_FREQ_SYNTH_MAY_ALIAS __attribute__((__may_alias__))
#else
#define ESY_FREQ_SYNTH_MAY_ALIAS
#endif

/** @brief Module-private state; callers serialize the complete handle lifecycle. */
typedef struct ESY_FREQ_SYNTH_MAY_ALIAS {
  esy_device_t *device;
  bool faulted;
} esy_freq_synth_runtime_t;

_Static_assert(sizeof(esy_freq_synth_runtime_t) <= ESY_FREQ_SYNTH_FRONTEND_STORAGE_BYTES,
               "Frequency-synthesizer frontend storage is too small");
_Static_assert(_Alignof(esy_freq_synth_runtime_t) <= _Alignof(esy_freq_synth_t),
               "Frequency-synthesizer frontend alignment is insufficient");
_Static_assert(offsetof(esy_freq_synth_t, frontend) % _Alignof(esy_freq_synth_runtime_t) == 0u,
               "Frequency-synthesizer frontend offset is misaligned");

/** @brief Borrow mutable module state; NULL stays NULL. */
static inline esy_freq_synth_runtime_t *esy_freq_synth_runtime(esy_freq_synth_t *synth) {
  return synth == NULL ? NULL : (esy_freq_synth_runtime_t *)(void *)synth->frontend.bytes;
}

/** @brief Borrow read-only module state; NULL stays NULL. */
static inline const esy_freq_synth_runtime_t *esy_freq_synth_runtime_const(
    const esy_freq_synth_t *synth) {
  return synth == NULL ? NULL :
      (const esy_freq_synth_runtime_t *)(const void *)synth->frontend.bytes;
}

#endif /* ESY_FREQ_SYNTH_INTERNAL_H */
