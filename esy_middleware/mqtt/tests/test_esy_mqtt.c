/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_mqtt.h"

#include <limits.h>
#include <string.h>

#define FIXTURE_STREAM_BYTES 8192u
#define FIXTURE_EVENT_COUNT 32u

typedef struct {
    esy_mqtt_t client;
    uint8_t network[1024];
    esy_mqtt_static_resources_t resources;
    esy_mqtt_config_t config;
    uint8_t tx[FIXTURE_STREAM_BYTES];
    size_t tx_size;
    uint8_t rx[FIXTURE_STREAM_BYTES];
    size_t rx_size;
    size_t rx_offset;
    size_t send_chunk;
    size_t recv_chunk;
    uint32_t recv_advance_ms;
    unsigned send_zero_count;
    unsigned progress_calls;
    bool connack_on_progress;
    esy_err_t progress_result;
    bool send_fail;
    bool recv_fail;
    bool send_oversize;
    bool recv_oversize;
    uint32_t now;
    uint32_t clock_step;
    esy_mqtt_event_t events[FIXTURE_EVENT_COUNT];
    size_t event_count;
    char message_topic[64];
    uint8_t message_payload[128];
    bool test_reentry;
    esy_err_t reentry[9];
    bool connected_in_callback;
} mqtt_test_ctx_t;

static mqtt_test_ctx_t s_ctx;

static uint32_t fixture_time(void)
{
    uint32_t result = s_ctx.now;
    s_ctx.now += s_ctx.clock_step;
    return result;
}

static int32_t fixture_send(void *context, const void *data, size_t size)
{
    mqtt_test_ctx_t *fixture = context;
    size_t count = size;
    if (fixture->send_fail) {
        return -1;
    }
    if (fixture->send_oversize) {
        return (int32_t)(size + 1u);
    }
    if (fixture->send_zero_count > 0u) {
        --fixture->send_zero_count;
        return 0;
    }
    if (count > fixture->send_chunk) {
        count = fixture->send_chunk;
    }
    if (count > sizeof(fixture->tx) - fixture->tx_size) {
        return -1;
    }
    memcpy(&fixture->tx[fixture->tx_size], data, count);
    fixture->tx_size += count;
    return (int32_t)count;
}

static int32_t fixture_recv(void *context, void *data, size_t size)
{
    mqtt_test_ctx_t *fixture = context;
    size_t count = fixture->rx_size - fixture->rx_offset;
    if (fixture->recv_fail) {
        return -1;
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
    memcpy(data, &fixture->rx[fixture->rx_offset], count);
    fixture->rx_offset += count;
    fixture->now += fixture->recv_advance_ms;
    return (int32_t)count;
}

static esy_mqtt_string_t string_view(const char *value)
{
    esy_mqtt_string_t result = {value, (uint16_t)strlen(value)};
    return result;
}

static esy_mqtt_message_t sample_message(esy_mqtt_qos_t qos)
{
    esy_mqtt_message_t result = {0};
    result.topic = string_view("s/t");
    result.payload = "x";
    result.payload_size = 1u;
    result.qos = qos;
    return result;
}

static esy_mqtt_connect_config_t connect_config(void)
{
    esy_mqtt_connect_config_t result = {0};
    result.client_id = string_view("client");
    result.timeout_ms = 100u;
    return result;
}

static void fixture_event(void *context, const esy_mqtt_event_t *event)
{
    mqtt_test_ctx_t *fixture = context;
    esy_mqtt_message_t message = sample_message(ESY_MQTT_QOS_0);
    esy_mqtt_connect_config_t connection = connect_config();
    uint16_t packet_id = 0u;
    TEST_ASSERT_LESS_THAN(FIXTURE_EVENT_COUNT, fixture->event_count);
    fixture->events[fixture->event_count++] = *event;
    if (event->type == ESY_MQTT_EVENT_MESSAGE) {
        TEST_ASSERT_LESS_THAN(sizeof(fixture->message_topic), event->message.topic.length);
        TEST_ASSERT_LESS_OR_EQUAL(sizeof(fixture->message_payload), event->message.payload_size);
        memcpy(fixture->message_topic, event->message.topic.data, event->message.topic.length);
        fixture->message_topic[event->message.topic.length] = '\0';
        memcpy(fixture->message_payload, event->message.payload, event->message.payload_size);
    }
    if (fixture->test_reentry) {
        fixture->connected_in_callback = esy_mqtt_is_connected(&fixture->client);
        fixture->reentry[0] = esy_mqtt_publish(&fixture->client, &message, &packet_id);
        fixture->reentry[1] = esy_mqtt_subscribe(&fixture->client, string_view("a"), ESY_MQTT_QOS_0, &packet_id);
        fixture->reentry[2] = esy_mqtt_unsubscribe(&fixture->client, string_view("a"), &packet_id);
        fixture->reentry[3] = esy_mqtt_process(&fixture->client);
        fixture->reentry[4] = esy_mqtt_disconnect(&fixture->client);
        fixture->reentry[5] = esy_mqtt_abort(&fixture->client);
        fixture->reentry[6] = esy_mqtt_deinit(&fixture->client);
        fixture->reentry[7] = esy_mqtt_connect(&fixture->client, &connection);
        fixture->reentry[8] = esy_mqtt_init(&fixture->client, &fixture->config);
    }
}

static void queue_bytes(const uint8_t *data, size_t size)
{
    TEST_ASSERT_LESS_OR_EQUAL(sizeof(s_ctx.rx) - s_ctx.rx_size, size);
    memcpy(&s_ctx.rx[s_ctx.rx_size], data, size);
    s_ctx.rx_size += size;
}

static esy_err_t fixture_progress(void *context)
{
    mqtt_test_ctx_t *fixture = context;
    ++fixture->progress_calls;
    if (fixture->connack_on_progress) {
        const uint8_t connack[] = {0x20u, 0x02u, 0x00u, 0x00u};
        fixture->connack_on_progress = false;
        queue_bytes(connack, sizeof(connack));
    }
    return fixture->progress_result;
}

static void queue_ack(uint8_t type, uint16_t packet_id)
{
    uint8_t packet[] = {type, 2u, (uint8_t)(packet_id >> 8), (uint8_t)packet_id};
    queue_bytes(packet, sizeof(packet));
}

static void initialize(void)
{
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_init(&s_ctx.client, &s_ctx.config));
}

