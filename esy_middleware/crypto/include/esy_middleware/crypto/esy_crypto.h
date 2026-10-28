/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_CRYPTO_H
#define ESY_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum output bytes in one random generation call. */
#define ESY_CRYPTO_RANDOM_MAX_REQUEST_BYTES 1024u
/** @brief Maximum optional personalization bytes supplied at initialization. */
#define ESY_CRYPTO_RANDOM_MAX_PERSONALIZATION_BYTES 128u
/** @brief Cryptographic entropy bytes requested at each seed or reseed. */
#define ESY_CRYPTO_RANDOM_ENTROPY_BYTES 48u

/**
 * @brief Synchronous random-byte dependency borrowed by another module.
 *
 * A successful fill writes every requested byte. Output on error is invalid;
 * consumers must fail closed. The provider and context remain alive until every
 * consumer has stopped. One owner serializes calls with provider lifecycle and
 * source reconfiguration; callbacks must not reenter their consuming operation.
 * This contract is for normal startup/task context, never ISR or fault context.
 * A deterministic test provider must never be used as production entropy.
 */
typedef struct {
    esy_err_t (*fill)(void *context, uint8_t *output, size_t length);
    /**< Fill the entire nonempty output or return a negative error. */
    void *context; /**< Borrowed provider context; NULL is provider-defined. */
} esy_crypto_random_provider_t;

/**
 * @brief Caller-owned random generator; include the selected adapter header.
 *
 * The Mbed TLS adapter completes this type in esy_crypto_mbedtls.h. First storage
 * must be initialized with {0}. Do not copy, move or edit a live generator. One
 * externally serialized owner controls initialization, generation and cleanup.
 */
typedef struct esy_crypto_random esy_crypto_random_t;

/** @brief Inputs copied by random initialization; no product entropy is chosen. */
typedef struct {
    esy_crypto_random_provider_t entropy;
    /**< Explicit cryptographic entropy source, retained through deinit. Each
     * successful request must provide the full requested amount of independent
     * cryptographic entropy, including initial seeding and later reseeds. The
     * adapter cannot measure entropy quality. Generic HAL SDK random bytes do
     * not by themselves satisfy this contract. */
    const uint8_t *personalization;
    /**< Optional nonsecret diversification bytes, borrowed only during init. */
    size_t personalization_length;
    /**< At most ESY_CRYPTO_RANDOM_MAX_PERSONALIZATION_BYTES; NULL requires zero. */
} esy_crypto_random_config_t;

/**
 * @brief Seed a caller-owned generator from explicitly supplied entropy.
 *
 * Normal startup/task context only. Calls the synchronous entropy source, which
 * may block according to its own contract; the consumer budgets that latency.
 * Adapter/provider initialization may create native resources depending on the
 * selected provider build. No module-owned task or hardware source is created.
 * The owner keeps entropy.context valid until deinit and serializes all calls.
 * Seeding and reseeding each request ESY_CRYPTO_RANDOM_ENTROPY_BYTES bytes.
 * Reentrant access, concurrent calls and ISR use are outside the contract.
 * On seed failure all native resources are freed and the handle returns to its
 * zero state, allowing retry. An already initialized handle remains unchanged.
 *
 * @param[in,out] random Zero-initialized or successfully deinitialized handle.
 * @param[in] config Required entropy callback and optional personalization.
 * @return OK, INVALID_ARG, STATE when already initialized, an entropy callback
 * error, or FAIL for another provider failure. Never downgrade to weak entropy.
 */
esy_err_t esy_crypto_random_init(esy_crypto_random_t *random,
                               const esy_crypto_random_config_t *config);

/**
 * @brief Generate cryptographically suitable bytes from the seeded DRBG.
 *
 * Suitability requires the configured entropy contract to hold. Normal
 * startup/task context only; synchronous generation may call entropy again for
 * automatic reseeding. At most ESY_CRYPTO_RANDOM_MAX_REQUEST_BYTES are accepted.
 * A zero-length request succeeds on a live handle and permits NULL output.
 * No output or caller storage is retained. On provider/entropy failure the
 * requested output is securely cleared and the generator is deinitialized;
 * further generation requires explicit successful init. Argument/state errors
 * leave output and the handle unchanged. Do not overlap output with the handle.
 *
 * @param[in,out] random Initialized generator, externally serialized.
 * @param[out] output Writable bytes, non-NULL for nonzero length.
 * @param[in] length Requested bytes within the public maximum.
 * @return OK, INVALID_ARG, STATE, an entropy callback error, or FAIL.
 */
esy_err_t esy_crypto_random_get(esy_crypto_random_t *random, uint8_t *output,
                              size_t length);

/**
 * @brief Callback-compatible bridge to esy_crypto_random_get().
 *
 * @param[in,out] context An esy_crypto_random_t pointer, with the same lifetime,
 * serialization and normal-context requirements as esy_crypto_random_get().
 * @param[out] output Writable bytes, with the same get contract.
 * @param[in] length Requested bytes within the public maximum.
 * @return The unchanged esy_crypto_random_get result.
 */
esy_err_t esy_crypto_random_fill(void *context, uint8_t *output, size_t length);

/**
 * @brief Release native generator resources and securely clear its state.
 *
 * Normal startup/task context only, after every consumer has stopped using the
 * provider, including every live TLS session. The owner serializes cleanup with
 * generation and entropy callbacks.
 * Repeating cleanup on the zero state succeeds. Success permits initialization
 * again. Cleanup does not destroy or reconfigure the borrowed entropy source.
 *
 * @param[in,out] random Zero-initialized, failed-init or live handle.
 * @return OK; INVALID_ARG for NULL.
 */
esy_err_t esy_crypto_random_deinit(esy_crypto_random_t *random);

#ifdef __cplusplus
}
#endif

#endif /* ESY_CRYPTO_H */
