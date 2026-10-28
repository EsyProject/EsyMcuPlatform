/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdbool.h>
#include "unity.h"
#include "esy_tls.h"
#include "esy_tls_mbedtls.h"
#include "esy_common/utils/time/esy_datetime.h"
#include "tls_test_ca.h"
#include "tls_test_server.h"

#define TEST_TX_CAPACITY 32u
#define TEST_PIPE_CAPACITY 32768u

typedef struct {
    esy_tls_t tls;
    esy_credentials_t credentials;
    esy_credentials_config_t bundle;
    esy_tls_config_t config;
    esy_tls_static_resources_t resources;
    mbedtls_ssl_context server;
    mbedtls_ssl_config server_config;
    mbedtls_x509_crt server_certificate;
    mbedtls_pk_context server_key;
    bool server_live;
    bool block_client_send;
    uint8_t client_to_server[TEST_PIPE_CAPACITY];
    size_t client_to_server_length;
    uint8_t server_to_client[TEST_PIPE_CAPACITY];
    size_t server_to_client_length;
    uint8_t tx[TEST_TX_CAPACITY];
    uint64_t now;
    uint64_t progress_advance;
    esy_err_t clock_error;
    esy_err_t utc_error;
    int64_t utc_seconds;
    uint32_t utc_nanoseconds;
    esy_err_t random_error;
    esy_err_t transport_error;
    unsigned progress_calls;
    unsigned random_calls;
} tls_test_ctx_t;

static tls_test_ctx_t s_ctx;

static esy_err_t test_clock(void *context, uint64_t *microseconds) {
    tls_test_ctx_t *ctx = context;
    *microseconds = ctx->now;
    return ctx->clock_error;
}

static esy_err_t test_utc(void *context, esy_timestamp_t *timestamp) {
    tls_test_ctx_t *ctx = context;
    *timestamp = (esy_timestamp_t){ctx->utc_seconds, ctx->utc_nanoseconds};
    return ctx->utc_error;
}

static esy_err_t test_random(void *context, uint8_t *output, size_t length) {
    tls_test_ctx_t *ctx = context;
    ++ctx->random_calls;
    /* Deterministic bytes exist only in this host unit fixture. */
    memset(output, 0x5a, length);
    return ctx->random_error;
}

static int32_t test_send(void *context, const void *data, size_t size) {
    tls_test_ctx_t *ctx = context;
    if (ctx->transport_error != ESY_OK) { return ctx->transport_error; }
    if (!ctx->server_live || ctx->block_client_send) { return 0; }
    size_t available = sizeof(ctx->client_to_server) - ctx->client_to_server_length;
    if (size > available) { size = available; }
    memcpy(ctx->client_to_server + ctx->client_to_server_length, data, size);
    ctx->client_to_server_length += size;
    return (int32_t)size;
}

static int32_t test_receive(void *context, void *data, size_t size) {
    tls_test_ctx_t *ctx = context;
    if (ctx->transport_error != ESY_OK) { return ctx->transport_error; }
    if (size > ctx->server_to_client_length) { size = ctx->server_to_client_length; }
    memcpy(data, ctx->server_to_client, size);
    ctx->server_to_client_length -= size;
    memmove(ctx->server_to_client, ctx->server_to_client + size,
            ctx->server_to_client_length);
    return (int32_t)size;
}

static esy_err_t test_progress(void *context) {
    tls_test_ctx_t *ctx = context;
    ++ctx->progress_calls;
    ctx->now += ctx->progress_advance;
    return ESY_OK;
}