static void connect_client(void)
{
    const uint8_t connack[] = {0x20u, 0x02u, 0x00u, 0x00u};
    esy_mqtt_connect_config_t config = connect_config();
    initialize();
    queue_bytes(connack, sizeof(connack));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_connect(&s_ctx.client, &config));
    TEST_ASSERT_TRUE(esy_mqtt_is_connected(&s_ctx.client));
    s_ctx.tx_size = 0u;
}

static void connect_keepalive_client(void)
{
    const uint8_t connack[] = {0x20u, 0x02u, 0x00u, 0x00u};
    esy_mqtt_connect_config_t config = connect_config();
    config.keep_alive_seconds = 1u;
    s_ctx.clock_step = 0u;
    initialize();
    queue_bytes(connack, sizeof(connack));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_connect(&s_ctx.client, &config));
    TEST_ASSERT_TRUE(esy_mqtt_is_connected(&s_ctx.client));
    s_ctx.tx_size = 0u;
}

void setUp(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.send_chunk = SIZE_MAX;
    s_ctx.recv_chunk = SIZE_MAX;
    s_ctx.clock_step = 1u;
    s_ctx.resources.network_buffer = s_ctx.network;
    s_ctx.resources.network_buffer_size = sizeof(s_ctx.network);
    s_ctx.config.transport.send = fixture_send;
    s_ctx.config.transport.recv = fixture_recv;
    s_ctx.config.transport.context = &s_ctx;
    s_ctx.config.static_resources = &s_ctx.resources;
    s_ctx.config.get_time_ms = fixture_time;
    s_ctx.config.acknowledgement_timeout_ms = 100u;
    s_ctx.config.event_cb = fixture_event;
    s_ctx.config.event_context = &s_ctx;
}

void tearDown(void)
{
}

void test_init_validates_required_inputs_and_retains_zero_client(void)
{
    esy_mqtt_t zero = {0};
    esy_mqtt_config_t config = s_ctx.config;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_init(NULL, &config));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_init(&s_ctx.client, NULL));
    config.transport.send = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_init(&s_ctx.client, &config));
    config = s_ctx.config;
    config.transport.recv = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_init(&s_ctx.client, &config));
    config = s_ctx.config;
    config.get_time_ms = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_init(&s_ctx.client, &config));
    config = s_ctx.config;
    config.event_cb = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_init(&s_ctx.client, &config));
    config = s_ctx.config;
    config.static_resources = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_init(&s_ctx.client, &config));
    config = s_ctx.config;
    config.acknowledgement_timeout_ms = 0u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_init(&s_ctx.client, &config));
    config.acknowledgement_timeout_ms = UINT32_MAX;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_init(&s_ctx.client, &config));
    s_ctx.resources.network_buffer = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_init(&s_ctx.client, &s_ctx.config));
    s_ctx.resources.network_buffer = s_ctx.network;
    s_ctx.resources.network_buffer_size = ESY_MQTT_MIN_BUFFER_SIZE - 1u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_init(&s_ctx.client, &s_ctx.config));
    TEST_ASSERT_EQUAL_MEMORY(&zero, &s_ctx.client, sizeof(zero));
    TEST_ASSERT_EQUAL(0u, s_ctx.tx_size);
}

void test_lifecycle_rejects_repeated_init_and_permits_reinit_after_deinit(void)
{
    esy_mqtt_t zero = {0};
    initialize();
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_mqtt_init(&s_ctx.client, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_deinit(&s_ctx.client));
    TEST_ASSERT_EQUAL_MEMORY(&zero, &s_ctx.client, sizeof(zero));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_mqtt_deinit(&s_ctx.client));
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_abort(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_disconnect(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_deinit(&s_ctx.client));
}

void test_uninitialized_and_null_calls_have_defined_errors(void)
{
    esy_mqtt_message_t message = sample_message(ESY_MQTT_QOS_0);
    esy_mqtt_connect_config_t config = connect_config();
    uint16_t packet_id = 5u;
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(NULL));
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_process(NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_connect(NULL, &config));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_publish(NULL, &message, &packet_id));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_disconnect(NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_abort(NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_deinit(NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_mqtt_connect(&s_ctx.client, &config));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
}

void test_connect_sends_clean_session_with_credentials_and_will(void)
{
    const uint8_t connack[] = {0x20, 0x02, 0x00, 0x00};
    const uint8_t expected[] = {
        0x10, 0x24, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0xee, 0x00, 0x3c,
        0x00, 0x06, 'c', 'l', 'i', 'e', 'n', 't',
        0x00, 0x04, 'l', 'a', 's', 't', 0x00, 0x03, 'b', 'y', 'e',
        0x00, 0x01, 'u', 0x00, 0x02, 'p', 0x00};
    esy_mqtt_message_t will = sample_message(ESY_MQTT_QOS_1);
    esy_mqtt_connect_config_t config = connect_config();
    will.topic = string_view("last");
    will.payload = "bye";
    will.payload_size = 3u;
    will.retain = true;
    config.will = &will;
    config.keep_alive_seconds = 60u;
    config.username = string_view("u");
    config.password.data = "p\0";
    config.password.length = 2u;
    initialize();
    queue_bytes(connack, sizeof(connack));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_connect(&s_ctx.client, &config));
    TEST_ASSERT_EQUAL(sizeof(expected), s_ctx.tx_size);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, s_ctx.tx, sizeof(expected));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_mqtt_connect(&s_ctx.client, &config));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_mqtt_deinit(&s_ctx.client));
}

void test_connect_rejects_invalid_inputs_before_sending(void)
{
    esy_mqtt_connect_config_t config = connect_config();
    initialize();
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_connect(&s_ctx.client, NULL));
    config.client_id.length = 0u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_connect(&s_ctx.client, &config));
    config = connect_config();
    config.password = string_view("secret");
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_connect(&s_ctx.client, &config));
    config = connect_config();
    config.timeout_ms = 0u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_connect(&s_ctx.client, &config));
    config.timeout_ms = UINT32_MAX;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_connect(&s_ctx.client, &config));
    TEST_ASSERT_EQUAL(0u, s_ctx.tx_size);
}

void test_connect_timeout_and_broker_rejection_leave_stopped_client(void)
{
    const uint8_t rejected[] = {0x20u, 0x02u, 0x00u, 0x05u};
    esy_mqtt_connect_config_t config = connect_config();
    initialize();
    config.timeout_ms = 5u;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_mqtt_connect(&s_ctx.client, &config));
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
    queue_bytes(rejected, sizeof(rejected));
    TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_mqtt_connect(&s_ctx.client, &config));
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_deinit(&s_ctx.client));
}

