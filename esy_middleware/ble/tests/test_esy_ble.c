/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/ble/esy_ble.h"
#include "unity.h"
#include <string.h>

typedef enum {
    CALL_INIT,
    CALL_DEINIT,
    CALL_ADV_START,
    CALL_ADV_STOP,
    CALL_SCAN_START,
    CALL_SCAN_STOP,
    CALL_CONNECT,
    CALL_DISCONNECT,
    CALL_SECURITY,
    CALL_BONDS,
    CALL_UNPAIR,
    CALL_NOTIFY,
    CALL_GATT_REGISTER,
    CALL_GATT_UNREGISTER,
    CALL_AUTH_REPLY,
    CALL_COUNT
} provider_call_t;

typedef struct {
    esy_ble_t ble;
    esy_err_t result;
    unsigned calls[CALL_COUNT];
    void *last_context;
    esy_ble_event_fn sink;
    void *sink_user;
    const esy_ble_adv_params_t *advertising;
    const esy_ble_scan_params_t *scan;
    const esy_ble_connect_params_t *connect;
    esy_ble_connection_t connection;
    uint8_t reason;
    uint8_t level;
    const esy_ble_address_t *unpair_peer;
    esy_ble_bond_fn visitor;
    void *visitor_user;
    uint16_t value_handle;
    const void *data;
    size_t length;
    unsigned received_events;
    const esy_ble_event_t *received_event;
    void *received_user;
    unsigned visited_bonds;
    esy_ble_address_t visited[2];
    void *bond_user;
    int provider_identity;
    int user_identity;
    esy_ble_gatt_characteristic_t characteristics[2];
    esy_ble_gatt_service_t service;
    esy_ble_gatt_service_t *last_service;
    esy_ble_gatt_service_t *registered_service;
    esy_ble_auth_reply_t reply;
    uint32_t passkey;
    uint8_t att_result;
    unsigned read_calls;
    unsigned write_calls;
    void *attribute_user;
    uint16_t offset;
    void *read_buffer;
    size_t read_capacity;
    size_t *read_length;
    bool write_command;
} ble_test_ctx_t;

static ble_test_ctx_t s_ctx;

static const esy_ble_address_t k_bonds[2] = {
    {.type = ESY_BLE_ADDRESS_PUBLIC, .bytes = {1u, 2u, 3u, 4u, 5u, 6u}},
    {.type = ESY_BLE_ADDRESS_RANDOM, .bytes = {7u, 8u, 9u, 10u, 11u, 12u}}
};

static esy_err_t record_call(provider_call_t call, void *context)
{
    ++s_ctx.calls[call];
    s_ctx.last_context = context;
    return s_ctx.result;
}

static esy_err_t provider_init(void *context, esy_ble_event_fn event, void *user)
{
    s_ctx.sink = event;
    s_ctx.sink_user = user;
    return record_call(CALL_INIT, context);
}

static esy_err_t provider_deinit(void *context)
{
    esy_err_t result = record_call(CALL_DEINIT, context);
    if (result == ESY_OK) {
        if (s_ctx.registered_service != NULL) {
            s_ctx.registered_service->handle = 0u;
            for (size_t i = 0u; i < s_ctx.registered_service->characteristic_count; ++i) {
                s_ctx.registered_service->characteristics[i].value_handle = 0u;
            }
            s_ctx.registered_service = NULL;
        }
        s_ctx.sink = NULL;
        s_ctx.sink_user = NULL;
    }
    return result;
}

static esy_err_t provider_adv_start(void *context, const esy_ble_adv_params_t *params)
{
    s_ctx.advertising = params;
    return record_call(CALL_ADV_START, context);
}

static esy_err_t provider_adv_stop(void *context)
{
    return record_call(CALL_ADV_STOP, context);
}

static esy_err_t provider_scan_start(void *context, const esy_ble_scan_params_t *params)
{
    s_ctx.scan = params;
    return record_call(CALL_SCAN_START, context);
}

static esy_err_t provider_scan_stop(void *context)
{
    return record_call(CALL_SCAN_STOP, context);
}

static esy_err_t provider_connect(void *context, const esy_ble_connect_params_t *params)
{
    s_ctx.connect = params;
    return record_call(CALL_CONNECT, context);
}

static esy_err_t provider_disconnect(void *context, esy_ble_connection_t connection, uint8_t reason)
{
    s_ctx.connection = connection;
    s_ctx.reason = reason;
    return record_call(CALL_DISCONNECT, context);
}

static esy_err_t provider_security(void *context, esy_ble_connection_t connection, uint8_t level)
{
    s_ctx.connection = connection;
    s_ctx.level = level;
    return record_call(CALL_SECURITY, context);
}

static esy_err_t provider_bonds(void *context, esy_ble_bond_fn visitor, void *user)
{
    s_ctx.visitor = visitor;
    s_ctx.visitor_user = user;
    esy_err_t result = record_call(CALL_BONDS, context);
    if (result == ESY_OK) {
        for (size_t i = 0u; i < sizeof(k_bonds) / sizeof(k_bonds[0]); ++i) {
            visitor(user, &k_bonds[i]);
        }
    }
    return result;
}

static esy_err_t provider_unpair(void *context, const esy_ble_address_t *peer)
{
    s_ctx.unpair_peer = peer;
    return record_call(CALL_UNPAIR, context);
}

static esy_err_t provider_notify(void *context, esy_ble_connection_t connection,
                               uint16_t value_handle, const void *data, size_t length)
{
    s_ctx.connection = connection;
    s_ctx.value_handle = value_handle;
    s_ctx.data = data;
    s_ctx.length = length;
    return record_call(CALL_NOTIFY, context);
}

static esy_err_t provider_gatt_register(void *context, esy_ble_gatt_service_t *service)
{
    s_ctx.last_service = service;
    esy_err_t result = record_call(CALL_GATT_REGISTER, context);
    if (result == ESY_OK) {
        if (s_ctx.registered_service != NULL) {
            return ESY_ERR_BUSY;
        }
        s_ctx.registered_service = service;
        service->handle = 0x10u;
        for (size_t i = 0u; i < service->characteristic_count; ++i) {
            service->characteristics[i].value_handle = (uint16_t)(0x11u + i);
        }
    }
    return result;
}