void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.utc_seconds = INT64_C(1800000000);
    s_ctx.bundle.trust_anchors = (esy_credentials_blob_t){
        k_test_ca, sizeof(k_test_ca), ESY_CREDENTIALS_FORMAT_PEM};
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_init(&s_ctx.credentials, &s_ctx.bundle));
    s_ctx.resources = (esy_tls_static_resources_t){s_ctx.tx, sizeof(s_ctx.tx)};
    s_ctx.config = (esy_tls_config_t){
        .transport = {test_send, test_receive, test_progress, &s_ctx},
        .credentials = &s_ctx.credentials,
        .random = {test_random, &s_ctx},
        .hostname = "localhost",
        .monotonic_us = test_clock,
        .utc_now = test_utc,
        .time_context = &s_ctx,
        .handshake_timeout_us = 100u,
        .static_resources = &s_ctx.resources};
}

void tearDown(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_deinit(&s_ctx.tls));
    mbedtls_ssl_free(&s_ctx.server);
    mbedtls_ssl_config_free(&s_ctx.server_config);
    mbedtls_x509_crt_free(&s_ctx.server_certificate);
    mbedtls_pk_free(&s_ctx.server_key);
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_deinit(&s_ctx.credentials));
}

static int server_random(void *context, unsigned char *output, size_t length) {
    return test_random(context, output, length) == ESY_OK ? 0 : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int server_send(void *context, const unsigned char *data, size_t size) {
    tls_test_ctx_t *ctx = context;
    size_t available = sizeof(ctx->server_to_client) - ctx->server_to_client_length;
    if (size > available) { size = available; }
    if (size == 0u) { return MBEDTLS_ERR_SSL_WANT_WRITE; }
    memcpy(ctx->server_to_client + ctx->server_to_client_length, data, size);
    ctx->server_to_client_length += size;
    return (int)size;
}

static int server_receive(void *context, unsigned char *data, size_t size) {
    tls_test_ctx_t *ctx = context;
    if (size > ctx->client_to_server_length) { size = ctx->client_to_server_length; }
    if (size == 0u) { return MBEDTLS_ERR_SSL_WANT_READ; }
    memcpy(data, ctx->client_to_server, size);
    ctx->client_to_server_length -= size;
    memmove(ctx->client_to_server, ctx->client_to_server + size,
            ctx->client_to_server_length);
    return (int)size;
}

static void start_native_server(void) {
    s_ctx.bundle.trust_anchors = (esy_credentials_blob_t){
        k_test_server_certificate, sizeof(k_test_server_certificate), ESY_CREDENTIALS_FORMAT_PEM};
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_replace(&s_ctx.credentials, &s_ctx.bundle));
    mbedtls_ssl_init(&s_ctx.server);
    mbedtls_ssl_config_init(&s_ctx.server_config);
    mbedtls_x509_crt_init(&s_ctx.server_certificate);
    mbedtls_pk_init(&s_ctx.server_key);
    TEST_ASSERT_EQUAL(0, mbedtls_x509_crt_parse(&s_ctx.server_certificate,
        k_test_server_certificate, sizeof(k_test_server_certificate)));
    TEST_ASSERT_EQUAL(0, mbedtls_pk_parse_key(&s_ctx.server_key,
        k_test_server_key, sizeof(k_test_server_key), NULL, 0u, server_random, &s_ctx));
    TEST_ASSERT_EQUAL(0, mbedtls_ssl_config_defaults(&s_ctx.server_config,
        MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT));
    mbedtls_ssl_conf_rng(&s_ctx.server_config, server_random, &s_ctx);
    mbedtls_ssl_conf_authmode(&s_ctx.server_config, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_min_tls_version(&s_ctx.server_config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&s_ctx.server_config, MBEDTLS_SSL_VERSION_TLS1_2);
    TEST_ASSERT_EQUAL(0, mbedtls_ssl_conf_own_cert(&s_ctx.server_config,
        &s_ctx.server_certificate, &s_ctx.server_key));
    TEST_ASSERT_EQUAL(0, mbedtls_ssl_setup(&s_ctx.server, &s_ctx.server_config));
    mbedtls_ssl_set_bio(&s_ctx.server, &s_ctx, server_send, server_receive, NULL);
    s_ctx.server_live = true;
}

static esy_err_t drive_native_handshake(void) {
    for (unsigned step = 0u; step < 256u; ++step) {
        esy_err_t result = esy_tls_process(&s_ctx.tls);
        if (result != ESY_OK) { return result; }
        esy_tls_status_t observed;
        TEST_ASSERT_EQUAL(ESY_OK, esy_tls_get_status(&s_ctx.tls, &observed));
        if (observed.state == ESY_TLS_READY && mbedtls_ssl_is_handshake_over(&s_ctx.server)) {
            return ESY_OK;
        }
        int native = mbedtls_ssl_handshake_step(&s_ctx.server);
        TEST_ASSERT_TRUE(native == 0 || native == MBEDTLS_ERR_SSL_WANT_READ ||
                         native == MBEDTLS_ERR_SSL_WANT_WRITE);
    }
    TEST_FAIL_MESSAGE("In-memory native TLS handshake did not complete");
    return ESY_ERR_FAIL;
}

void test_tls_initialization_rejects_missing_required_dependencies(void) {
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_tls_init(NULL, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_tls_init(&s_ctx.tls, NULL));
    s_ctx.config.random.fill = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    s_ctx.config.random.fill = test_random;
    s_ctx.config.hostname = "";
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    s_ctx.config.hostname = "localhost";
    s_ctx.config.utc_now = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_replace(&s_ctx.credentials, &s_ctx.bundle));
}

