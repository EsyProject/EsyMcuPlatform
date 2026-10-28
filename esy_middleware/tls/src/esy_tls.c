/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/tls/esy_tls_mbedtls.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>
#include "esy_common/utils/time/esy_datetime.h"
#include "mbedtls/platform_util.h"

#if !defined(MBEDTLS_SSL_CLI_C) || !defined(MBEDTLS_SSL_PROTO_TLS1_2) || \
    !defined(MBEDTLS_X509_CRT_PARSE_C) || !defined(MBEDTLS_PK_PARSE_C)
#error "esy_tls requires TLS 1.2 client, X.509 and private key parsing support"
#endif

static bool is_live(const esy_tls_t *tls) {
  return tls != NULL && tls->owner == tls;
}

static esy_err_t fail(esy_tls_t *tls, esy_err_t error, int native_error) {
  tls->status.state = ESY_TLS_FAILED;
  tls->status.error = error;
  tls->status.native_error = native_error;
  return error;
}

static esy_err_t native_failure(esy_tls_t *tls, int result) {
  esy_err_t error = tls->callback_error;
  if (error == ESY_OK) {
    error = result == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED
                ? ESY_ERR_HASH_FAIL : ESY_ERR_IO;
  }
  return fail(tls, error, result);
}

static bool would_block(int result) {
  return result == MBEDTLS_ERR_SSL_WANT_READ ||
         result == MBEDTLS_ERR_SSL_WANT_WRITE ||
         result == MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS;
}

