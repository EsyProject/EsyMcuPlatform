/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "fake_esy_os.h"

/* Global fake state. */
static fake_esy_os_state_t s_ctx;

fake_esy_os_state_t *fake_esy_os_get_state(void)
{
    return &s_ctx;
}

void fake_esy_os_reset(void)
{
    s_ctx.core_count                = 1u;
    s_ctx.scheduler_started         = false;
    s_ctx.current_free_heap         = 1024;
    s_ctx.minimum_ever_free_heap    = 1024;
    s_ctx.last_isr_yield_requested  = false;
    s_ctx.malloc_call_count         = 0;
    s_ctx.fatal_handler             = NULL;
    s_ctx.fatal_bind_result         = ESY_OK;
    s_ctx.fatal_bind_count          = 0u;
}

esy_err_t esy_os_set_fatal_handler(esy_os_fatal_handler_t handler)
{
    ++s_ctx.fatal_bind_count;
    if (handler == NULL) return ESY_ERR_INVALID_ARG;
    if (s_ctx.fatal_bind_result != ESY_OK) return s_ctx.fatal_bind_result;
    if (s_ctx.scheduler_started || s_ctx.fatal_handler != NULL) return ESY_ERR_STATE;
    s_ctx.fatal_handler = handler;
    return ESY_OK;
}

void fake_esy_os_set_started(bool started)
{
    s_ctx.scheduler_started = started;
}

void fake_esy_os_set_free_heap_size(size_t bytes)
{
    s_ctx.current_free_heap = bytes;
}

void fake_esy_os_set_minimum_ever_free_heap_size(size_t bytes)
{
    s_ctx.minimum_ever_free_heap = bytes;
}

/* Fake implementation of the esy_os.h API. */

void esy_os_init(void)
{
    /* Reset on init so repeated test setup remains idempotent. */
    fake_esy_os_reset();
}

void esy_os_start_scheduler(void)
{
    /* The real scheduler does not return; the fake records the request only. */
    s_ctx.scheduler_started = true;
}

void esy_os_yield_from_isr(bool should_yield)
{
    /* Keep the last request observable for tests. */
    s_ctx.last_isr_yield_requested = should_yield;
}

size_t esy_os_get_free_heap_size(void)
{
    return s_ctx.current_free_heap;
}

size_t esy_os_get_minimum_ever_free_heap_size(void)
{
    return s_ctx.minimum_ever_free_heap;
}

bool esy_os_started(void)
{
    return s_ctx.scheduler_started;
}

void esy_os_systick(void)
{
    /* Unit tests usually do not need to simulate RTOS tick side effects. */
}

uint8_t esy_os_core_count(void) { return s_ctx.core_count; }