void test_tls_live_session_pins_credentials_and_rejects_repeated_initialization(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_credentials_deinit(&s_ctx.credentials));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_credentials_replace(&s_ctx.credentials, &s_ctx.bundle));
    esy_network_stream_t stream = {0};
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_tls_get_stream(&s_ctx.tls, &stream));
    TEST_ASSERT_NULL(stream.send);
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_deinit(&s_ctx.tls));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_replace(&s_ctx.credentials, &s_ctx.bundle));
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
}

void test_tls_malformed_certificate_failure_releases_lease_and_allows_retry(void) {
    static const uint8_t k_malformed[] = "invalid certificate";
    esy_credentials_config_t malformed = s_ctx.bundle;
    malformed.trust_anchors = (esy_credentials_blob_t){
        k_malformed, sizeof(k_malformed), ESY_CREDENTIALS_FORMAT_PEM};
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_replace(&s_ctx.credentials, &malformed));
    TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_deinit(&s_ctx.tls));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_replace(&s_ctx.credentials, &s_ctx.bundle));
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
}

void test_tls_partial_trust_bundle_cannot_hide_a_malformed_certificate(void) {
    static const uint8_t k_invalid[] =
        "-----BEGIN CERTIFICATE-----\n!invalid!\n-----END CERTIFICATE-----\n";
    uint8_t combined[sizeof(k_test_ca) + sizeof(k_invalid) - 1u];
    memcpy(combined, k_test_ca, sizeof(k_test_ca) - 1u);
    memcpy(combined + sizeof(k_test_ca) - 1u, k_invalid, sizeof(k_invalid));
    esy_credentials_config_t bundle = s_ctx.bundle;
    bundle.trust_anchors.data = combined;
    bundle.trust_anchors.size = sizeof(combined);
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_replace(&s_ctx.credentials, &bundle));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_deinit(&s_ctx.tls));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_replace(&s_ctx.credentials, &s_ctx.bundle));
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
}

void test_tls_clock_and_utc_init_failures_leave_handle_reusable(void) {
    s_ctx.clock_error = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    s_ctx.clock_error = ESY_OK;
    s_ctx.utc_error = ESY_ERR_FAIL;
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    s_ctx.utc_error = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
}

