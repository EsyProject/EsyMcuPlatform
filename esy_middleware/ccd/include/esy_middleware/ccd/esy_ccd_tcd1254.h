/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_CCD_TCD1254_H
#define ESY_CCD_TCD1254_H

#include <stdbool.h>
#include <stdint.h>
#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Complete TCD1254GFG line, including dummy outputs. */
#define ESY_CCD_TCD1254_LINE_SAMPLES 2547u
/** @brief Outputs preceding the first effective pixel. */
#define ESY_CCD_TCD1254_LEADING_SAMPLES 32u
/** @brief Effective image samples in a complete line. */
#define ESY_CCD_TCD1254_ACTIVE_SAMPLES 2500u
/** @brief Outputs following the effective image. */
#define ESY_CCD_TCD1254_TRAILING_SAMPLES 15u
/** @brief Minimum discarded complete readouts after power becomes stable. */
#define ESY_CCD_TCD1254_STARTUP_READOUTS 10u

/**
 * @brief Requested device timing, independent of ADC, channel count and pins.
 *
 * tick_hz is the waveform executor's constant hardware clock, not a timestamp.
 * The even master period gives a 50% duty cycle and an actual 0.4..4 MHz clock.
 * Requests are rounded upward to executor ticks; SH period and integration are
 * further rounded to whole master periods so repeated frames retain their phase.
 */
typedef struct {
  uint32_t tick_hz; /**< Positive waveform clock frequency in Hz. */
  uint32_t master_period_ticks; /**< Even master period, at least two ticks. */
  uint32_t icg_delay_ns; /**< t1, SH fall to ICG rise: at least 1000 ns. */
  uint32_t icg_to_sh_ns; /**< t2, ICG fall to SH rise: 100..1000 ns. */
  uint32_t sh_high_ns; /**< Identical width of every SH pulse: at least 1000 ns. */
  uint32_t sh_period_ns; /**< SH purge rising-edge period: at least 10000 ns. */
  uint32_t integration_ns; /**< Last SH rising-edge gap: strictly above 10000 ns. */
} esy_ccd_tcd1254_config_t;

/**
 * @brief Finite periodic waveform description; every field uses executor ticks.
 *
 * Keep the master clock continuous, high on phase [0, master_period_ticks/2).
 * At each frame origin drive ICG low and SH low. Raise SH at sh_rise_ticks,
 * lower it sh_high_ticks later, and raise ICG at icg_rise_ticks. ICG rise is
 * aligned to master phase zero, when the master is high. During the remaining
 * frame, generate purge_pulses SH pulses at sh_rise_ticks + k*sh_period_ticks,
 * k=1..purge_pulses, each with the same sh_high_ticks width. The next frame
 * begins at frame_period_ticks; its SH rise follows the last purge SH rise by
 * integration_ticks. Zero purge pulses gives the ordinary full-frame exposure.
 *
 * Read all 2547 outputs starting at the executor's correctly phased D0 window
 * after ICG rise. readout_ticks covers 5094 master periods; the next ICG fall
 * occurs after this entire interval. This plan does not select an ADC sampling
 * phase or execute hardware. A hardware owner must preserve the stated phase,
 * provide the continuous clock, and realize all edges through public HAL.
 */
typedef struct {
  uint32_t tick_hz; /**< Waveform clock frequency. */
  uint32_t master_period_ticks; /**< Master period; its high duration is half. */
  uint32_t sh_rise_ticks; /**< SH rise relative to ICG fall/frame origin. */
  uint32_t sh_high_ticks; /**< Constant SH high duration. */
  uint32_t icg_rise_ticks; /**< ICG rise relative to the frame origin. */
  uint32_t sh_period_ticks; /**< Rising-edge spacing of purge pulses. */
  uint32_t integration_ticks; /**< Gap from final purge to next transfer SH. */
  uint32_t purge_pulses; /**< Extra SH pulses between frame transfers. */
  uint32_t readout_ticks; /**< Complete line duration at master/2 data rate. */
  uint32_t frame_period_ticks; /**< Phase-preserving repeated frame period. */
} esy_ccd_tcd1254_plan_t;

