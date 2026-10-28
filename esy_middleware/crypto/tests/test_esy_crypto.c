/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"

#include <string.h>

#include "esy_middleware/crypto/esy_crypto_mbedtls.h"

#define TEST_OUTPUT_BYTES 32u

typedef struct {
    uint8_t next;
    size_t calls;
    size_t last_length;
    esy_err_t error;
} entropy_source_t;

typedef struct {
    esy_crypto_random_t random;
    entropy_source_t entropy;
} crypto_test_ctx_t;

static crypto_test_ctx_t s_ctx;

static esy_err_t test_entropy(void *context, uint8_t *output, size_t length) {
    entropy_source_t *source = context;
    ++source->calls;
    source->last_length = length;
    if (source->error != ESY_OK) return source->error;
    for (size_t i = 0u; i < length; ++i) output[i] = source->next++;
    return ESY_OK;
}

static esy_crypto_random_config_t test_config(void) {
    esy_crypto_random_config_t config = {
        .entropy = {.fill = test_entropy, .context = &s_ctx.entropy},
    };
    return config;
}

void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
}

void tearDown(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_crypto_random_deinit(&s_ctx.random));
}

void test_crypto_rejects_invalid_initialization_arguments(void) {
    esy_crypto_random_config_t config = test_config();
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_crypto_random_init(NULL, &config));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                      esy_crypto_random_init(&s_ctx.random, NULL));
    config.entropy.fill = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                      esy_crypto_random_init(&s_ctx.random, &config));
    config = test_config();
    config.personalization_length = 1u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                      esy_crypto_random_init(&s_ctx.random, &config));
    config.personalization = (const uint8_t *)"x";
    config.personalization_length =
        ESY_CRYPTO_RANDOM_MAX_PERSONALIZATION_BYTES + 1u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                      esy_crypto_random_init(&s_ctx.random, &config));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.entropy.calls);
}

void test_crypto_requires_initialization_before_generation(void) {
    uint8_t output = 0xabu;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
                      esy_crypto_random_get(&s_ctx.random, &output, 1u));
    TEST_ASSERT_EQUAL_HEX8(0xabu, output);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
                      esy_crypto_random_get(&s_ctx.random, NULL, 0u));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                      esy_crypto_random_get(NULL, &output, 1u));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_crypto_random_deinit(NULL));
}

void test_crypto_seed_requests_explicit_entropy_and_rejects_repeated_init(void) {
    esy_crypto_random_config_t config = test_config();
    TEST_ASSERT_EQUAL(ESY_OK, esy_crypto_random_init(&s_ctx.random, &config));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.entropy.calls);
    TEST_ASSERT_EQUAL_UINT(ESY_CRYPTO_RANDOM_ENTROPY_BYTES,
                           s_ctx.entropy.last_length);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
                      esy_crypto_random_init(&s_ctx.random, &config));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.entropy.calls);
}

void test_crypto_matches_provider_published_aes256_known_answer(void) {
    /* CTR_DRBG_withDF.pdf vector reproduced in Mbed TLS's upstream
     * test_suite_ctr_drbg.data: AES-256, PR=no, perso=no, add=no. */
    static const uint8_t k_expected[TEST_OUTPUT_BYTES] = {
        0x8d, 0xa6, 0xcc, 0x59, 0xe7, 0x03, 0xce, 0xd0,
        0x7d, 0x58, 0xd9, 0x6e, 0x5b, 0x6d, 0x78, 0x36,
        0xc3, 0x25, 0x99, 0x73, 0x5b, 0x73, 0x4f, 0x88,
        0xc1, 0xa7, 0x3b, 0x53, 0xc7, 0xa6, 0xd8, 0x2e,
    };
    uint8_t nonce[16];
    uint8_t output[TEST_OUTPUT_BYTES];
    for (size_t i = 0u; i < sizeof(nonce); ++i)
        nonce[i] = (uint8_t)(0x20u + i);
    esy_crypto_random_config_t config = test_config();
    config.personalization = nonce;
    config.personalization_length = sizeof(nonce);
    TEST_ASSERT_EQUAL(ESY_OK, esy_crypto_random_init(&s_ctx.random, &config));
    TEST_ASSERT_EQUAL(ESY_OK,
                      esy_crypto_random_get(&s_ctx.random, output,
                                            sizeof(output)));
    TEST_ASSERT_EQUAL(ESY_OK,
                      esy_crypto_random_fill(&s_ctx.random, output,
                                             sizeof(output)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_expected, output, sizeof(output));
}

void test_crypto_accepts_zero_and_maximum_requests_and_preserves_on_bad_args(void) {
    esy_crypto_random_config_t config = test_config();
    uint8_t output[ESY_CRYPTO_RANDOM_MAX_REQUEST_BYTES];
    TEST_ASSERT_EQUAL(ESY_OK, esy_crypto_random_init(&s_ctx.random, &config));
    TEST_ASSERT_EQUAL(ESY_OK,
                      esy_crypto_random_get(&s_ctx.random, NULL, 0u));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                      esy_crypto_random_get(&s_ctx.random, NULL, 1u));
    memset(output, 0xa5, sizeof(output));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                      esy_crypto_random_get(&s_ctx.random, output,
                          ESY_CRYPTO_RANDOM_MAX_REQUEST_BYTES + 1u));
    TEST_ASSERT_EACH_EQUAL_HEX8(0xa5u, output, sizeof(output));
    TEST_ASSERT_EQUAL(ESY_OK,
                      esy_crypto_random_get(&s_ctx.random, output,
                                            sizeof(output)));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.entropy.calls);
}

