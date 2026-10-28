/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"

#include <string.h>

#include "esy_middleware/protocol/esy_proto_json_usb_channel.h"
#include "tusb.h"

#define USB_TEST_MESSAGE_BYTES 128u
#define USB_TEST_WIRE_BYTES 256u
#define USB_TEST_MESSAGE_COUNT 8u

typedef struct {
    esy_proto_json_usb_channel_t channel;
    esy_usb_cdc_t cdc;
    esy_usb_cdc_config_t cdc_config;
    esy_proto_json_usb_channel_config_t config;
    esy_proto_json_usb_channel_static_resources_t resources;
    uint8_t rx_buffer[USB_TEST_MESSAGE_BYTES];
    uint8_t tx_buffer[USB_TEST_MESSAGE_BYTES + ESY_PROTO_JSON_USB_HEADER_BYTES];
    fake_tinyusb_cdc_itf_state_t *interface;
    uint8_t wire[USB_TEST_WIRE_BYTES];
    size_t wire_length;
    uint8_t transmitted[USB_TEST_WIRE_BYTES];
    size_t transmitted_length;
    unsigned messages;
    char documents[USB_TEST_MESSAGE_COUNT][USB_TEST_MESSAGE_BYTES + 1u];
    esy_json_type_t types[USB_TEST_MESSAGE_COUNT];
    bool callback_echo;
    bool callback_reentry;
    esy_err_t callback_send_result;
} json_usb_test_ctx_t;

static json_usb_test_ctx_t s_ctx;

static void receive_document(esy_proto_json_usb_channel_t *channel,
                              const esy_json_value_t *root, void *context) {
    json_usb_test_ctx_t *ctx = context;
    TEST_ASSERT_EQUAL_PTR(&ctx->channel, channel);
    TEST_ASSERT_LESS_THAN_UINT(USB_TEST_MESSAGE_COUNT, ctx->messages);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(USB_TEST_MESSAGE_BYTES, root->length);
    TEST_ASSERT_TRUE((uintptr_t)root->data >= (uintptr_t)ctx->rx_buffer);
    TEST_ASSERT_TRUE((uintptr_t)root->data + root->length <=
                     (uintptr_t)ctx->rx_buffer + sizeof(ctx->rx_buffer));
    memcpy(ctx->documents[ctx->messages], root->data, root->length);
    ctx->documents[ctx->messages][root->length] = '\0';
    ctx->types[ctx->messages++] = root->type;
    if (ctx->callback_reentry) {
        esy_proto_json_usb_channel_status_t status;
        unsigned pumps = ctx->interface->task_count;
        TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_usb_channel_get_status(channel, &status));
        TEST_ASSERT_EQUAL_UINT(pumps, ctx->interface->task_count);
        TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_json_usb_channel_poll(channel));
        TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_json_usb_channel_init(channel, &ctx->config));
        TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_json_usb_channel_deinit(channel));
    }
    if (ctx->callback_echo) {
        ctx->callback_send_result = esy_proto_json_usb_channel_send(channel, root->data, root->length);
    }
}

void setUp(void) {
    esy_usb_cdc_deinit(&s_ctx.cdc);
    memset(&s_ctx, 0, sizeof(s_ctx));
    fake_tinyusb_reset();
    s_ctx.interface = fake_tinyusb_cdc_get_itf(0);
    s_ctx.interface->connected = true;
    s_ctx.interface->line_state = 1;
    esy_usb_cdc_default_config(&s_ctx.cdc_config);
    s_ctx.cdc_config.discard_on_disconnect = true;
    s_ctx.resources.rx_buffer = s_ctx.rx_buffer;
    s_ctx.resources.rx_buffer_size = sizeof(s_ctx.rx_buffer);
    s_ctx.resources.tx_buffer = s_ctx.tx_buffer;
    s_ctx.resources.tx_buffer_size = sizeof(s_ctx.tx_buffer);
    s_ctx.config.cdc = &s_ctx.cdc;
    s_ctx.config.static_resources = &s_ctx.resources;
    s_ctx.config.on_message = receive_document;
    s_ctx.config.user_ctx = &s_ctx;
}