static esy_err_t provider_gatt_unregister(void *context, esy_ble_gatt_service_t *service)
{
    s_ctx.last_service = service;
    esy_err_t result = record_call(CALL_GATT_UNREGISTER, context);
    if (result == ESY_OK) {
        if (service != s_ctx.registered_service) {
            return ESY_ERR_STATE;
        }
        s_ctx.registered_service = NULL;
    }
    return result;
}

static esy_err_t provider_auth_reply(void *context, esy_ble_connection_t connection,
                                   esy_ble_auth_reply_t reply, uint32_t passkey)
{
    s_ctx.connection = connection;
    s_ctx.reply = reply;
    s_ctx.passkey = passkey;
    return record_call(CALL_AUTH_REPLY, context);
}

static const esy_ble_ops_t k_ops = {
    .init = provider_init,
    .deinit = provider_deinit,
    .adv_start = provider_adv_start,
    .adv_stop = provider_adv_stop,
    .scan_start = provider_scan_start,
    .scan_stop = provider_scan_stop,
    .connect = provider_connect,
    .disconnect = provider_disconnect,
    .security = provider_security,
    .bonds_foreach = provider_bonds,
    .unpair = provider_unpair,
    .notify = provider_notify,
    .gatt_register = provider_gatt_register,
    .gatt_unregister = provider_gatt_unregister,
    .auth_reply = provider_auth_reply
};

static const esy_ble_ops_t k_lifecycle_ops = {
    .init = provider_init,
    .deinit = provider_deinit
};

static void receive_event(void *user, const esy_ble_event_t *event)
{
    ++s_ctx.received_events;
    s_ctx.received_user = user;
    s_ctx.received_event = event;
}

static void receive_bond(void *user, const esy_ble_address_t *peer)
{
    TEST_ASSERT_LESS_THAN_UINT32(2u, s_ctx.visited_bonds);
    s_ctx.visited[s_ctx.visited_bonds++] = *peer;
    s_ctx.bond_user = user;
}

static uint8_t read_attribute(void *user, esy_ble_connection_t connection,
                              uint16_t offset, void *buffer, size_t capacity, size_t *length)
{
    ++s_ctx.read_calls;
    s_ctx.attribute_user = user;
    s_ctx.connection = connection;
    s_ctx.offset = offset;
    s_ctx.read_buffer = buffer;
    s_ctx.read_capacity = capacity;
    s_ctx.read_length = length;
    if (s_ctx.att_result == ESY_BLE_ATT_SUCCESS) {
        *length = 0u;
    }
    return s_ctx.att_result;
}

static uint8_t write_attribute(void *user, esy_ble_connection_t connection,
                               uint16_t offset, const void *data, size_t length, bool command)
{
    ++s_ctx.write_calls;
    s_ctx.attribute_user = user;
    s_ctx.connection = connection;
    s_ctx.offset = offset;
    s_ctx.data = data;
    s_ctx.length = length;
    s_ctx.write_command = command;
    return s_ctx.att_result;
}

static void reset_service(void)
{
    memset(s_ctx.characteristics, 0, sizeof(s_ctx.characteristics));
    for (size_t i = 0u; i < sizeof(s_ctx.characteristics) / sizeof(s_ctx.characteristics[0]); ++i) {
        s_ctx.characteristics[i].uuid.length = 2u;
        s_ctx.characteristics[i].uuid.bytes[0] = (uint8_t)(0x37u + i);
        s_ctx.characteristics[i].uuid.bytes[1] = 0x2au;
        s_ctx.characteristics[i].properties = ESY_BLE_GATT_READ | ESY_BLE_GATT_WRITE |
                                             ESY_BLE_GATT_WRITE_COMMAND | ESY_BLE_GATT_NOTIFY;
        s_ctx.characteristics[i].read_security = 1u;
        s_ctx.characteristics[i].write_security = 1u;
        s_ctx.characteristics[i].notify_security = 1u;
        s_ctx.characteristics[i].read = read_attribute;
        s_ctx.characteristics[i].write = write_attribute;
        s_ctx.characteristics[i].user = &s_ctx.user_identity;
    }
    esy_ble_gatt_service_t service = {
        .uuid = {.length = 2u, .bytes = {0x0du, 0x18u}}, .primary = true,
        .characteristics = s_ctx.characteristics, .characteristic_count = 2u
    };
    s_ctx.service = service;
}

static void init_with_ops(const esy_ble_ops_t *ops)
{
    esy_ble_provider_t provider = {.ops = ops, .ctx = &s_ctx.provider_identity};
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_init(&s_ctx.ble, &provider, receive_event, &s_ctx.user_identity));
}

static esy_ble_adv_params_t advertising_params(void)
{
    esy_ble_adv_params_t params = {
        .connectable = true, .interval_min = 0x0020u, .interval_max = 0x4000u
    };
    return params;
}

static esy_ble_scan_params_t scan_params(void)
{
    esy_ble_scan_params_t params = {
        .active = true, .filter_duplicates = true, .interval = 0x4000u, .window = 0x0004u
    };
    return params;
}

static esy_ble_connect_params_t connect_params(void)
{
    esy_ble_connect_params_t params = {
        .peer = {.type = ESY_BLE_ADDRESS_PUBLIC, .bytes = {1u, 2u, 3u, 4u, 5u, 6u}},
        .interval_min = 0x0006u, .interval_max = 0x0006u,
        .latency = 0u, .supervision_timeout = 0x000au
    };
    return params;
}

static void assert_control_status(esy_ble_t *ble, esy_err_t expected)
{
    esy_ble_adv_params_t advertising = advertising_params();
    esy_ble_scan_params_t scan = scan_params();
    esy_ble_connect_params_t connect = connect_params();
    TEST_ASSERT_EQUAL_INT(expected, esy_ble_adv_start(ble, &advertising));
    TEST_ASSERT_EQUAL_INT(expected, esy_ble_adv_stop(ble));
    TEST_ASSERT_EQUAL_INT(expected, esy_ble_scan_start(ble, &scan));
    TEST_ASSERT_EQUAL_INT(expected, esy_ble_scan_stop(ble));
    TEST_ASSERT_EQUAL_INT(expected, esy_ble_connect(ble, &connect));
    TEST_ASSERT_EQUAL_INT(expected, esy_ble_disconnect(ble, 0u, 0x13u));
    TEST_ASSERT_EQUAL_INT(expected, esy_ble_security(ble, 0u, 1u));
    TEST_ASSERT_EQUAL_INT(expected, esy_ble_bonds_foreach(ble, receive_bond, NULL));
    TEST_ASSERT_EQUAL_INT(expected, esy_ble_unpair(ble, NULL));
    TEST_ASSERT_EQUAL_INT(expected, esy_ble_notify(ble, 0u, 1u, NULL, 0u));
    TEST_ASSERT_EQUAL_INT(expected, esy_ble_gatt_register(ble, &s_ctx.service));
    uint16_t saved_service_handle = s_ctx.service.handle;
    s_ctx.service.handle = 1u;
    TEST_ASSERT_EQUAL_INT(expected, esy_ble_gatt_unregister(ble, &s_ctx.service));
    s_ctx.service.handle = saved_service_handle;
    TEST_ASSERT_EQUAL_INT(expected, esy_ble_auth_reply(ble, 0u, ESY_BLE_AUTH_PASSKEY_ENTRY, 123456u));
}

