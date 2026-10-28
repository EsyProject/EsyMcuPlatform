/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_http_client.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define FIXTURE_STREAM_BYTES 32768u
#define FIXTURE_HEADER_COUNT 32u
#define FIXTURE_HEADER_BYTES 256u
#define FIXTURE_DRIVE_LIMIT 4096u

typedef struct {
    char name[FIXTURE_HEADER_BYTES];
    char value[FIXTURE_HEADER_BYTES];
    uint16_t status_code;
    bool trailer;
} fixture_header_t;

typedef struct {
    esy_http_client_t client;
    ESY_HTTP_CLIENT_STATIC_STORAGE(storage, 1024u, 64u, 256u);
    esy_http_client_static_resources_t resources;
    esy_http_client_config_t config;
    uint8_t tx[FIXTURE_STREAM_BYTES];
    size_t tx_size;
    uint8_t rx[FIXTURE_STREAM_BYTES];
    size_t rx_size;
    size_t rx_offset;
    uint8_t body[FIXTURE_STREAM_BYTES];
    size_t body_size;
    fixture_header_t headers[FIXTURE_HEADER_COUNT];
    size_t header_count;
    size_t send_chunk;
    size_t recv_chunk;
    size_t largest_send;
    size_t largest_recv;
    unsigned send_zero_count;
    unsigned body_busy_count;
    unsigned progress_calls;
    unsigned send_calls;
    unsigned recv_calls;
    unsigned clock_calls;
    unsigned body_calls;
    unsigned header_calls;
    esy_err_t progress_result;
    esy_err_t send_result;
    esy_err_t recv_result;
    esy_err_t body_result;
    esy_err_t header_result;
    esy_err_t clock_result;
    bool send_oversize;
    bool recv_oversize;
    bool test_reentry;
    bool check_busy_bytes;
    uint8_t busy_bytes[64];
    size_t busy_size;
    uint64_t now;
    uint64_t provider_advance_us;
    uint64_t callback_advance_us;
} http_test_ctx_t;

static http_test_ctx_t s_ctx;

static esy_http_string_t fixture_string(const char *text)
{
    esy_http_string_t result = {text, strlen(text)};
    return result;
}

static esy_http_request_t fixture_request(void)
{
    esy_http_request_t request = {0};
    request.method = ESY_HTTP_GET;
    request.host = fixture_string("example.test");
    request.target = fixture_string("/firmware.bin?version=2");
    return request;
}

static esy_err_t fixture_clock(void *context, uint64_t *microseconds)
{
    http_test_ctx_t *fixture = context;
    ++fixture->clock_calls;
    if (fixture->clock_result != ESY_OK) {
        return fixture->clock_result;
    }
    *microseconds = fixture->now;
    return ESY_OK;
}

static esy_err_t fixture_progress(void *context)
{
    http_test_ctx_t *fixture = context;
    ++fixture->progress_calls;
    fixture->now += fixture->provider_advance_us;
    return fixture->progress_result;
}

static int32_t fixture_send(void *context, const void *data, size_t size)
{
    http_test_ctx_t *fixture = context;
    size_t count = size;
    ++fixture->send_calls;
    if (size > fixture->largest_send) {
        fixture->largest_send = size;
    }
    if (fixture->send_result != ESY_OK) {
        return fixture->send_result;
    }
    if (fixture->send_oversize) {
        return (int32_t)(size + 1u);
    }
    if (fixture->send_zero_count != 0u) {
        --fixture->send_zero_count;
        return 0;
    }
    if (count > fixture->send_chunk) {
        count = fixture->send_chunk;
    }
    TEST_ASSERT_LESS_OR_EQUAL(sizeof(fixture->tx) - fixture->tx_size, count);
    memcpy(fixture->tx + fixture->tx_size, data, count);
    fixture->tx_size += count;
    return (int32_t)count;
}

static int32_t fixture_recv(void *context, void *data, size_t size)
{
    http_test_ctx_t *fixture = context;
    size_t count = fixture->rx_size - fixture->rx_offset;
    ++fixture->recv_calls;
    if (size > fixture->largest_recv) {
        fixture->largest_recv = size;
    }
    if (fixture->recv_result != ESY_OK) {
        return fixture->recv_result;
    }
    if (fixture->recv_oversize) {
        return (int32_t)(size + 1u);
    }
    if (count > size) {
        count = size;
    }
    if (count > fixture->recv_chunk) {
        count = fixture->recv_chunk;
    }
    memcpy(data, fixture->rx + fixture->rx_offset, count);
    fixture->rx_offset += count;
    return (int32_t)count;
}

static void fixture_check_reentry(void)
{
    esy_http_request_t request = fixture_request();
    esy_http_client_status_t status = {0};
    if (!s_ctx.test_reentry) {
        return;
    }
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_get_status(&s_ctx.client, &status));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_http_client_init(&s_ctx.client, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_http_client_submit(&s_ctx.client, &request));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_http_client_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_http_client_cancel(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_http_client_deinit(&s_ctx.client));
}

static esy_err_t fixture_body(void *context, const uint8_t *data, size_t size)
{
    http_test_ctx_t *fixture = context;
    ++fixture->body_calls;
    fixture_check_reentry();
    fixture->now += fixture->callback_advance_us;
    if (fixture->check_busy_bytes) {
        TEST_ASSERT_EQUAL(fixture->busy_size, size);
        TEST_ASSERT_EQUAL_MEMORY(fixture->busy_bytes, data, size);
    }
    if (fixture->body_busy_count != 0u) {
        --fixture->body_busy_count;
        TEST_ASSERT_LESS_OR_EQUAL(sizeof(fixture->busy_bytes), size);
        memcpy(fixture->busy_bytes, data, size);
        fixture->busy_size = size;
        fixture->check_busy_bytes = true;
        return ESY_ERR_BUSY;
    }
    fixture->check_busy_bytes = false;
    if (fixture->body_result != ESY_OK) {
        return fixture->body_result;
    }
    TEST_ASSERT_LESS_OR_EQUAL(sizeof(fixture->body) - fixture->body_size, size);
    memcpy(fixture->body + fixture->body_size, data, size);
    fixture->body_size += size;
    return ESY_OK;
}