void test_publish_qos_zero_and_one_wire_bytes_and_puback_event(void)
{
    const uint8_t expected_zero[] = {0x30, 0x06, 0x00, 0x03, 's', '/', 't', 'x'};
    uint8_t expected_one[] = {0x32, 0x08, 0x00, 0x03, 's', '/', 't', 0, 0, 'x'};
    esy_mqtt_message_t message = sample_message(ESY_MQTT_QOS_0);
    uint16_t packet_id = 999u;
    connect_client();
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    TEST_ASSERT_EQUAL(0u, packet_id);
    TEST_ASSERT_EQUAL(sizeof(expected_zero), s_ctx.tx_size);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_zero, s_ctx.tx, sizeof(expected_zero));
    TEST_ASSERT_EQUAL(0u, s_ctx.event_count);
    s_ctx.tx_size = 0u;
    message.qos = ESY_MQTT_QOS_1;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    TEST_ASSERT_NOT_EQUAL(0u, packet_id);
    expected_one[7] = (uint8_t)(packet_id >> 8);
    expected_one[8] = (uint8_t)packet_id;
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_one, s_ctx.tx, sizeof(expected_one));
    queue_ack(0x40u, packet_id);
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_PUBLISHED, s_ctx.events[0].type);
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.events[0].result);
    TEST_ASSERT_EQUAL(packet_id, s_ctx.events[0].packet_id);
}

void test_subscribe_and_unsubscribe_wire_bytes_and_completion_events(void)
{
    uint8_t expected_sub[] = {0x82, 0x08, 0, 0, 0, 3, 'a', '/', '+', 1};
    uint8_t expected_unsub[] = {0xa2, 0x07, 0, 0, 0, 3, 'a', '/', '+'};
    uint8_t suback[] = {0x90, 0x03, 0, 0, 1};
    uint16_t packet_id;
    connect_client();
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_subscribe(&s_ctx.client, string_view("a/+"), ESY_MQTT_QOS_1, &packet_id));
    expected_sub[2] = suback[2] = (uint8_t)(packet_id >> 8);
    expected_sub[3] = suback[3] = (uint8_t)packet_id;
    TEST_ASSERT_EQUAL(sizeof(expected_sub), s_ctx.tx_size);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_sub, s_ctx.tx, sizeof(expected_sub));
    queue_bytes(suback, sizeof(suback));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_SUBSCRIBED, s_ctx.events[0].type);
    TEST_ASSERT_EQUAL(ESY_MQTT_QOS_1, s_ctx.events[0].granted_qos);
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.events[0].result);
    s_ctx.tx_size = 0u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_unsubscribe(&s_ctx.client, string_view("a/+"), &packet_id));
    expected_unsub[2] = (uint8_t)(packet_id >> 8);
    expected_unsub[3] = (uint8_t)packet_id;
    TEST_ASSERT_EQUAL(sizeof(expected_unsub), s_ctx.tx_size);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_unsub, s_ctx.tx, sizeof(expected_unsub));
    queue_ack(0xb0u, packet_id);
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_UNSUBSCRIBED, s_ctx.events[1].type);
    TEST_ASSERT_EQUAL(packet_id, s_ctx.events[1].packet_id);
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.events[1].result);
}

void test_suback_rejection_completes_operation_without_losing_connection(void)
{
    uint8_t suback[] = {0x90, 0x03, 0, 0, 0x80};
    uint16_t packet_id;
    connect_client();
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_subscribe(&s_ctx.client, string_view("a/#"), ESY_MQTT_QOS_0, &packet_id));
    suback[2] = (uint8_t)(packet_id >> 8);
    suback[3] = (uint8_t)packet_id;
    queue_bytes(suback, sizeof(suback));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, s_ctx.events[0].result);
    TEST_ASSERT_TRUE(esy_mqtt_is_connected(&s_ctx.client));
}

void test_pending_capacity_is_eight_and_qos_zero_requires_no_slot(void)
{
    esy_mqtt_message_t message = sample_message(ESY_MQTT_QOS_1);
    uint16_t ids[ESY_MQTT_MAX_PENDING_OPERATIONS];
    uint16_t unchanged = 0xbeefu;
    size_t tx_before;
    unsigned index;
    connect_client();
    s_ctx.clock_step = 0u;
    for (index = 0u; index < ESY_MQTT_MAX_PENDING_OPERATIONS; ++index) {
        TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_publish(&s_ctx.client, &message, &ids[index]));
    }
    tx_before = s_ctx.tx_size;
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_mqtt_publish(&s_ctx.client, &message, &unchanged));
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_mqtt_subscribe(&s_ctx.client, string_view("a"), ESY_MQTT_QOS_0, &unchanged));
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_mqtt_unsubscribe(&s_ctx.client, string_view("a"), &unchanged));
    TEST_ASSERT_EQUAL(0xbeefu, unchanged);
    TEST_ASSERT_EQUAL(tx_before, s_ctx.tx_size);
    message.qos = ESY_MQTT_QOS_0;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_publish(&s_ctx.client, &message, &unchanged));
    queue_ack(0x40u, ids[0]);
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    message.qos = ESY_MQTT_QOS_1;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_publish(&s_ctx.client, &message, &unchanged));
}

void test_topic_filter_validation_accepts_level_wildcards_and_rejects_embedded_ones(void)
{
    const char *valid[] = {"#", "+", "a/+", "/+/#", "a/", "/", "a//b"};
    const char *invalid[] = {"", "a+", "a#", "a/#/b", "a/+b", "##", "a/#/"};
    uint16_t packet_id;
    unsigned index;
    connect_client();
    s_ctx.clock_step = 0u;
    for (index = 0u; index < sizeof(valid) / sizeof(valid[0]); ++index) {
        TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_subscribe(&s_ctx.client, string_view(valid[index]), ESY_MQTT_QOS_0, &packet_id));
    }
    for (index = 0u; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_subscribe(&s_ctx.client, string_view(invalid[index]), ESY_MQTT_QOS_0, &packet_id));
        TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_unsubscribe(&s_ctx.client, string_view(invalid[index]), &packet_id));
    }
}

