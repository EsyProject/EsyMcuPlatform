/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CRYPTO_TEST_CONFIG_H
#define CRYPTO_TEST_CONFIG_H

/* Compile the real software provider with a short deterministic reseed period. */
#define MBEDTLS_AES_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_CTR_DRBG_RESEED_INTERVAL 4

#endif /* CRYPTO_TEST_CONFIG_H */