void tearDown(void) {
    (void)esy_proto_json_usb_channel_deinit(&s_ctx.channel);
    esy_usb_cdc_deinit(&s_ctx.cdc);
}

static void bind_cdc(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_usb_cdc_init(&s_ctx.cdc, &s_ctx.cdc_config));
}

static void start_channel(void) {
    bind_cdc();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_usb_channel_init(&s_ctx.channel, &s_ctx.config));
}

static esy_proto_json_usb_channel_status_t local_status(void) {
    esy_proto_json_usb_channel_status_t status;
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_usb_channel_get_status(&s_ctx.channel, &status));
    return status;
}

static void append_header(uint32_t length) {
    TEST_ASSERT_LESS_OR_EQUAL_size_t(USB_TEST_WIRE_BYTES, s_ctx.wire_length + 4u);
    s_ctx.wire[s_ctx.wire_length++] = (uint8_t)(length >> 24u);
    s_ctx.wire[s_ctx.wire_length++] = (uint8_t)(length >> 16u);
    s_ctx.wire[s_ctx.wire_length++] = (uint8_t)(length >> 8u);
    s_ctx.wire[s_ctx.wire_length++] = (uint8_t)length;
}

static void append_document(const char *document) {
    size_t length = strlen(document);
    append_header((uint32_t)length);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(USB_TEST_WIRE_BYTES, s_ctx.wire_length + length);
    memcpy(s_ctx.wire + s_ctx.wire_length, document, length);
    s_ctx.wire_length += length;
}

static void supply_wire(void) {
    fake_tinyusb_cdc_set_rx_data(0, s_ctx.wire, (uint32_t)s_ctx.wire_length);
}

static esy_err_t poll_capture(void) {
    uint32_t available = s_ctx.interface->write_available;
    uint32_t writes = s_ctx.interface->write_count;
    uint32_t pumps = s_ctx.interface->task_count;
    esy_err_t result = esy_proto_json_usb_channel_poll(&s_ctx.channel);
    TEST_ASSERT_EQUAL_UINT32(pumps, s_ctx.interface->task_count);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(writes + 1u, s_ctx.interface->write_count);
    if (s_ctx.interface->write_count != writes) {
        size_t accepted = available - s_ctx.interface->write_available;
        TEST_ASSERT_LESS_OR_EQUAL_size_t(sizeof(s_ctx.transmitted), s_ctx.transmitted_length + accepted);
        memcpy(s_ctx.transmitted + s_ctx.transmitted_length, s_ctx.interface->written_data, accepted);
        s_ctx.transmitted_length += accepted;
    }
    return result;
}

static void close_and_reopen(void) {
    s_ctx.interface->line_state = 0;
    tud_cdc_line_state_cb(0, false, false);
    s_ctx.interface->line_state = 1;
    tud_cdc_line_state_cb(0, true, false);
}

static void assert_sticky(esy_err_t expected) {
    TEST_ASSERT_EQUAL(expected, local_status().error);
    TEST_ASSERT_EQUAL(expected, esy_proto_json_usb_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL(expected, esy_proto_json_usb_channel_send(&s_ctx.channel, "null", 4));
}

void test_usb_init_borrows_existing_cdc_without_pump_or_io_and_deinit_keeps_it_bound(void) {
    start_channel();
    TEST_ASSERT_EQUAL_UINT32(1, local_status().session_id);
    TEST_ASSERT_TRUE(local_status().connected);
    TEST_ASSERT_EQUAL_UINT32(0, s_ctx.interface->task_count + s_ctx.interface->read_count +
        s_ctx.interface->write_count + s_ctx.interface->flush_count);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_json_usb_channel_init(&s_ctx.channel, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_usb_channel_deinit(&s_ctx.channel));
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_usb_channel_deinit(&s_ctx.channel));
    TEST_ASSERT_TRUE(esy_usb_cdc_connected(&s_ctx.cdc));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_json_usb_channel_poll(&s_ctx.channel));
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_usb_channel_init(&s_ctx.channel, &s_ctx.config));
}