/**
 * @brief Compile timing without hardware access; output changes only on success.
 * @return INVALID_ARG for NULL, invalid requested ranges or master frequency;
 * NOT_SUPPORTED when quantization violates t2, SH pulses overlap, a purge starts
 * before ICG rises, or any compiled duration exceeds UINT32_MAX; otherwise OK.
 *
 * All arithmetic uses checked 64-bit durations. Durations are not wrapping wall
 * clocks; no polling, timeout, maximum observation interval or ISR is involved.
 */
esy_err_t esy_ccd_tcd1254_plan_compile(const esy_ccd_tcd1254_config_t *config,
                                         esy_ccd_tcd1254_plan_t *plan);

/** @brief One software-authorized readout; does not start hardware. */
typedef struct {
  uint64_t token; /**< Nonzero operation identity; acknowledge this exact value. */
  bool discard; /**< Startup line: never present its pixels as a valid capture. */
  esy_ccd_tcd1254_plan_t plan; /**< Value copy for the borrowed executor. */
} esy_ccd_tcd1254_readout_t;

/**
 * @brief Caller-owned software state; initialize with {0} before its first use.
 *
 * Fields are implementation-owned. One main-loop/task owner serializes every
 * call; no operation is ISR-safe. Never copy, modify or clear a used handle.
 * No task, timer, allocation, callback, bus, buffer or hardware resource exists
 * inside it. The caller owns and quiesces the separate waveform/ADC executor.
 * Tokens never wrap and remain reserved across deinit/reinit of this object.
 */
typedef struct {
  esy_ccd_tcd1254_plan_t plan; /**< Private compiled timing. */
  uint64_t sequence; /**< Private nonwrapping operation sequence. */
  uint32_t startup_readouts; /**< Private confirmed startup readout count. */
  bool initialized; /**< Private software attachment state. */
  bool pending; /**< Private outstanding readout state. */
} esy_ccd_tcd1254_t;

/**
 * @brief Compile a plan and reset the required startup-readout count.
 *
 * Call after sensor power is stable. This performs software initialization only.
 * Repeated init returns STATE without change. Compilation failures retain the
 * detached state and sequence, allowing retry. Other errors match plan_compile.
 */
esy_err_t esy_ccd_tcd1254_init(esy_ccd_tcd1254_t *sensor,
                                 const esy_ccd_tcd1254_config_t *config);
/**
 * @brief Authorize one readout; output changes only on success.
 * @return INVALID_ARG for NULL, STATE if detached or UINT64_MAX tokens exhausted,
 * BUSY for an outstanding readout, otherwise OK. First ten readouts are discarded.
 * Hardware must execute the copied plan separately, retaining the returned token.
 */
esy_err_t esy_ccd_tcd1254_begin(esy_ccd_tcd1254_t *sensor,
                                  esy_ccd_tcd1254_readout_t *readout);
/**
 * @brief Acknowledge a hardware-confirmed complete readout after all I/O finishes.
 *
 * The owner confirms valid waveform timing, sample phase and exactly 2547 output
 * samples, including dummy pixels. The first ten successful acknowledgements set
 * usable=false; the eleventh and subsequent ones set usable=true. This only
 * validates software sequencing and length; it cannot verify external waveforms.
 * @return INVALID_ARG for NULL, STATE for a missing/stale token or detached state,
 * IO for incorrect sample count (consumes the token and resets startup), or OK.
 * usable changes only on OK. Completed tokens cannot acknowledge a later readout.
 */
esy_err_t esy_ccd_tcd1254_complete(esy_ccd_tcd1254_t *sensor, uint64_t token,
                                     uint32_t sample_count, bool *usable);
/**
 * @brief Cancel a pending token after the owner has stopped all associated I/O.
 *
 * Resets the startup count. It neither aborts DMA nor changes output pins.
 * @return INVALID_ARG for NULL, STATE for detached/missing/stale token, or OK.
 */
esy_err_t esy_ccd_tcd1254_cancel(esy_ccd_tcd1254_t *sensor, uint64_t token);
/**
 * @brief Detach software state after external I/O has stopped; preserves tokens.
 * @return INVALID_ARG for NULL, BUSY for a pending readout, otherwise OK.
 * Repeated deinit succeeds. Reinit restarts startup; this never stops hardware.
 */
esy_err_t esy_ccd_tcd1254_deinit(esy_ccd_tcd1254_t *sensor);

#ifdef __cplusplus
}
#endif
#endif /* ESY_CCD_TCD1254_H */