void setUp(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.result = ESY_OK;
    reset_service();
}

void tearDown(void)
{
    s_ctx.result = ESY_OK;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_deinit(&s_ctx.ble));
}

void test_init_requires_handle_provider_operations_and_event(void)
{
    esy_ble_provider_t provider = {.ops = &k_ops, .ctx = &s_ctx.provider_identity};
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_init(NULL, &provider, receive_event, NULL));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_init(&s_ctx.ble, NULL, receive_event, NULL));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_init(&s_ctx.ble, &provider, NULL, NULL));
    provider.ops = NULL;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_init(&s_ctx.ble, &provider, receive_event, NULL));
    esy_ble_ops_t missing = k_ops;
    provider.ops = &missing;
    missing.init = NULL;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_init(&s_ctx.ble, &provider, receive_event, NULL));
    missing = k_ops;
    missing.deinit = NULL;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_init(&s_ctx.ble, &provider, receive_event, NULL));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_INIT]);
    assert_control_status(&s_ctx.ble, ESY_ERR_STATE);
    init_with_ops(&k_ops);
}

void test_init_copies_descriptor_and_borrows_original_context_and_sink(void)
{
    esy_ble_provider_t provider = {.ops = &k_ops, .ctx = &s_ctx.provider_identity};
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_init(&s_ctx.ble, &provider, receive_event, &s_ctx.user_identity));
    TEST_ASSERT_EQUAL_PTR(&s_ctx.provider_identity, s_ctx.last_context);
    TEST_ASSERT_TRUE(s_ctx.sink == receive_event);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.user_identity, s_ctx.sink_user);
    provider.ops = NULL;
    provider.ctx = NULL;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_adv_stop(&s_ctx.ble));
    TEST_ASSERT_EQUAL_PTR(&s_ctx.provider_identity, s_ctx.last_context);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_deinit(&s_ctx.ble));
    TEST_ASSERT_EQUAL_PTR(&s_ctx.provider_identity, s_ctx.last_context);
    TEST_ASSERT_NULL(s_ctx.sink);
}

void test_init_allows_stateless_provider_and_null_user(void)
{
    esy_ble_provider_t provider = {.ops = &k_ops, .ctx = NULL};
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_init(&s_ctx.ble, &provider, receive_event, NULL));
    TEST_ASSERT_NULL(s_ctx.last_context);
    TEST_ASSERT_NULL(s_ctx.sink_user);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_adv_stop(&s_ctx.ble));
    TEST_ASSERT_NULL(s_ctx.last_context);
}

void test_duplicate_init_does_not_replace_active_binding(void)
{
    init_with_ops(&k_ops);
    esy_ble_provider_t different = {.ops = &k_lifecycle_ops, .ctx = NULL};
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ble_init(&s_ctx.ble, &different, receive_event, NULL));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.calls[CALL_INIT]);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_adv_stop(&s_ctx.ble));
    TEST_ASSERT_EQUAL_PTR(&s_ctx.provider_identity, s_ctx.last_context);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.user_identity, s_ctx.sink_user);
}

void test_failed_init_requires_deinit_and_preserves_binding_during_cleanup_retry(void)
{
    esy_ble_provider_t provider = {.ops = &k_ops, .ctx = &s_ctx.provider_identity};
    s_ctx.result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_ble_init(&s_ctx.ble, &provider, receive_event, &s_ctx.user_identity));
    assert_control_status(&s_ctx.ble, ESY_ERR_STATE);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ble_init(&s_ctx.ble, &provider, receive_event, NULL));
    s_ctx.result = ESY_ERR_BUSY;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_ble_deinit(&s_ctx.ble));
    TEST_ASSERT_EQUAL_PTR(&s_ctx.provider_identity, s_ctx.last_context);
    TEST_ASSERT_TRUE(s_ctx.sink == receive_event);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.user_identity, s_ctx.sink_user);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ble_init(&s_ctx.ble, &provider, receive_event, NULL));
    assert_control_status(&s_ctx.ble, ESY_ERR_STATE);
    s_ctx.result = ESY_OK;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_deinit(&s_ctx.ble));
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.calls[CALL_DEINIT]);
    TEST_ASSERT_NULL(s_ctx.sink);
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.calls[CALL_INIT]);
}

void test_deinit_failure_retains_binding_rejects_control_and_allows_only_cleanup_retry(void)
{
    init_with_ops(&k_ops);
    s_ctx.result = ESY_ERR_TIMEOUT;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_TIMEOUT, esy_ble_deinit(&s_ctx.ble));
    TEST_ASSERT_TRUE(s_ctx.sink == receive_event);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.user_identity, s_ctx.sink_user);
    assert_control_status(&s_ctx.ble, ESY_ERR_STATE);
    for (size_t i = CALL_ADV_START; i < CALL_COUNT; ++i) {
        TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[i]);
    }
    esy_ble_provider_t provider = {.ops = &k_ops, .ctx = &s_ctx.provider_identity};
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ble_init(&s_ctx.ble, &provider, receive_event, NULL));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.calls[CALL_INIT]);
    s_ctx.result = ESY_OK;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_deinit(&s_ctx.ble));
    TEST_ASSERT_EQUAL_PTR(&s_ctx.provider_identity, s_ctx.last_context);
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.calls[CALL_DEINIT]);
    TEST_ASSERT_NULL(s_ctx.sink);
    assert_control_status(&s_ctx.ble, ESY_ERR_STATE);
    init_with_ops(&k_ops);
}