void test_publish_rejects_invalid_topic_payload_qos_and_duplicate_flag(void)
{
    esy_mqtt_message_t message = sample_message(ESY_MQTT_QOS_0);
    uint16_t packet_id = 0x1234u;
    connect_client();
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_publish(&s_ctx.client, NULL, &packet_id));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_publish(&s_ctx.client, &message, NULL));
    message.topic = string_view("a/+");
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    message = sample_message(ESY_MQTT_QOS_0);
    message.topic.data = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    message = sample_message(ESY_MQTT_QOS_0);
    message.payload = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    message = sample_message((esy_mqtt_qos_t)2);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    message = sample_message(ESY_MQTT_QOS_0);
    message.duplicate = true;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    TEST_ASSERT_EQUAL(0x1234u, packet_id);
    TEST_ASSERT_EQUAL(0u, s_ctx.tx_size);
}

void test_partial_transmit_and_temporary_backpressure_preserve_packet(void)
{
    const uint8_t expected[] = {0x30, 0x06, 0, 3, 's', '/', 't', 'x'};
    esy_mqtt_message_t message = sample_message(ESY_MQTT_QOS_0);
    uint16_t packet_id;
    connect_client();
    s_ctx.send_chunk = 1u;
    s_ctx.send_zero_count = 2u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    TEST_ASSERT_EQUAL(sizeof(expected), s_ctx.tx_size);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, s_ctx.tx, sizeof(expected));
}

void test_partial_receive_delivers_one_message_and_qos_one_puback(void)
{
    const uint8_t incoming[] = {0x3b, 0x08, 0, 3, 's', '/', 't', 0, 7, 'x'};
    const uint8_t expected_ack[] = {0x40, 0x02, 0x00, 0x07};
    unsigned index;
    connect_client();
    s_ctx.recv_chunk = 1u;
    queue_bytes(incoming, sizeof(incoming));
    for (index = 0u; index < sizeof(incoming) - 1u; ++index) {
        TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
        TEST_ASSERT_EQUAL(0u, s_ctx.event_count);
    }
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_MESSAGE, s_ctx.events[0].type);
    TEST_ASSERT_EQUAL_STRING("s/t", s_ctx.message_topic);
    TEST_ASSERT_EQUAL('x', s_ctx.message_payload[0]);
    TEST_ASSERT_TRUE(s_ctx.events[0].message.duplicate);
    TEST_ASSERT_TRUE(s_ctx.events[0].message.retain);
    TEST_ASSERT_EQUAL(ESY_MQTT_QOS_1, s_ctx.events[0].message.qos);
    TEST_ASSERT_EQUAL(sizeof(expected_ack), s_ctx.tx_size);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_ack, s_ctx.tx, sizeof(expected_ack));
}

void test_process_handles_at_most_one_buffered_packet_per_call(void)
{
    const uint8_t incoming[] = {0x30, 0x04, 0, 1, 'a', '1', 0x30, 0x04, 0, 1, 'b', '2'};
    connect_client();
    queue_bytes(incoming, sizeof(incoming));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
    TEST_ASSERT_EQUAL_STRING("a", s_ctx.message_topic);
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(2u, s_ctx.event_count);
    TEST_ASSERT_EQUAL_STRING("b", s_ctx.message_topic);
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(2u, s_ctx.event_count);
}

void test_callback_may_query_connection_but_mutation_and_recursive_processing_are_busy(void)
{
    const uint8_t incoming[] = {0x30, 0x04, 0, 1, 'a', '1'};
    unsigned index;
    connect_client();
    s_ctx.test_reentry = true;
    queue_bytes(incoming, sizeof(incoming));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_TRUE(s_ctx.connected_in_callback);
    for (index = 0u; index < 8u; ++index) {
        TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.reentry[index]);
    }
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.reentry[8]);
    TEST_ASSERT_TRUE(esy_mqtt_is_connected(&s_ctx.client));
}

void test_receive_eof_cancels_pending_commands_and_emits_disconnect_last(void)
{
    esy_mqtt_message_t message = sample_message(ESY_MQTT_QOS_1);
    uint16_t publish_id;
    uint16_t subscribe_id;
    connect_client();
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_publish(&s_ctx.client, &message, &publish_id));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_subscribe(&s_ctx.client, string_view("a"), ESY_MQTT_QOS_0, &subscribe_id));
    s_ctx.recv_fail = true;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
    TEST_ASSERT_EQUAL(3u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_PUBLISHED, s_ctx.events[0].type);
    TEST_ASSERT_EQUAL(publish_id, s_ctx.events[0].packet_id);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_SUBSCRIBED, s_ctx.events[1].type);
    TEST_ASSERT_EQUAL(subscribe_id, s_ctx.events[1].packet_id);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_DISCONNECTED, s_ctx.events[2].type);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, s_ctx.events[2].result);
}

void test_send_failure_invalidates_connection_and_does_not_write_output_id(void)
{
    esy_mqtt_message_t message = sample_message(ESY_MQTT_QOS_1);
    uint16_t packet_id = 0xbeefu;
    connect_client();
    s_ctx.send_fail = true;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    TEST_ASSERT_EQUAL(0xbeefu, packet_id);
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_DISCONNECTED, s_ctx.events[s_ctx.event_count - 1u].type);
}

void test_permanent_send_backpressure_returns_error_and_stops_client(void)
{
    esy_mqtt_message_t message = sample_message(ESY_MQTT_QOS_0);
    uint16_t packet_id;
    connect_client();
    s_ctx.send_zero_count = UINT_MAX;
    TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
}

void test_provider_oversize_counts_are_rejected_without_buffer_overrun(void)
{
    connect_client();
    s_ctx.recv_oversize = true;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
}

void test_acknowledgement_timeout_at_exact_boundary_across_uint32_wrap(void)
{
    esy_mqtt_message_t message = sample_message(ESY_MQTT_QOS_1);
    uint16_t packet_id;
    connect_client();
    s_ctx.clock_step = 0u;
    s_ctx.now = UINT32_MAX - 49u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    s_ctx.now += 99u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(0u, s_ctx.event_count);
    s_ctx.now += 1u;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
    TEST_ASSERT_EQUAL(2u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, s_ctx.events[0].result);
    TEST_ASSERT_EQUAL(packet_id, s_ctx.events[0].packet_id);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_DISCONNECTED, s_ctx.events[1].type);
}