static esy_err_t fixture_header(void *context, const esy_http_response_header_t *header)
{
    http_test_ctx_t *fixture = context;
    fixture_header_t *capture;
    ++fixture->header_calls;
    fixture_check_reentry();
    fixture->now += fixture->callback_advance_us;
    if (fixture->header_result != ESY_OK) {
        return fixture->header_result;
    }
    TEST_ASSERT_LESS_THAN(FIXTURE_HEADER_COUNT, fixture->header_count);
    TEST_ASSERT_LESS_THAN(FIXTURE_HEADER_BYTES, header->name.length);
    TEST_ASSERT_LESS_THAN(FIXTURE_HEADER_BYTES, header->value.length);
    capture = &fixture->headers[fixture->header_count++];
    memcpy(capture->name, header->name.data, header->name.length);
    memcpy(capture->value, header->value.data, header->value.length);
    capture->name[header->name.length] = '\0';
    capture->value[header->value.length] = '\0';
    capture->status_code = header->status_code;
    capture->trailer = header->trailer;
    return ESY_OK;
}

static esy_http_client_status_t fixture_status(void)
{
    esy_http_client_status_t status = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_get_status(&s_ctx.client, &status));
    return status;
}

static void fixture_queue(const char *text)
{
    size_t size = strlen(text);
    TEST_ASSERT_LESS_OR_EQUAL(sizeof(s_ctx.rx) - s_ctx.rx_size, size);
    memcpy(s_ctx.rx + s_ctx.rx_size, text, size);
    s_ctx.rx_size += size;
}

static void fixture_init_submit(esy_http_request_t request)
{
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_init(&s_ctx.client, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_submit(&s_ctx.client, &request));
}

static void fixture_send_request(void)
{
    unsigned count;
    for (count = 0u; count < FIXTURE_DRIVE_LIMIT; ++count) {
        esy_http_client_status_t status = fixture_status();
        if (status.state != ESY_HTTP_SENDING) {
            TEST_ASSERT_EQUAL(ESY_HTTP_RECEIVING, status.state);
            return;
        }
        TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_process(&s_ctx.client));
    }
    TEST_FAIL_MESSAGE("Request did not finish within the bounded fixture drive");
}

static esy_http_client_status_t fixture_drive(void)
{
    unsigned count;
    for (count = 0u; count < FIXTURE_DRIVE_LIMIT; ++count) {
        esy_http_client_status_t status = fixture_status();
        if (status.state == ESY_HTTP_COMPLETE || status.state == ESY_HTTP_FAILED ||
            status.state == ESY_HTTP_CANCELLED) {
            return status;
        }
        (void)esy_http_client_process(&s_ctx.client);
    }
    TEST_FAIL_MESSAGE("Response did not reach a terminal state within the fixture drive");
    return fixture_status();
}

static void fixture_assert_response(const char *response, uint16_t code,
                                    const char *body)
{
    esy_http_client_status_t status;
    fixture_init_submit(fixture_request());
    fixture_queue(response);
    status = fixture_drive();
    TEST_ASSERT_EQUAL(ESY_HTTP_COMPLETE, status.state);
    TEST_ASSERT_EQUAL(ESY_OK, status.error);
    TEST_ASSERT_EQUAL(code, status.status_code);
    TEST_ASSERT_EQUAL(strlen(body), status.body_bytes);
    TEST_ASSERT_EQUAL(strlen(body), s_ctx.body_size);
    if (s_ctx.body_size != 0u) {
        TEST_ASSERT_EQUAL_MEMORY(body, s_ctx.body, s_ctx.body_size);
    }
}

void setUp(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    ESY_HTTP_CLIENT_BIND_STATIC_RESOURCES(&s_ctx.resources, s_ctx.storage);
    s_ctx.config.transport.send = fixture_send;
    s_ctx.config.transport.recv = fixture_recv;
    s_ctx.config.transport.progress = fixture_progress;
    s_ctx.config.transport.context = &s_ctx;
    s_ctx.config.static_resources = &s_ctx.resources;
    s_ctx.config.monotonic_us = fixture_clock;
    s_ctx.config.clock_context = &s_ctx;
    s_ctx.config.total_timeout_us = 10000u;
    s_ctx.config.idle_timeout_us = 1000u;
    s_ctx.config.io_budget_bytes = 64u;
    s_ctx.config.max_header_bytes = 4096u;
    s_ctx.config.max_body_bytes = FIXTURE_STREAM_BYTES;
    s_ctx.config.body_cb = fixture_body;
    s_ctx.config.header_cb = fixture_header;
    s_ctx.config.callback_context = &s_ctx;
    s_ctx.send_chunk = SIZE_MAX;
    s_ctx.recv_chunk = SIZE_MAX;
}

void tearDown(void)
{
    (void)esy_http_client_cancel(&s_ctx.client);
    (void)esy_http_client_deinit(&s_ctx.client);
}