void test_tls_timeout_boundary_wrap_and_sticky_failure_across_full_cycle(void) {
    s_ctx.now = UINT64_MAX - UINT64_C(50);
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    s_ctx.now += 99u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_process(&s_ctx.tls));
    esy_tls_status_t observed;
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_get_status(&s_ctx.tls, &observed));
    TEST_ASSERT_EQUAL(ESY_TLS_HANDSHAKING, observed.state);
    ++s_ctx.now;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_tls_process(&s_ctx.tls));
    unsigned progress_calls = s_ctx.progress_calls;
    /* The original reading represents another complete modulo counter cycle. */
    s_ctx.now = UINT64_MAX - UINT64_C(50);
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_tls_process(&s_ctx.tls));
    TEST_ASSERT_EQUAL(progress_calls, s_ctx.progress_calls);
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_get_status(&s_ctx.tls, &observed));
    TEST_ASSERT_EQUAL(ESY_TLS_FAILED, observed.state);
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, observed.error);
}

void test_tls_maximum_timeout_and_explicit_restart_use_new_start(void) {
    s_ctx.now = UINT64_MAX - 4u;
    s_ctx.config.handshake_timeout_us = UINT32_MAX;
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    s_ctx.now += (uint64_t)UINT32_MAX - 1u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_process(&s_ctx.tls));
    ++s_ctx.now;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_tls_process(&s_ctx.tls));
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_deinit(&s_ctx.tls));
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_process(&s_ctx.tls));
}

void test_tls_timeout_includes_synchronous_transport_progress(void) {
    s_ctx.progress_advance = 100u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_tls_process(&s_ctx.tls));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.random_calls);
}

void test_tls_abort_cancels_wait_and_does_not_reactivate_after_counter_cycle(void) {
    s_ctx.now = UINT64_MAX;
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_deinit(&s_ctx.tls));
    s_ctx.now = UINT64_MAX;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_tls_process(&s_ctx.tls));
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_deinit(&s_ctx.tls));
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_process(&s_ctx.tls));
}

void test_tls_copied_native_handle_cannot_query_process_or_free_live_session(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    esy_tls_t copy = s_ctx.tls;
    esy_tls_status_t observed;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_tls_get_status(&copy, &observed));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_tls_process(&copy));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_tls_deinit(&copy));
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_process(&s_ctx.tls));
}

void test_tls_entropy_failure_propagates_and_is_sticky(void) {
    s_ctx.random_error = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    esy_err_t result = ESY_OK;
    for (unsigned step = 0u; step < 8u && result == ESY_OK; ++step) {
        result = esy_tls_process(&s_ctx.tls);
    }
    TEST_ASSERT_EQUAL(ESY_ERR_IO, result);
    unsigned random_calls = s_ctx.random_calls;
    s_ctx.random_error = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_tls_process(&s_ctx.tls));
    TEST_ASSERT_EQUAL(random_calls, s_ctx.random_calls);
}

void test_tls_transport_failure_propagates_without_exposing_a_stream(void) {
    s_ctx.transport_error = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    esy_err_t result = ESY_OK;
    for (unsigned step = 0u; step < 8u && result == ESY_OK; ++step) {
        result = esy_tls_process(&s_ctx.tls);
    }
    TEST_ASSERT_EQUAL(ESY_ERR_IO, result);
    esy_network_stream_t stream = {0};
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_tls_get_stream(&s_ctx.tls, &stream));
    TEST_ASSERT_NULL(stream.send);
}

