/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/crypto/esy_crypto_mbedtls.h"

#include "mbedtls/platform_util.h"

_Static_assert(MBEDTLS_CTR_DRBG_MAX_REQUEST >=
                   ESY_CRYPTO_RANDOM_MAX_REQUEST_BYTES,
               "Mbed TLS random request budget is too small");
_Static_assert(MBEDTLS_CTR_DRBG_MAX_SEED_INPUT >=
                   ESY_CRYPTO_RANDOM_ENTROPY_BYTES +
                       ESY_CRYPTO_RANDOM_MAX_PERSONALIZATION_BYTES,
               "Mbed TLS personalization budget is too small");

static int crypto_entropy(void *context, unsigned char *output, size_t length) {
    esy_crypto_random_t *random = context;
    esy_err_t result = random->entropy.fill(random->entropy.context, output,
                                          length);
    if (result == ESY_OK) return 0;
    random->entropy_error = result < 0 ? result : ESY_ERR_FAIL;
    /* Native entropy failures may return before wiping their seed buffer. */
    mbedtls_platform_zeroize(output, length);
    return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
}

esy_err_t esy_crypto_random_init(esy_crypto_random_t *random,
                               const esy_crypto_random_config_t *config) {
    if (random == NULL || config == NULL || config->entropy.fill == NULL ||
        (config->personalization == NULL &&
         config->personalization_length != 0u) ||
        config->personalization_length >
            ESY_CRYPTO_RANDOM_MAX_PERSONALIZATION_BYTES)
        return ESY_ERR_INVALID_ARG;
    if (random->initialized) return ESY_ERR_STATE;

    random->entropy = config->entropy;
    random->entropy_error = ESY_OK;
    mbedtls_ctr_drbg_init(&random->native);
    mbedtls_ctr_drbg_set_entropy_len(&random->native,
                                    ESY_CRYPTO_RANDOM_ENTROPY_BYTES);
    (void)mbedtls_ctr_drbg_set_nonce_len(&random->native, 0u);
    int result = mbedtls_ctr_drbg_seed(&random->native, crypto_entropy, random,
                                     config->personalization,
                                     config->personalization_length);
    if (result != 0) {
        esy_err_t error = random->entropy_error != ESY_OK
                              ? random->entropy_error
                              : ESY_ERR_FAIL;
        mbedtls_ctr_drbg_free(&random->native);
        mbedtls_platform_zeroize(random, sizeof(*random));
        return error;
    }
    random->initialized = true;
    return ESY_OK;
}

esy_err_t esy_crypto_random_get(esy_crypto_random_t *random, uint8_t *output,
                              size_t length) {
    if (random == NULL || (output == NULL && length != 0u) ||
        length > ESY_CRYPTO_RANDOM_MAX_REQUEST_BYTES)
        return ESY_ERR_INVALID_ARG;
    if (!random->initialized) return ESY_ERR_STATE;
    if (length == 0u) return ESY_OK;

    random->entropy_error = ESY_OK;
    if (mbedtls_ctr_drbg_random(&random->native, output, length) != 0) {
        esy_err_t error = random->entropy_error != ESY_OK
                              ? random->entropy_error
                              : ESY_ERR_FAIL;
        mbedtls_platform_zeroize(output, length);
        (void)esy_crypto_random_deinit(random);
        return error;
    }
    return ESY_OK;
}

esy_err_t esy_crypto_random_fill(void *context, uint8_t *output, size_t length) {
    return esy_crypto_random_get(context, output, length);
}

esy_err_t esy_crypto_random_deinit(esy_crypto_random_t *random) {
    if (random == NULL) return ESY_ERR_INVALID_ARG;
    if (random->initialized) mbedtls_ctr_drbg_free(&random->native);
    mbedtls_platform_zeroize(random, sizeof(*random));
    return ESY_OK;
}