void test_zero_state_argument_validation_and_idempotent_deinit(void)
{
    esy_http_client_status_t status;
    esy_http_client_status_t original;
    esy_http_request_t request = fixture_request();
    memset(&status, 0xA5, sizeof(status));
    original = status;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_http_client_init(NULL, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_http_client_init(&s_ctx.client, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_http_client_get_status(NULL, &status));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_http_client_get_status(&s_ctx.client, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_http_client_get_status(&s_ctx.client, &status));
    TEST_ASSERT_EQUAL_MEMORY(&original, &status, sizeof(status));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_http_client_submit(&s_ctx.client, &request));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_http_client_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_http_client_cancel(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_deinit(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_deinit(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_http_client_submit(NULL, &request));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_http_client_process(NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_http_client_cancel(NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_http_client_deinit(NULL));
}

void test_init_rejects_missing_callbacks_and_invalid_budgets_without_borrowing(void)
{
    esy_http_client_config_t candidate;
    unsigned variant;
    for (variant = 0u; variant < 9u; ++variant) {
        candidate = s_ctx.config;
        switch (variant) {
        case 0u: candidate.transport.send = NULL; break;
        case 1u: candidate.transport.recv = NULL; break;
        case 2u: candidate.monotonic_us = NULL; break;
        case 3u: candidate.body_cb = NULL; break;
        case 4u: candidate.total_timeout_us = 0u; break;
        case 5u: candidate.idle_timeout_us = 0u; break;
        case 6u: candidate.io_budget_bytes = 0u; break;
        case 7u: candidate.max_header_bytes = 0u; break;
        default: candidate.max_body_bytes = 0u; break;
        }
        TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_http_client_init(&s_ctx.client, &candidate));
    }
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_init(&s_ctx.client, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_http_client_init(&s_ctx.client, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_HTTP_IDLE, fixture_status().state);
    TEST_ASSERT_EQUAL(0u, s_ctx.progress_calls);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_http_client_process(&s_ctx.client));
}

void test_init_rejects_missing_short_and_overlapping_workspaces(void)
{
    esy_http_client_static_resources_t valid = s_ctx.resources;
    unsigned variant;
    s_ctx.config.static_resources = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_http_client_init(&s_ctx.client, &s_ctx.config));
    s_ctx.config.static_resources = &s_ctx.resources;
    for (variant = 0u; variant < 12u; ++variant) {
        s_ctx.resources = valid;
        switch (variant) {
        case 0u: s_ctx.resources.request_buffer = NULL; break;
        case 1u: s_ctx.resources.receive_buffer = NULL; break;
        case 2u: s_ctx.resources.body_buffer = NULL; break;
        case 3u: s_ctx.resources.header_buffer = NULL; break;
        case 4u: s_ctx.resources.request_buffer_size = 0u; break;
        case 5u: s_ctx.resources.receive_buffer_size = 0u; break;
        case 6u: s_ctx.resources.body_buffer_size = valid.receive_buffer_size - 1u; break;
        case 7u: s_ctx.resources.header_buffer_size = 1u; break;
        case 8u: s_ctx.resources.request_buffer_size = (size_t)INT32_MAX + 1u; break;
        case 9u: s_ctx.resources.receive_buffer = valid.body_buffer; break;
        case 10u: s_ctx.resources.header_buffer = valid.request_buffer + 1u; break;
        default: s_ctx.resources.request_buffer = (uint8_t *)&s_ctx.client; break;
        }
        TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_http_client_init(&s_ctx.client, &s_ctx.config));
    }
    s_ctx.resources = valid;
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_init(&s_ctx.client, &s_ctx.config));
}

void test_fragmented_content_length_response_and_request_encoding(void)
{
    esy_http_request_t request = fixture_request();
    esy_http_request_header_t headers[] = {
        {{"Authorization", 13u}, {"Bearer token", 12u}},
        {{"X-Empty", 7u}, {NULL, 0u}}
    };
    esy_http_client_status_t status;
    request.headers = headers;
    request.header_count = sizeof(headers) / sizeof(headers[0]);
    s_ctx.send_chunk = 3u;
    s_ctx.recv_chunk = 1u;
    s_ctx.config.io_budget_bytes = 7u;
    fixture_init_submit(request);
    fixture_queue("HTTP/1.1 200 OK\r\nContent-Length: 11\r\nX-Test: yes\r\n\r\nhello world");
    status = fixture_drive();
    TEST_ASSERT_EQUAL(ESY_HTTP_COMPLETE, status.state);
    TEST_ASSERT_EQUAL(200u, status.status_code);
    TEST_ASSERT_EQUAL(11u, status.body_bytes);
    TEST_ASSERT_EQUAL_MEMORY("hello world", s_ctx.body, 11u);
    s_ctx.tx[s_ctx.tx_size] = '\0';
    TEST_ASSERT_NOT_NULL(strstr((char *)s_ctx.tx, "GET /firmware.bin?version=2 HTTP/1.1\r\n"));
    TEST_ASSERT_NOT_NULL(strstr((char *)s_ctx.tx, "Host: example.test\r\n"));
    TEST_ASSERT_NOT_NULL(strstr((char *)s_ctx.tx, "Connection: close\r\n"));
    TEST_ASSERT_NOT_NULL(strstr((char *)s_ctx.tx, "Authorization: Bearer token\r\n"));
    TEST_ASSERT_EQUAL_STRING("Content-Length", s_ctx.headers[0].name);
    TEST_ASSERT_EQUAL_STRING("11", s_ctx.headers[0].value);
    TEST_ASSERT_EQUAL_STRING("X-Test", s_ctx.headers[1].name);
    TEST_ASSERT_EQUAL_STRING("yes", s_ctx.headers[1].value);
    TEST_ASSERT_LESS_OR_EQUAL(7u, s_ctx.largest_send);
    TEST_ASSERT_LESS_OR_EQUAL(7u, s_ctx.largest_recv);
}

void test_known_length_request_body_with_prefix_sends_and_backpressure(void)
{
    const char payload[] = "{\"state\":true}";
    esy_http_request_t request = fixture_request();
    esy_http_client_status_t status;
    request.method = ESY_HTTP_POST;
    request.body = (const uint8_t *)payload;
    request.body_size = sizeof(payload) - 1u;
    s_ctx.send_chunk = 2u;
    s_ctx.send_zero_count = 3u;
    fixture_init_submit(request);
    fixture_queue("HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n");
    TEST_ASSERT_EQUAL(0u, s_ctx.send_calls);
    status = fixture_drive();
    TEST_ASSERT_EQUAL(ESY_HTTP_COMPLETE, status.state);
    TEST_ASSERT_EQUAL(201u, status.status_code);
    s_ctx.tx[s_ctx.tx_size] = '\0';
    TEST_ASSERT_NOT_NULL(strstr((char *)s_ctx.tx, "POST /"));
    TEST_ASSERT_NOT_NULL(strstr((char *)s_ctx.tx, "Content-Length: 14\r\n"));
    TEST_ASSERT_EQUAL_MEMORY(payload, s_ctx.tx + s_ctx.tx_size - request.body_size, request.body_size);
    TEST_ASSERT_EQUAL(0u, s_ctx.body_calls);
}

