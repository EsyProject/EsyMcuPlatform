/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file fake_esy_os.h
 * @brief Fake implementation helper for esy_os.h tests.
 */

#ifndef FAKE_ESY_OS_H
#define FAKE_ESY_OS_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Include the production contract first to keep declarations aligned. */
#include "esy_middleware/os/esy_os.h"
#include "esy_middleware/os/esy_os_fatal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Internal fake OS state exposed for test assertions.
 *
 * Tests may read this structure directly or update it through helper APIs.
 */
typedef struct
{
    uint8_t core_count;                /**< Number of schedulable fake cores. */
    bool   scheduler_started;           /**< Whether the fake scheduler is marked as started. */
    size_t current_free_heap;           /**< Value returned by esy_os_get_free_heap_size(). */
    size_t minimum_ever_free_heap;      /**< Value returned by esy_os_get_minimum_ever_free_heap_size(). */
    bool   last_isr_yield_requested;    /**< Last esy_os_yield_from_isr() request. */
    uint32_t malloc_call_count;         /**< Allocation call count used by no-malloc tests. */
    esy_os_fatal_handler_t fatal_handler; /**< Successfully bound fatal callback. */
    esy_err_t fatal_bind_result;        /**< Forced binding result; default ESY_OK. */
    uint32_t fatal_bind_count;          /**< Number of fatal binding attempts. */
} fake_esy_os_state_t;

/**
 * @brief Get the global fake state for direct test inspection.
 */
fake_esy_os_state_t *fake_esy_os_get_state(void);

/**
 * @brief Reset the fake state to clean defaults.
 *
 * Defaults:
 *  - scheduler not started
 *  - free heap = 1024
 *  - min ever free heap = 1024
 *  - ISR yield flag cleared
 */
void fake_esy_os_reset(void);

/**
 * @brief Mark the fake scheduler as started or stopped.
 */
void fake_esy_os_set_started(bool started);

/**
 * @brief Set the current fake free heap size.
 */
void fake_esy_os_set_free_heap_size(size_t bytes);

/**
 * @brief Set the fake minimum-ever free heap size.
 */
void fake_esy_os_set_minimum_ever_free_heap_size(size_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESY_OS_H */
