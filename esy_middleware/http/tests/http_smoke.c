/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "esy_middleware/http/esy_http_client.h"

#if defined(HTTP_SMOKE_TLS)
#include <sys/random.h>
#include "esy_middleware/tls/esy_tls_mbedtls.h"
#include "esy_middleware/crypto/esy_crypto_mbedtls.h"
#if defined(MBEDTLS_USE_PSA_CRYPTO)
#include "psa/crypto.h"
#endif
#endif

#define DOWNLOAD_BYTES 196613u
#define CERTIFICATE_CAPACITY 8192u
#define SEND_PREFIX_BYTES 31u
#define RECEIVE_PREFIX_BYTES 113u
#define TRANSACTION_TIMEOUT_US UINT32_C(20000000)

typedef struct {
    esy_http_client_t http;
    ESY_HTTP_CLIENT_STATIC_STORAGE(storage, 1024u, 257u, 1024u);
    esy_http_client_static_resources_t resources;
    int descriptor;
    const char *path;
    size_t received;
    unsigned body_attempts;
    unsigned observed_headers;
    bool saw_marker;
    bool saw_trailer;
#if defined(HTTP_SMOKE_TLS)
    esy_tls_t tls;
    esy_crypto_random_t random;
    esy_credentials_t credentials;
    uint8_t trust[CERTIFICATE_CAPACITY];
    uint8_t tls_tx[512];
#endif
} http_smoke_ctx_t;

static http_smoke_ctx_t s_ctx;
static const char k_post_body[] = "{\"enabled\":true}";

static uint64_t actual_microseconds(void)
{
    struct timespec timestamp;
    assert(clock_gettime(CLOCK_MONOTONIC, &timestamp) == 0);
    return (uint64_t)timestamp.tv_sec * UINT64_C(1000000) +
           (uint64_t)timestamp.tv_nsec / UINT64_C(1000);
}

static esy_err_t monotonic_us(void *context, uint64_t *microseconds)
{
    (void)context;
    *microseconds = actual_microseconds();
    return ESY_OK;
}

static void pause_iteration(void)
{
    struct timespec delay = {0, 100000};
    (void)nanosleep(&delay, NULL);
}

static int32_t transport_send(void *context, const void *data, size_t size)
{
    http_smoke_ctx_t *ctx = context;
    if (size > SEND_PREFIX_BYTES) { size = SEND_PREFIX_BYTES; }
    ssize_t written = send(ctx->descriptor, data, size, MSG_NOSIGNAL);
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return 0;
    }
    return written < 0 ? ESY_ERR_IO : (int32_t)written;
}

static int32_t transport_recv(void *context, void *data, size_t size)
{
    http_smoke_ctx_t *ctx = context;
    if (size > RECEIVE_PREFIX_BYTES) { size = RECEIVE_PREFIX_BYTES; }
    ssize_t received = recv(ctx->descriptor, data, size, 0);
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return 0;
    }
    return received <= 0 ? ESY_ERR_IO : (int32_t)received;
}

static bool equal_string(esy_http_string_t actual, const char *expected)
{
    return actual.length == strlen(expected) &&
           memcmp(actual.data, expected, actual.length) == 0;
}

static esy_err_t receive_header(void *context,
                                const esy_http_response_header_t *header)
{
    http_smoke_ctx_t *ctx = context;
    ++ctx->observed_headers;
    assert(header->status_code == 200u);
    if (equal_string(header->name, "X-Smoke-Test")) {
        assert(equal_string(header->value, "loopback"));
        ctx->saw_marker = true;
    }
    if (header->trailer) {
        assert(equal_string(header->name, "X-Smoke-Trailer"));
        assert(equal_string(header->value, "complete"));
        ctx->saw_trailer = true;
    }
    return ESY_OK;
}

