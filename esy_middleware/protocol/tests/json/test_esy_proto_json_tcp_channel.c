/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"

#include <limits.h>
#include <string.h>

#include "esy_middleware/protocol/esy_proto_json_tcp_channel.h"

#define TEST_MESSAGE_BYTES 128u
#define TEST_WIRE_BYTES 1024u
#define TEST_MESSAGE_COUNT 8u

typedef struct {
    esy_proto_json_tcp_channel_t channel;
    esy_proto_json_tcp_channel_config_t config;
    esy_proto_json_tcp_channel_static_resources_t resources;
    uint8_t rx_buffer[TEST_MESSAGE_BYTES];
    uint8_t tx_buffer[TEST_MESSAGE_BYTES + ESY_PROTO_JSON_TCP_HEADER_BYTES];
    esy_network_connection_status_t peer;
    uint8_t rx_wire[TEST_WIRE_BYTES];
    size_t rx_length;
    size_t rx_offset;
    uint8_t tx_wire[TEST_WIRE_BYTES];
    size_t tx_length;
    size_t send_chunk;
    size_t recv_chunk;
    size_t largest_send;
    unsigned sends;
    unsigned receives;
    unsigned progresses;
    unsigned statuses;
    esy_err_t status_result;
    esy_err_t progress_result;
    int32_t send_error;
    int32_t recv_error;
    bool send_zero;
    bool recv_zero;
    bool send_overreport;
    bool recv_overreport;
    bool busy_after_send;
    bool busy_after_recv;
    bool session_after_send;
    bool session_after_recv;
    bool session_after_progress;
    bool immediate_ack;
    unsigned messages;
    char documents[TEST_MESSAGE_COUNT][TEST_MESSAGE_BYTES + 1u];
    esy_json_type_t types[TEST_MESSAGE_COUNT];
    bool callback_echo;
    bool callback_reentry;
    esy_err_t callback_send_result;
} json_tcp_test_ctx_t;

static json_tcp_test_ctx_t s_ctx;

static esy_err_t peer_status(void *context, esy_network_connection_status_t *status) {
    json_tcp_test_ctx_t *ctx = context;
    ++ctx->statuses;
    if (ctx->status_result != ESY_OK) return ctx->status_result;
    *status = ctx->peer;
    status->rx_available = (uint32_t)(ctx->rx_length - ctx->rx_offset);
    return ESY_OK;
}

static int32_t peer_send(void *context, const void *data, size_t size) {
    json_tcp_test_ctx_t *ctx = context;
    ++ctx->sends;
    if (ctx->send_error != ESY_OK) return ctx->send_error;
    if (ctx->send_zero) return 0;
    if (ctx->send_overreport) return (int32_t)size + 1;
    if (size > ctx->send_chunk) size = ctx->send_chunk;
    TEST_ASSERT_LESS_OR_EQUAL_size_t(sizeof(ctx->tx_wire), ctx->tx_length + size);
    memcpy(ctx->tx_wire + ctx->tx_length, data, size);
    ctx->tx_length += size;
    if (size > ctx->largest_send) ctx->largest_send = size;
    if (!ctx->immediate_ack) ctx->peer.unacked_bytes += (uint32_t)size;
    if (ctx->busy_after_send) {
        ctx->busy_after_send = false;
        ctx->status_result = ESY_ERR_BUSY;
    }
    if (ctx->session_after_send) {
        ctx->session_after_send = false;
        ++ctx->peer.session_id;
    }
    return (int32_t)size;
}

static int32_t peer_recv(void *context, void *data, size_t size) {
    json_tcp_test_ctx_t *ctx = context;
    ++ctx->receives;
    if (ctx->recv_error != ESY_OK) return ctx->recv_error;
    if (ctx->recv_zero) return 0;
    if (ctx->recv_overreport) return (int32_t)size + 1;
    if (size > ctx->recv_chunk) size = ctx->recv_chunk;
    if (size > ctx->rx_length - ctx->rx_offset) size = ctx->rx_length - ctx->rx_offset;
    memcpy(data, ctx->rx_wire + ctx->rx_offset, size);
    ctx->rx_offset += size;
    if (size && ctx->busy_after_recv) {
        ctx->busy_after_recv = false;
        ctx->status_result = ESY_ERR_BUSY;
    }
    if (size && ctx->session_after_recv) {
        ctx->session_after_recv = false;
        ++ctx->peer.session_id;
    }
    return (int32_t)size;
}

