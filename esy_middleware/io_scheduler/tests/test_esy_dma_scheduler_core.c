/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/io_scheduler/esy_dma_scheduler_core.h"
#include "fake_esy_hal_dma.h"
#include "unity.h"

#include <string.h>

typedef struct {
    unsigned complete;
    unsigned half;
    unsigned errors;
    unsigned terminal;
    unsigned cancelled;
    esy_err_t result;
    unsigned id;
    bool check_reentry;
} request_probe_t;

typedef struct {
    esy_dma_scheduler_core_t core;
    esy_dma_t dma;
    esy_dma_scheduler_request_t slots[4];
    esy_dma_scheduler_core_config_t config;
    uint16_t samples[4];
    unsigned order[8];
    unsigned order_count;
    bool processing;
} dma_scheduler_test_ctx_t;

static dma_scheduler_test_ctx_t s_ctx;
static const esy_dma_inner_config_t k_routes[] = {
    {.dreq = 10u, .channel = 5u}, {.dreq = 11u, .channel = 6u},
};

static void assert_reentry_rejected(void) {
    esy_dma_scheduler_request_t request = {.transfer = {
        .length = sizeof(s_ctx.samples), .inner = k_routes[0]}};
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_dma_scheduler_core_process(&s_ctx.core));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_dma_scheduler_core_submit(&s_ctx.core, &request));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_dma_scheduler_core_request_stop(&s_ctx.core, false));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_dma_scheduler_core_request_abort(&s_ctx.core));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_dma_scheduler_core_deinit(&s_ctx.core));
    TEST_ASSERT_FALSE(esy_dma_scheduler_core_is_idle(&s_ctx.core));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX,
        esy_dma_scheduler_core_next_wait_ms(&s_ctx.core));
}

static void on_complete(void *arg) {
    request_probe_t *probe = arg;
    TEST_ASSERT_TRUE(s_ctx.processing);
    ++probe->complete;
    if (probe->check_reentry) assert_reentry_rejected();
}

static void on_half(void *arg) {
    request_probe_t *probe = arg;
    TEST_ASSERT_TRUE(s_ctx.processing);
    ++probe->half;
}

static void on_error(void *arg) {
    request_probe_t *probe = arg;
    TEST_ASSERT_TRUE(s_ctx.processing);
    ++probe->errors;
}

static void on_done(esy_err_t result, bool cancelled, void *arg) {
    request_probe_t *probe = arg;
    TEST_ASSERT_TRUE(s_ctx.processing);
    TEST_ASSERT_TRUE(esy_hal_dma_is_idle(&s_ctx.dma));
    ++probe->terminal;
    probe->cancelled += cancelled;
    probe->result = result;
    TEST_ASSERT_LESS_THAN_UINT(8u, s_ctx.order_count);
    s_ctx.order[s_ctx.order_count++] = probe->id;
    if (probe->check_reentry) assert_reentry_rejected();
}

static esy_dma_scheduler_request_t make_request(request_probe_t *probe,
                                                unsigned route) {
    return (esy_dma_scheduler_request_t){.transfer = {
        .src = s_ctx.samples, .dst = s_ctx.samples, .length = sizeof(s_ctx.samples),
        .transfer_size = ESY_DMA_TRANSFER_SIZE_16, .inner = k_routes[route],
        .callback = on_complete, .callback_arg = probe,
        .half_callback = on_half, .half_callback_arg = probe,
        .error_callback = on_error, .error_callback_arg = probe},
        .done = on_done, .done_arg = probe};
}

static esy_err_t process(void) {
    s_ctx.processing = true;
    esy_err_t result = esy_dma_scheduler_core_process(&s_ctx.core);
    s_ctx.processing = false;
    return result;
}

static void init_core(void) {
    TEST_ASSERT_EQUAL(ESY_OK,
        esy_dma_scheduler_core_init(&s_ctx.core, &s_ctx.dma, &s_ctx.config));
}

void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    fake_esy_hal_dma_reset();
    const esy_dma_controller_config_t config = {
        .channels = k_routes, .channel_count = 2u};
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_init(&s_ctx.dma, &config));
    s_ctx.config.static_resources.request_storage = s_ctx.slots;
    s_ctx.config.static_resources.request_storage_count = 4u;
}

void tearDown(void) {}

