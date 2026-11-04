/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file esy_pwm_scheduler_core.h
 * @brief Caller-driven FIFO of PWM DMA waveforms; no OS or native SDK dependency.
 *
 * One non-ISR owner serializes all calls and drives processing regularly while
 * active. The borrowed HAL must already be initialized and idle;
 * this core exclusively owns its waveform output until deinit, never its HAL
 * lifecycle. Core, HAL, FIFO backing storage and sample buffers must not overlap.
 * Each terminal request disables output before releasing samples.
 * DMA source completion does not guarantee a full final PWM sample period.
 */
#ifndef ESY_PWM_SCHEDULER_CORE_H
#define ESY_PWM_SCHEDULER_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdalign.h>
#include "esy_hal/esy_hal_pwm.h"
#ifdef __cplusplus
extern "C" {
#endif
/** @brief Opaque caller-owned capacity, including private alignment slack. */
#define ESY_PWM_SCHEDULER_CORE_STORAGE_BYTES 512u
/** @brief Initialize with {0}; retain unchanged until successful deinit. */
typedef struct {
  alignas(max_align_t) uint8_t bytes[ESY_PWM_SCHEDULER_CORE_STORAGE_BYTES];
} esy_pwm_scheduler_core_t;
/** @brief Exactly-once terminal outcome. */
typedef enum {
  ESY_PWM_SCHEDULER_COMPLETE = 0, /**< Finite DMA retired and output disabled. */
  ESY_PWM_SCHEDULER_ERROR, /**< Submission/output/transfer failed and retired. */
  ESY_PWM_SCHEDULER_CANCELLED /**< Unsent work or successfully aborted output. */
} esy_pwm_scheduler_status_t;
/** @brief Coalesced nonterminal progress flags; no sample ownership is released. */
typedef enum {
  ESY_PWM_SCHEDULER_PROGRESS_HALF = 1u << 0,
  ESY_PWM_SCHEDULER_PROGRESS_CYCLE = 1u << 1
} esy_pwm_scheduler_progress_t;
struct esy_pwm_scheduler_request_s;
/** @brief Owner-only terminal callback; request is a temporary copied descriptor.
 * Samples and user_data remain borrowed through return. error is ESY_OK except
 * on ERROR. Partial progress is unknown. Do not reenter any core API. */
typedef void (*esy_pwm_scheduler_complete_cb_t)(
    const struct esy_pwm_scheduler_request_s *request,
    esy_pwm_scheduler_status_t status, esy_err_t error, void *user_data);
/** @brief Owner-only half/cycle progress; flags may coalesce between service
 * turns. Do not reenter core APIs; circular work remains active until abort. */
typedef void (*esy_pwm_scheduler_progress_cb_t)(
    const struct esy_pwm_scheduler_request_s *request, uint32_t events, void *user_data);
/** @brief One borrowed waveform. The core copies this descriptor at submit. */
typedef struct esy_pwm_scheduler_request_s {
  esy_dma_config_t transfer; /**< Samples in src; length is bytes. All DMA callback fields must be empty; core supplies them. circular requests require explicit abort. */
  esy_pwm_scheduler_complete_cb_t on_complete; /**< Required terminal callback. */
  esy_pwm_scheduler_progress_cb_t on_progress; /**< Optional coalesced half/cycle
      progress; requests half IRQ support. Pico rejects such requests with
      NOT_SUPPORTED during processing. Leave NULL for finite terminal-only DMA. */
  void *user_data; /**< Borrowed through terminal callback return. */
} esy_pwm_scheduler_request_t;
/** @brief Caller-owned FIFO backing storage; no task or dynamic allocation. */
typedef struct {
  esy_pwm_scheduler_request_t *requests; /**< Aligned array, alive until deinit. */
  uint32_t request_count; /**< Power of two >=2; total backing bytes must fit UINT32_MAX and SIZE_MAX; usable capacity is count minus one. */
} esy_pwm_scheduler_core_static_resources_t;
/** @brief Runtime binding copied by init. */
typedef struct {
  esy_pwm_scheduler_core_static_resources_t static_resources; /**< Required backing storage. */
} esy_pwm_scheduler_core_config_t;
/** @brief Bind an initialized, idle HAL without altering output or its lifecycle.
 * @return OK; INVALID_ARG for invalid storage; STATE for live core or invalid HAL;
 * BUSY for existing output/DMA. Failure leaves core/storage unchanged. */
esy_err_t esy_pwm_scheduler_core_init(esy_pwm_scheduler_core_t *core, esy_pwm_t *pwm,
    const esy_pwm_scheduler_core_config_t *config);
/** @brief Copy one descriptor, borrow its samples/context for owner processing.
 * No HAL call or terminal callback occurs here. Rejection borrows nothing.
 * @return OK, INVALID_ARG, STATE if stopped/reentrant, or NO_MEM if FIFO full. */
esy_err_t esy_pwm_scheduler_core_submit(esy_pwm_scheduler_core_t *core,
    const esy_pwm_scheduler_request_t *request);
/** @brief Advance at most one HAL processing call plus one abort or disable cleanup, one progress
 * and one terminal callback; or start one queued waveform. No task wait occurs.
 * Provider/callback execution determines duration; no wall-clock bound is promised.
 * Cleanup failure retains the active FIFO slot, samples and callback arguments.
 * Retry using next_wait_ms. Terminal error takes precedence over completion;
 * cancellation takes effect only after successful abort. @return OK, INVALID_ARG,
 * STATE if inactive/reentrant, or provider cleanup/service error. */
esy_err_t esy_pwm_scheduler_core_process(esy_pwm_scheduler_core_t *core);
/** @brief Close admission; optionally cancel unsent requests in process.
 * Active finite output drains naturally. Circular output continues until
 * request_abort; stop never implicitly cancels it. Repeated stop only escalates.
 * @return OK, INVALID_ARG, or STATE if inactive/reentrant. */
esy_err_t esy_pwm_scheduler_core_request_stop(esy_pwm_scheduler_core_t *core,
    bool cancel_queued);
/** @brief Close admission and request active/queued cancellation. process owns
 * abort/retry and terminal callbacks; this call performs no hardware access.
 * @return OK, INVALID_ARG, or STATE if inactive/reentrant. */
esy_err_t esy_pwm_scheduler_core_request_abort(esy_pwm_scheduler_core_t *core);
/** @brief True only for an initialized core with no queued/current work and idle
 * borrowed HAL. Invalid or reentrant queries return false. */
bool esy_pwm_scheduler_core_is_idle(const esy_pwm_scheduler_core_t *core);
/** @brief Relative wait: queued work 0 ms, active/cleanup 1 ms, idle UINT32_MAX.
 * Query after each process turn. There is no stored deadline or clock: the owner
 * controls processing and retry pacing; repeated queries do not advance time.
 */
uint32_t esy_pwm_scheduler_core_next_wait_ms(const esy_pwm_scheduler_core_t *core);
/** @brief Release an idle core; never deinitialize the borrowed HAL.
 * Stop producers first. @return OK also for zero core; INVALID_ARG for NULL;
 * STATE during callback/process; BUSY retains all storage/work for service retry. */
esy_err_t esy_pwm_scheduler_core_deinit(esy_pwm_scheduler_core_t *core);
#ifdef __cplusplus
}
#endif
#endif