void test_deinit_is_idempotent_for_detached_handle_and_rejects_null(void)
{
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_deinit(NULL));
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_deinit(&s_ctx.ble));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_DEINIT]);
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_deinit(&s_ctx.ble));
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_deinit(&s_ctx.ble));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.calls[CALL_DEINIT]);
}

void test_all_control_calls_reject_null_or_detached_handle(void)
{
    assert_control_status(NULL, ESY_ERR_INVALID_ARG);
    assert_control_status(&s_ctx.ble, ESY_ERR_STATE);
    for (size_t i = CALL_ADV_START; i < CALL_COUNT; ++i) {
        TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[i]);
    }
}

void test_each_missing_optional_operation_reports_not_supported(void)
{
    init_with_ops(&k_lifecycle_ops);
    assert_control_status(&s_ctx.ble, ESY_ERR_NOT_SUPPORTED);
}

void test_advertising_forwards_encoded_packets_parameters_and_provider_error(void)
{
    const uint8_t data[] = {2u, 1u, 6u, 3u, 9u, 'B', 'L'};
    const uint8_t response[] = {1u, 0xffu};
    esy_ble_adv_params_t params = advertising_params();
    params.data = data;
    params.data_length = sizeof(data);
    params.scan_response = response;
    params.scan_response_length = sizeof(response);
    init_with_ops(&k_ops);
    s_ctx.result = ESY_ERR_BUSY;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_ble_adv_start(&s_ctx.ble, &params));
    TEST_ASSERT_EQUAL_PTR(&params, s_ctx.advertising);
    TEST_ASSERT_EQUAL_PTR(data, s_ctx.advertising->data);
    TEST_ASSERT_EQUAL_PTR(response, s_ctx.advertising->scan_response);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.provider_identity, s_ctx.last_context);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_ble_adv_stop(&s_ctx.ble));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.calls[CALL_ADV_STOP]);
}

void test_advertising_accepts_empty_payload_full_legacy_payload_and_zero_padding(void)
{
    uint8_t full[ESY_BLE_LEGACY_DATA_MAX] = {30u, 0xffu};
    const uint8_t padding[] = {2u, 1u, 6u, 0u, 0u};
    esy_ble_adv_params_t params = advertising_params();
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_adv_start(&s_ctx.ble, &params));
    params.connectable = false;
    params.data = full;
    params.data_length = sizeof(full);
    params.scan_response = padding;
    params.scan_response_length = sizeof(padding);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_adv_start(&s_ctx.ble, &params));
    params.interval_min = 0x4000u;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_adv_start(&s_ctx.ble, &params));
}

void test_advertising_rejects_missing_params_or_invalid_interval(void)
{
    esy_ble_adv_params_t params = advertising_params();
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_adv_start(&s_ctx.ble, NULL));
    params.interval_min = 0x001fu;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_adv_start(&s_ctx.ble, &params));
    params = advertising_params();
    params.interval_max = 0x001fu;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_adv_start(&s_ctx.ble, &params));
    params.interval_max = 0x4001u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_adv_start(&s_ctx.ble, &params));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_ADV_START]);
}

void test_advertising_rejects_null_nonempty_or_oversized_data_and_response(void)
{
    const uint8_t data[ESY_BLE_LEGACY_DATA_MAX + 1u] = {0u};
    esy_ble_adv_params_t params = advertising_params();
    init_with_ops(&k_ops);
    params.data_length = 1u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_adv_start(&s_ctx.ble, &params));
    params.data = data;
    params.data_length = sizeof(data);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_adv_start(&s_ctx.ble, &params));
    params = advertising_params();
    params.scan_response_length = 1u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_adv_start(&s_ctx.ble, &params));
    params.scan_response = data;
    params.scan_response_length = sizeof(data);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_adv_start(&s_ctx.ble, &params));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_ADV_START]);
}

void test_advertising_rejects_truncated_structures_and_data_after_zero_terminator(void)
{
    const uint8_t truncated[] = {2u, 1u};
    const uint8_t trailing[] = {0u, 1u};
    esy_ble_adv_params_t params = advertising_params();
    init_with_ops(&k_ops);
    params.data = truncated;
    params.data_length = sizeof(truncated);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_adv_start(&s_ctx.ble, &params));
    params.data = trailing;
    params.data_length = sizeof(trailing);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_adv_start(&s_ctx.ble, &params));
    params = advertising_params();
    params.scan_response = truncated;
    params.scan_response_length = sizeof(truncated);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_adv_start(&s_ctx.ble, &params));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_ADV_START]);
}

void test_scan_forwards_parameters_provider_error_and_valid_boundaries(void)
{
    esy_ble_scan_params_t params = scan_params();
    init_with_ops(&k_ops);
    s_ctx.result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_ble_scan_start(&s_ctx.ble, &params));
    TEST_ASSERT_EQUAL_PTR(&params, s_ctx.scan);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.provider_identity, s_ctx.last_context);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_ble_scan_stop(&s_ctx.ble));
    s_ctx.result = ESY_OK;
    params.interval = 0x0004u;
    params.active = false;
    params.filter_duplicates = false;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_scan_start(&s_ctx.ble, &params));
    params.interval = 0x4000u;
    params.window = params.interval;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_scan_start(&s_ctx.ble, &params));
}

void test_scan_rejects_missing_params_and_invalid_interval_or_window(void)
{
    esy_ble_scan_params_t params = scan_params();
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_scan_start(&s_ctx.ble, NULL));
    params.interval = 3u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_scan_start(&s_ctx.ble, &params));
    params.interval = 0x4001u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_scan_start(&s_ctx.ble, &params));
    params = scan_params();
    params.window = 3u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_scan_start(&s_ctx.ble, &params));
    params.window = params.interval + 1u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_scan_start(&s_ctx.ble, &params));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_SCAN_START]);
}

void test_connect_forwards_parameters_peer_and_provider_error(void)
{
    esy_ble_connect_params_t params = connect_params();
    init_with_ops(&k_ops);
    s_ctx.result = ESY_ERR_BUSY;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_ble_connect(&s_ctx.ble, &params));
    TEST_ASSERT_EQUAL_PTR(&params, s_ctx.connect);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.provider_identity, s_ctx.last_context);
    s_ctx.result = ESY_OK;
    params.peer.type = ESY_BLE_ADDRESS_RANDOM;
    params.interval_min = 0x0c80u;
    params.interval_max = 0x0c80u;
    params.supervision_timeout = 0x0c80u;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_connect(&s_ctx.ble, &params));
    params.interval_min = 6u;
    params.interval_max = 6u;
    params.latency = 499u;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_connect(&s_ctx.ble, &params));
}