void test_dma_core_init_validates_resources_and_rejects_prepared_controller(void) {
    esy_dma_scheduler_core_config_t config = s_ctx.config;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_dma_scheduler_core_init(NULL, &s_ctx.dma, &config));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_dma_scheduler_core_init(&s_ctx.core, NULL, &config));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_dma_scheduler_core_init(&s_ctx.core, &s_ctx.dma, NULL));
    config.static_resources.request_storage = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_dma_scheduler_core_init(&s_ctx.core, &s_ctx.dma, &config));
    config = s_ctx.config;
    const uint32_t invalid_counts[] = {0u, 1u, 3u, UINT32_C(1) << 31};
    for (unsigned i = 0u; i < 4u; ++i) {
        config.static_resources.request_storage_count = invalid_counts[i];
        TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
            esy_dma_scheduler_core_init(&s_ctx.core, &s_ctx.dma, &config));
    }
    config = s_ctx.config;
    config.static_resources.request_storage = (esy_dma_scheduler_request_t *)
        ((uint8_t *)s_ctx.slots + 1u);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_dma_scheduler_core_init(&s_ctx.core, &s_ctx.dma, &config));
    esy_dma_t uninitialized = {0};
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY,
        esy_dma_scheduler_core_init(&s_ctx.core, &uninitialized, &s_ctx.config));
    request_probe_t probe = {0};
    esy_dma_scheduler_request_t request = make_request(&probe, 0u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_prepare(&s_ctx.dma, &request.transfer));
    TEST_ASSERT_FALSE(esy_hal_dma_is_busy(&s_ctx.dma, 10u, 5u));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY,
        esy_dma_scheduler_core_init(&s_ctx.core, &s_ctx.dma, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_abort(&s_ctx.dma, 10u, 5u));
    init_core();
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_dma_scheduler_core_init(&s_ctx.core, &s_ctx.dma, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_deinit(&s_ctx.core));
    TEST_ASSERT_TRUE(esy_hal_dma_is_idle(&s_ctx.dma));
    init_core();
}

void test_dma_core_inactive_and_null_api_contracts(void) {
    request_probe_t probe = {0};
    esy_dma_scheduler_request_t request = make_request(&probe, 0u);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_dma_scheduler_core_submit(NULL, &request));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_dma_scheduler_core_submit(&s_ctx.core, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_dma_scheduler_core_submit(&s_ctx.core, &request));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_dma_scheduler_core_process(NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, process());
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_dma_scheduler_core_request_stop(NULL, true));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_dma_scheduler_core_request_stop(&s_ctx.core, false));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_dma_scheduler_core_request_abort(NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_dma_scheduler_core_request_abort(&s_ctx.core));
    TEST_ASSERT_FALSE(esy_dma_scheduler_core_is_idle(NULL));
    TEST_ASSERT_FALSE(esy_dma_scheduler_core_is_idle(&s_ctx.core));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, esy_dma_scheduler_core_next_wait_ms(NULL));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX,
        esy_dma_scheduler_core_next_wait_ms(&s_ctx.core));
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_deinit(NULL));
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_deinit(&s_ctx.core));
}

void test_dma_core_fifo_uses_distinct_routes_and_copies_descriptors(void) {
    init_core();
    request_probe_t probes[3] = {{.id = 1u}, {.id = 2u}, {.id = 3u}};
    for (unsigned i = 0u; i < 3u; ++i) {
        esy_dma_scheduler_request_t request = make_request(&probes[i], i % 2u);
        TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_submit(&s_ctx.core, &request));
        request.transfer.length = 0u;
    }
    esy_dma_scheduler_request_t extra = make_request(&probes[0], 0u);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_dma_scheduler_core_submit(&s_ctx.core, &extra));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_dma_scheduler_core_deinit(&s_ctx.core));
    for (unsigned i = 0u; i < 3u; ++i) {
        TEST_ASSERT_EQUAL_UINT32(0u, esy_dma_scheduler_core_next_wait_ms(&s_ctx.core));
        TEST_ASSERT_EQUAL(ESY_OK, process());
        size_t remaining = 0u;
        const esy_dma_inner_config_t route = k_routes[i % 2u];
        TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_get_trans_count_remaining(
            &s_ctx.dma, route.dreq, route.channel, &remaining));
        TEST_ASSERT_EQUAL_UINT32(4u, remaining);
        esy_hal_dma_handle_irq(route.dreq, route.channel);
        TEST_ASSERT_EQUAL_UINT32(0u, probes[i].terminal);
        TEST_ASSERT_EQUAL(ESY_OK, process());
        TEST_ASSERT_EQUAL_UINT32(1u, probes[i].complete);
        TEST_ASSERT_EQUAL_UINT32(1u, probes[i].terminal);
        TEST_ASSERT_EQUAL_UINT32(i + 1u, s_ctx.order[i]);
    }
    TEST_ASSERT_TRUE(esy_dma_scheduler_core_is_idle(&s_ctx.core));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_deinit(&s_ctx.core));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_deinit(&s_ctx.dma));
}