void test_completed_wait_never_reactivates_after_a_full_counter_period(void)
{
    esy_mqtt_message_t message = sample_message(ESY_MQTT_QOS_1);
    uint16_t packet_id;
    unsigned index;
    connect_client();
    s_ctx.clock_step = 0u;
    s_ctx.now = 1000u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    queue_ack(0x40u, packet_id);
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    s_ctx.tx_size = 0u;
    /* Each observation is below INT32_MAX; four steps span one full period. */
    for (index = 0u; index < 4u; ++index) {
        s_ctx.now += UINT32_C(0x40000000);
        TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    }
    s_ctx.now += 100u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(0u, s_ctx.tx_size);
    TEST_ASSERT_TRUE(esy_mqtt_is_connected(&s_ctx.client));
}

void test_abort_cancels_wait_once_and_reconnect_starts_a_new_deadline(void)
{
    const uint8_t connack[] = {0x20, 0x02, 0x00, 0x00};
    esy_mqtt_connect_config_t connection = connect_config();
    esy_mqtt_message_t message = sample_message(ESY_MQTT_QOS_1);
    uint16_t packet_id;
    unsigned index;
    connect_client();
    s_ctx.clock_step = 0u;
    s_ctx.now = 1000u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_abort(&s_ctx.client));
    TEST_ASSERT_EQUAL(2u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, s_ctx.events[0].result);
    for (index = 0u; index < 4u; ++index) {
        s_ctx.now += UINT32_C(0x40000000);
        TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_mqtt_process(&s_ctx.client));
        TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_abort(&s_ctx.client));
    }
    TEST_ASSERT_EQUAL(2u, s_ctx.event_count);
    queue_bytes(connack, sizeof(connack));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_connect(&s_ctx.client, &connection));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    s_ctx.now += 99u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    s_ctx.now += 1u;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(4u, s_ctx.event_count);
}

void test_disconnect_sends_wire_packet_cancels_pending_and_is_idempotent(void)
{
    const uint8_t expected[] = {0xe0, 0};
    uint16_t packet_id;
    connect_client();
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_unsubscribe(&s_ctx.client, string_view("a"), &packet_id));
    s_ctx.tx_size = 0u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_disconnect(&s_ctx.client));
    TEST_ASSERT_EQUAL(sizeof(expected), s_ctx.tx_size);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, s_ctx.tx, sizeof(expected));
    TEST_ASSERT_EQUAL(2u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_UNSUBSCRIBED, s_ctx.events[0].type);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, s_ctx.events[0].result);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_DISCONNECTED, s_ctx.events[1].type);
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_disconnect(&s_ctx.client));
    TEST_ASSERT_EQUAL(2u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_deinit(&s_ctx.client));
}

void test_disconnect_send_failure_still_stops_local_processing(void)
{
    connect_client();
    s_ctx.send_fail = true;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_mqtt_disconnect(&s_ctx.client));
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_deinit(&s_ctx.client));
}

void test_malformed_packet_is_fatal_and_cleanup_allows_reinitialization(void)
{
    const uint8_t malformed[] = {0xf0, 0x00};
    connect_client();
    queue_bytes(malformed, sizeof(malformed));
    TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_DISCONNECTED, s_ctx.events[0].type);
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_deinit(&s_ctx.client));
    initialize();
}

void test_utf8_validation_accepts_multibyte_topics_and_rejects_invalid_sequences(void)
{
    const char *valid[] = {"\xc2\xa2", "\xe2\x82\xac", "\xf0\x9f\x98\x80"};
    const char *invalid[] = {"\x80", "\xc0\x80", "\xe0\x80\x80", "\xed\xa0\x80",
                             "\xef\xbf\xbe", "\xf4\x90\x80\x80", "\xf0\x9f", "\xc2x"};
    const char embedded_zero[] = {'a', '\0', 'b'};
    esy_mqtt_message_t message = sample_message(ESY_MQTT_QOS_0);
    uint16_t packet_id;
    unsigned index;
    connect_client();
    for (index = 0u; index < sizeof(valid) / sizeof(valid[0]); ++index) {
        message.topic = string_view(valid[index]);
        TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    }
    for (index = 0u; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        message.topic = string_view(invalid[index]);
        TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    }
    message.topic.data = embedded_zero;
    message.topic.length = sizeof(embedded_zero);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
}

void test_empty_payload_is_valid_and_binary_payload_is_preserved(void)
{
    const uint8_t binary[] = {0u, 0xffu, 0x80u};
    const uint8_t expected_empty[] = {0x31u, 0x05u, 0, 3, 's', '/', 't'};
    const uint8_t expected_binary[] = {0x31u, 0x08u, 0, 3, 's', '/', 't', 0, 0xffu, 0x80u};
    esy_mqtt_message_t message = sample_message(ESY_MQTT_QOS_0);
    uint16_t packet_id;
    connect_client();
    message.payload = NULL;
    message.payload_size = 0u;
    message.retain = true;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    TEST_ASSERT_EQUAL(sizeof(expected_empty), s_ctx.tx_size);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_empty, s_ctx.tx, sizeof(expected_empty));
    s_ctx.tx_size = 0u;
    message.payload = binary;
    message.payload_size = sizeof(binary);
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    TEST_ASSERT_EQUAL(sizeof(expected_binary), s_ctx.tx_size);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_binary, s_ctx.tx, sizeof(expected_binary));
}

void test_suback_cannot_grant_a_higher_qos_than_requested(void)
{
    uint8_t suback[] = {0x90u, 0x03u, 0, 0, 1};
    uint16_t packet_id;
    connect_client();
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_subscribe(&s_ctx.client, string_view("a"), ESY_MQTT_QOS_0, &packet_id));
    suback[2] = (uint8_t)(packet_id >> 8);
    suback[3] = (uint8_t)packet_id;
    queue_bytes(suback, sizeof(suback));
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
    TEST_ASSERT_EQUAL(2u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, s_ctx.events[0].result);
}

void test_suback_must_have_exactly_one_return_code(void)
{
    uint8_t suback[] = {0x90u, 0x04u, 0, 0, 0, 0};
    uint16_t packet_id;
    connect_client();
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_subscribe(&s_ctx.client, string_view("a"), ESY_MQTT_QOS_0, &packet_id));
    suback[2] = (uint8_t)(packet_id >> 8);
    suback[3] = (uint8_t)packet_id;
    queue_bytes(suback, sizeof(suback));
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
}

