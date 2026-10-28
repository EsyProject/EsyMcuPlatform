/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_TLS_MBEDTLS_H
#define ESY_TLS_MBEDTLS_H

#include "esy_middleware/tls/esy_tls.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/version.h"
#include "mbedtls/x509_crt.h"

#if MBEDTLS_VERSION_NUMBER < 0x03060000 || MBEDTLS_VERSION_NUMBER >= 0x03070000
#error "esy_tls requires a consumer-provided Mbed TLS 3.6.x configuration"
#endif

/**
 * @brief Mbed TLS 3.6 adapter storage; ALL fields are implementation-owned.
 *
 * Initialize the complete object with {0}. This explicit software adapter
 * exposes native types so callers supply correctly sized/aligned state for
 * their provider configuration. Rebuild consumers after provider changes.
 * Mbed TLS owns internal allocations during setup, handshake and record work;
 * this typed storage does not imply a heap-free TLS stack. Private keys parsed
 * here are software keys; hardware key references are outside this adapter.
 * If MBEDTLS_USE_PSA_CRYPTO is enabled, the consumer's global provider owner
 * must successfully call psa_crypto_init() before the first TLS init and keep
 * PSA available until all native consumers have stopped. This adapter neither
 * initializes nor frees global PSA state; its injected RNG does not configure
 * the provider's independent PSA entropy sources or hardware drivers.
 */
struct esy_tls {
  esy_tls_t *owner; /**< Identity guard; live handles cannot be moved. */
  mbedtls_ssl_context ssl; /**< Native session. */
  mbedtls_ssl_config native_config; /**< Native client configuration. */
  mbedtls_x509_crt ca; /**< Parsed trust chain. */
  mbedtls_x509_crt client; /**< Optional parsed client chain. */
  mbedtls_pk_context key; /**< Optional parsed private key. */
  esy_tls_config_t config; /**< Borrowed provider descriptors. */
  esy_credentials_lease_t lease; /**< Pins the credential bundle. */
  esy_tls_status_t status; /**< Connection state and failure evidence. */
  uint8_t *tx_buffer; /**< Stable TX workspace. */
  size_t tx_capacity; /**< Workspace capacity. */
  size_t tx_length; /**< Pending write length. */
  size_t tx_offset; /**< Completed prefix of the pending write. */
  uint64_t handshake_start_us; /**< Armed only in HANDSHAKING state. */
  esy_err_t callback_error; /**< Exact transport/clock/RNG callback failure. */
};

#endif /* ESY_TLS_MBEDTLS_H */