void test_connect_rejects_null_params_invalid_address_and_interval_ranges(void)
{
    esy_ble_connect_params_t params = connect_params();
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_connect(&s_ctx.ble, NULL));
    params.peer.type = 2u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_connect(&s_ctx.ble, &params));
    params = connect_params();
    params.interval_min = 5u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_connect(&s_ctx.ble, &params));
    params.interval_min = 7u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_connect(&s_ctx.ble, &params));
    params = connect_params();
    params.interval_max = 0x0c81u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_connect(&s_ctx.ble, &params));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_CONNECT]);
}

void test_connect_rejects_latency_and_supervision_timeout_outside_range(void)
{
    esy_ble_connect_params_t params = connect_params();
    init_with_ops(&k_ops);
    params.latency = 500u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_connect(&s_ctx.ble, &params));
    params = connect_params();
    params.supervision_timeout = 9u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_connect(&s_ctx.ble, &params));
    params.supervision_timeout = 0x0c81u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_connect(&s_ctx.ble, &params));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_CONNECT]);
}

void test_connect_supervision_timeout_must_strictly_exceed_twice_latency_interval(void)
{
    esy_ble_connect_params_t params = connect_params();
    init_with_ops(&k_ops);
    params.interval_max = 40u;
    params.latency = 1u;
    params.supervision_timeout = 19u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_connect(&s_ctx.ble, &params));
    params.supervision_timeout = 20u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_connect(&s_ctx.ble, &params));
    params.supervision_timeout = 21u;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_connect(&s_ctx.ble, &params));
    params.interval_max = 0x0c80u;
    params.latency = 499u;
    params.supervision_timeout = 0x0c80u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_connect(&s_ctx.ble, &params));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.calls[CALL_CONNECT]);
}

void test_disconnect_forwards_connection_reason_and_errors_and_rejects_invalid_id(void)
{
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_disconnect(&s_ctx.ble, ESY_BLE_CONNECTION_INVALID, 0u));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_DISCONNECT]);
    s_ctx.result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_ble_disconnect(&s_ctx.ble, 0u, 0x13u));
    TEST_ASSERT_EQUAL_UINT16(0u, s_ctx.connection);
    TEST_ASSERT_EQUAL_UINT8(0x13u, s_ctx.reason);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.provider_identity, s_ctx.last_context);
}

void test_security_validates_level_and_connection_and_forwards_provider_error(void)
{
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_security(&s_ctx.ble, ESY_BLE_CONNECTION_INVALID, 1u));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_security(&s_ctx.ble, 1u, 0u));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_security(&s_ctx.ble, 1u, 5u));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_SECURITY]);
    s_ctx.result = ESY_ERR_BUSY;
    for (uint8_t level = 1u; level <= 4u; ++level) {
        TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_ble_security(&s_ctx.ble, 2u, level));
        TEST_ASSERT_EQUAL_UINT16(2u, s_ctx.connection);
        TEST_ASSERT_EQUAL_UINT8(level, s_ctx.level);
    }
    TEST_ASSERT_EQUAL_PTR(&s_ctx.provider_identity, s_ctx.last_context);
}

void test_provider_event_preserves_sink_context_and_borrowed_scan_payload(void)
{
    const uint8_t data[] = {2u, 1u, 6u};
    esy_ble_event_t event = {
        .type = ESY_BLE_EVENT_SCAN_RESULT,
        .connection = ESY_BLE_CONNECTION_INVALID,
        .peer = {.type = ESY_BLE_ADDRESS_RANDOM, .bytes = {1u, 2u, 3u, 4u, 5u, 6u}},
        .status = ESY_OK, .rssi = -57, .data = data, .data_length = sizeof(data)
    };
    init_with_ops(&k_ops);
    s_ctx.sink(s_ctx.sink_user, &event);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.received_events);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.user_identity, s_ctx.received_user);
    TEST_ASSERT_EQUAL_PTR(&event, s_ctx.received_event);
    TEST_ASSERT_EQUAL_INT(ESY_BLE_EVENT_SCAN_RESULT, s_ctx.received_event->type);
    TEST_ASSERT_EQUAL_INT8(-57, s_ctx.received_event->rssi);
    TEST_ASSERT_EQUAL_PTR(data, s_ctx.received_event->data);
    TEST_ASSERT_EQUAL_MEMORY(data, s_ctx.received_event->data, sizeof(data));
}

void test_provider_event_preserves_pairing_and_connection_results(void)
{
    esy_ble_event_t event = {
        .type = ESY_BLE_EVENT_PAIRING_COMPLETE, .connection = 5u,
        .status = ESY_OK, .security_level = 4u, .bonded = true
    };
    init_with_ops(&k_ops);
    s_ctx.sink(s_ctx.sink_user, &event);
    TEST_ASSERT_EQUAL_PTR(&event, s_ctx.received_event);
    TEST_ASSERT_TRUE(s_ctx.received_event->bonded);
    TEST_ASSERT_EQUAL_UINT8(4u, s_ctx.received_event->security_level);
    event.type = ESY_BLE_EVENT_CONNECTED;
    event.connection = ESY_BLE_CONNECTION_INVALID;
    event.status = ESY_ERR_IO;
    event.reason = 0x3eu;
    s_ctx.sink(s_ctx.sink_user, &event);
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.received_events);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, s_ctx.received_event->status);
    TEST_ASSERT_EQUAL_UINT8(0x3eu, s_ctx.received_event->reason);
}

void test_bonds_enumerate_identity_addresses_and_forward_visitor_context(void)
{
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_bonds_foreach(&s_ctx.ble, receive_bond, &s_ctx.user_identity));
    TEST_ASSERT_TRUE(s_ctx.visitor == receive_bond);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.user_identity, s_ctx.visitor_user);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.user_identity, s_ctx.bond_user);
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.visited_bonds);
    TEST_ASSERT_EQUAL_MEMORY(k_bonds, s_ctx.visited, sizeof(k_bonds));
    TEST_ASSERT_EQUAL_PTR(&s_ctx.provider_identity, s_ctx.last_context);
}