void test_supported_methods_encode_without_provider_or_os_types(void)
{
    static const char *const k_methods[] = {"GET", "HEAD", "POST", "PUT", "DELETE", "PATCH"};
    unsigned method;
    for (method = 0u; method < sizeof(k_methods) / sizeof(k_methods[0]); ++method) {
        esy_http_request_t request = fixture_request();
        request.method = (esy_http_method_t)method;
        fixture_init_submit(request);
        fixture_send_request();
        TEST_ASSERT_EQUAL_MEMORY(k_methods[method], s_ctx.tx, strlen(k_methods[method]));
        TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_cancel(&s_ctx.client));
        TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_deinit(&s_ctx.client));
        s_ctx.tx_size = 0u;
    }
}

void test_large_binary_body_streams_through_small_workspaces(void)
{
    esy_http_client_status_t status;
    const size_t body_size = 10000u;
    size_t i;
    fixture_init_submit(fixture_request());
    fixture_queue("HTTP/1.1 200 OK\r\nContent-Length: 10000\r\n\r\n");
    for (i = 0u; i < body_size; ++i) {
        s_ctx.rx[s_ctx.rx_size++] = (uint8_t)i;
    }
    status = fixture_drive();
    TEST_ASSERT_EQUAL(ESY_HTTP_COMPLETE, status.state);
    TEST_ASSERT_EQUAL(body_size, status.body_bytes);
    TEST_ASSERT_EQUAL(body_size, s_ctx.body_size);
    for (i = 0u; i < body_size; ++i) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)i, s_ctx.body[i]);
    }
    TEST_ASSERT_GREATER_THAN(1u, s_ctx.body_calls);
    TEST_ASSERT_LESS_OR_EQUAL(sizeof(s_ctx.storage.receive_buffer), s_ctx.largest_recv);
}

void test_chunked_body_extensions_and_trailers_are_decoded_incrementally(void)
{
    esy_http_client_status_t status;
    s_ctx.recv_chunk = 3u;
    fixture_init_submit(fixture_request());
    fixture_queue("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTrailer: X-Digest\r\n\r\n"
                  "5;part=one\r\nhello\r\n6\r\n world\r\n0\r\nX-Digest: checked\r\n\r\n");
    status = fixture_drive();
    TEST_ASSERT_EQUAL(ESY_HTTP_COMPLETE, status.state);
    TEST_ASSERT_EQUAL(11u, status.body_bytes);
    TEST_ASSERT_EQUAL_MEMORY("hello world", s_ctx.body, 11u);
    TEST_ASSERT_EQUAL(3u, s_ctx.header_count);
    TEST_ASSERT_TRUE(s_ctx.headers[2].trailer);
    TEST_ASSERT_EQUAL_STRING("X-Digest", s_ctx.headers[2].name);
    TEST_ASSERT_EQUAL_STRING("checked", s_ctx.headers[2].value);
}

void test_head_response_does_not_expect_advertised_content_length_body(void)
{
    esy_http_request_t request = fixture_request();
    esy_http_client_status_t status;
    request.method = ESY_HTTP_HEAD;
    fixture_init_submit(request);
    fixture_queue("HTTP/1.1 200 OK\r\nContent-Length: 123456789\r\n\r\n");
    status = fixture_drive();
    TEST_ASSERT_EQUAL(ESY_HTTP_COMPLETE, status.state);
    TEST_ASSERT_EQUAL(200u, status.status_code);
    TEST_ASSERT_EQUAL(0u, status.body_bytes);
    TEST_ASSERT_EQUAL(0u, s_ctx.body_calls);
}

void test_204_and_304_responses_complete_without_body_framing(void)
{
    fixture_assert_response("HTTP/1.1 204 No Content\r\n\r\n", 204u, "");
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_deinit(&s_ctx.client));
    s_ctx.rx_size = 0u;
    s_ctx.rx_offset = 0u;
    fixture_assert_response("HTTP/1.1 304 Not Modified\r\nContent-Length: 1234\r\n\r\n", 304u, "");
}

void test_informational_responses_precede_one_final_response(void)
{
    esy_http_client_status_t status;
    fixture_init_submit(fixture_request());
    fixture_queue("HTTP/1.1 100 Continue\r\nX-Interim: first\r\n\r\n"
                  "HTTP/1.1 103 Early Hints\r\nLink: </style.css>\r\n\r\n"
                  "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    status = fixture_drive();
    TEST_ASSERT_EQUAL(ESY_HTTP_COMPLETE, status.state);
    TEST_ASSERT_EQUAL(200u, status.status_code);
    TEST_ASSERT_EQUAL_MEMORY("ok", s_ctx.body, 2u);
    TEST_ASSERT_EQUAL(100u, s_ctx.headers[0].status_code);
    TEST_ASSERT_EQUAL(103u, s_ctx.headers[1].status_code);
    TEST_ASSERT_EQUAL(200u, s_ctx.headers[2].status_code);
}

void test_http_error_and_redirect_statuses_are_successful_transactions(void)
{
    fixture_assert_response("HTTP/1.1 404 Not Found\r\nContent-Length: 7\r\n\r\nmissing", 404u, "missing");
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_deinit(&s_ctx.client));
    s_ctx.body_size = 0u;
    s_ctx.rx_size = 0u;
    s_ctx.rx_offset = 0u;
    fixture_assert_response("HTTP/1.1 302 Found\r\nLocation: /new.bin\r\nContent-Length: 0\r\n\r\n", 302u, "");
    TEST_ASSERT_EQUAL_STRING("Location", s_ctx.headers[1].name);
    TEST_ASSERT_EQUAL_STRING("/new.bin", s_ctx.headers[1].value);
}