void test_usb_init_accepts_closed_session_and_closed_send_has_no_future_peer_queue(void) {
    s_ctx.interface->connected = false;
    s_ctx.interface->line_state = 0;
    start_channel();
    TEST_ASSERT_FALSE(local_status().connected);
    TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_json_usb_channel_send(&s_ctx.channel, "null", 4));
    TEST_ASSERT_FALSE(local_status().tx_pending);
    TEST_ASSERT_EQUAL(ESY_OK, local_status().error);
    s_ctx.interface->connected = true;
    s_ctx.interface->line_state = 1;
    tud_cdc_line_state_cb(0, true, false);
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_usb_channel_send(&s_ctx.channel, "true", 4));
    TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
    const uint8_t expected[] = {0, 0, 0, 4, 't', 'r', 'u', 'e'};
    TEST_ASSERT_EQUAL_MEMORY(expected, s_ctx.transmitted, sizeof(expected));
}

void test_usb_init_requires_session_isolation_and_valid_resource_configuration(void) {
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_usb_channel_init(&s_ctx.channel, &s_ctx.config));
    s_ctx.cdc_config.discard_on_disconnect = false;
    bind_cdc();
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_usb_channel_init(&s_ctx.channel, &s_ctx.config));
    esy_usb_cdc_deinit(&s_ctx.cdc);
    s_ctx.cdc_config.discard_on_disconnect = true;
    bind_cdc();
    esy_proto_json_usb_channel_config_t config = s_ctx.config;
    config.cdc = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_usb_channel_init(&s_ctx.channel, &config));
    config = s_ctx.config;
    config.static_resources = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_usb_channel_init(&s_ctx.channel, &config));
    config = s_ctx.config;
    config.on_message = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_usb_channel_init(&s_ctx.channel, &config));
    s_ctx.resources.rx_buffer_size = 0;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_usb_channel_init(&s_ctx.channel, &s_ctx.config));
    s_ctx.resources.rx_buffer_size = sizeof(s_ctx.rx_buffer);
    s_ctx.resources.tx_buffer_size = 4;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_usb_channel_init(&s_ctx.channel, &s_ctx.config));
    TEST_ASSERT_EQUAL_UINT32(0, s_ctx.interface->task_count);
}

void test_usb_init_rejects_pairwise_overlap_including_cdc_and_nonwrapping_ranges(void) {
    bind_cdc();
    s_ctx.resources.rx_buffer = (uint8_t *)&s_ctx.cdc;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_usb_channel_init(&s_ctx.channel, &s_ctx.config));
    s_ctx.resources.rx_buffer = s_ctx.channel.storage.bytes;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_usb_channel_init(&s_ctx.channel, &s_ctx.config));
    s_ctx.resources.rx_buffer = s_ctx.rx_buffer;
    s_ctx.resources.tx_buffer = s_ctx.rx_buffer + 1;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_usb_channel_init(&s_ctx.channel, &s_ctx.config));
    s_ctx.resources.tx_buffer = (uint8_t *)&s_ctx.resources;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_usb_channel_init(&s_ctx.channel, &s_ctx.config));
    s_ctx.resources.tx_buffer = s_ctx.tx_buffer;
    s_ctx.resources.rx_buffer = (uint8_t *)(UINTPTR_MAX - 1u);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_usb_channel_init(&s_ctx.channel, &s_ctx.config));
}