void test_crypto_failed_seed_returns_source_error_and_can_retry(void) {
    esy_crypto_random_config_t config = test_config();
    s_ctx.entropy.error = ESY_ERR_NOT_SUPPORTED;
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED,
                      esy_crypto_random_init(&s_ctx.random, &config));
    TEST_ASSERT_EACH_EQUAL_HEX8(0u, (const uint8_t *)&s_ctx.random,
                               sizeof(s_ctx.random));
    s_ctx.entropy.error = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_crypto_random_init(&s_ctx.random, &config));
}

void test_crypto_invalid_positive_entropy_status_fails_closed(void) {
    esy_crypto_random_config_t config = test_config();
    s_ctx.entropy.error = 1;
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL,
                      esy_crypto_random_init(&s_ctx.random, &config));
}

void test_crypto_reseed_failure_clears_output_and_requires_explicit_restart(void) {
    esy_crypto_random_config_t config = test_config();
    uint8_t output[TEST_OUTPUT_BYTES];
    TEST_ASSERT_EQUAL(ESY_OK, esy_crypto_random_init(&s_ctx.random, &config));
    s_ctx.entropy.error = ESY_ERR_IO;
    esy_err_t result = ESY_OK;
    for (size_t i = 0u; i <= MBEDTLS_CTR_DRBG_RESEED_INTERVAL; ++i) {
        memset(output, 0xa5, sizeof(output));
        result = esy_crypto_random_get(&s_ctx.random, output, sizeof(output));
        if (result != ESY_OK) break;
    }
    TEST_ASSERT_EQUAL(ESY_ERR_IO, result);
    TEST_ASSERT_EACH_EQUAL_HEX8(0u, output, sizeof(output));
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.entropy.calls);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
                      esy_crypto_random_get(&s_ctx.random, output,
                                            sizeof(output)));
    s_ctx.entropy.error = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_crypto_random_init(&s_ctx.random, &config));
}

void test_crypto_reseed_success_and_cleanup_allow_fresh_lifecycle(void) {
    esy_crypto_random_config_t config = test_config();
    uint8_t output[TEST_OUTPUT_BYTES];
    TEST_ASSERT_EQUAL(ESY_OK, esy_crypto_random_init(&s_ctx.random, &config));
    for (size_t i = 0u; i <= MBEDTLS_CTR_DRBG_RESEED_INTERVAL; ++i)
        TEST_ASSERT_EQUAL(ESY_OK,
                          esy_crypto_random_fill(&s_ctx.random, output,
                                                 sizeof(output)));
    TEST_ASSERT_TRUE(s_ctx.entropy.calls > 1u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_crypto_random_deinit(&s_ctx.random));
    TEST_ASSERT_EQUAL(ESY_OK, esy_crypto_random_deinit(&s_ctx.random));
    TEST_ASSERT_EQUAL(ESY_OK, esy_crypto_random_init(&s_ctx.random, &config));
}

void test_crypto_personalization_is_borrowed_only_during_init(void) {
    esy_crypto_random_config_t config = test_config();
    uint8_t personalization[ESY_CRYPTO_RANDOM_MAX_PERSONALIZATION_BYTES];
    uint8_t output[TEST_OUTPUT_BYTES];
    memset(personalization, 0x17, sizeof(personalization));
    config.personalization = personalization;
    config.personalization_length = sizeof(personalization);
    TEST_ASSERT_EQUAL(ESY_OK, esy_crypto_random_init(&s_ctx.random, &config));
    memset(personalization, 0, sizeof(personalization));
    TEST_ASSERT_EQUAL(ESY_OK,
                      esy_crypto_random_get(&s_ctx.random, output,
                                            sizeof(output)));
}