void test_tls_native_pair_copies_backpressured_bytes_exchanges_records_and_flushes_close(void) {
    start_native_server();
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, drive_native_handshake());
    esy_network_stream_t stream;
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_get_stream(&s_ctx.tls, &stream));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, stream.send(stream.context, NULL, 1u));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, stream.recv(stream.context, NULL, 1u));
    TEST_ASSERT_EQUAL(0, stream.send(stream.context, NULL, 0u));
    TEST_ASSERT_EQUAL(0, stream.recv(stream.context, NULL, 0u));
    uint8_t request[] = "copied bytes";
    TEST_ASSERT_EQUAL(sizeof(request), stream.send(stream.context, request, sizeof(request)));
    memset(request, 0xa5, sizeof(request));
    s_ctx.block_client_send = true;
    TEST_ASSERT_EQUAL(ESY_OK, stream.progress(stream.context));
    TEST_ASSERT_EQUAL(0, stream.send(stream.context, request, sizeof(request)));
    uint8_t received[64] = {0};
    TEST_ASSERT_EQUAL(0, stream.recv(stream.context, received, sizeof(received)));
    esy_tls_status_t observed;
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_get_status(&s_ctx.tls, &observed));
    TEST_ASSERT_EQUAL(sizeof(request), observed.pending_tx_bytes);
    s_ctx.block_client_send = false;
    TEST_ASSERT_EQUAL(ESY_OK, stream.progress(stream.context));
    TEST_ASSERT_EQUAL(sizeof(request), mbedtls_ssl_read(&s_ctx.server, received, sizeof(received)));
    TEST_ASSERT_EQUAL_MEMORY("copied bytes", received, sizeof(request));
    TEST_ASSERT_EQUAL(5, mbedtls_ssl_write(&s_ctx.server, (const unsigned char *)"reply", 5u));
    TEST_ASSERT_EQUAL(5, stream.recv(stream.context, received, sizeof(received)));
    TEST_ASSERT_EQUAL_MEMORY("reply", received, 5u);
    TEST_ASSERT_EQUAL(0, stream.recv(stream.context, received, sizeof(received)));
    TEST_ASSERT_EQUAL(TEST_TX_CAPACITY, stream.send(stream.context, received, sizeof(received)));
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_close(&s_ctx.tls));
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_close(&s_ctx.tls));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, stream.send(stream.context, request, sizeof(request)));
    TEST_ASSERT_EQUAL(ESY_OK, stream.progress(stream.context));
    TEST_ASSERT_EQUAL(TEST_TX_CAPACITY, mbedtls_ssl_read(&s_ctx.server, received, sizeof(received)));
    TEST_ASSERT_EQUAL(ESY_OK, stream.progress(stream.context));
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_get_status(&s_ctx.tls, &observed));
    TEST_ASSERT_EQUAL(ESY_TLS_CLOSED, observed.state);
    TEST_ASSERT_EQUAL(MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY,
                      mbedtls_ssl_read(&s_ctx.server, received, sizeof(received)));
    s_ctx.now = UINT64_MAX;
    TEST_ASSERT_EQUAL(ESY_OK, stream.progress(stream.context));
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_get_status(&s_ctx.tls, &observed));
    TEST_ASSERT_EQUAL(ESY_TLS_CLOSED, observed.state);
}

void test_tls_native_peer_orderly_close_changes_state_without_reporting_temporary_empty(void) {
    start_native_server();
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, drive_native_handshake());
    esy_network_stream_t stream;
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_get_stream(&s_ctx.tls, &stream));
    TEST_ASSERT_EQUAL(0, mbedtls_ssl_close_notify(&s_ctx.server));
    uint8_t received[8];
    TEST_ASSERT_EQUAL(ESY_ERR_IO, stream.recv(stream.context, received, sizeof(received)));
    esy_tls_status_t observed;
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_get_status(&s_ctx.tls, &observed));
    TEST_ASSERT_EQUAL(ESY_TLS_CLOSED, observed.state);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, stream.recv(stream.context, received, sizeof(received)));
}

void test_tls_native_record_write_failure_is_sticky_and_abort_clears_staged_bytes(void) {
    start_native_server();
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, drive_native_handshake());
    esy_network_stream_t stream;
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_get_stream(&s_ctx.tls, &stream));
    TEST_ASSERT_EQUAL(6, stream.send(stream.context, "secret", 6u));
    s_ctx.transport_error = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, stream.progress(stream.context));
    TEST_ASSERT_EQUAL(ESY_ERR_IO, stream.progress(stream.context));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, stream.send(stream.context, "next", 4u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_deinit(&s_ctx.tls));
    TEST_ASSERT_EACH_EQUAL_UINT8(0u, s_ctx.tx, 6u);
}