static esy_err_t peer_progress(void *context) {
    json_tcp_test_ctx_t *ctx = context;
    ++ctx->progresses;
    if (ctx->session_after_progress) {
        ctx->session_after_progress = false;
        ++ctx->peer.session_id;
    }
    return ctx->progress_result;
}

static void message_received(esy_proto_json_tcp_channel_t *channel,
                              const esy_json_value_t *root, void *context) {
    json_tcp_test_ctx_t *ctx = context;
    TEST_ASSERT_EQUAL_PTR(&ctx->channel, channel);
    TEST_ASSERT_LESS_THAN_UINT(TEST_MESSAGE_COUNT, ctx->messages);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(TEST_MESSAGE_BYTES, root->length);
    TEST_ASSERT_TRUE((uintptr_t)root->data >= (uintptr_t)ctx->rx_buffer);
    TEST_ASSERT_TRUE((uintptr_t)root->data + root->length <=
                     (uintptr_t)ctx->rx_buffer + sizeof(ctx->rx_buffer));
    memcpy(ctx->documents[ctx->messages], root->data, root->length);
    ctx->documents[ctx->messages][root->length] = '\0';
    ctx->types[ctx->messages++] = root->type;
    if (ctx->callback_reentry) {
        unsigned before = ctx->statuses;
        esy_proto_json_tcp_channel_status_t status;
        TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_get_status(channel, &status));
        TEST_ASSERT_EQUAL_UINT(before, ctx->statuses);
        TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_json_tcp_channel_poll(channel));
        TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_json_tcp_channel_init(channel, &ctx->config));
        TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_json_tcp_channel_deinit(channel));
    }
    if (ctx->callback_echo) {
        ctx->callback_send_result = esy_proto_json_tcp_channel_send(channel, root->data, root->length);
    }
}

void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.peer.state = ESY_NETWORK_CONNECTION_OPEN;
    s_ctx.peer.session_id = 17u;
    s_ctx.peer.send_capacity = UINT32_MAX;
    s_ctx.send_chunk = SIZE_MAX;
    s_ctx.recv_chunk = SIZE_MAX;
    s_ctx.resources.rx_buffer = s_ctx.rx_buffer;
    s_ctx.resources.rx_buffer_size = sizeof(s_ctx.rx_buffer);
    s_ctx.resources.tx_buffer = s_ctx.tx_buffer;
    s_ctx.resources.tx_buffer_size = sizeof(s_ctx.tx_buffer);
    s_ctx.config.connection.stream.send = peer_send;
    s_ctx.config.connection.stream.recv = peer_recv;
    s_ctx.config.connection.stream.progress = peer_progress;
    s_ctx.config.connection.stream.context = &s_ctx;
    s_ctx.config.connection.get_status = peer_status;
    s_ctx.config.static_resources = &s_ctx.resources;
    s_ctx.config.on_message = message_received;
    s_ctx.config.user_ctx = &s_ctx;
}

void tearDown(void) {}

static void start_channel(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_init(&s_ctx.channel, &s_ctx.config));
}

static esy_proto_json_tcp_channel_status_t channel_status(void) {
    esy_proto_json_tcp_channel_status_t result;
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_get_status(&s_ctx.channel, &result));
    return result;
}

static void append_header(uint32_t length) {
    TEST_ASSERT_LESS_OR_EQUAL_size_t(TEST_WIRE_BYTES, s_ctx.rx_length + 4u);
    s_ctx.rx_wire[s_ctx.rx_length++] = (uint8_t)(length >> 24u);
    s_ctx.rx_wire[s_ctx.rx_length++] = (uint8_t)(length >> 16u);
    s_ctx.rx_wire[s_ctx.rx_length++] = (uint8_t)(length >> 8u);
    s_ctx.rx_wire[s_ctx.rx_length++] = (uint8_t)length;
}

static void append_message(const char *document) {
    size_t length = strlen(document);
    append_header((uint32_t)length);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(TEST_WIRE_BYTES, s_ctx.rx_length + length);
    memcpy(s_ctx.rx_wire + s_ctx.rx_length, document, length);
    s_ctx.rx_length += length;
}