void test_dma_core_native_irq_events_are_delivered_by_the_scheduler_owner(void) {
    request_probe_t probe = {0};
    init_core();
    esy_dma_scheduler_request_t request = make_request(&probe, 0u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_submit(&s_ctx.core, &request));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL_UINT32(1u, esy_dma_scheduler_core_next_wait_ms(&s_ctx.core));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL_UINT32(0u, probe.terminal + probe.half);
    TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_dma_signal_half(&s_ctx.dma, 10u, 5u));
    TEST_ASSERT_EQUAL_UINT32(0u, probe.half);
    TEST_ASSERT_EQUAL_UINT32(1u, esy_dma_scheduler_core_next_wait_ms(&s_ctx.core));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL_UINT32(1u, probe.half);
    TEST_ASSERT_EQUAL_UINT32(0u, probe.terminal);
    TEST_ASSERT_EQUAL_UINT32(1u, esy_dma_scheduler_core_next_wait_ms(&s_ctx.core));
    esy_hal_dma_handle_irq(10u, 5u);
    TEST_ASSERT_EQUAL_UINT32(0u, probe.terminal);
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL_UINT32(1u, probe.terminal);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, esy_dma_scheduler_core_next_wait_ms(&s_ctx.core));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL_UINT32(1u, probe.terminal);
}

void test_dma_core_published_terminal_waits_for_hal_callback_borrow_to_retire(void) {
    request_probe_t probe = {0};
    init_core();
    esy_dma_scheduler_request_t request = make_request(&probe, 0u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_submit(&s_ctx.core, &request));
    TEST_ASSERT_EQUAL(ESY_OK, process());

    /* A peer owner can observe publication before the HAL IRQ wrapper returns.
     * Inject that publication while the fake HAL still owns its native borrow. */
    esy_dma_channel_runtime_t *runtime = &s_ctx.dma.runtime[0];
    TEST_ASSERT_NOT_NULL(runtime->complete_cb);
    runtime->complete_cb(runtime->complete_arg);
    TEST_ASSERT_FALSE(esy_hal_dma_is_idle(&s_ctx.dma));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL_UINT32(0u, probe.complete + probe.terminal);
    TEST_ASSERT_FALSE(esy_dma_scheduler_core_is_idle(&s_ctx.core));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_dma_scheduler_core_deinit(&s_ctx.core));

    esy_hal_dma_handle_irq(10u, 5u);
    TEST_ASSERT_TRUE(esy_hal_dma_is_idle(&s_ctx.dma));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL_UINT32(1u, probe.complete);
    TEST_ASSERT_EQUAL_UINT32(1u, probe.terminal);
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL_UINT32(1u, probe.complete);
    TEST_ASSERT_EQUAL_UINT32(1u, probe.terminal);
    TEST_ASSERT_TRUE(esy_dma_scheduler_core_is_idle(&s_ctx.core));
}

void test_dma_core_prepare_error_is_terminal_without_original_dma_error_callback(void) {
    request_probe_t probe = {0};
    init_core();
    esy_dma_scheduler_request_t request = make_request(&probe, 0u);
    request.transfer.length = 0u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_dma_scheduler_core_submit(&s_ctx.core, &request));
    request = make_request(&probe, 0u);
    request.transfer.inner.channel = 31u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_dma_scheduler_core_submit(&s_ctx.core, &request));
    request = make_request(&probe, 0u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_submit(&s_ctx.core, &request));
    fake_esy_hal_dma_set_prepare_result(ESY_ERR_IO);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, process());
    TEST_ASSERT_EQUAL_UINT32(1u, probe.terminal);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, probe.result);
    TEST_ASSERT_EQUAL_UINT32(0u, probe.errors + probe.complete + probe.cancelled);
    TEST_ASSERT_TRUE(esy_dma_scheduler_core_is_idle(&s_ctx.core));
}