void test_tls_native_record_read_failure_stops_application_io(void) {
    start_native_server();
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, drive_native_handshake());
    esy_network_stream_t stream;
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_get_stream(&s_ctx.tls, &stream));
    s_ctx.transport_error = ESY_ERR_IO;
    uint8_t received[8];
    TEST_ASSERT_EQUAL(ESY_ERR_IO, stream.recv(stream.context, received, sizeof(received)));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, stream.recv(stream.context, received, sizeof(received)));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_tls_close(&s_ctx.tls));
}

void test_tls_native_certificate_verification_propagates_clock_failure(void) {
    start_native_server();
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    s_ctx.utc_error = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, drive_native_handshake());
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_tls_process(&s_ctx.tls));
}

void test_tls_native_certificate_verification_uses_injected_future_and_past_utc(void) {
    start_native_server();
    s_ctx.utc_seconds = INT64_C(1577836800);
    TEST_ASSERT_EQUAL(ESY_ERR_HASH_FAIL, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_deinit(&s_ctx.tls));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_replace(&s_ctx.credentials, &s_ctx.bundle));
    s_ctx.utc_seconds = INT64_C(5000000000);
    TEST_ASSERT_EQUAL(ESY_ERR_HASH_FAIL, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_replace(&s_ctx.credentials, &s_ctx.bundle));
    s_ctx.utc_seconds = INT64_C(1800000000);
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, drive_native_handshake());
}

static void set_utc_from_certificate_time(const mbedtls_x509_time *time) {
    esy_datetime_t calendar = {
        .year = (uint16_t)time->year, .month = (uint8_t)time->mon,
        .day = (uint8_t)time->day, .hour = (uint8_t)time->hour,
        .minute = (uint8_t)time->min, .second = (uint8_t)time->sec};
    esy_timestamp_t timestamp;
    TEST_ASSERT_EQUAL(ESY_OK, esy_datetime_to_timestamp(&calendar, &timestamp));
    s_ctx.utc_seconds = timestamp.seconds;
    s_ctx.utc_nanoseconds = 0u;
}

void test_tls_anchor_selection_checks_exact_validity_boundaries_and_retries_after_failure(void) {
    mbedtls_x509_crt certificate;
    mbedtls_x509_crt_init(&certificate);
    TEST_ASSERT_EQUAL(0, mbedtls_x509_crt_parse(&certificate, k_test_ca, sizeof(k_test_ca)));
    set_utc_from_certificate_time(&certificate.valid_from);
    --s_ctx.utc_seconds;
    s_ctx.utc_nanoseconds = 999999999u;
    TEST_ASSERT_EQUAL(ESY_ERR_HASH_FAIL, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_replace(&s_ctx.credentials, &s_ctx.bundle));
    set_utc_from_certificate_time(&certificate.valid_from);
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_deinit(&s_ctx.tls));
    set_utc_from_certificate_time(&certificate.valid_to);
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_deinit(&s_ctx.tls));
    s_ctx.utc_nanoseconds = 1u;
    TEST_ASSERT_EQUAL(ESY_ERR_HASH_FAIL, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_replace(&s_ctx.credentials, &s_ctx.bundle));
    s_ctx.utc_nanoseconds = 0u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    mbedtls_x509_crt_free(&certificate);
}

void test_tls_certificate_that_expires_after_anchor_selection_is_still_rejected(void) {
    start_native_server();
    TEST_ASSERT_EQUAL(ESY_OK, esy_tls_init(&s_ctx.tls, &s_ctx.config));
    s_ctx.utc_seconds = INT64_C(5000000000);
    TEST_ASSERT_EQUAL(ESY_ERR_HASH_FAIL, drive_native_handshake());
}