void test_unexpected_ack_is_fatal_without_reporting_a_false_completion(void)
{
    connect_client();
    queue_ack(0xb0u, 999u);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_DISCONNECTED, s_ctx.events[0].type);
}

void test_send_provider_oversize_count_is_an_io_error(void)
{
    esy_mqtt_message_t message = sample_message(ESY_MQTT_QOS_0);
    uint16_t packet_id;
    connect_client();
    s_ctx.send_oversize = true;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
}

void test_keepalive_sends_ping_at_boundary_and_times_out_without_pingresp(void)
{
    const uint8_t connack[] = {0x20u, 0x02u, 0x00u, 0x00u};
    const uint8_t pingreq[] = {0xc0u, 0x00u};
    esy_mqtt_connect_config_t config = connect_config();
    config.keep_alive_seconds = 1u;
    s_ctx.clock_step = 0u;
    initialize();
    queue_bytes(connack, sizeof(connack));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_connect(&s_ctx.client, &config));
    s_ctx.tx_size = 0u;
    s_ctx.now = 999u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(0u, s_ctx.tx_size);
    s_ctx.now = 1000u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(sizeof(pingreq), s_ctx.tx_size);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pingreq, s_ctx.tx, sizeof(pingreq));
    s_ctx.now = 1020u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    s_ctx.now = 1021u;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_DISCONNECTED, s_ctx.events[0].type);
}

void test_continuous_qos_zero_receive_cannot_starve_keepalive_or_ping_timeout(void)
{
    const uint8_t incoming[] = {0x30u, 0x04u, 0, 1, 'a', 'x'};
    const uint8_t pingreq[] = {0xc0u, 0x00u};
    connect_keepalive_client();
    s_ctx.now = 500u;
    queue_bytes(incoming, sizeof(incoming));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(0u, s_ctx.tx_size);
    s_ctx.now = 1000u;
    queue_bytes(incoming, sizeof(incoming));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(sizeof(pingreq), s_ctx.tx_size);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pingreq, s_ctx.tx, sizeof(pingreq));
    s_ctx.now = 1020u;
    queue_bytes(incoming, sizeof(incoming));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(3u, s_ctx.event_count);
    s_ctx.now = 1021u;
    queue_bytes(incoming, sizeof(incoming));
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_DISCONNECTED,
                      s_ctx.events[s_ctx.event_count - 1u].type);
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT,
                      s_ctx.events[s_ctx.event_count - 1u].result);
    TEST_ASSERT_EQUAL(sizeof(pingreq), s_ctx.tx_size);
}

void test_pingresp_retires_wait_and_next_keepalive_starts_a_new_wait(void)
{
    const uint8_t pingresp[] = {0xd0u, 0x00u};
    const uint8_t two_pings[] = {0xc0u, 0x00u, 0xc0u, 0x00u};
    connect_keepalive_client();
    s_ctx.now = 1000u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    s_ctx.now = 1020u;
    queue_bytes(pingresp, sizeof(pingresp));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    s_ctx.now = 1021u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(0u, s_ctx.event_count);
    s_ctx.now = 1999u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(2u, s_ctx.tx_size);
    s_ctx.now = 2000u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(sizeof(two_pings), s_ctx.tx_size);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(two_pings, s_ctx.tx, sizeof(two_pings));
    s_ctx.now = 2021u;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
    TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_DISCONNECTED, s_ctx.events[0].type);
}

void test_pingresp_already_late_before_process_cannot_revive_connection(void)
{
    const uint8_t pingresp[] = {0xd0u, 0x00u};
    size_t received_before;
    connect_keepalive_client();
    s_ctx.now = 1000u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    s_ctx.now = 1021u;
    queue_bytes(pingresp, sizeof(pingresp));
    received_before = s_ctx.rx_offset;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(received_before, s_ctx.rx_offset);
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
    TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_DISCONNECTED, s_ctx.events[0].type);
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, s_ctx.events[0].result);
}

void test_receive_crossing_pingresp_deadline_rejects_late_response(void)
{
    const uint8_t pingresp[] = {0xd0u, 0x00u};
    connect_keepalive_client();
    s_ctx.now = 1000u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    s_ctx.now = 1019u;
    s_ctx.recv_advance_ms = 2u;
    queue_bytes(pingresp, sizeof(pingresp));
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
    TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_DISCONNECTED, s_ctx.events[0].type);
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, s_ctx.events[0].result);
}

void test_completed_ping_wait_stays_retired_after_disabled_reconnect_and_full_period(void)
{
    const uint8_t pingresp[] = {0xd0u, 0x00u};
    const uint8_t connack[] = {0x20u, 0x02u, 0x00u, 0x00u};
    esy_mqtt_connect_config_t config = connect_config();
    unsigned index;
    connect_keepalive_client();
    s_ctx.now = 1000u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    s_ctx.now = 1020u;
    queue_bytes(pingresp, sizeof(pingresp));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(0u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_disconnect(&s_ctx.client));
    queue_bytes(connack, sizeof(connack));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_connect(&s_ctx.client, &config));
    s_ctx.tx_size = 0u;
    for (index = 0u; index < 4u; ++index) {
        s_ctx.now += UINT32_C(0x40000000);
        TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    }
    s_ctx.now += 21u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_TRUE(esy_mqtt_is_connected(&s_ctx.client));
    TEST_ASSERT_EQUAL(0u, s_ctx.tx_size);
    TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_DISCONNECTED, s_ctx.events[0].type);
}

void test_keepalive_and_pingresp_wait_cross_uint32_wrap_at_their_boundaries(void)
{
    const uint8_t pingreq[] = {0xc0u, 0x00u};
    const uint32_t starts[] = {UINT32_MAX - 499u, UINT32_MAX - 1009u};
    unsigned index;
    for (index = 0u; index < sizeof(starts) / sizeof(starts[0]); ++index) {
        uint32_t started = starts[index];
        setUp();
        s_ctx.now = started;
        connect_keepalive_client();
        s_ctx.now = started + 999u;
        TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
        TEST_ASSERT_EQUAL(0u, s_ctx.tx_size);
        s_ctx.now = started + 1000u;
        TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
        TEST_ASSERT_EQUAL(sizeof(pingreq), s_ctx.tx_size);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(pingreq, s_ctx.tx, sizeof(pingreq));
        s_ctx.now = started + 1020u;
        TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
        s_ctx.now = started + 1021u;
        TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_mqtt_process(&s_ctx.client));
        TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
        TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
        TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, s_ctx.events[0].result);
        TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_deinit(&s_ctx.client));
    }
}

