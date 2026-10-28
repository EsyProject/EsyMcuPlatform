/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file fake_esy_hal_system.h
 * @brief Fake HAL system contract for unit tests.
 *
 * Used by unit and integration tests to replace the real board system APIs.
 * All state is kept in one context to make reset and assertions simple.
 */
#ifndef FAKE_ESY_HAL_SYSTEM_H
#define FAKE_ESY_HAL_SYSTEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Hook invoked after a fake platform reset request is recorded. */
typedef void (*fake_esy_hal_system_reset_hook_t)(void);

/** @brief Observable state maintained by the fake platform system. */
typedef struct {
  uintptr_t pc; /**< Fake execution address for image-region identification. */
  uint32_t irq_status;      /**< Fake local interrupt mask; zero is enabled. */
  uint32_t wait_event_count; /**< Number of low-level event waits. */
  uint32_t signal_event_count; /**< Number of low-level event signals. */
  void (*wait_event_hook)(void); /**< Synchronous scheduler hook, reset to NULL. */
  void (*irq_save_hook)(void); /**< Hook after masking, reset to NULL. */
  uint32_t tick_ms;         /**< Mock millisecond tick. */
  uint64_t time_us;         /**< Mock always-on microsecond counter. */
  uint32_t last_delay_ms;   /**< Last delay_ms argument. */
  uint32_t last_delay_us;   /**< Last delay_us argument. */
  uint32_t reset_count;     /**< Number of platform reset requests. */
  esy_err_t random_result; /**< Configurable random-source result. */
  uint32_t random_count; /**< Nonempty valid random requests. */
  uint64_t random_sequence; /**< Deterministic successful-request sequence. */
  bool inited;              /**< Whether init was called. */
  uint32_t clk_sys_hz;      /**< Mock system clock in Hz. */
  uint8_t core_id;          /**< Current core ID. */
  uint8_t core_count;       /**< Total core count. */
  bool is_in_isr;           /**< Whether execution is in ISR context. */
  bool tick_suspended;      /**< Whether the fake kernel tick is stopped. */
  bool tick_pending;        /**< Pending kernel tick observation. */
  uint32_t partial_tick_us; /**< Elapsed portion of the current tick. */
  uint32_t always_on_state_mask; /**< States supported by the clocksource. */
  fake_esy_hal_system_reset_hook_t reset_hook; /**< Optional reset hook. */
} fake_esy_hal_system_ctx_t;

/**
 * @brief Return the global fake context for test assertions.
 *
 * @return Mutable fake context.
 */
fake_esy_hal_system_ctx_t *fake_esy_hal_system_get_ctx(void);

/** @brief Reset the fake state to conservative defaults. */
void fake_esy_hal_system_reset(void);

/** @brief Clear the recorded platform reset request count. */
void fake_esy_hal_system_clear_reset_requests(void);

/**
 * @brief Return the recorded platform reset request count.
 *
 * @return Number of calls to @ref esy_hal_system_reset.
 */
uint32_t fake_esy_hal_system_get_reset_count(void);

/**
 * @brief Configure a hook invoked after each platform reset request.
 *
 * @param[in] hook Hook to invoke, or NULL to disable the hook.
 */
void fake_esy_hal_system_set_reset_hook(
    fake_esy_hal_system_reset_hook_t hook);

/**
 * @brief Set the fake millisecond tick.
 *
 * @param[in] now_ms New fake time in milliseconds.
 */
void fake_esy_hal_system_set_tick(uint32_t now_ms);

/**
 * @brief Advance the fake millisecond tick.
 *
 * @param[in] delta_ms Milliseconds to add to the fake time.
 */
void fake_esy_hal_system_advance_tick(uint32_t delta_ms);

/** @brief Fake implementation of the corresponding HAL public API. */
esy_err_t esy_hal_system_init(void);
/** @brief Deterministic random bytes; zero-length requests do not advance state. */
esy_err_t esy_hal_system_random_bytes(uint8_t *buffer, size_t length);
uint32_t esy_hal_system_clk_sys_get_hz(void);
void esy_hal_system_delay_ms(uint32_t ms);
void esy_hal_system_delay_us(uint32_t us);
uint32_t esy_hal_system_get_tick(void);
void esy_hal_system_tick_from_isr(void);
uint32_t esy_hal_system_time_32_us(void);
esy_err_t esy_hal_system_always_on_time_us(uint64_t *out_time_us);
uint32_t esy_hal_system_always_on_state_mask(void);
esy_err_t esy_hal_system_get_tick_partial_us(
    uint32_t tick_rate_hz, uint32_t *out_partial_tick_us);
esy_err_t esy_hal_system_suspend_tick(uint32_t tick_rate_hz,
                                           uint32_t *out_partial_tick_us);
void esy_hal_system_resume_tick(void);
bool esy_hal_system_tick_is_pending(void);
uint8_t esy_hal_system_get_core_id(void);
uint8_t esy_hal_system_get_core_count(void);
bool esy_hal_system_is_in_isr(void);

/** @brief Record a platform reset request and invoke the configured hook. */
void esy_hal_system_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESY_HAL_SYSTEM_H */