static void assert_sticky(esy_err_t expected) {
    TEST_ASSERT_EQUAL(expected, channel_status().error);
    unsigned before = s_ctx.statuses + s_ctx.progresses + s_ctx.sends + s_ctx.receives;
    TEST_ASSERT_EQUAL(expected, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL(expected, esy_proto_json_tcp_channel_send(&s_ctx.channel, "null", 4));
    TEST_ASSERT_EQUAL_UINT(before, s_ctx.statuses + s_ctx.progresses + s_ctx.sends + s_ctx.receives);
}

void test_init_observes_peer_without_io_and_deinit_is_idempotent(void) {
    start_channel();
    esy_proto_json_tcp_channel_status_t status = channel_status();
    TEST_ASSERT_EQUAL_UINT64(17, status.session_id);
    TEST_ASSERT_FALSE(status.tx_pending);
    TEST_ASSERT_FALSE(status.peer_drained);
    TEST_ASSERT_EQUAL(ESY_OK, status.error);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.statuses);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.sends + s_ctx.receives + s_ctx.progresses);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_json_tcp_channel_init(&s_ctx.channel, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_deinit(&s_ctx.channel));
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_deinit(&s_ctx.channel));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    start_channel();
}

void test_init_rejects_missing_resources_and_required_callbacks_before_provider_calls(void) {
    esy_proto_json_tcp_channel_config_t config = s_ctx.config;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_init(NULL, &config));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_init(&s_ctx.channel, NULL));
    config.static_resources = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_init(&s_ctx.channel, &config));
    config = s_ctx.config;
    config.on_message = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_init(&s_ctx.channel, &config));
    config = s_ctx.config;
    config.connection.stream.send = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_init(&s_ctx.channel, &config));
    config = s_ctx.config;
    config.connection.stream.recv = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_init(&s_ctx.channel, &config));
    config = s_ctx.config;
    config.connection.get_status = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_init(&s_ctx.channel, &config));
    s_ctx.resources.rx_buffer_size = 0;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_init(&s_ctx.channel, &s_ctx.config));
    s_ctx.resources.rx_buffer_size = sizeof(s_ctx.rx_buffer);
    s_ctx.resources.tx_buffer_size = 4;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_init(&s_ctx.channel, &s_ctx.config));
    s_ctx.resources.tx_buffer_size = sizeof(s_ctx.tx_buffer);
    s_ctx.resources.rx_buffer = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_init(&s_ctx.channel, &s_ctx.config));
    s_ctx.resources.rx_buffer = s_ctx.rx_buffer;
    s_ctx.resources.tx_buffer = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_init(&s_ctx.channel, &s_ctx.config));
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.statuses);
}

void test_init_rejects_overlapping_resource_ranges_before_provider_observation(void) {
    uint8_t *original_rx = s_ctx.resources.rx_buffer;
    uint8_t *original_tx = s_ctx.resources.tx_buffer;
    s_ctx.resources.tx_buffer = s_ctx.rx_buffer + 1;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_init(&s_ctx.channel, &s_ctx.config));
    s_ctx.resources.tx_buffer = original_tx;
    s_ctx.resources.rx_buffer = s_ctx.channel.storage.bytes;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_init(&s_ctx.channel, &s_ctx.config));
    s_ctx.resources.rx_buffer = (uint8_t *)&s_ctx.config;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_init(&s_ctx.channel, &s_ctx.config));
    s_ctx.resources.rx_buffer = original_rx;
    s_ctx.resources.tx_buffer = (uint8_t *)&s_ctx.resources;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_init(&s_ctx.channel, &s_ctx.config));
    s_ctx.resources.tx_buffer = original_tx;
    s_ctx.resources.rx_buffer = (uint8_t *)(UINTPTR_MAX - 1u);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_init(&s_ctx.channel, &s_ctx.config));
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.statuses);
}