void test_optional_progress_and_header_observer_are_not_required(void)
{
    s_ctx.config.transport.progress = NULL;
    s_ctx.config.header_cb = NULL;
    fixture_assert_response("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", 200u, "ok");
    TEST_ASSERT_EQUAL(0u, s_ctx.progress_calls);
    TEST_ASSERT_EQUAL(0u, s_ctx.header_calls);
}

void test_temporary_no_data_is_not_response_completion(void)
{
    fixture_init_submit(fixture_request());
    fixture_send_request();
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_HTTP_RECEIVING, fixture_status().state);
    fixture_queue("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    TEST_ASSERT_EQUAL(ESY_HTTP_COMPLETE, fixture_drive().state);
}

void test_body_backpressure_retries_identical_bytes_without_reading_or_progress(void)
{
    esy_http_client_status_t status;
    unsigned progress_calls;
    unsigned recv_calls;
    unsigned calls;
    s_ctx.body_busy_count = 3u;
    fixture_init_submit(fixture_request());
    fixture_queue("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
    fixture_send_request();
    for (calls = 0u; calls < 16u && s_ctx.body_calls == 0u; ++calls) {
        TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_process(&s_ctx.client));
    }
    TEST_ASSERT_EQUAL(1u, s_ctx.body_calls);
    TEST_ASSERT_EQUAL(ESY_HTTP_RECEIVING, fixture_status().state);
    progress_calls = s_ctx.progress_calls;
    recv_calls = s_ctx.recv_calls;
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(progress_calls, s_ctx.progress_calls);
    TEST_ASSERT_EQUAL(recv_calls, s_ctx.recv_calls);
    TEST_ASSERT_EQUAL(0u, fixture_status().body_bytes);
    status = fixture_drive();
    TEST_ASSERT_EQUAL(ESY_HTTP_COMPLETE, status.state);
    TEST_ASSERT_EQUAL(4u, s_ctx.body_calls);
    TEST_ASSERT_EQUAL(5u, status.body_bytes);
    TEST_ASSERT_EQUAL_MEMORY("hello", s_ctx.body, 5u);
}

void test_mutating_reentry_is_busy_but_status_is_callback_safe(void)
{
    s_ctx.test_reentry = true;
    fixture_assert_response("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", 200u, "ok");
    TEST_ASSERT_GREATER_THAN(0u, s_ctx.header_calls);
    TEST_ASSERT_GREATER_THAN(0u, s_ctx.body_calls);
}

void test_body_consumer_failure_is_sticky_and_not_retried(void)
{
    unsigned calls;
    s_ctx.body_result = ESY_ERR_HASH_FAIL;
    fixture_init_submit(fixture_request());
    fixture_queue("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
    TEST_ASSERT_EQUAL(ESY_HTTP_FAILED, fixture_drive().state);
    TEST_ASSERT_EQUAL(ESY_ERR_HASH_FAIL, fixture_status().error);
    calls = s_ctx.body_calls;
    TEST_ASSERT_EQUAL(ESY_ERR_HASH_FAIL, esy_http_client_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(calls, s_ctx.body_calls);
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_cancel(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_HTTP_FAILED, fixture_status().state);
}

void test_header_rejection_is_terminal_and_busy_is_not_retried(void)
{
    s_ctx.header_result = ESY_ERR_BUSY;
    fixture_init_submit(fixture_request());
    fixture_queue("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
    TEST_ASSERT_EQUAL(ESY_HTTP_FAILED, fixture_drive().state);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, fixture_status().error);
    TEST_ASSERT_EQUAL(1u, s_ctx.header_calls);
    TEST_ASSERT_EQUAL(0u, s_ctx.body_calls);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_http_client_process(&s_ctx.client));
}

void test_close_delimited_response_is_rejected_before_body_delivery(void)
{
    fixture_init_submit(fixture_request());
    fixture_queue("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nambiguous");
    TEST_ASSERT_EQUAL(ESY_HTTP_FAILED, fixture_drive().state);
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, fixture_status().error);
    TEST_ASSERT_EQUAL(0u, s_ctx.body_calls);
}

void test_unsupported_transfer_content_coding_and_upgrade_fail_closed(void)
{
    static const char *const k_responses[] = {
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 3\r\n\r\nzip",
        "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n"
    };
    unsigned i;
    for (i = 0u; i < sizeof(k_responses) / sizeof(k_responses[0]); ++i) {
        fixture_init_submit(fixture_request());
        fixture_queue(k_responses[i]);
        TEST_ASSERT_EQUAL(ESY_HTTP_FAILED, fixture_drive().state);
        TEST_ASSERT_EQUAL(0u, s_ctx.body_calls);
        TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_deinit(&s_ctx.client));
        s_ctx.rx_size = 0u;
        s_ctx.rx_offset = 0u;
        s_ctx.header_count = 0u;
    }
}

void test_fragmented_non_http_protocols_are_rejected_for_get_and_head(void)
{
    static const char *const k_responses[] = {
        "RTSP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n",
        "ICE/1.0 200 OK\r\nContent-Length: 0\r\n\r\n"
    };
    static const esy_http_method_t k_methods[] = {ESY_HTTP_GET, ESY_HTTP_HEAD};
    unsigned protocol;
    unsigned method;
    s_ctx.recv_chunk = 1u;
    for (protocol = 0u; protocol < sizeof(k_responses) / sizeof(k_responses[0]); ++protocol) {
        for (method = 0u; method < sizeof(k_methods) / sizeof(k_methods[0]); ++method) {
            esy_http_request_t request = fixture_request();
            request.method = k_methods[method];
            fixture_init_submit(request);
            fixture_queue(k_responses[protocol]);
            TEST_ASSERT_EQUAL(ESY_HTTP_FAILED, fixture_drive().state);
            TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, fixture_status().error);
            TEST_ASSERT_EQUAL(0u, s_ctx.body_calls);
            TEST_ASSERT_EQUAL(0u, s_ctx.header_calls);
            TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_deinit(&s_ctx.client));
            s_ctx.rx_size = 0u;
            s_ctx.rx_offset = 0u;
        }
    }
}