void test_bonds_require_visitor_allow_null_user_and_forward_provider_failure(void)
{
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_bonds_foreach(&s_ctx.ble, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_BONDS]);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_bonds_foreach(&s_ctx.ble, receive_bond, NULL));
    TEST_ASSERT_NULL(s_ctx.bond_user);
    s_ctx.result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_ble_bonds_foreach(&s_ctx.ble, receive_bond, NULL));
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.visited_bonds);
}

void test_unpair_forwards_public_random_and_all_peers_and_provider_error(void)
{
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_unpair(&s_ctx.ble, &k_bonds[0]));
    TEST_ASSERT_EQUAL_PTR(&k_bonds[0], s_ctx.unpair_peer);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_unpair(&s_ctx.ble, &k_bonds[1]));
    TEST_ASSERT_EQUAL_PTR(&k_bonds[1], s_ctx.unpair_peer);
    s_ctx.result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_ble_unpair(&s_ctx.ble, NULL));
    TEST_ASSERT_NULL(s_ctx.unpair_peer);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.provider_identity, s_ctx.last_context);
}

void test_unpair_rejects_unsupported_address_type_without_calling_provider(void)
{
    esy_ble_address_t peer = {.type = 2u};
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_unpair(&s_ctx.ble, &peer));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_UNPAIR]);
}

void test_notify_forwards_live_id_local_handle_borrowed_data_length_and_errors(void)
{
    const uint8_t data[] = {0x80u, 0x01u, 0xffu};
    init_with_ops(&k_ops);
    s_ctx.result = ESY_ERR_BUSY;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_ble_notify(&s_ctx.ble, 0u, 0x1234u, data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT16(0u, s_ctx.connection);
    TEST_ASSERT_EQUAL_UINT16(0x1234u, s_ctx.value_handle);
    TEST_ASSERT_EQUAL_PTR(data, s_ctx.data);
    TEST_ASSERT_EQUAL_UINT(sizeof(data), s_ctx.length);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.provider_identity, s_ctx.last_context);
}

void test_notify_accepts_zero_length_and_maximum_public_length(void)
{
    uint8_t data[UINT16_MAX] = {0u};
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_notify(&s_ctx.ble, 1u, UINT16_MAX, NULL, 0u));
    TEST_ASSERT_NULL(s_ctx.data);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.length);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_notify(&s_ctx.ble, 1u, 1u, data, 0u));
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_notify(&s_ctx.ble, 1u, 1u, data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT(UINT16_MAX, s_ctx.length);
}

void test_notify_rejects_invalid_id_zero_handle_missing_data_and_oversized_length(void)
{
    const uint8_t data = 0u;
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_notify(&s_ctx.ble, ESY_BLE_CONNECTION_INVALID, 1u, &data, 1u));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_notify(&s_ctx.ble, 1u, 0u, &data, 1u));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_notify(&s_ctx.ble, 1u, 1u, NULL, 1u));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_notify(&s_ctx.ble, 1u, 1u, &data, (size_t)UINT16_MAX + 1u));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_NOTIFY]);
}

void test_gatt_register_forwards_schema_assigns_handles_and_rejects_duplicate_registration(void)
{
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
    TEST_ASSERT_EQUAL_PTR(&s_ctx.service, s_ctx.last_service);
    TEST_ASSERT_EQUAL_PTR(s_ctx.characteristics, s_ctx.last_service->characteristics);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.provider_identity, s_ctx.last_context);
    TEST_ASSERT_EQUAL_UINT16(0x10u, s_ctx.service.handle);
    TEST_ASSERT_EQUAL_UINT16(0x11u, s_ctx.characteristics[0].value_handle);
    TEST_ASSERT_EQUAL_UINT16(0x12u, s_ctx.characteristics[1].value_handle);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.calls[CALL_GATT_REGISTER]);
}

void test_gatt_register_provider_failure_preserves_schema_and_allows_retry(void)
{
    esy_ble_gatt_service_t service_before = s_ctx.service;
    esy_ble_gatt_characteristic_t characteristics_before[2];
    memcpy(characteristics_before, s_ctx.characteristics, sizeof(characteristics_before));
    init_with_ops(&k_ops);
    s_ctx.result = ESY_ERR_BUSY;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
    TEST_ASSERT_EQUAL_MEMORY(&service_before, &s_ctx.service, sizeof(service_before));
    TEST_ASSERT_EQUAL_MEMORY(characteristics_before, s_ctx.characteristics, sizeof(characteristics_before));
    TEST_ASSERT_NULL(s_ctx.registered_service);
    s_ctx.result = ESY_OK;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.calls[CALL_GATT_REGISTER]);
}

void test_gatt_register_accepts_uuid_widths_secondary_service_and_each_supported_property(void)
{
    static const uint8_t k_properties[] = {
        ESY_BLE_GATT_READ, ESY_BLE_GATT_WRITE_COMMAND, ESY_BLE_GATT_WRITE, ESY_BLE_GATT_NOTIFY
    };
    init_with_ops(&k_ops);
    for (size_t i = 0u; i < sizeof(k_properties) / sizeof(k_properties[0]); ++i) {
        reset_service();
        s_ctx.service.primary = false;
        s_ctx.service.uuid.length = 16u;
        s_ctx.characteristics[0].uuid.length = 16u;
        s_ctx.characteristics[0].properties = k_properties[i];
        s_ctx.service.characteristic_count = 1u;
        TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
        TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_gatt_unregister(&s_ctx.ble, &s_ctx.service));
    }
}

void test_gatt_register_rejects_null_schema_missing_characteristics_and_invalid_counts(void)
{
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_gatt_register(&s_ctx.ble, NULL));
    s_ctx.service.characteristics = NULL;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
    reset_service();
    s_ctx.service.characteristic_count = 0u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
    s_ctx.service.characteristic_count = (size_t)UINT16_MAX + 1u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_GATT_REGISTER]);
}

void test_gatt_register_rejects_invalid_service_or_characteristic_uuid_widths(void)
{
    static const uint8_t k_widths[] = {0u, 1u, 3u, 4u, 15u, 17u, UINT8_MAX};
    init_with_ops(&k_ops);
    for (size_t i = 0u; i < sizeof(k_widths) / sizeof(k_widths[0]); ++i) {
        reset_service();
        s_ctx.service.uuid.length = k_widths[i];
        TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
        reset_service();
        s_ctx.characteristics[1].uuid.length = k_widths[i];
        TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
    }
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_GATT_REGISTER]);
}