void test_init_failures_preserve_zero_handle_and_allow_retry(void) {
    s_ctx.peer.session_id = 0;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_json_tcp_channel_init(&s_ctx.channel, &s_ctx.config));
    s_ctx.peer.session_id = 17;
    s_ctx.peer.state = ESY_NETWORK_CONNECTION_INACTIVE;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_json_tcp_channel_init(&s_ctx.channel, &s_ctx.config));
    s_ctx.peer.state = ESY_NETWORK_CONNECTION_OPEN;
    s_ctx.status_result = ESY_ERR_BUSY;
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_json_tcp_channel_init(&s_ctx.channel, &s_ctx.config));
    s_ctx.status_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_json_tcp_channel_init(&s_ctx.channel, &s_ctx.config));
    s_ctx.status_result = ESY_OK;
    s_ctx.config.connection.stream.progress = NULL;
    start_channel();
}

void test_send_stages_copy_and_big_endian_prefix_without_io_or_terminator(void) {
    start_channel();
    char document[] = "{\"x\":1}";
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_send(&s_ctx.channel, document, strlen(document)));
    memset(document, 'x', strlen(document));
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.sends);
    TEST_ASSERT_EQUAL_size_t(11, channel_status().tx_remaining);
    TEST_ASSERT_TRUE(channel_status().tx_pending);
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    const uint8_t expected[] = {0, 0, 0, 7, '{', '"', 'x', '"', ':', '1', '}'};
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), s_ctx.tx_length);
    TEST_ASSERT_EQUAL_MEMORY(expected, s_ctx.tx_wire, sizeof(expected));
    TEST_ASSERT_FALSE(channel_status().tx_pending);
    TEST_ASSERT_GREATER_THAN_UINT32(0, s_ctx.peer.unacked_bytes);
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_send(&s_ctx.channel, "true", 4));
}

void test_send_input_errors_do_not_poison_healthy_channel(void) {
    start_channel();
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_send(&s_ctx.channel, NULL, 1));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_send(&s_ctx.channel, "", 0));
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_proto_json_tcp_channel_send(&s_ctx.channel, "{", 1));
    char oversized[sizeof(s_ctx.tx_buffer)];
    memset(oversized, ' ', sizeof(oversized));
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_proto_json_tcp_channel_send(&s_ctx.channel, oversized, sizeof(oversized)));
    memcpy(s_ctx.tx_buffer, "null", 4);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_send(&s_ctx.channel, (char *)s_ctx.tx_buffer, 4));
    TEST_ASSERT_EQUAL(ESY_OK, channel_status().error);
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_send(&s_ctx.channel, "null", 4));
}

void test_tx_single_slot_and_byte_budget_resume_partial_prefix_without_replay(void) {
    s_ctx.config.tx_byte_budget = 3;
    s_ctx.send_chunk = 2;
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_send(&s_ctx.channel, "null", 4));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_json_tcp_channel_send(&s_ctx.channel, "true", 4));
    for (unsigned i = 0; i < 4; ++i) {
        unsigned before = s_ctx.sends;
        TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
        TEST_ASSERT_EQUAL_UINT(before + 1, s_ctx.sends);
    }
    const uint8_t expected[] = {0, 0, 0, 4, 'n', 'u', 'l', 'l'};
    TEST_ASSERT_EQUAL_MEMORY(expected, s_ctx.tx_wire, sizeof(expected));
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), s_ctx.tx_length);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(3, s_ctx.largest_send);
    TEST_ASSERT_FALSE(channel_status().tx_pending);
}

void test_tx_backpressure_and_status_busy_after_acceptance_preserve_exact_offset(void) {
    s_ctx.send_chunk = 2;
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_send(&s_ctx.channel, "null", 4));
    s_ctx.send_zero = true;
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL_size_t(8, channel_status().tx_remaining);
    s_ctx.send_zero = false;
    s_ctx.busy_after_send = true;
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL_size_t(2, s_ctx.tx_length);
    TEST_ASSERT_EQUAL_size_t(6, channel_status().tx_remaining);
    s_ctx.status_result = ESY_OK;
    for (unsigned i = 0; i < 3; ++i) TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    const uint8_t expected[] = {0, 0, 0, 4, 'n', 'u', 'l', 'l'};
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), s_ctx.tx_length);
    TEST_ASSERT_EQUAL_MEMORY(expected, s_ctx.tx_wire, sizeof(expected));
}