void test_dma_core_failed_start_retains_prepared_route_and_its_error_through_abort_upgrade(void) {
    request_probe_t probe = {0};
    init_core();
    esy_dma_scheduler_request_t request = make_request(&probe, 0u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_submit(&s_ctx.core, &request));
    fake_esy_hal_dma_set_start_result(ESY_ERR_IO);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, process());
    TEST_ASSERT_FALSE(esy_hal_dma_is_busy(&s_ctx.dma, 10u, 5u));
    TEST_ASSERT_FALSE(esy_hal_dma_is_idle(&s_ctx.dma));
    TEST_ASSERT_FALSE(esy_dma_scheduler_core_is_idle(&s_ctx.core));
    TEST_ASSERT_EQUAL_UINT32(1u, esy_dma_scheduler_core_next_wait_ms(&s_ctx.core));
    fake_esy_hal_dma_set_abort_result(ESY_ERR_TIMEOUT);
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, process());
    TEST_ASSERT_EQUAL_UINT32(0u, probe.terminal);
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_request_abort(&s_ctx.core));
    fake_esy_hal_dma_set_abort_result(ESY_OK);
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL_UINT32(1u, probe.terminal);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, probe.result);
    TEST_ASSERT_EQUAL_UINT32(0u, probe.cancelled + probe.errors);
    TEST_ASSERT_TRUE(esy_dma_scheduler_core_is_idle(&s_ctx.core));
}

void test_dma_core_abort_cancels_active_and_queue_once_with_retry_and_no_downgrade(void) {
    request_probe_t probes[3] = {0};
    init_core();
    for (unsigned i = 0u; i < 3u; ++i) {
        esy_dma_scheduler_request_t request = make_request(&probes[i], 0u);
        TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_submit(&s_ctx.core, &request));
    }
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_request_abort(&s_ctx.core));
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_request_stop(&s_ctx.core, false));
    esy_dma_scheduler_request_t request = make_request(&probes[0], 0u);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_dma_scheduler_core_submit(&s_ctx.core, &request));
    fake_esy_hal_dma_set_abort_result(ESY_ERR_TIMEOUT);
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, process());
    TEST_ASSERT_EQUAL_UINT32(1u, esy_dma_scheduler_core_next_wait_ms(&s_ctx.core));
    TEST_ASSERT_EQUAL_UINT32(0u, probes[0].terminal);
    fake_esy_hal_dma_set_abort_result(ESY_OK);
    for (unsigned i = 0u; i < 3u; ++i) {
        TEST_ASSERT_EQUAL(ESY_OK, process());
        TEST_ASSERT_EQUAL_UINT32(1u, probes[i].terminal);
        TEST_ASSERT_EQUAL_UINT32(1u, probes[i].cancelled);
        TEST_ASSERT_EQUAL_UINT32(0u, probes[i].complete + probes[i].errors);
    }
    TEST_ASSERT_TRUE(esy_dma_scheduler_core_is_idle(&s_ctx.core));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.order_count);
}

void test_dma_core_stop_drains_finite_and_cancels_only_queued_requests(void) {
    request_probe_t active = {0}, queued = {0};
    init_core();
    esy_dma_scheduler_request_t request = make_request(&active, 0u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_submit(&s_ctx.core, &request));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    request = make_request(&queued, 1u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_submit(&s_ctx.core, &request));
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_request_stop(&s_ctx.core, true));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL_UINT32(0u, active.terminal + queued.terminal);
    esy_hal_dma_handle_irq(10u, 5u);
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL_UINT32(1u, active.complete);
    TEST_ASSERT_EQUAL_UINT32(0u, active.cancelled);
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL_UINT32(1u, queued.cancelled);
    TEST_ASSERT_TRUE(esy_dma_scheduler_core_is_idle(&s_ctx.core));
}

void test_dma_core_stop_without_cancellation_drains_the_queued_fifo(void) {
    request_probe_t probe = {0};
    init_core();
    esy_dma_scheduler_request_t request = make_request(&probe, 0u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_submit(&s_ctx.core, &request));
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_request_stop(&s_ctx.core, false));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    esy_hal_dma_handle_irq(10u, 5u);
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL_UINT32(1u, probe.complete);
    TEST_ASSERT_EQUAL_UINT32(0u, probe.cancelled);
}

void test_dma_core_circular_half_full_are_nonterminal_until_explicit_abort(void) {
    request_probe_t probe = {0};
    init_core();
    esy_dma_scheduler_request_t request = make_request(&probe, 0u);
    request.transfer.circular = true;
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_submit(&s_ctx.core, &request));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL_UINT32(1u, esy_dma_scheduler_core_next_wait_ms(&s_ctx.core));
    TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_dma_signal_half(&s_ctx.dma, 10u, 5u));
    esy_hal_dma_handle_irq(10u, 5u);
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL_UINT32(1u, probe.half);
    TEST_ASSERT_EQUAL_UINT32(1u, probe.complete);
    TEST_ASSERT_EQUAL_UINT32(0u, probe.terminal);
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_request_stop(&s_ctx.core, false));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_FALSE(esy_dma_scheduler_core_is_idle(&s_ctx.core));
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_request_abort(&s_ctx.core));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL_UINT32(1u, probe.cancelled);
}