void test_abort_retires_ping_wait_across_full_clock_period_and_reconnects_fresh(void)
{
    const uint8_t connack[] = {0x20u, 0x02u, 0x00u, 0x00u};
    esy_mqtt_connect_config_t config = connect_config();
    uint32_t restarted;
    unsigned index;
    connect_keepalive_client();
    s_ctx.now = 1000u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(2u, s_ctx.tx_size);
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_abort(&s_ctx.client));
    TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
    for (index = 0u; index < 4u; ++index) {
        s_ctx.now += UINT32_C(0x40000000);
        TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_mqtt_process(&s_ctx.client));
        TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_abort(&s_ctx.client));
    }
    TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(2u, s_ctx.tx_size);
    config.keep_alive_seconds = 1u;
    queue_bytes(connack, sizeof(connack));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_connect(&s_ctx.client, &config));
    restarted = s_ctx.now;
    s_ctx.tx_size = 0u;
    s_ctx.now = restarted + 999u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(0u, s_ctx.tx_size);
    s_ctx.now = restarted + 1000u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(2u, s_ctx.tx_size);
    s_ctx.now = restarted + 1021u;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(2u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_DISCONNECTED, s_ctx.events[1].type);
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, s_ctx.events[1].result);
}

void test_oversized_packet_is_fatal_without_draining_payload_and_cancels_pending(void)
{
    uint8_t incoming[25] = {0x30u, 23u, 0, 1, 'a'};
    esy_mqtt_message_t message = sample_message(ESY_MQTT_QOS_1);
    uint16_t publish_id;
    uint16_t subscribe_id;
    size_t received_before;
    s_ctx.resources.network_buffer_size = ESY_MQTT_MIN_BUFFER_SIZE;
    connect_client();
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_publish(&s_ctx.client, &message, &publish_id));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_subscribe(&s_ctx.client, string_view("a"), ESY_MQTT_QOS_0, &subscribe_id));
    s_ctx.clock_step = 0u;
    queue_bytes(incoming, sizeof(incoming));
    received_before = s_ctx.rx_offset;
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
    TEST_ASSERT_LESS_OR_EQUAL(ESY_MQTT_MIN_BUFFER_SIZE,
                              s_ctx.rx_offset - received_before);
    TEST_ASSERT_LESS_THAN(s_ctx.rx_size, s_ctx.rx_offset);
    TEST_ASSERT_EQUAL(3u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_PUBLISHED, s_ctx.events[0].type);
    TEST_ASSERT_EQUAL(publish_id, s_ctx.events[0].packet_id);
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, s_ctx.events[0].result);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_SUBSCRIBED, s_ctx.events[1].type);
    TEST_ASSERT_EQUAL(subscribe_id, s_ctx.events[1].packet_id);
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, s_ctx.events[1].result);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_DISCONNECTED, s_ctx.events[2].type);
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, s_ctx.events[2].result);
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_deinit(&s_ctx.client));
}

void test_split_multibyte_remaining_length_rejects_oversized_packet_at_header_end(void)
{
    uint8_t incoming[131] = {0x30u, 0x80u, 0x01u, 0, 1, 'a'};
    size_t received_before;
    s_ctx.resources.network_buffer_size = ESY_MQTT_MIN_BUFFER_SIZE;
    connect_client();
    s_ctx.recv_chunk = 1u;
    queue_bytes(incoming, sizeof(incoming));
    received_before = s_ctx.rx_offset;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(3u, s_ctx.rx_offset - received_before);
    TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_DISCONNECTED, s_ctx.events[0].type);
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, s_ctx.events[0].result);
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
}

void test_oversized_header_after_buffered_message_is_checked_across_receive_calls(void)
{
    const uint8_t message[] = {0x30u, 0x04u, 0, 1, 'a', 'x'};
    uint8_t oversized[131] = {0x30u, 0x80u, 0x01u, 0, 1, 'a'};
    size_t received_before;
    s_ctx.resources.network_buffer_size = ESY_MQTT_MIN_BUFFER_SIZE;
    connect_client();
    queue_bytes(message, sizeof(message));
    queue_bytes(oversized, sizeof(oversized));
    received_before = s_ctx.rx_offset;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_MQTT_MIN_BUFFER_SIZE,
                      s_ctx.rx_offset - received_before);
    TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_MESSAGE, s_ctx.events[0].type);
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_LESS_THAN(s_ctx.rx_size, s_ctx.rx_offset);
    TEST_ASSERT_EQUAL(2u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_DISCONNECTED, s_ctx.events[1].type);
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, s_ctx.events[1].result);
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
}

void test_buffered_complete_oversized_header_is_rejected_before_another_receive(void)
{
    const uint8_t message[] = {0x30u, 0x04u, 0, 1, 'a', 'x'};
    uint8_t oversized[25] = {0x30u, 23u, 0, 1, 'a'};
    size_t received_before;
    s_ctx.resources.network_buffer_size = ESY_MQTT_MIN_BUFFER_SIZE;
    connect_client();
    queue_bytes(message, sizeof(message));
    queue_bytes(oversized, sizeof(oversized));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_MESSAGE, s_ctx.events[0].type);
    received_before = s_ctx.rx_offset;
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(received_before, s_ctx.rx_offset);
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
    TEST_ASSERT_EQUAL(2u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_DISCONNECTED, s_ctx.events[1].type);
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, s_ctx.events[1].result);
}

void test_packets_exactly_filling_network_buffer_are_delivered_without_false_overflow(void)
{
    const uint8_t incoming[] = {0x30u, 0x06u, 0, 1, 'a', '1', '2', '3'};
    s_ctx.resources.network_buffer_size = ESY_MQTT_MIN_BUFFER_SIZE;
    connect_client();
    queue_bytes(incoming, sizeof(incoming));
    queue_bytes(incoming, sizeof(incoming));
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_MESSAGE, s_ctx.events[0].type);
    TEST_ASSERT_EQUAL_STRING("a", s_ctx.message_topic);
    TEST_ASSERT_EQUAL(3u, s_ctx.events[0].message.payload_size);
    TEST_ASSERT_EQUAL_MEMORY("123", s_ctx.message_payload, 3u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(2u, s_ctx.event_count);
    TEST_ASSERT_TRUE(esy_mqtt_is_connected(&s_ctx.client));
}

