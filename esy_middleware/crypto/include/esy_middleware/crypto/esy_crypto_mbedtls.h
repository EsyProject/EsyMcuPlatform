/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_CRYPTO_MBEDTLS_H
#define ESY_CRYPTO_MBEDTLS_H

#include <stdbool.h>

#include "esy_middleware/crypto/esy_crypto.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/version.h"

#if MBEDTLS_VERSION_NUMBER < 0x03060000 || MBEDTLS_VERSION_NUMBER >= 0x03070000
#error "Esy crypto requires Mbed TLS 3.6.x"
#endif
#if !defined(MBEDTLS_CTR_DRBG_C) || !defined(MBEDTLS_AES_C)
#error "Esy crypto requires Mbed TLS CTR_DRBG and AES"
#endif
#if defined(MBEDTLS_CTR_DRBG_USE_128_BIT_KEY) || \
    defined(MBEDTLS_AES_ONLY_128_BIT_KEY_LENGTH)
#error "Esy crypto requires the AES-256 CTR_DRBG configuration"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Caller-owned Mbed TLS 3.6.x DRBG state, initially {0}.
 *
 * This provider-specific header intentionally exposes the native context to
 * avoid a private storage-size ABI. The selected Mbed TLS configuration and
 * headers must match the linked esy_mbedtls_provider target. Members are private
 * implementation state; callers must not inspect, copy or edit a live object.
 * Source configuration and entropy quality remain with the consumer. The
 * adapter owns only the native DRBG resources, freed on failure or deinit.
 */
struct esy_crypto_random {
    mbedtls_ctr_drbg_context native; /**< Private native generator state. */
    esy_crypto_random_provider_t entropy; /**< Borrowed cryptographic source. */
    esy_err_t entropy_error; /**< Private callback error propagation state. */
    bool initialized; /**< Private successful-seed lifecycle state. */
};

#ifdef __cplusplus
}
#endif

#endif /* ESY_CRYPTO_MBEDTLS_H */