void test_ambiguous_malformed_and_conflicting_response_framing_is_rejected(void)
{
    static const char *const k_responses[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!",
        "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\nx",
        "HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551616\r\n\r\nx",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\ng\r\nx\r\n0\r\n\r\n",
        "HTTX/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 200 OK\rContent-Length: 0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding:\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"
    };
    unsigned i;
    for (i = 0u; i < sizeof(k_responses) / sizeof(k_responses[0]); ++i) {
        fixture_init_submit(fixture_request());
        fixture_queue(k_responses[i]);
        TEST_ASSERT_EQUAL(ESY_HTTP_FAILED, fixture_drive().state);
        TEST_ASSERT_NOT_EQUAL(ESY_OK, fixture_status().error);
        TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_deinit(&s_ctx.client));
        s_ctx.rx_size = 0u;
        s_ctx.rx_offset = 0u;
        s_ctx.header_count = 0u;
    }
}

void test_framing_trailer_is_rejected_before_its_notification(void)
{
    fixture_init_submit(fixture_request());
    fixture_queue("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                  "0\r\nContent-Length: 0\r\n\r\n");
    TEST_ASSERT_EQUAL(ESY_HTTP_FAILED, fixture_drive().state);
    TEST_ASSERT_EQUAL(1u, s_ctx.header_count);
}

void test_extra_bytes_in_final_response_input_are_not_parsed_as_another_response(void)
{
    s_ctx.resources.receive_buffer_size = 64u;
    fixture_init_submit(fixture_request());
    fixture_queue("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nxjunk");
    TEST_ASSERT_EQUAL(ESY_HTTP_FAILED, fixture_drive().state);
}

void test_header_workspace_and_aggregate_header_limits_fail_without_overflow(void)
{
    esy_http_client_status_t status;
    s_ctx.resources.header_buffer_size = 8u;
    fixture_init_submit(fixture_request());
    fixture_queue("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    status = fixture_drive();
    TEST_ASSERT_EQUAL(ESY_HTTP_FAILED, status.state);
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, status.error);
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_deinit(&s_ctx.client));
    s_ctx.resources.header_buffer_size = sizeof(s_ctx.storage.header_buffer);
    s_ctx.config.max_header_bytes = 10u;
    s_ctx.rx_size = 0u;
    s_ctx.rx_offset = 0u;
    fixture_init_submit(fixture_request());
    fixture_queue("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    status = fixture_drive();
    TEST_ASSERT_EQUAL(ESY_HTTP_FAILED, status.state);
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, status.error);
}

void test_declared_and_chunked_body_limits_are_enforced(void)
{
    s_ctx.config.max_body_bytes = 4u;
    fixture_init_submit(fixture_request());
    fixture_queue("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
    TEST_ASSERT_EQUAL(ESY_HTTP_FAILED, fixture_drive().state);
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, fixture_status().error);
    TEST_ASSERT_EQUAL(0u, s_ctx.body_calls);
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_deinit(&s_ctx.client));
    s_ctx.rx_size = 0u;
    s_ctx.rx_offset = 0u;
    fixture_init_submit(fixture_request());
    fixture_queue("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
    TEST_ASSERT_EQUAL(ESY_HTTP_FAILED, fixture_drive().state);
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, fixture_status().error);
}

void test_truncated_length_response_does_not_complete_on_transport_close(void)
{
    unsigned i;
    fixture_init_submit(fixture_request());
    fixture_queue("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort");
    fixture_send_request();
    for (i = 0u; i < 16u && s_ctx.rx_offset != s_ctx.rx_size; ++i) {
        TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_process(&s_ctx.client));
    }
    TEST_ASSERT_EQUAL(ESY_HTTP_RECEIVING, fixture_status().state);
    s_ctx.recv_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_http_client_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_HTTP_FAILED, fixture_status().state);
}

void test_missing_terminal_chunk_times_out_instead_of_completing(void)
{
    unsigned i;
    fixture_init_submit(fixture_request());
    fixture_queue("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n");
    fixture_send_request();
    for (i = 0u; i < 16u && s_ctx.rx_offset != s_ctx.rx_size; ++i) {
        TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_process(&s_ctx.client));
    }
    TEST_ASSERT_EQUAL(ESY_HTTP_RECEIVING, fixture_status().state);
    s_ctx.now += s_ctx.config.idle_timeout_us;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_http_client_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_HTTP_FAILED, fixture_status().state);
}

void test_request_rejects_injection_invalid_method_and_invalid_pointer_lengths(void)
{
    esy_http_request_t request;
    unsigned variant;
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_init(&s_ctx.client, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_http_client_submit(&s_ctx.client, NULL));
    for (variant = 0u; variant < 10u; ++variant) {
        request = fixture_request();
        switch (variant) {
        case 0u: request.method = (esy_http_method_t)99; break;
        case 1u: request.host = fixture_string("bad\r\nInjected: yes"); break;
        case 2u: request.host.data = NULL; break;
        case 3u: request.host.length = 0u; break;
        case 4u: request.target = fixture_string("https://example.test/"); break;
        case 5u: request.target = fixture_string("/bad path"); break;
        case 6u: request.target = fixture_string("/path#fragment"); break;
        case 7u: request.target.data = NULL; break;
        case 8u: request.header_count = 1u; break;
        default: request.body_size = 1u; break;
        }
        TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_http_client_submit(&s_ctx.client, &request));
        TEST_ASSERT_EQUAL(ESY_HTTP_IDLE, fixture_status().state);
    }
    TEST_ASSERT_EQUAL(0u, s_ctx.send_calls);
}