void test_gatt_register_rejects_zero_or_unsupported_property_bits(void)
{
    static const uint8_t k_properties[] = {0u, 0x01u, 0x20u, 0x40u, 0x80u, 0x1fu};
    init_with_ops(&k_ops);
    for (size_t i = 0u; i < sizeof(k_properties) / sizeof(k_properties[0]); ++i) {
        s_ctx.characteristics[1].properties = k_properties[i];
        TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
    }
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_GATT_REGISTER]);
}

void test_gatt_register_requires_read_callback_and_valid_read_security(void)
{
    init_with_ops(&k_ops);
    s_ctx.characteristics[1].read = NULL;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
    reset_service();
    s_ctx.characteristics[1].read_security = 0u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
    s_ctx.characteristics[1].read_security = 5u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_GATT_REGISTER]);
}

void test_gatt_register_requires_write_callback_and_security_for_both_write_modes(void)
{
    static const uint8_t k_properties[] = {ESY_BLE_GATT_WRITE, ESY_BLE_GATT_WRITE_COMMAND};
    init_with_ops(&k_ops);
    for (size_t i = 0u; i < sizeof(k_properties) / sizeof(k_properties[0]); ++i) {
        reset_service();
        s_ctx.characteristics[1].properties = k_properties[i];
        s_ctx.characteristics[1].write = NULL;
        TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
        s_ctx.characteristics[1].write = write_attribute;
        s_ctx.characteristics[1].write_security = 0u;
        TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
        s_ctx.characteristics[1].write_security = 5u;
        TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
    }
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_GATT_REGISTER]);
}

void test_gatt_register_requires_notification_security_and_accepts_all_security_levels(void)
{
    init_with_ops(&k_ops);
    s_ctx.characteristics[1].notify_security = 0u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
    s_ctx.characteristics[1].notify_security = 5u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
    for (uint8_t level = 1u; level <= 4u; ++level) {
        reset_service();
        s_ctx.characteristics[1].read_security = level;
        s_ctx.characteristics[1].write_security = level;
        s_ctx.characteristics[1].notify_security = level;
        TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
        TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_gatt_unregister(&s_ctx.ble, &s_ctx.service));
    }
}

void test_gatt_register_ignores_unused_callbacks_security_and_allows_null_user(void)
{
    init_with_ops(&k_ops);
    s_ctx.characteristics[0].properties = ESY_BLE_GATT_NOTIFY;
    s_ctx.characteristics[0].read = NULL;
    s_ctx.characteristics[0].write = NULL;
    s_ctx.characteristics[0].read_security = UINT8_MAX;
    s_ctx.characteristics[0].write_security = UINT8_MAX;
    s_ctx.characteristics[0].user = NULL;
    s_ctx.characteristics[1].properties = ESY_BLE_GATT_READ;
    s_ctx.characteristics[1].notify_security = UINT8_MAX;
    s_ctx.characteristics[1].write_security = UINT8_MAX;
    s_ctx.characteristics[1].write = NULL;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
}

void test_gatt_register_rejects_preassigned_characteristic_handle_before_provider_call(void)
{
    init_with_ops(&k_ops);
    s_ctx.characteristics[1].value_handle = 0x42u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_GATT_REGISTER]);
    TEST_ASSERT_EQUAL_UINT16(0x42u, s_ctx.characteristics[1].value_handle);
}

void test_gatt_unregister_failure_retains_handles_and_success_clears_all_handles(void)
{
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
    s_ctx.result = ESY_ERR_BUSY;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_ble_gatt_unregister(&s_ctx.ble, &s_ctx.service));
    TEST_ASSERT_EQUAL_PTR(&s_ctx.service, s_ctx.last_service);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.provider_identity, s_ctx.last_context);
    TEST_ASSERT_EQUAL_UINT16(0x10u, s_ctx.service.handle);
    TEST_ASSERT_EQUAL_UINT16(0x11u, s_ctx.characteristics[0].value_handle);
    TEST_ASSERT_EQUAL_UINT16(0x12u, s_ctx.characteristics[1].value_handle);
    s_ctx.result = ESY_OK;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_gatt_unregister(&s_ctx.ble, &s_ctx.service));
    TEST_ASSERT_EQUAL_UINT16(0u, s_ctx.service.handle);
    TEST_ASSERT_EQUAL_UINT16(0u, s_ctx.characteristics[0].value_handle);
    TEST_ASSERT_EQUAL_UINT16(0u, s_ctx.characteristics[1].value_handle);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ble_gatt_unregister(&s_ctx.ble, &s_ctx.service));
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
}

void test_gatt_unregister_rejects_null_schema_missing_characteristics_and_invalid_counts(void)
{
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_gatt_unregister(&s_ctx.ble, NULL));
    s_ctx.service.handle = 0x10u;
    s_ctx.service.characteristics = NULL;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_gatt_unregister(&s_ctx.ble, &s_ctx.service));
    s_ctx.service.characteristics = s_ctx.characteristics;
    s_ctx.service.characteristic_count = 0u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_gatt_unregister(&s_ctx.ble, &s_ctx.service));
    s_ctx.service.characteristic_count = (size_t)UINT16_MAX + 1u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ble_gatt_unregister(&s_ctx.ble, &s_ctx.service));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_GATT_UNREGISTER]);
}

void test_gatt_unregister_uses_provider_to_reject_wrong_service_identity(void)
{
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
    esy_ble_gatt_service_t different = s_ctx.service;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ble_gatt_unregister(&s_ctx.ble, &different));
    TEST_ASSERT_EQUAL_UINT16(0x10u, different.handle);
    TEST_ASSERT_EQUAL_UINT16(0x11u, s_ctx.characteristics[0].value_handle);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.service, s_ctx.registered_service);
}

