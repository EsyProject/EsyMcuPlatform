/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/freq_synth/esy_freq_synth.h"

#include <string.h>

#include "unity.h"

typedef struct {
    uint64_t last_freq_hz;
} fake_synth_ctx_t;

static esy_err_t fake_set_frequency_hz(void *ctx, uint64_t freq_hz)
{
    fake_synth_ctx_t *fake = (fake_synth_ctx_t *)ctx;
    if (fake == NULL) return ESY_ERR_INVALID_ARG;

    fake->last_freq_hz = freq_hz;
    return ESY_OK;
}

static const esy_freq_synth_vtable_t k_fake_ops = {
    .set_frequency_hz = fake_set_frequency_hz,
    .set_output_enable = NULL,
    .set_power_level = NULL,
    .get_info = NULL,
};

static fake_synth_ctx_t *attach_minimal_fake(esy_freq_synth_t *synth)
{
    fake_synth_ctx_t *fake = (fake_synth_ctx_t *)synth->backend.bytes;

    memset(synth, 0, sizeof(*synth));
    memset(fake, 0, sizeof(*fake));
    synth->ctx = fake;
    synth->ops = &k_fake_ops;
    return fake;
}

void setUp(void) {}

void tearDown(void) {}

void test_freq_synth_null_handle_returns_invalid_arg(void)
{
    esy_freq_synth_info_value_t info;

    TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                            esy_freq_synth_set_frequency_hz(NULL, 1u));
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                            esy_freq_synth_set_output_enable(NULL,
                                                             ESY_FREQ_SYNTH_PORT_MAIN,
                                                             true));
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                            esy_freq_synth_set_power_level(NULL,
                                                           ESY_FREQ_SYNTH_PORT_MAIN,
                                                           1u));
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                            esy_freq_synth_get_info(NULL,
                                                    ESY_FREQ_SYNTH_INFO_ACTUAL_HZ,
                                                    &info));
}

void test_freq_synth_unattached_handle_returns_state_error(void)
{
    esy_freq_synth_t synth = {0};
    esy_freq_synth_info_value_t info;

    memset(&synth, 0, sizeof(synth));

    TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                            esy_freq_synth_set_frequency_hz(&synth, 1u));
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                            esy_freq_synth_set_output_enable(&synth,
                                                             ESY_FREQ_SYNTH_PORT_MAIN,
                                                             true));
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                            esy_freq_synth_set_power_level(&synth,
                                                           ESY_FREQ_SYNTH_PORT_MAIN,
                                                           1u));
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                            esy_freq_synth_get_info(&synth,
                                                    ESY_FREQ_SYNTH_INFO_ACTUAL_HZ,
                                                    &info));
}

void test_freq_synth_detach_moves_handle_to_unattached_state(void)
{
    esy_freq_synth_t synth = {0};

    esy_freq_synth_detach(NULL);
    TEST_ASSERT_FALSE(esy_freq_synth_is_attached(NULL));

    (void)attach_minimal_fake(&synth);

    TEST_ASSERT_TRUE(esy_freq_synth_is_attached(&synth));
    esy_freq_synth_detach(&synth);
    TEST_ASSERT_FALSE(esy_freq_synth_is_attached(&synth));
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                            esy_freq_synth_set_frequency_hz(&synth, 100u));
}

void test_freq_synth_missing_optional_ops_return_not_supported(void)
{
    esy_freq_synth_t synth = {0};
    fake_synth_ctx_t *fake = attach_minimal_fake(&synth);
    esy_freq_synth_info_value_t info;

    TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_freq_synth_set_frequency_hz(&synth, 123u));
    TEST_ASSERT_EQUAL_UINT64(123u, fake->last_freq_hz);
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                            esy_freq_synth_set_output_enable(&synth,
                                                             ESY_FREQ_SYNTH_PORT_MAIN,
                                                             true));
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                            esy_freq_synth_set_power_level(&synth,
                                                           ESY_FREQ_SYNTH_PORT_MAIN,
                                                           1u));
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                            esy_freq_synth_get_info(&synth,
                                                    ESY_FREQ_SYNTH_INFO_ACTUAL_HZ,
                                                    &info));
}

void test_freq_synth_attached_handle_rejects_invalid_operation_arguments(void)
{
    esy_freq_synth_t synth = {0};

    (void)attach_minimal_fake(&synth);

    TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                            esy_freq_synth_get_info(&synth,
                                                    ESY_FREQ_SYNTH_INFO_ACTUAL_HZ,
                                                    NULL));
}

void test_freq_synth_shared_const_ops_keep_independent_handles(void)
{
    esy_freq_synth_t first = {0};
    esy_freq_synth_t second = {0};
    fake_synth_ctx_t *first_ctx = attach_minimal_fake(&first);
    fake_synth_ctx_t *second_ctx = attach_minimal_fake(&second);
    TEST_ASSERT_EQUAL_PTR(first.ops, second.ops);
    TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_freq_synth_set_frequency_hz(&first, 123u));
    TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_freq_synth_set_frequency_hz(&second, 456u));
    TEST_ASSERT_EQUAL_UINT64(123u, first_ctx->last_freq_hz);
    TEST_ASSERT_EQUAL_UINT64(456u, second_ctx->last_freq_hz);
    esy_freq_synth_detach(&first);
    TEST_ASSERT_NULL(first.ops);
    TEST_ASSERT_TRUE(esy_freq_synth_is_attached(&second));
    TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_freq_synth_set_frequency_hz(&second, 789u));
    TEST_ASSERT_EQUAL_UINT64(789u, second_ctx->last_freq_hz);
    esy_freq_synth_detach(&second);
}

void test_freq_synth_context_without_ops_is_not_attached(void)
{
    esy_freq_synth_t synth = {0};
    uint64_t context = 0u;
    synth.ctx = &context;
    TEST_ASSERT_FALSE(esy_freq_synth_is_attached(&synth));
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_freq_synth_set_frequency_hz(&synth, 1u));
    esy_freq_synth_detach(&synth);
}