void test_custom_headers_reject_reserved_names_and_invalid_tokens(void)
{
    static const char *const k_names[] = {
        "Host", "CONTENT-length", "Connection", "Transfer-Encoding", "Expect",
        "Upgrade", "Trailer", "TE", "Bad Name", "Bad:Name", ""
    };
    esy_http_request_header_t header;
    esy_http_request_t request = fixture_request();
    unsigned i;
    request.headers = &header;
    request.header_count = 1u;
    header.value = fixture_string("value");
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_init(&s_ctx.client, &s_ctx.config));
    for (i = 0u; i < sizeof(k_names) / sizeof(k_names[0]); ++i) {
        header.name = fixture_string(k_names[i]);
        TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_http_client_submit(&s_ctx.client, &request));
    }
    header.name = fixture_string("X-Test");
    header.value = fixture_string("bad\nvalue");
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_http_client_submit(&s_ctx.client, &request));
    header.value.data = NULL;
    header.value.length = 1u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_http_client_submit(&s_ctx.client, &request));
}

void test_request_capacity_and_clock_failure_leave_idle_and_allow_retry(void)
{
    esy_http_request_t request = fixture_request();
    s_ctx.resources.request_buffer_size = 8u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_init(&s_ctx.client, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_http_client_submit(&s_ctx.client, &request));
    TEST_ASSERT_EQUAL(ESY_HTTP_IDLE, fixture_status().state);
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_deinit(&s_ctx.client));
    s_ctx.resources.request_buffer_size = sizeof(s_ctx.storage.request_buffer);
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_init(&s_ctx.client, &s_ctx.config));
    s_ctx.clock_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_http_client_submit(&s_ctx.client, &request));
    TEST_ASSERT_EQUAL(ESY_HTTP_IDLE, fixture_status().state);
    s_ctx.clock_result = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_submit(&s_ctx.client, &request));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_http_client_submit(&s_ctx.client, &request));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_http_client_deinit(&s_ctx.client));
}

void test_request_data_and_status_output_cannot_alias_client_workspaces(void)
{
    esy_http_request_t request = fixture_request();
    esy_http_request_header_t header;
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_init(&s_ctx.client, &s_ctx.config));
    request.body = s_ctx.storage.body_buffer;
    request.body_size = 1u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_http_client_submit(&s_ctx.client, &request));
    request = fixture_request();
    memcpy(s_ctx.storage.receive_buffer, "/valid", 6u);
    request.target.data = (const char *)s_ctx.storage.receive_buffer;
    request.target.length = 6u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_http_client_submit(&s_ctx.client, &request));
    request = fixture_request();
    memcpy(s_ctx.storage.request_buffer, "example.test", 12u);
    request.host.data = (const char *)s_ctx.storage.request_buffer;
    request.host.length = 12u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_http_client_submit(&s_ctx.client, &request));
    request = fixture_request();
    header.name = fixture_string("X-Test");
    memcpy(s_ctx.storage.header_buffer, "value", 5u);
    header.value.data = (const char *)s_ctx.storage.header_buffer;
    header.value.length = 5u;
    request.headers = &header;
    request.header_count = 1u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_http_client_submit(&s_ctx.client, &request));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                      esy_http_client_get_status(&s_ctx.client,
                        (esy_http_client_status_t *)(void *)&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_HTTP_IDLE, fixture_status().state);
}

void test_status_output_cannot_overwrite_any_workspace_while_active(void)
{
    uint8_t *const workspaces[] = {
        s_ctx.storage.request_buffer, s_ctx.storage.receive_buffer,
        s_ctx.storage.body_buffer, s_ctx.storage.header_buffer
    };
    uint8_t original[sizeof(esy_http_client_status_t)];
    unsigned i;
    fixture_init_submit(fixture_request());
    for (i = 0u; i < sizeof(workspaces) / sizeof(workspaces[0]); ++i) {
        memcpy(original, workspaces[i], sizeof(original));
        TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                          esy_http_client_get_status(&s_ctx.client,
                            (esy_http_client_status_t *)(void *)workspaces[i]));
        TEST_ASSERT_EQUAL_MEMORY(original, workspaces[i], sizeof(original));
    }
    TEST_ASSERT_EQUAL(ESY_HTTP_SENDING, fixture_status().state);
    fixture_queue("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    TEST_ASSERT_EQUAL(ESY_HTTP_COMPLETE, fixture_drive().state);
    TEST_ASSERT_EQUAL_MEMORY("ok", s_ctx.body, 2u);
}

void test_progress_send_receive_and_clock_errors_are_sticky(void)
{
    esy_http_client_status_t status;
    unsigned variant;
    for (variant = 0u; variant < 4u; ++variant) {
        fixture_init_submit(fixture_request());
        if (variant == 0u) {
            s_ctx.progress_result = ESY_ERR_IO;
        } else if (variant == 1u) {
            s_ctx.send_result = ESY_ERR_IO;
        } else if (variant == 2u) {
            fixture_send_request();
            s_ctx.recv_result = ESY_ERR_IO;
        } else {
            s_ctx.clock_result = ESY_ERR_IO;
        }
        TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_http_client_process(&s_ctx.client));
        status = fixture_status();
        TEST_ASSERT_EQUAL(ESY_HTTP_FAILED, status.state);
        TEST_ASSERT_EQUAL(ESY_ERR_IO, status.error);
        TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_http_client_process(&s_ctx.client));
        TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_deinit(&s_ctx.client));
        s_ctx.progress_result = ESY_OK;
        s_ctx.send_result = ESY_OK;
        s_ctx.recv_result = ESY_OK;
        s_ctx.clock_result = ESY_OK;
    }
}

void test_provider_oversized_transfer_counts_fail_closed(void)
{
    fixture_init_submit(fixture_request());
    s_ctx.send_oversize = true;
    TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_http_client_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_HTTP_FAILED, fixture_status().state);
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_deinit(&s_ctx.client));
    s_ctx.send_oversize = false;
    fixture_init_submit(fixture_request());
    fixture_send_request();
    s_ctx.recv_oversize = true;
    TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_http_client_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_HTTP_FAILED, fixture_status().state);
}