static esy_err_t receive_body(void *context, const uint8_t *data, size_t size)
{
    http_smoke_ctx_t *ctx = context;
    assert(size > 0u && size <= sizeof(ctx->storage.body_buffer));
    bool post = strcmp(ctx->path, "/post") == 0;
    for (size_t index = 0u; index < size; ++index) {
        size_t position = ctx->received + index;
        if (post) {
            assert(position < sizeof(k_post_body) - 1u);
            assert(data[index] == (uint8_t)k_post_body[position]);
        } else {
            assert(position < DOWNLOAD_BYTES);
            assert(data[index] == (uint8_t)(position % 251u));
        }
    }
    /* Exercise whole-block consumer backpressure on actual socket traffic. */
    ++ctx->body_attempts;
    if ((ctx->body_attempts & 1u) != 0u) { return ESY_ERR_BUSY; }
    ctx->received += size;
    return ESY_OK;
}

static void connect_socket(const char *port_string)
{
    unsigned long port = strtoul(port_string, NULL, 10);
    assert(port > 0u && port <= UINT16_MAX);
    s_ctx.descriptor = socket(AF_INET, SOCK_STREAM, 0);
    assert(s_ctx.descriptor >= 0);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(connect(s_ctx.descriptor, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(fcntl(s_ctx.descriptor, F_SETFL, O_NONBLOCK) == 0);
}

#if defined(HTTP_SMOKE_TLS)
static esy_err_t utc_now(void *context, esy_timestamp_t *timestamp)
{
    (void)context;
    timestamp->seconds = (int64_t)time(NULL);
    timestamp->nanoseconds = 0u;
    return ESY_OK;
}

static esy_err_t entropy_fill(void *context, uint8_t *output, size_t length)
{
    (void)context;
    size_t offset = 0u;
    while (offset < length) {
        ssize_t count = getrandom(output + offset, length - offset, 0);
        if (count < 0 && errno == EINTR) { continue; }
        if (count <= 0) { return ESY_ERR_IO; }
        offset += (size_t)count;
    }
    return ESY_OK;
}

static bool authenticate_stream(const char *ca_path, const char *hostname,
                                 esy_network_stream_t *stream)
{
#if defined(MBEDTLS_USE_PSA_CRYPTO)
    assert(psa_crypto_init() == PSA_SUCCESS);
#endif
    FILE *file = fopen(ca_path, "rb");
    assert(file != NULL);
    size_t length = fread(s_ctx.trust, 1u, sizeof(s_ctx.trust) - 1u, file);
    assert(length > 0u && feof(file) && !ferror(file));
    assert(fclose(file) == 0);
    s_ctx.trust[length] = 0u;
    esy_credentials_config_t credentials = {0};
    credentials.trust_anchors = (esy_credentials_blob_t){
        s_ctx.trust, length + 1u, ESY_CREDENTIALS_FORMAT_PEM};
    assert(esy_credentials_init(&s_ctx.credentials, &credentials) == ESY_OK);
    esy_crypto_random_config_t random = {.entropy = {entropy_fill, NULL}};
    assert(esy_crypto_random_init(&s_ctx.random, &random) == ESY_OK);
    esy_tls_static_resources_t resources = {s_ctx.tls_tx, sizeof(s_ctx.tls_tx)};
    esy_tls_config_t tls = {
        .transport = *stream,
        .credentials = &s_ctx.credentials,
        .random = {esy_crypto_random_fill, &s_ctx.random},
        .hostname = hostname,
        .monotonic_us = monotonic_us,
        .utc_now = utc_now,
        .time_context = NULL,
        .handshake_timeout_us = UINT32_C(10000000),
        .static_resources = &resources};
    assert(esy_tls_init(&s_ctx.tls, &tls) == ESY_OK);
    esy_tls_status_t status;
    uint64_t started = actual_microseconds();
    do {
        (void)esy_tls_process(&s_ctx.tls);
        assert(esy_tls_get_status(&s_ctx.tls, &status) == ESY_OK);
        pause_iteration();
    } while (status.state == ESY_TLS_HANDSHAKING &&
             actual_microseconds() - started < UINT64_C(12000000));
    if (status.state != ESY_TLS_READY) { return false; }
    assert(esy_tls_get_stream(&s_ctx.tls, stream) == ESY_OK);
    return true;
}
#endif

static void exchange(esy_network_stream_t stream)
{
    ESY_HTTP_CLIENT_BIND_STATIC_RESOURCES(&s_ctx.resources, s_ctx.storage);
    esy_http_client_config_t config = {
        .transport = stream,
        .static_resources = &s_ctx.resources,
        .monotonic_us = monotonic_us,
        .clock_context = NULL,
        .total_timeout_us = TRANSACTION_TIMEOUT_US,
        .idle_timeout_us = UINT32_C(5000000),
        .io_budget_bytes = 257u,
        .max_header_bytes = 4096u,
        .max_body_bytes = DOWNLOAD_BYTES,
        .body_cb = receive_body,
        .header_cb = receive_header,
        .callback_context = &s_ctx};
    assert(esy_http_client_init(&s_ctx.http, &config) == ESY_OK);
    esy_http_request_header_t content_type = {
        {"Content-Type", sizeof("Content-Type") - 1u},
        {"application/json", sizeof("application/json") - 1u}};
    esy_http_request_t request = {
        .method = ESY_HTTP_GET,
        .host = {"localhost", sizeof("localhost") - 1u},
        .target = {s_ctx.path, strlen(s_ctx.path)}};
    bool post = strcmp(s_ctx.path, "/post") == 0;
    if (post) {
        request.method = ESY_HTTP_POST;
        request.headers = &content_type;
        request.header_count = 1u;
        request.body = (const uint8_t *)k_post_body;
        request.body_size = sizeof(k_post_body) - 1u;
    }
    assert(esy_http_client_submit(&s_ctx.http, &request) == ESY_OK);
    esy_http_client_status_t status;
    uint64_t started = actual_microseconds();
    do {
        esy_err_t result = esy_http_client_process(&s_ctx.http);
        assert(esy_http_client_get_status(&s_ctx.http, &status) == ESY_OK);
        if (result != ESY_OK) {
            fprintf(stderr, "HTTP failed: result=%d parser=%d state=%d status=%u bytes=%llu\n",
                    (int)result, status.parser_error, (int)status.state,
                    (unsigned)status.status_code, (unsigned long long)status.body_bytes);
        }
        assert(result == ESY_OK);
        pause_iteration();
    } while (status.state != ESY_HTTP_COMPLETE &&
             actual_microseconds() - started < TRANSACTION_TIMEOUT_US);
    assert(status.state == ESY_HTTP_COMPLETE);
    assert(status.status_code == 200u && status.error == ESY_OK);
    assert(status.body_bytes == (post ? sizeof(k_post_body) - 1u : DOWNLOAD_BYTES));
    assert(s_ctx.received == status.body_bytes);
    assert(s_ctx.saw_marker && s_ctx.observed_headers >= 2u);
    assert(s_ctx.saw_trailer == (strcmp(s_ctx.path, "/chunked") == 0));
    assert(esy_http_client_deinit(&s_ctx.http) == ESY_OK);
}

int main(int argc, char **argv)
{
    assert(argc == 6);
    bool secure = strcmp(argv[1], "plain") != 0;
    bool reject = strcmp(argv[1], "reject") == 0;
    s_ctx.descriptor = -1;
    s_ctx.path = argv[3];
    connect_socket(argv[2]);
    esy_network_stream_t stream = {transport_send, transport_recv, NULL, &s_ctx};
#if defined(HTTP_SMOKE_TLS)
    if (secure) {
        bool authenticated = authenticate_stream(argv[4], argv[5], &stream);
        assert(authenticated != reject);
        if (authenticated) { exchange(stream); }
        assert(esy_tls_deinit(&s_ctx.tls) == ESY_OK);
        assert(esy_credentials_deinit(&s_ctx.credentials) == ESY_OK);
        assert(esy_crypto_random_deinit(&s_ctx.random) == ESY_OK);
    } else {
        exchange(stream);
    }
#else
    assert(!secure && !reject);
    exchange(stream);
#endif
    assert(close(s_ctx.descriptor) == 0);
    return 0;
}