void test_usb_send_copies_frame_without_io_and_rejects_bad_input_without_poisoning(void) {
    start_channel();
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_usb_channel_send(&s_ctx.channel, NULL, 1));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_usb_channel_send(&s_ctx.channel, "", 0));
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_proto_json_usb_channel_send(&s_ctx.channel, "{", 1));
    char oversized[sizeof(s_ctx.tx_buffer)];
    memset(oversized, ' ', sizeof(oversized));
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_proto_json_usb_channel_send(&s_ctx.channel, oversized, sizeof(oversized)));
    memcpy(s_ctx.tx_buffer, "null", 4);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_usb_channel_send(&s_ctx.channel, (char *)s_ctx.tx_buffer, 4));
    TEST_ASSERT_EQUAL(ESY_OK, local_status().error);
    char document[] = "{\"x\":1}";
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_usb_channel_send(&s_ctx.channel, document, strlen(document)));
    memset(document, 'x', strlen(document));
    TEST_ASSERT_EQUAL_UINT32(0, s_ctx.interface->write_count);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_json_usb_channel_send(&s_ctx.channel, "null", 4));
    TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
    const uint8_t expected[] = {0, 0, 0, 7, '{', '"', 'x', '"', ':', '1', '}'};
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), s_ctx.transmitted_length);
    TEST_ASSERT_EQUAL_MEMORY(expected, s_ctx.transmitted, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT32(1, s_ctx.interface->flush_count);
    TEST_ASSERT_FALSE(local_status().tx_pending);
}

void test_usb_tx_budget_and_fifo_backpressure_resume_prefix_once_without_replay(void) {
    s_ctx.config.tx_byte_budget = 3;
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_usb_channel_send(&s_ctx.channel, "null", 4));
    s_ctx.interface->write_available = 0;
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, poll_capture());
    TEST_ASSERT_EQUAL_size_t(8, local_status().tx_remaining);
    TEST_ASSERT_EQUAL_UINT32(0, s_ctx.interface->flush_count);
    for (unsigned i = 0; i < 4; ++i) {
        s_ctx.interface->write_available = 2;
        esy_err_t result = poll_capture();
        TEST_ASSERT_TRUE(result == ESY_OK || result == ESY_ERR_BUSY);
        TEST_ASSERT_EQUAL_size_t(6u - 2u * i, local_status().tx_remaining);
    }
    const uint8_t expected[] = {0, 0, 0, 4, 'n', 'u', 'l', 'l'};
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), s_ctx.transmitted_length);
    TEST_ASSERT_EQUAL_MEMORY(expected, s_ctx.transmitted, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT32(4, s_ctx.interface->flush_count);
    TEST_ASSERT_FALSE(local_status().tx_pending);
}

void test_usb_rx_reassembles_fragmented_prefix_and_body_under_budget(void) {
    s_ctx.config.rx_byte_budget = 2;
    append_document("{\"x\":1}");
    supply_wire();
    start_channel();
    unsigned polls = 0;
    while (s_ctx.messages == 0) {
        uint32_t before = s_ctx.interface->available;
        TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(2, before - s_ctx.interface->available);
        TEST_ASSERT_LESS_THAN_UINT(10, polls++);
    }
    TEST_ASSERT_EQUAL_STRING("{\"x\":1}", s_ctx.documents[0]);
    TEST_ASSERT_EQUAL(ESY_JSON_OBJECT, s_ctx.types[0]);
    TEST_ASSERT_EQUAL_size_t(0, local_status().rx_received);
}

void test_usb_actual_short_and_zero_io_preserve_offsets_with_positive_advertised_capacity(void) {
    append_document("null");
    supply_wire();
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_usb_channel_send(&s_ctx.channel, "true", 4));
    s_ctx.interface->write_limit = 0;
    s_ctx.interface->read_limit = 0;
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, poll_capture());
    TEST_ASSERT_EQUAL_size_t(8, local_status().tx_remaining);
    TEST_ASSERT_EQUAL_UINT32(8, s_ctx.interface->available);
    TEST_ASSERT_EQUAL_UINT32(0, s_ctx.interface->flush_count);
    s_ctx.interface->write_limit = 2;
    s_ctx.interface->read_limit = 1;
    for (unsigned i = 0; i < 4; ++i) TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
    TEST_ASSERT_FALSE(local_status().tx_pending);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.messages);
    TEST_ASSERT_EQUAL_STRING("null", s_ctx.documents[0]);
    const uint8_t expected[] = {0, 0, 0, 4, 't', 'r', 'u', 'e'};
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), s_ctx.transmitted_length);
    TEST_ASSERT_EQUAL_MEMORY(expected, s_ctx.transmitted, sizeof(expected));
}