void test_rx_fragmented_prefix_and_body_follow_byte_budget_and_dispatch_once(void) {
    s_ctx.config.rx_byte_budget = 2;
    s_ctx.recv_chunk = 1;
    append_message("{\"a\":1}");
    start_channel();
    while (!s_ctx.messages) {
        unsigned receives = s_ctx.receives;
        unsigned progresses = s_ctx.progresses;
        size_t offset = s_ctx.rx_offset;
        TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
        TEST_ASSERT_LESS_OR_EQUAL_size_t(2, s_ctx.rx_offset - offset);
        TEST_ASSERT_LESS_OR_EQUAL_UINT(3, s_ctx.receives - receives);
        TEST_ASSERT_EQUAL_UINT(progresses + 1, s_ctx.progresses);
        TEST_ASSERT_LESS_OR_EQUAL_UINT(1, s_ctx.messages);
    }
    TEST_ASSERT_EQUAL_STRING("{\"a\":1}", s_ctx.documents[0]);
    TEST_ASSERT_EQUAL(ESY_JSON_OBJECT, s_ctx.types[0]);
    TEST_ASSERT_EQUAL_size_t(0, channel_status().rx_received);
}

void test_rx_coalesced_frames_deliver_at_most_one_each_poll_with_formatted_json(void) {
    append_message("{\n \"a\": 1\n}");
    append_message("[true,\"x\"]");
    append_message("false");
    start_channel();
    for (unsigned i = 1; i <= 3; ++i) {
        TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
        TEST_ASSERT_EQUAL_UINT(i, s_ctx.messages);
    }
    TEST_ASSERT_EQUAL_STRING("{\n \"a\": 1\n}", s_ctx.documents[0]);
    TEST_ASSERT_EQUAL(ESY_JSON_ARRAY, s_ctx.types[1]);
    TEST_ASSERT_EQUAL(ESY_JSON_BOOL, s_ctx.types[2]);
}

void test_zero_receive_means_idle_and_local_status_never_calls_provider(void) {
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    unsigned statuses = s_ctx.statuses;
    esy_proto_json_tcp_channel_status_t status = channel_status();
    TEST_ASSERT_FALSE(status.peer_drained);
    TEST_ASSERT_EQUAL_UINT(statuses, s_ctx.statuses);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.messages);
}

void test_message_callback_can_stage_rx_echo_without_losing_reply_or_reentering_lifecycle(void) {
    append_message("{\"n\":18446744073709551615}");
    s_ctx.callback_echo = true;
    s_ctx.callback_reentry = true;
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.messages);
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.callback_send_result);
    TEST_ASSERT_TRUE(channel_status().tx_pending);
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_FALSE(channel_status().tx_pending);
    TEST_ASSERT_EQUAL_size_t(s_ctx.rx_length, s_ctx.tx_length);
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.rx_wire, s_ctx.tx_wire, s_ctx.rx_length);
}

void test_receive_continues_while_prior_tx_slot_is_busy_and_reply_can_be_retried(void) {
    s_ctx.send_chunk = 2;
    s_ctx.callback_echo = true;
    append_message("1");
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_send(&s_ctx.channel, "null", 4));
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.messages);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.callback_send_result);
    TEST_ASSERT_EQUAL_size_t(6, channel_status().tx_remaining);
    for (unsigned i = 0; i < 3; ++i) TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    const uint8_t expected[] = {0, 0, 0, 4, 'n', 'u', 'l', 'l'};
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), s_ctx.tx_length);
    TEST_ASSERT_EQUAL_MEMORY(expected, s_ctx.tx_wire, sizeof(expected));
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_send(&s_ctx.channel, s_ctx.documents[0], 1));
    for (unsigned i = 0; i < 3; ++i) TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_FALSE(channel_status().tx_pending);
    TEST_ASSERT_EQUAL_size_t(13, s_ctx.tx_length);
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.rx_wire, s_ctx.tx_wire + sizeof(expected), 5);
}

void test_send_observes_fresh_peer_status_without_progress_or_io(void) {
    start_channel();
    s_ctx.status_result = ESY_ERR_BUSY;
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_json_tcp_channel_send(&s_ctx.channel, "null", 4));
    TEST_ASSERT_FALSE(channel_status().tx_pending);
    TEST_ASSERT_EQUAL(ESY_OK, channel_status().error);
    s_ctx.status_result = ESY_OK;
    ++s_ctx.peer.session_id;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_json_tcp_channel_send(&s_ctx.channel, "null", 4));
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.sends + s_ctx.receives + s_ctx.progresses);
    assert_sticky(ESY_ERR_STATE);
}