void test_exact_capacity_packet_with_split_multibyte_remaining_length_is_valid(void)
{
    uint8_t incoming[131] = {0x30u, 0x80u, 0x01u, 0, 1, 'a'};
    size_t index;
    s_ctx.resources.network_buffer_size = sizeof(incoming);
    connect_client();
    s_ctx.recv_chunk = 1u;
    queue_bytes(incoming, sizeof(incoming));
    for (index = 0u; index < sizeof(incoming) - 1u; ++index) {
        TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
        TEST_ASSERT_EQUAL(0u, s_ctx.event_count);
    }
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_MESSAGE, s_ctx.events[0].type);
    TEST_ASSERT_EQUAL(125u, s_ctx.events[0].message.payload_size);
    TEST_ASSERT_EQUAL_STRING("a", s_ctx.message_topic);
    TEST_ASSERT_TRUE(esy_mqtt_is_connected(&s_ctx.client));
}

void test_unterminated_maximum_remaining_length_is_a_fatal_protocol_error(void)
{
    const uint8_t malformed[] = {0x30u, 0x80u, 0x80u, 0x80u, 0x80u};
    connect_client();
    queue_bytes(malformed, sizeof(malformed));
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
    TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_DISCONNECTED, s_ctx.events[0].type);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, s_ctx.events[0].result);
}

void test_maximum_acknowledgement_wait_expires_at_its_boundary(void)
{
    esy_mqtt_message_t message = sample_message(ESY_MQTT_QOS_1);
    uint16_t packet_id;
    s_ctx.config.acknowledgement_timeout_ms = INT32_MAX;
    connect_client();
    s_ctx.clock_step = 0u;
    s_ctx.now = UINT32_MAX - 100u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    s_ctx.now += (uint32_t)INT32_MAX - 1u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(0u, s_ctx.event_count);
    s_ctx.now += 1u;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(2u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(packet_id, s_ctx.events[0].packet_id);
}

void test_ack_received_one_millisecond_before_deadline_completes_successfully(void)
{
    esy_mqtt_message_t message = sample_message(ESY_MQTT_QOS_1);
    uint16_t packet_id;
    connect_client();
    s_ctx.clock_step = 0u;
    s_ctx.now = 1000u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    s_ctx.now = 1099u;
    queue_ack(0x40u, packet_id);
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_TRUE(esy_mqtt_is_connected(&s_ctx.client));
    TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_PUBLISHED, s_ctx.events[0].type);
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.events[0].result);
}

void test_ack_queued_at_deadline_cannot_complete_an_expired_operation(void)
{
    esy_mqtt_message_t message = sample_message(ESY_MQTT_QOS_1);
    uint16_t packet_id;
    size_t received_before;
    connect_client();
    s_ctx.clock_step = 0u;
    s_ctx.now = 1000u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    s_ctx.now = 1100u;
    queue_ack(0x40u, packet_id);
    received_before = s_ctx.rx_offset;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(received_before, s_ctx.rx_offset);
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
    TEST_ASSERT_EQUAL(2u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_PUBLISHED, s_ctx.events[0].type);
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, s_ctx.events[0].result);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_DISCONNECTED, s_ctx.events[1].type);
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, s_ctx.events[1].result);
}

void test_ack_deadline_order_is_identical_across_uint32_wrap(void)
{
    esy_mqtt_message_t message = sample_message(ESY_MQTT_QOS_1);
    uint16_t packet_id;
    unsigned elapsed;
    for (elapsed = 99u; elapsed <= 100u; ++elapsed) {
        setUp();
        connect_client();
        s_ctx.clock_step = 0u;
        s_ctx.now = UINT32_MAX - 49u;
        TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
        s_ctx.now += elapsed;
        queue_ack(0x40u, packet_id);
        if (elapsed == 99u) {
            TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_process(&s_ctx.client));
            TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
            TEST_ASSERT_EQUAL(ESY_OK, s_ctx.events[0].result);
        } else {
            TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_mqtt_process(&s_ctx.client));
            TEST_ASSERT_EQUAL(2u, s_ctx.event_count);
            TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, s_ctx.events[0].result);
            TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_DISCONNECTED, s_ctx.events[1].type);
        }
        TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_abort(&s_ctx.client));
        TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_deinit(&s_ctx.client));
    }
}

void test_receive_crossing_deadline_rejects_the_late_ack(void)
{
    esy_mqtt_message_t message = sample_message(ESY_MQTT_QOS_1);
    uint16_t packet_id;
    connect_client();
    s_ctx.clock_step = 0u;
    s_ctx.now = 1000u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    s_ctx.now = 1099u;
    s_ctx.recv_advance_ms = 2u;
    queue_ack(0x40u, packet_id);
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
    TEST_ASSERT_EQUAL(2u, s_ctx.event_count);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_PUBLISHED, s_ctx.events[0].type);
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, s_ctx.events[0].result);
    TEST_ASSERT_EQUAL(ESY_MQTT_EVENT_DISCONNECTED, s_ctx.events[1].type);
}

void test_shared_stream_progress_advances_synchronous_connect_and_publish(void)
{
    s_ctx.config.transport.progress = fixture_progress;
    s_ctx.connack_on_progress = true;
    initialize();
    esy_mqtt_connect_config_t connection = connect_config();
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_connect(&s_ctx.client, &connection));
    TEST_ASSERT_TRUE(s_ctx.progress_calls > 1u);
    unsigned before = s_ctx.progress_calls;
    esy_mqtt_message_t message = sample_message(ESY_MQTT_QOS_0);
    uint16_t packet_id = 0u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_mqtt_publish(&s_ctx.client, &message, &packet_id));
    TEST_ASSERT_TRUE(s_ctx.progress_calls > before);
    s_ctx.progress_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_mqtt_process(&s_ctx.client));
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
}

void test_shared_stream_progress_failure_stops_connect_before_accepting_bytes(void)
{
    s_ctx.config.transport.progress = fixture_progress;
    s_ctx.progress_result = ESY_ERR_IO;
    initialize();
    esy_mqtt_connect_config_t connection = connect_config();
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_mqtt_connect(&s_ctx.client, &connection));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.tx_size);
    TEST_ASSERT_FALSE(esy_mqtt_is_connected(&s_ctx.client));
}