void test_usb_rx_coalesced_roots_dispatch_at_most_once_per_poll(void) {
    append_document("{\n \"x\": 1\n}");
    append_document("[true,\"x\"]");
    append_document("false");
    supply_wire();
    start_channel();
    for (unsigned i = 1; i <= 3; ++i) {
        TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
        TEST_ASSERT_EQUAL_UINT(i, s_ctx.messages);
    }
    TEST_ASSERT_EQUAL_STRING("{\n \"x\": 1\n}", s_ctx.documents[0]);
    TEST_ASSERT_EQUAL(ESY_JSON_ARRAY, s_ctx.types[1]);
    TEST_ASSERT_EQUAL(ESY_JSON_BOOL, s_ctx.types[2]);
}

void test_usb_callback_echoes_rx_and_rejects_recursive_lifecycle_without_losing_reply(void) {
    append_document("{\"n\":18446744073709551615}");
    supply_wire();
    s_ctx.callback_echo = true;
    s_ctx.callback_reentry = true;
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.callback_send_result);
    TEST_ASSERT_TRUE(local_status().tx_pending);
    TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
    TEST_ASSERT_FALSE(local_status().tx_pending);
    TEST_ASSERT_EQUAL_size_t(s_ctx.wire_length, s_ctx.transmitted_length);
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.wire, s_ctx.transmitted, s_ctx.wire_length);
}

void test_usb_full_duplex_busy_callback_reply_can_be_retained_and_retried(void) {
    s_ctx.config.tx_byte_budget = 2;
    s_ctx.callback_echo = true;
    append_document("1");
    supply_wire();
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_usb_channel_send(&s_ctx.channel, "null", 4));
    TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.messages);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.callback_send_result);
    for (unsigned i = 0; i < 3; ++i) TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_usb_channel_send(&s_ctx.channel, s_ctx.documents[0], 1));
    for (unsigned i = 0; i < 3; ++i) TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
    const uint8_t expected[] = {0, 0, 0, 4, 'n', 'u', 'l', 'l', 0, 0, 0, 1, '1'};
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), s_ctx.transmitted_length);
    TEST_ASSERT_EQUAL_MEMORY(expected, s_ctx.transmitted, sizeof(expected));
}

void test_usb_poll_never_pumps_stack_even_with_multiple_cdc_interfaces(void) {
    esy_usb_cdc_t second = {0};
    esy_usb_cdc_config_t config = s_ctx.cdc_config;
    config.itf = 1;
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_usb_cdc_init(&second, &config));
    for (unsigned i = 0; i < 3; ++i) TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
    TEST_ASSERT_EQUAL_UINT32(0, s_ctx.interface->task_count);
    esy_usb_cdc_task();
    TEST_ASSERT_EQUAL_UINT32(1, s_ctx.interface->task_count);
    TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
    TEST_ASSERT_EQUAL_UINT32(1, s_ctx.interface->task_count);
    esy_usb_cdc_deinit(&second);
}