void test_receive_backpressure_preserves_unconsumed_frame(void) {
    append_message("null");
    start_channel();
    s_ctx.recv_error = ESY_ERR_BUSY;
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL(ESY_OK, channel_status().error);
    TEST_ASSERT_EQUAL_size_t(0, s_ctx.rx_offset);
    s_ctx.recv_error = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.messages);
}

void test_eof_drains_messages_and_replies_before_fresh_ack_zero_completion(void) {
    append_message("1");
    append_message("2");
    s_ctx.peer.state = ESY_NETWORK_CONNECTION_EOF;
    s_ctx.callback_echo = true;
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.messages);
    TEST_ASSERT_FALSE(channel_status().peer_drained);
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL_UINT(2, s_ctx.messages);
    TEST_ASSERT_FALSE(channel_status().peer_drained);
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_FALSE(channel_status().tx_pending);
    TEST_ASSERT_FALSE(channel_status().peer_drained);
    s_ctx.peer.unacked_bytes = 0;
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_TRUE(channel_status().peer_drained);
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_send(&s_ctx.channel, "null", 4));
    TEST_ASSERT_FALSE(channel_status().peer_drained);
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
}

void test_eof_delayed_second_reply_can_be_sent_after_local_first_reply_drain(void) {
    append_message("1");
    append_message("2");
    s_ctx.peer.state = ESY_NETWORK_CONNECTION_EOF;
    s_ctx.config.tx_byte_budget = 2;
    s_ctx.callback_echo = true;
    s_ctx.immediate_ack = true;
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.messages);
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.callback_send_result);
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL_UINT(2, s_ctx.messages);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.callback_send_result);
    TEST_ASSERT_FALSE(channel_status().peer_drained);
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_TRUE(channel_status().peer_drained);
    TEST_ASSERT_EQUAL_size_t(5, s_ctx.tx_length);
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_send(&s_ctx.channel, s_ctx.documents[1], 1));
    TEST_ASSERT_FALSE(channel_status().peer_drained);
    for (unsigned i = 0; i < 3; ++i) TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    const uint8_t expected[] = {0, 0, 0, 1, '1', 0, 0, 0, 1, '2'};
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), s_ctx.tx_length);
    TEST_ASSERT_EQUAL_MEMORY(expected, s_ctx.tx_wire, sizeof(expected));
    TEST_ASSERT_TRUE(channel_status().peer_drained);
}

void test_eof_with_incomplete_header_or_body_fails_stickily(void) {
    s_ctx.rx_wire[0] = 0;
    s_ctx.rx_length = 1;
    s_ctx.peer.state = ESY_NETWORK_CONNECTION_EOF;
    start_channel();
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    assert_sticky(ESY_ERR_FAIL);
    setUp();
    append_header(4);
    s_ctx.rx_wire[s_ctx.rx_length++] = 'n';
    s_ctx.peer.state = ESY_NETWORK_CONNECTION_EOF;
    start_channel();
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    assert_sticky(ESY_ERR_FAIL);
}

void test_zero_length_oversized_and_invalid_json_frames_are_sticky_errors(void) {
    append_header(0);
    start_channel();
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    assert_sticky(ESY_ERR_FAIL);
    setUp();
    append_header(TEST_MESSAGE_BYTES + 1);
    start_channel();
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    assert_sticky(ESY_ERR_NO_MEM);
    setUp();
    append_message("{\"a\":");
    start_channel();
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.messages);
    assert_sticky(ESY_ERR_FAIL);
}

void test_session_change_in_progress_stops_admission_and_never_replays_tx(void) {
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_send(&s_ctx.channel, "null", 4));
    s_ctx.session_after_progress = true;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.sends);
    assert_sticky(ESY_ERR_STATE);
}

void test_session_change_after_send_or_receive_is_detected_before_dispatch(void) {
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_send(&s_ctx.channel, "null", 4));
    s_ctx.session_after_send = true;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL_size_t(8, s_ctx.tx_length);
    assert_sticky(ESY_ERR_STATE);
    setUp();
    append_message("null");
    s_ctx.session_after_recv = true;
    start_channel();
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.messages);
    assert_sticky(ESY_ERR_STATE);
}