void test_idle_timeout_crosses_uint64_wrap_and_expires_at_exact_boundary(void)
{
    s_ctx.now = UINT64_MAX - 50u;
    s_ctx.config.idle_timeout_us = 100u;
    s_ctx.send_zero_count = UINT_MAX;
    fixture_init_submit(fixture_request());
    s_ctx.now += 99u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_process(&s_ctx.client));
    s_ctx.now += 1u;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_http_client_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_HTTP_FAILED, fixture_status().state);
    s_ctx.now += 1u;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_http_client_process(&s_ctx.client));
}

void test_total_timeout_is_not_renewed_by_regular_send_progress(void)
{
    s_ctx.config.total_timeout_us = 100u;
    s_ctx.config.idle_timeout_us = 1000u;
    s_ctx.send_chunk = 1u;
    fixture_init_submit(fixture_request());
    s_ctx.now = 40u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_process(&s_ctx.client));
    s_ctx.now = 80u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_process(&s_ctx.client));
    s_ctx.now = 100u;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_http_client_process(&s_ctx.client));
}

void test_body_backpressure_does_not_extend_idle_deadline(void)
{
    unsigned count;
    s_ctx.body_busy_count = UINT_MAX;
    s_ctx.config.idle_timeout_us = 100u;
    fixture_init_submit(fixture_request());
    fixture_queue("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
    fixture_send_request();
    for (count = 0u; count < 16u && s_ctx.body_calls == 0u; ++count) {
        TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_process(&s_ctx.client));
    }
    s_ctx.now = 99u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_process(&s_ctx.client));
    s_ctx.now = 100u;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_http_client_process(&s_ctx.client));
}

void test_provider_and_callback_elapsed_time_count_toward_deadline(void)
{
    unsigned count;
    s_ctx.config.idle_timeout_us = 100u;
    s_ctx.provider_advance_us = 100u;
    fixture_init_submit(fixture_request());
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_http_client_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_deinit(&s_ctx.client));
    s_ctx.provider_advance_us = 0u;
    s_ctx.callback_advance_us = 100u;
    s_ctx.config.header_cb = NULL;
    fixture_init_submit(fixture_request());
    fixture_queue("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
    fixture_send_request();
    for (count = 0u; count < 16u && fixture_status().state != ESY_HTTP_FAILED; ++count) {
        (void)esy_http_client_process(&s_ctx.client);
    }
    TEST_ASSERT_EQUAL(ESY_HTTP_FAILED, fixture_status().state);
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, fixture_status().error);
    TEST_ASSERT_EQUAL(5u, s_ctx.body_size);
    TEST_ASSERT_EQUAL(5u, fixture_status().body_bytes);
}

void test_header_callback_timeout_takes_precedence_over_its_rejection(void)
{
    s_ctx.config.idle_timeout_us = 100u;
    s_ctx.callback_advance_us = 100u;
    s_ctx.header_result = ESY_ERR_BUSY;
    fixture_init_submit(fixture_request());
    fixture_queue("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
    TEST_ASSERT_EQUAL(ESY_HTTP_FAILED, fixture_drive().state);
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, fixture_status().error);
    TEST_ASSERT_EQUAL(1u, s_ctx.header_calls);
    TEST_ASSERT_EQUAL(0u, s_ctx.body_calls);
}

void test_maximum_timeout_budget_and_longest_observation_boundary(void)
{
    s_ctx.config.total_timeout_us = UINT32_MAX;
    s_ctx.config.idle_timeout_us = UINT32_MAX;
    s_ctx.send_zero_count = UINT_MAX;
    fixture_init_submit(fixture_request());
    s_ctx.now = (uint64_t)UINT32_MAX - 1u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_process(&s_ctx.client));
    s_ctx.now += 1u;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_http_client_process(&s_ctx.client));
}

void test_complete_and_cancelled_waits_stay_retired_across_full_clock_cycles(void)
{
    unsigned clock_calls;
    esy_http_request_t request = fixture_request();
    fixture_assert_response("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", 200u, "ok");
    clock_calls = s_ctx.clock_calls;
    /* The same sample represents an elapsed complete modulo-2^64 clock cycle. */
    s_ctx.now = 0u;
    s_ctx.clock_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(clock_calls, s_ctx.clock_calls);
    TEST_ASSERT_EQUAL(ESY_HTTP_COMPLETE, fixture_status().state);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_http_client_submit(&s_ctx.client, &request));
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_cancel(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_HTTP_COMPLETE, fixture_status().state);
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_deinit(&s_ctx.client));
    s_ctx.clock_result = ESY_OK;
    fixture_init_submit(request);
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_cancel(&s_ctx.client));
    clock_calls = s_ctx.clock_calls;
    s_ctx.now = 0u;
    s_ctx.clock_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_http_client_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(clock_calls, s_ctx.clock_calls);
    TEST_ASSERT_EQUAL(ESY_HTTP_CANCELLED, fixture_status().state);
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_cancel(&s_ctx.client));
}

void test_cancel_active_or_idle_is_idempotent_and_reinit_arms_fresh_waits(void)
{
    unsigned progress_calls;
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_init(&s_ctx.client, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_cancel(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_HTTP_CANCELLED, fixture_status().state);
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_deinit(&s_ctx.client));
    s_ctx.config.idle_timeout_us = 100u;
    s_ctx.now = 1000u;
    s_ctx.send_zero_count = UINT_MAX;
    fixture_init_submit(fixture_request());
    progress_calls = s_ctx.progress_calls;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_http_client_deinit(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_cancel(&s_ctx.client));
    TEST_ASSERT_EQUAL(progress_calls, s_ctx.progress_calls);
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_cancel(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_deinit(&s_ctx.client));
    s_ctx.now = 2000u;
    fixture_init_submit(fixture_request());
    s_ctx.now = 2099u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_http_client_process(&s_ctx.client));
    s_ctx.now = 2100u;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_http_client_process(&s_ctx.client));
}