void test_usb_dtr_close_reopen_discards_old_partial_work_but_preserves_new_rx(void) {
    s_ctx.config.tx_byte_budget = 2;
    s_ctx.config.rx_byte_budget = 6;
    append_document("null");
    supply_wire();
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_usb_channel_send(&s_ctx.channel, "false", 5));
    TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
    TEST_ASSERT_EQUAL_size_t(2, local_status().rx_received);
    TEST_ASSERT_TRUE(local_status().tx_pending);
    uint32_t previous = local_status().session_id;
    close_and_reopen();
    s_ctx.wire_length = 0;
    append_document("true");
    supply_wire();
    uint32_t reads = s_ctx.interface->read_count;
    uint32_t writes = s_ctx.interface->write_count;
    uint32_t clears = s_ctx.interface->read_flush_count;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, poll_capture());
    TEST_ASSERT_NOT_EQUAL(previous, local_status().session_id);
    TEST_ASSERT_EQUAL(ESY_OK, local_status().error);
    TEST_ASSERT_FALSE(local_status().tx_pending);
    TEST_ASSERT_EQUAL_UINT32(reads, s_ctx.interface->read_count);
    TEST_ASSERT_EQUAL_UINT32(writes, s_ctx.interface->write_count);
    TEST_ASSERT_EQUAL_UINT32(clears, s_ctx.interface->read_flush_count);
    TEST_ASSERT_EQUAL_UINT32(8, s_ctx.interface->available);
    TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
    TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.messages);
    TEST_ASSERT_EQUAL_STRING("true", s_ctx.documents[0]);
}

void test_usb_send_checks_new_generation_before_old_pending_slot_or_sticky_error(void) {
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_usb_channel_send(&s_ctx.channel, "null", 4));
    close_and_reopen();
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_json_usb_channel_send(&s_ctx.channel, "true", 4));
    TEST_ASSERT_FALSE(local_status().tx_pending);
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_usb_channel_send(&s_ctx.channel, "false", 5));
    TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
    const uint8_t expected[] = {0, 0, 0, 5, 'f', 'a', 'l', 's', 'e'};
    TEST_ASSERT_EQUAL_MEMORY(expected, s_ctx.transmitted, sizeof(expected));
    append_document("{");
    supply_wire();
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, poll_capture());
    close_and_reopen();
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_json_usb_channel_send(&s_ctx.channel, "1", 1));
    TEST_ASSERT_EQUAL(ESY_OK, local_status().error);
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_usb_channel_send(&s_ctx.channel, "1", 1));
}

void test_usb_session_change_clears_malformed_old_session_before_using_new_peer_bytes(void) {
    append_header(0);
    supply_wire();
    start_channel();
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, poll_capture());
    assert_sticky(ESY_ERR_FAIL);
    close_and_reopen();
    s_ctx.wire_length = 0;
    append_document("null");
    supply_wire();
    TEST_ASSERT_EQUAL(ESY_ERR_IO, poll_capture());
    TEST_ASSERT_EQUAL(ESY_OK, local_status().error);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.messages);
    TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.messages);
}

void test_usb_physical_disconnect_is_observed_by_external_stack_pump(void) {
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_usb_channel_send(&s_ctx.channel, "null", 4));
    uint32_t previous = local_status().session_id;
    s_ctx.interface->connected = false;
    esy_usb_cdc_task();
    TEST_ASSERT_EQUAL(ESY_ERR_IO, poll_capture());
    TEST_ASSERT_NOT_EQUAL(previous, local_status().session_id);
    TEST_ASSERT_FALSE(local_status().connected);
    TEST_ASSERT_FALSE(local_status().tx_pending);
    TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
    s_ctx.interface->connected = true;
    esy_usb_cdc_task();
    append_document("true");
    supply_wire();
    TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.messages);
}

void test_usb_unobserved_close_without_generation_advance_is_lifecycle_error(void) {
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_usb_channel_send(&s_ctx.channel, "null", 4));
    s_ctx.interface->connected = false;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, poll_capture());
    assert_sticky(ESY_ERR_STATE);
    esy_usb_cdc_task();
    TEST_ASSERT_EQUAL(ESY_ERR_IO, poll_capture());
    TEST_ASSERT_EQUAL(ESY_OK, local_status().error);
    TEST_ASSERT_FALSE(local_status().tx_pending);
}