void test_disconnection_and_fatal_provider_errors_are_sticky(void) {
    start_channel();
    s_ctx.peer.state = ESY_NETWORK_CONNECTION_INACTIVE;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    assert_sticky(ESY_ERR_STATE);
    setUp();
    start_channel();
    s_ctx.progress_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    assert_sticky(ESY_ERR_IO);
    setUp();
    append_message("null");
    start_channel();
    s_ctx.recv_error = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    assert_sticky(ESY_ERR_IO);
}

void test_busy_provider_observations_preserve_channel_and_consumed_rx_offset(void) {
    append_message("null");
    s_ctx.config.rx_byte_budget = 6;
    start_channel();
    s_ctx.progress_result = ESY_ERR_BUSY;
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL(ESY_OK, channel_status().error);
    s_ctx.progress_result = ESY_OK;
    s_ctx.recv_chunk = 2;
    s_ctx.busy_after_recv = true;
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL_size_t(6, s_ctx.rx_offset);
    TEST_ASSERT_EQUAL_size_t(2, channel_status().rx_received);
    s_ctx.status_result = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.messages);
    TEST_ASSERT_EQUAL_STRING("null", s_ctx.documents[0]);
}

void test_completed_eof_frame_waits_for_fresh_status_after_busy_without_truncation(void) {
    append_message("null");
    s_ctx.peer.state = ESY_NETWORK_CONNECTION_EOF;
    s_ctx.busy_after_recv = true;
    start_channel();
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL_size_t(8, s_ctx.rx_offset);
    TEST_ASSERT_EQUAL_size_t(4, channel_status().rx_received);
    TEST_ASSERT_FALSE(channel_status().peer_drained);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.messages);
    s_ctx.status_result = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.messages);
    TEST_ASSERT_EQUAL_STRING("null", s_ctx.documents[0]);
    TEST_ASSERT_TRUE(channel_status().peer_drained);
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.messages);
}

void test_send_and_receive_overreport_are_fatal_provider_contract_errors(void) {
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_send(&s_ctx.channel, "null", 4));
    s_ctx.send_overreport = true;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    assert_sticky(ESY_ERR_IO);
    setUp();
    append_message("null");
    start_channel();
    s_ctx.recv_overreport = true;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    assert_sticky(ESY_ERR_IO);
}

void test_provider_send_busy_does_not_poison_but_fatal_send_does(void) {
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_send(&s_ctx.channel, "null", 4));
    s_ctx.send_error = ESY_ERR_BUSY;
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL(ESY_OK, channel_status().error);
    TEST_ASSERT_EQUAL_size_t(8, channel_status().tx_remaining);
    s_ctx.send_error = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_json_tcp_channel_poll(&s_ctx.channel));
    assert_sticky(ESY_ERR_IO);
}

void test_deinit_cancels_local_pending_work_without_touching_endpoint(void) {
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_send(&s_ctx.channel, "null", 4));
    unsigned before = s_ctx.statuses + s_ctx.progresses + s_ctx.sends + s_ctx.receives;
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_tcp_channel_deinit(&s_ctx.channel));
    TEST_ASSERT_EQUAL_UINT(before, s_ctx.statuses + s_ctx.progresses + s_ctx.sends + s_ctx.receives);
    TEST_ASSERT_EQUAL(ESY_NETWORK_CONNECTION_OPEN, s_ctx.peer.state);
    ++s_ctx.peer.session_id;
    start_channel();
    TEST_ASSERT_FALSE(channel_status().tx_pending);
}

void test_public_boundaries_preserve_failed_status_outputs(void) {
    esy_proto_json_tcp_channel_status_t status;
    memset(&status, 0x5a, sizeof(status));
    const esy_proto_json_tcp_channel_status_t before = status;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_json_tcp_channel_get_status(&s_ctx.channel, &status));
    TEST_ASSERT_EQUAL_MEMORY(&before, &status, sizeof(status));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_get_status(NULL, &status));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_get_status(&s_ctx.channel, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_poll(NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_send(NULL, "null", 4));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_json_tcp_channel_send(&s_ctx.channel, "null", 4));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_tcp_channel_deinit(NULL));
}