static int random_bytes(void *context, unsigned char *output, size_t length) {
  esy_tls_t *tls = context;
  /* Native requests can exceed the public DRBG per-call bound. */
  while (length != 0u) {
    size_t count = length > ESY_CRYPTO_RANDOM_MAX_REQUEST_BYTES
                       ? ESY_CRYPTO_RANDOM_MAX_REQUEST_BYTES : length;
    esy_err_t result = tls->config.random.fill(tls->config.random.context,
                                              output, count);
    if (result != ESY_OK) {
      tls->callback_error = result < ESY_OK ? result : ESY_ERR_FAIL;
      return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    output += count;
    length -= count;
  }
  return 0;
}

static int transport_send(void *context, const unsigned char *data, size_t size) {
  esy_tls_t *tls = context;
  if (size > INT32_MAX) size = INT32_MAX;
  int32_t result = tls->config.transport.send(tls->config.transport.context,
                                             data, size);
  if (result < 0 || (size_t)result > size) {
    tls->callback_error = result < 0 ? result : ESY_ERR_IO;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  }
  return result == 0 ? MBEDTLS_ERR_SSL_WANT_WRITE : (int)result;
}

static int transport_recv(void *context, unsigned char *data, size_t size) {
  esy_tls_t *tls = context;
  if (size > INT32_MAX) size = INT32_MAX;
  int32_t result = tls->config.transport.recv(tls->config.transport.context,
                                             data, size);
  if (result < 0 || (size_t)result > size) {
    tls->callback_error = result < 0 ? result : ESY_ERR_IO;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  }
  return result == 0 ? MBEDTLS_ERR_SSL_WANT_READ : (int)result;
}

static int compare_time(const mbedtls_x509_time *certificate,
                        const esy_datetime_t *now) {
  const int left[] = {certificate->year, certificate->mon, certificate->day,
                      certificate->hour, certificate->min, certificate->sec};
  const int right[] = {now->year, now->month, now->day,
                       now->hour, now->minute, now->second};
  for (size_t i = 0u; i < sizeof(left) / sizeof(left[0]); ++i) {
    if (left[i] < right[i]) return -1;
    if (left[i] > right[i]) return 1;
  }
  return 0;
}

static uint32_t certificate_date_flags(const mbedtls_x509_crt *certificate,
                                       const esy_timestamp_t *timestamp,
                                       const esy_datetime_t *now) {
  uint32_t flags = 0u;
  if (compare_time(&certificate->valid_from, now) > 0)
    flags |= MBEDTLS_X509_BADCERT_FUTURE;
  int expiry = compare_time(&certificate->valid_to, now);
  if (expiry < 0 || (expiry == 0 && timestamp->nanoseconds != 0u))
    flags |= MBEDTLS_X509_BADCERT_EXPIRED;
  return flags;
}

static int verify_certificate(void *context, mbedtls_x509_crt *certificate,
                              int depth, uint32_t *flags) {
  esy_tls_t *tls = context;
  esy_timestamp_t timestamp = {0};
  esy_datetime_t now = {0};
  (void)depth;
  esy_err_t result = tls->config.utc_now(tls->config.time_context, &timestamp);
  if (result == ESY_OK) result = esy_datetime_from_timestamp(&timestamp, &now);
  if (result != ESY_OK) {
    tls->callback_error = result < ESY_OK ? result : ESY_ERR_FAIL;
    *flags |= MBEDTLS_X509_BADCERT_OTHER;
    return MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
  }
  /* Replace ONLY libc-clock certificate-date evidence with the session's
   * explicit trusted UTC. Chain, hostname, key usage and algorithm flags stay. */
  *flags &= ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
  *flags |= certificate_date_flags(certificate, &timestamp, &now);
  return 0;
}

static esy_err_t check_handshake_time(esy_tls_t *tls) {
  uint64_t now = 0u;
  esy_err_t result = tls->config.monotonic_us(tls->config.time_context, &now);
  if (result != ESY_OK)
    return fail(tls, result < ESY_OK ? result : ESY_ERR_FAIL, 0);
  if ((uint64_t)(now - tls->handshake_start_us) >= tls->config.handshake_timeout_us)
    return fail(tls, ESY_ERR_TIMEOUT, 0);
  return ESY_OK;
}

static void free_native(esy_tls_t *tls) {
  mbedtls_ssl_free(&tls->ssl);
  mbedtls_ssl_config_free(&tls->native_config);
  mbedtls_pk_free(&tls->key);
  mbedtls_x509_crt_free(&tls->client);
  mbedtls_x509_crt_free(&tls->ca);
}

static int parse_certificate(mbedtls_x509_crt *chain,
                              const esy_credentials_blob_t *blob) {
  if (blob->format == ESY_CREDENTIALS_FORMAT_DER)
    return mbedtls_x509_crt_parse_der(chain, blob->data, blob->size);
  return mbedtls_x509_crt_parse(chain, blob->data, blob->size);
}

static int parse_trust_anchors(mbedtls_x509_crt *chain,
                               const esy_credentials_blob_t *blob,
                               const esy_timestamp_t *timestamp,
                               const esy_datetime_t *now) {
  mbedtls_x509_crt candidates;
  mbedtls_x509_crt_init(&candidates);
  int result = parse_certificate(&candidates, blob);
  bool selected = false;
  if (result == 0) {
    /* Native path building precedes the verification callback and can prefer
     * an anchor using libc time. Give it only candidates valid at this init's
     * trusted UTC; never edit or shallow-copy provider-owned certificate nodes. */
    for (const mbedtls_x509_crt *candidate = &candidates;
         candidate != NULL; candidate = candidate->next) {
      if (certificate_date_flags(candidate, timestamp, now) != 0u) continue;
      result = mbedtls_x509_crt_parse_der(chain, candidate->raw.p, candidate->raw.len);
      if (result != 0) break;
      selected = true;
    }
    if (result == 0 && !selected) result = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
  }
  mbedtls_x509_crt_free(&candidates);
  return result;
}

esy_err_t esy_tls_init(esy_tls_t *tls, const esy_tls_config_t *config) {
  if (tls == NULL || config == NULL) return ESY_ERR_INVALID_ARG;
  if (tls->owner != NULL) return ESY_ERR_STATE;
  if (config->transport.send == NULL || config->transport.recv == NULL ||
      config->credentials == NULL || config->random.fill == NULL ||
      config->hostname == NULL || config->hostname[0] == '\0' ||
      config->monotonic_us == NULL || config->utc_now == NULL ||
      config->handshake_timeout_us == 0u || config->static_resources == NULL ||
      config->static_resources->tx_buffer == NULL ||
      config->static_resources->tx_buffer_size == 0u ||
      config->static_resources->tx_buffer_size > INT32_MAX)
    return ESY_ERR_INVALID_ARG;

  uint64_t start = 0u;
  esy_timestamp_t utc = {0};
  esy_datetime_t calendar = {0};
  esy_err_t result = config->monotonic_us(config->time_context, &start);
  if (result == ESY_OK) result = config->utc_now(config->time_context, &utc);
  if (result == ESY_OK) result = esy_datetime_from_timestamp(&utc, &calendar);
  if (result != ESY_OK) return result < ESY_OK ? result : ESY_ERR_FAIL;

  esy_credentials_config_t credentials = {0};
  result = esy_credentials_acquire(config->credentials, &tls->lease, &credentials);
  if (result != ESY_OK) return result;
  tls->config = *config;
  tls->config.static_resources = NULL;
  tls->config.hostname = NULL;
  tls->tx_buffer = config->static_resources->tx_buffer;
  tls->tx_capacity = config->static_resources->tx_buffer_size;
  tls->handshake_start_us = start;
  mbedtls_ssl_init(&tls->ssl);
  mbedtls_ssl_config_init(&tls->native_config);
  mbedtls_x509_crt_init(&tls->ca);
  mbedtls_x509_crt_init(&tls->client);
  mbedtls_pk_init(&tls->key);

  int native = parse_trust_anchors(&tls->ca, &credentials.trust_anchors, &utc, &calendar);
  if (native == 0 && credentials.client_certificate.size != 0u)
    native = parse_certificate(&tls->client, &credentials.client_certificate);
  if (native == 0 && credentials.private_key.size != 0u)
    native = mbedtls_pk_parse_key(&tls->key, credentials.private_key.data,
                                  credentials.private_key.size, NULL, 0u,
                                  random_bytes, tls);
  if (native == 0 && credentials.private_key.size != 0u)
    native = mbedtls_pk_check_pair(&tls->client.pk, &tls->key, random_bytes, tls);
  if (native == 0)
    native = mbedtls_ssl_config_defaults(&tls->native_config,
        MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
  if (native == 0) {
    mbedtls_ssl_conf_authmode(&tls->native_config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&tls->native_config, &tls->ca, NULL);
    mbedtls_ssl_conf_rng(&tls->native_config, random_bytes, tls);
    mbedtls_ssl_conf_verify(&tls->native_config, verify_certificate, tls);
    mbedtls_ssl_conf_min_tls_version(&tls->native_config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&tls->native_config, MBEDTLS_SSL_VERSION_TLS1_2);
#if defined(MBEDTLS_SSL_RENEGOTIATION)
    mbedtls_ssl_conf_renegotiation(&tls->native_config, MBEDTLS_SSL_RENEGOTIATION_DISABLED);
#endif
#if defined(MBEDTLS_SSL_SESSION_TICKETS)
    mbedtls_ssl_conf_session_tickets(&tls->native_config, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
#endif
    if (credentials.client_certificate.size != 0u)
      native = mbedtls_ssl_conf_own_cert(&tls->native_config, &tls->client, &tls->key);
  }
  if (native == 0) native = mbedtls_ssl_setup(&tls->ssl, &tls->native_config);
  if (native == 0) native = mbedtls_ssl_set_hostname(&tls->ssl, config->hostname);
  if (native != 0) {
    result = tls->callback_error != ESY_OK ? tls->callback_error : ESY_ERR_INVALID_ARG;
    if (native == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) result = ESY_ERR_HASH_FAIL;
    if (native == MBEDTLS_ERR_SSL_ALLOC_FAILED || native == MBEDTLS_ERR_X509_ALLOC_FAILED ||
        native == MBEDTLS_ERR_PK_ALLOC_FAILED) result = ESY_ERR_NO_MEM;
    free_native(tls);
    (void)esy_credentials_release(&tls->lease);
    mbedtls_platform_zeroize(tls, sizeof(*tls));
    return result;
  }
  mbedtls_ssl_set_bio(&tls->ssl, tls, transport_send, transport_recv, NULL);
  tls->status.state = ESY_TLS_HANDSHAKING;
  tls->owner = tls;
  return ESY_OK;
}

static esy_err_t flush_tx(esy_tls_t *tls) {
  if (tls->tx_length == 0u) return ESY_OK;
  size_t remaining = tls->tx_length - tls->tx_offset;
  int result = mbedtls_ssl_write(&tls->ssl, tls->tx_buffer + tls->tx_offset, remaining);
  if (would_block(result)) return ESY_OK;
  if (result <= 0 || (size_t)result > remaining) return native_failure(tls, result);
  mbedtls_platform_zeroize(tls->tx_buffer + tls->tx_offset, (size_t)result);
  tls->tx_offset += (size_t)result;
  tls->status.pending_tx_bytes = tls->tx_length - tls->tx_offset;
  if (tls->tx_offset == tls->tx_length) {
    tls->tx_offset = 0u;
    tls->tx_length = 0u;
  }
  return ESY_OK;
}

esy_err_t esy_tls_process(esy_tls_t *tls) {
  if (!is_live(tls)) return tls == NULL ? ESY_ERR_INVALID_ARG : ESY_ERR_STATE;
  if (tls->status.state == ESY_TLS_FAILED) return tls->status.error;
  if (tls->status.state == ESY_TLS_CLOSED) return ESY_OK;
  bool handshake = tls->status.state == ESY_TLS_HANDSHAKING;
  if (handshake && check_handshake_time(tls) != ESY_OK) return tls->status.error;
  if (tls->config.transport.progress != NULL) {
    esy_err_t result = tls->config.transport.progress(tls->config.transport.context);
    if (result != ESY_OK) return fail(tls, result < ESY_OK ? result : ESY_ERR_FAIL, 0);
  }
  if (handshake) {
    if (check_handshake_time(tls) != ESY_OK) return tls->status.error;
    int result = mbedtls_ssl_handshake_step(&tls->ssl);
    if (check_handshake_time(tls) != ESY_OK) return tls->status.error;
    if (result != 0 && !would_block(result)) return native_failure(tls, result);
    if (mbedtls_ssl_is_handshake_over(&tls->ssl)) {
      if (mbedtls_ssl_get_verify_result(&tls->ssl) != 0u)
        return fail(tls, ESY_ERR_HASH_FAIL, MBEDTLS_ERR_X509_CERT_VERIFY_FAILED);
      tls->status.state = ESY_TLS_READY;
    }
    return ESY_OK;
  }
  if (tls->tx_length != 0u) return flush_tx(tls);
  if (tls->status.state == ESY_TLS_CLOSING) {
    int result = mbedtls_ssl_close_notify(&tls->ssl);
    if (would_block(result)) return ESY_OK;
    if (result != 0) return native_failure(tls, result);
    tls->status.state = ESY_TLS_CLOSED;
  }
  return ESY_OK;
}

static int32_t stream_send(void *context, const void *data, size_t size) {
  esy_tls_t *tls = context;
  if (!is_live(tls)) return ESY_ERR_STATE;
  if (tls->status.state != ESY_TLS_READY) return ESY_ERR_STATE;
  if (data == NULL && size != 0u) return ESY_ERR_INVALID_ARG;
  if (tls->tx_length != 0u || size == 0u) return 0;
  size_t count = size > tls->tx_capacity ? tls->tx_capacity : size;
  memcpy(tls->tx_buffer, data, count);
  tls->tx_length = count;
  tls->status.pending_tx_bytes = count;
  return (int32_t)count;
}

static int32_t stream_recv(void *context, void *data, size_t size) {
  esy_tls_t *tls = context;
  if (!is_live(tls)) return ESY_ERR_STATE;
  if (tls->status.state != ESY_TLS_READY) return ESY_ERR_STATE;
  if (data == NULL && size != 0u) return ESY_ERR_INVALID_ARG;
  if (size == 0u) return 0;
  /* A native WANT_WRITE/READ must be retried with the same write arguments
   * before attempting another native record operation. */
  if (tls->tx_length != 0u) return 0;
  if (size > INT32_MAX) size = INT32_MAX;
  int result = mbedtls_ssl_read(&tls->ssl, data, size);
  if (result == MBEDTLS_ERR_SSL_WANT_READ) return 0;
  /* This TLS 1.2 client has no post-handshake control-write scheduler. Reject
   * WANT_WRITE rather than letting a later application write retire an alert
   * as though it contained application data. */
  if (result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || result == 0) {
    tls->status.state = ESY_TLS_CLOSED;
    return ESY_ERR_IO;
  }
  if (result < 0) return native_failure(tls, result);
  return (int32_t)result;
}

static esy_err_t stream_progress(void *context) {
  return esy_tls_process(context);
}

esy_err_t esy_tls_get_stream(esy_tls_t *tls, esy_network_stream_t *stream) {
  if (tls == NULL || stream == NULL) return ESY_ERR_INVALID_ARG;
  if (!is_live(tls) || tls->status.state != ESY_TLS_READY) return ESY_ERR_STATE;
  *stream = (esy_network_stream_t){stream_send, stream_recv, stream_progress, tls};
  return ESY_OK;
}

esy_err_t esy_tls_get_status(const esy_tls_t *tls, esy_tls_status_t *status) {
  if (tls == NULL || status == NULL) return ESY_ERR_INVALID_ARG;
  if (!is_live(tls)) return ESY_ERR_STATE;
  *status = tls->status;
  return ESY_OK;
}

esy_err_t esy_tls_close(esy_tls_t *tls) {
  if (!is_live(tls)) return tls == NULL ? ESY_ERR_INVALID_ARG : ESY_ERR_STATE;
  if (tls->status.state == ESY_TLS_READY) tls->status.state = ESY_TLS_CLOSING;
  return tls->status.state == ESY_TLS_CLOSING || tls->status.state == ESY_TLS_CLOSED
             ? ESY_OK : ESY_ERR_STATE;
}

esy_err_t esy_tls_deinit(esy_tls_t *tls) {
  if (tls == NULL) return ESY_ERR_INVALID_ARG;
  if (tls->owner == NULL) return ESY_OK;
  if (!is_live(tls)) return ESY_ERR_STATE;
  free_native(tls);
  esy_err_t result = esy_credentials_release(&tls->lease);
  if (result != ESY_OK) return fail(tls, result, 0);
  if (tls->tx_length != 0u)
    mbedtls_platform_zeroize(tls->tx_buffer, tls->tx_length);
  mbedtls_platform_zeroize(tls, sizeof(*tls));
  return ESY_OK;
}