void test_usb_rts_only_line_change_does_not_cancel_current_session_work(void) {
    s_ctx.config.tx_byte_budget = 2;
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_usb_channel_send(&s_ctx.channel, "null", 4));
    TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
    uint32_t generation = local_status().session_id;
    s_ctx.interface->line_state = 3;
    tud_cdc_line_state_cb(0, true, true);
    for (unsigned i = 0; i < 3; ++i) TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
    TEST_ASSERT_EQUAL_UINT32(generation, local_status().session_id);
    const uint8_t expected[] = {0, 0, 0, 4, 'n', 'u', 'l', 'l'};
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), s_ctx.transmitted_length);
    TEST_ASSERT_EQUAL_MEMORY(expected, s_ctx.transmitted, sizeof(expected));
}

void test_usb_cdc_owner_discards_closed_fifo_without_discarding_new_peer_rx(void) {
    s_ctx.interface->connected = false;
    s_ctx.interface->line_state = 0;
    start_channel();
    append_document("{");
    supply_wire();
    esy_usb_cdc_task();
    TEST_ASSERT_EQUAL_UINT32(0, s_ctx.interface->available);
    TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
    s_ctx.interface->connected = true;
    s_ctx.interface->line_state = 1;
    tud_cdc_line_state_cb(0, true, false);
    s_ctx.wire_length = 0;
    append_document("null");
    supply_wire();
    uint32_t clears = s_ctx.interface->read_flush_count;
    TEST_ASSERT_EQUAL(ESY_OK, poll_capture());
    TEST_ASSERT_EQUAL_UINT32(clears, s_ctx.interface->read_flush_count);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.messages);
}

void test_usb_oversized_or_invalid_json_is_sticky_within_one_session(void) {
    append_header(USB_TEST_MESSAGE_BYTES + 1);
    supply_wire();
    start_channel();
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, poll_capture());
    assert_sticky(ESY_ERR_NO_MEM);
    tearDown();
    setUp();
    append_document("{\"x\":");
    supply_wire();
    start_channel();
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, poll_capture());
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.messages);
    assert_sticky(ESY_ERR_FAIL);
}

void test_usb_impossible_backend_read_or_write_counts_latch_io_error(void) {
    append_document("null");
    supply_wire();
    start_channel();
    s_ctx.interface->read_overreport = true;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, poll_capture());
    assert_sticky(ESY_ERR_IO);
    tearDown();
    setUp();
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_usb_channel_send(&s_ctx.channel, "null", 4));
    s_ctx.interface->write_overreport = true;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, poll_capture());
    assert_sticky(ESY_ERR_IO);
}

void test_usb_detached_cdc_reports_lifecycle_error_and_channel_deinit_never_touches_fifos(void) {
    start_channel();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_usb_channel_send(&s_ctx.channel, "null", 4));
    esy_usb_cdc_deinit(&s_ctx.cdc);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, poll_capture());
    uint32_t reads = s_ctx.interface->read_count;
    uint32_t writes = s_ctx.interface->write_count;
    uint32_t clears = s_ctx.interface->read_flush_count;
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_json_usb_channel_deinit(&s_ctx.channel));
    TEST_ASSERT_EQUAL_UINT32(reads, s_ctx.interface->read_count);
    TEST_ASSERT_EQUAL_UINT32(writes, s_ctx.interface->write_count);
    TEST_ASSERT_EQUAL_UINT32(clears, s_ctx.interface->read_flush_count);
}

void test_usb_public_api_rejects_null_arguments_and_preserves_failed_status_output(void) {
    esy_proto_json_usb_channel_status_t status;
    memset(&status, 0x5a, sizeof(status));
    const esy_proto_json_usb_channel_status_t before = status;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_json_usb_channel_get_status(&s_ctx.channel, &status));
    TEST_ASSERT_EQUAL_MEMORY(&before, &status, sizeof(status));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_usb_channel_get_status(NULL, &status));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_usb_channel_get_status(&s_ctx.channel, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_usb_channel_init(NULL, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_usb_channel_init(&s_ctx.channel, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_usb_channel_poll(NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_usb_channel_send(NULL, "null", 4));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_json_usb_channel_send(&s_ctx.channel, "null", 4));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_json_usb_channel_deinit(NULL));
}