void test_gatt_provider_calls_borrowed_read_write_callbacks_with_context_and_payload(void)
{
    uint8_t buffer[8] = {0u};
    const uint8_t written[] = {1u, 2u, 3u};
    size_t length = 1u;
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
    esy_ble_gatt_characteristic_t *characteristic = &s_ctx.registered_service->characteristics[1];
    TEST_ASSERT_TRUE(characteristic->read == read_attribute);
    TEST_ASSERT_TRUE(characteristic->write == write_attribute);
    TEST_ASSERT_EQUAL_UINT8(ESY_BLE_ATT_SUCCESS,
        characteristic->read(characteristic->user, 3u, 7u, buffer, sizeof(buffer), &length));
    TEST_ASSERT_EQUAL_PTR(&s_ctx.user_identity, s_ctx.attribute_user);
    TEST_ASSERT_EQUAL_UINT16(3u, s_ctx.connection);
    TEST_ASSERT_EQUAL_UINT16(7u, s_ctx.offset);
    TEST_ASSERT_EQUAL_PTR(buffer, s_ctx.read_buffer);
    TEST_ASSERT_EQUAL_UINT(sizeof(buffer), s_ctx.read_capacity);
    TEST_ASSERT_EQUAL_PTR(&length, s_ctx.read_length);
    TEST_ASSERT_EQUAL_UINT(0u, length);
    s_ctx.att_result = ESY_BLE_ATT_INVALID_OFFSET;
    TEST_ASSERT_EQUAL_UINT8(ESY_BLE_ATT_INVALID_OFFSET,
        characteristic->read(characteristic->user, 3u, 7u, buffer, sizeof(buffer), &length));
    s_ctx.att_result = ESY_BLE_ATT_INVALID_LENGTH;
    TEST_ASSERT_EQUAL_UINT8(ESY_BLE_ATT_INVALID_LENGTH,
        characteristic->write(characteristic->user, 4u, 2u, written, sizeof(written), true));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.write_calls);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.user_identity, s_ctx.attribute_user);
    TEST_ASSERT_EQUAL_UINT16(4u, s_ctx.connection);
    TEST_ASSERT_EQUAL_UINT16(2u, s_ctx.offset);
    TEST_ASSERT_EQUAL_PTR(written, s_ctx.data);
    TEST_ASSERT_EQUAL_UINT(sizeof(written), s_ctx.length);
    TEST_ASSERT_TRUE(s_ctx.write_command);
    s_ctx.att_result = ESY_BLE_ATT_SUCCESS;
    TEST_ASSERT_EQUAL_UINT8(ESY_BLE_ATT_SUCCESS,
        characteristic->write(characteristic->user, 4u, 0u, NULL, 0u, false));
    TEST_ASSERT_FALSE(s_ctx.write_command);
}

void test_deinit_releases_registered_schema_only_after_provider_cleanup_succeeds(void)
{
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
    s_ctx.result = ESY_ERR_BUSY;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_ble_deinit(&s_ctx.ble));
    TEST_ASSERT_EQUAL_PTR(&s_ctx.service, s_ctx.registered_service);
    TEST_ASSERT_EQUAL_UINT16(0x10u, s_ctx.service.handle);
    TEST_ASSERT_EQUAL_UINT16(0x11u, s_ctx.characteristics[0].value_handle);
    s_ctx.result = ESY_OK;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_deinit(&s_ctx.ble));
    TEST_ASSERT_NULL(s_ctx.registered_service);
    TEST_ASSERT_EQUAL_UINT16(0u, s_ctx.service.handle);
    TEST_ASSERT_EQUAL_UINT16(0u, s_ctx.characteristics[0].value_handle);
    TEST_ASSERT_EQUAL_UINT16(0u, s_ctx.characteristics[1].value_handle);
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service));
}

void test_auth_reply_forwards_all_actions_passkey_boundaries_and_provider_errors(void)
{
    init_with_ops(&k_ops);
    s_ctx.result = ESY_ERR_STATE;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ble_auth_reply(&s_ctx.ble, 0u, ESY_BLE_AUTH_PASSKEY_ENTRY, 0u));
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.passkey);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ble_auth_reply(&s_ctx.ble, 5u, ESY_BLE_AUTH_PASSKEY_ENTRY, 999999u));
    TEST_ASSERT_EQUAL_UINT16(5u, s_ctx.connection);
    TEST_ASSERT_EQUAL_INT(ESY_BLE_AUTH_PASSKEY_ENTRY, s_ctx.reply);
    TEST_ASSERT_EQUAL_UINT32(999999u, s_ctx.passkey);
    for (esy_ble_auth_reply_t reply = ESY_BLE_AUTH_PASSKEY_CONFIRM; reply <= ESY_BLE_AUTH_CANCEL; ++reply) {
        TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ble_auth_reply(&s_ctx.ble, 5u, reply, UINT32_MAX));
        TEST_ASSERT_EQUAL_INT(reply, s_ctx.reply);
        TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s_ctx.passkey);
    }
    TEST_ASSERT_EQUAL_PTR(&s_ctx.provider_identity, s_ctx.last_context);
}

void test_auth_reply_rejects_invalid_connection_action_and_out_of_range_passkey(void)
{
    init_with_ops(&k_ops);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
        esy_ble_auth_reply(&s_ctx.ble, ESY_BLE_CONNECTION_INVALID, ESY_BLE_AUTH_CANCEL, 0u));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
        esy_ble_auth_reply(&s_ctx.ble, 1u, (esy_ble_auth_reply_t)-1, 0u));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
        esy_ble_auth_reply(&s_ctx.ble, 1u, (esy_ble_auth_reply_t)(ESY_BLE_AUTH_CANCEL + 1), 0u));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
        esy_ble_auth_reply(&s_ctx.ble, 1u, ESY_BLE_AUTH_PASSKEY_ENTRY, 1000000u));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_AUTH_REPLY]);
}

void test_authentication_events_forward_passkey_and_cancellation_with_owner_context(void)
{
    esy_ble_event_t event = {
        .type = ESY_BLE_EVENT_PASSKEY_CONFIRM, .connection = 7u, .passkey = 999999u
    };
    init_with_ops(&k_ops);
    s_ctx.sink(s_ctx.sink_user, &event);
    TEST_ASSERT_EQUAL_PTR(&event, s_ctx.received_event);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.user_identity, s_ctx.received_user);
    TEST_ASSERT_EQUAL_UINT32(999999u, s_ctx.received_event->passkey);
    event.type = ESY_BLE_EVENT_AUTH_CANCELLED;
    s_ctx.sink(s_ctx.sink_user, &event);
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.received_events);
    TEST_ASSERT_EQUAL_INT(ESY_BLE_EVENT_AUTH_CANCELLED, s_ctx.received_event->type);
}