void test_dma_core_error_irq_suppresses_pending_circular_success_callbacks(void) {
    request_probe_t probe = {0};
    init_core();
    esy_dma_scheduler_request_t request = make_request(&probe, 0u);
    request.transfer.circular = true;
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_submit(&s_ctx.core, &request));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    esy_hal_dma_handle_irq(10u, 5u);
    TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_dma_signal_error(&s_ctx.dma, 10u, 5u));
    TEST_ASSERT_EQUAL_UINT32(0u, probe.terminal + probe.errors + probe.complete);
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL_UINT32(1u, probe.errors);
    TEST_ASSERT_EQUAL_UINT32(1u, probe.terminal);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, probe.result);
    TEST_ASSERT_EQUAL_UINT32(0u, probe.complete + probe.cancelled);
}

void test_dma_core_failed_native_stop_keeps_storage_until_explicit_abort(void) {
    request_probe_t probe = {0};
    init_core();
    esy_dma_scheduler_request_t request = make_request(&probe, 0u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_submit(&s_ctx.core, &request));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    fake_esy_hal_dma_set_abort_result(ESY_ERR_TIMEOUT);
    TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_dma_signal_error(&s_ctx.dma, 10u, 5u));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL_UINT32(0u, probe.terminal + probe.errors);
    TEST_ASSERT_FALSE(esy_dma_scheduler_core_is_idle(&s_ctx.core));
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_request_abort(&s_ctx.core));
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, process());
    fake_esy_hal_dma_set_abort_result(ESY_OK);
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL_UINT32(1u, probe.cancelled);
    TEST_ASSERT_EQUAL_UINT32(1u, probe.terminal);
    TEST_ASSERT_EQUAL_UINT32(0u, probe.errors + probe.complete);
}

void test_dma_core_callbacks_reject_recursive_processing_and_resource_release(void) {
    request_probe_t probe = {.check_reentry = true};
    init_core();
    esy_dma_scheduler_request_t request = make_request(&probe, 0u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_submit(&s_ctx.core, &request));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    esy_hal_dma_handle_irq(10u, 5u);
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL_UINT32(1u, probe.complete);
    TEST_ASSERT_EQUAL_UINT32(1u, probe.terminal);
}

void test_dma_core_optional_original_and_terminal_callbacks_allow_idle_observation(void) {
    init_core();
    esy_dma_scheduler_request_t request = {.transfer = {
        .src = s_ctx.samples, .dst = s_ctx.samples, .length = sizeof(s_ctx.samples),
        .transfer_size = ESY_DMA_TRANSFER_SIZE_16, .inner = k_routes[0]}};
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_submit(&s_ctx.core, &request));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_dma_signal_half(&s_ctx.dma, 10u, 5u));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    esy_hal_dma_handle_irq(10u, 5u);
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_TRUE(esy_dma_scheduler_core_is_idle(&s_ctx.core));
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_submit(&s_ctx.core, &request));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_dma_signal_error(&s_ctx.dma, 10u, 5u));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_TRUE(esy_dma_scheduler_core_is_idle(&s_ctx.core));
}

void test_dma_core_without_half_irq_support_preserves_terminal_only_requests(void) {
    request_probe_t probe = {0};
    init_core();
    fake_esy_hal_dma_set_half_supported(false);
    esy_dma_scheduler_request_t request = make_request(&probe, 0u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_submit(&s_ctx.core, &request));
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, process());
    TEST_ASSERT_EQUAL_UINT32(1u, probe.terminal);
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, probe.result);
    TEST_ASSERT_EQUAL_UINT32(0u, probe.half + probe.complete);
    TEST_ASSERT_TRUE(esy_dma_scheduler_core_is_idle(&s_ctx.core));

    request.transfer.half_callback = NULL;
    TEST_ASSERT_EQUAL(ESY_OK, esy_dma_scheduler_core_submit(&s_ctx.core, &request));
    TEST_ASSERT_EQUAL(ESY_OK, process());
    esy_hal_dma_handle_irq(10u, 5u);
    TEST_ASSERT_EQUAL(ESY_OK, process());
    TEST_ASSERT_EQUAL_UINT32(2u, probe.terminal);
    TEST_ASSERT_EQUAL_UINT32(1u, probe.complete);
    TEST_ASSERT_EQUAL_UINT32(0u, probe.half);
    TEST_ASSERT_EQUAL(ESY_OK, probe.result);
    TEST_ASSERT_TRUE(esy_dma_scheduler_core_is_idle(&s_ctx.core));
}
