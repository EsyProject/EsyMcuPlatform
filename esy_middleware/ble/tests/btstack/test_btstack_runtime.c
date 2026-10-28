/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/ble/esy_ble_btstack.h"
#include "btstack.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define DATABASE_SIZE 512u
#define SERVICE_CAPACITY 4u
#define LINK_CAPACITY 2u
#define SUBSCRIPTION_CAPACITY 4u
#define EVENT_CAPACITY 16u
#define DEVICE_CAPACITY 4u
#define READ_BUFFER_SIZE 64u
#define TEST_ATT_MTU 23u
#define GATT_REUSE_ITERATIONS 10000u

typedef struct {
    esy_ble_btstack_t adapter;
    esy_ble_t ble;
    esy_ble_provider_t provider;
    esy_ble_btstack_static_resources_t resources;
    esy_ble_btstack_config_t config;
    uint8_t database[DATABASE_SIZE];
    uint8_t read_buffer[READ_BUFFER_SIZE];
    esy_ble_gatt_service_t *services[SERVICE_CAPACITY];
    esy_ble_btstack_link_t links[LINK_CAPACITY];
    esy_ble_btstack_subscription_t subscriptions[SUBSCRIPTION_CAPACITY];
    esy_ble_gatt_service_t service;
    esy_ble_gatt_characteristic_t characteristics[2];
    esy_err_t restore_result;
    esy_err_t flush_result;
    esy_err_t quiesce_result;
    unsigned restore_calls;
    unsigned flush_calls;
    unsigned quiesce_calls;
    HCI_STATE hci_state;
    btstack_packet_callback_registration_t *hci_handler;
    btstack_packet_callback_registration_t *sm_handler;
    unsigned hci_add_calls;
    unsigned hci_remove_calls;
    unsigned sm_add_calls;
    unsigned sm_remove_calls;
    unsigned att_init_calls;
    unsigned att_deinit_calls;
    const uint8_t *native_database;
    att_read_callback_t read;
    att_write_callback_t write;
    unsigned events;
    esy_ble_event_t event[EVENT_CAPACITY];
    uint8_t event_payload[ESY_BLE_LEGACY_DATA_MAX];
    void *event_user;
    uint8_t io_capability;
    uint8_t auth_requirements;
    uint8_t key_min;
    uint8_t key_max;
    int advertising_enabled;
    unsigned advertising_calls;
    uint16_t advertising_min;
    uint16_t advertising_max;
    uint8_t advertising_type;
    const uint8_t *advertising_data;
    uint8_t advertising_length;
    const uint8_t *scan_response;
    uint8_t scan_response_length;
    unsigned scan_start_calls;
    unsigned scan_stop_calls;
    uint8_t scan_type;
    uint16_t scan_interval;
    uint16_t scan_window;
    bool duplicates;
    unsigned connect_calls;
    unsigned cancel_calls;
    unsigned disconnect_calls;
    bd_addr_t connect_address;
    bd_addr_type_t connect_address_type;
    uint16_t connection_min;
    uint16_t connection_max;
    uint16_t connection_latency;
    uint16_t connection_timeout;
    uint8_t native_result;
    hci_con_handle_t last_connection;
    unsigned security_requests;
    unsigned notify_calls;
    uint16_t notify_handle;
    const uint8_t *notify_data;
    uint16_t notify_length;
    unsigned passkey_calls;
    unsigned numeric_calls;
    unsigned confirm_calls;
    unsigned decline_calls;
    uint32_t passkey;
    int device_type[DEVICE_CAPACITY];
    bd_addr_t device_address[DEVICE_CAPACITY];
    unsigned deleted_bonds;
    unsigned visited_bonds;
    esy_ble_address_t visited[DEVICE_CAPACITY];
    unsigned read_calls;
    unsigned write_calls;
    void *attribute_user;
    uint16_t attribute_offset;
    bool command;
    uint8_t att_result;
    bool invalid_read_length;
    bool short_read_length;
    uint8_t read_slice_error;
    size_t queried_length;
    uint16_t mtu;
    bool preexisting_connection;
    uint16_t native_connections[LINK_CAPACITY];
    uint8_t encryption_key_size;
    bool authenticated;
    bool secure_connection;
    bool secure_only;
    int bonded_device_index;
    int identity;
} btstack_runtime_ctx_t;

static btstack_runtime_ctx_t s_ctx;
static const uint8_t k_value[] = {0x11u, 0x22u, 0x33u, 0x44u};

static esy_err_t restore(void *user)
{
    assert(user == &s_ctx.identity);
    ++s_ctx.restore_calls;
    return s_ctx.restore_result;
}

static esy_err_t flush(void *user)
{
    assert(user == &s_ctx.identity);
    ++s_ctx.flush_calls;
    return s_ctx.flush_result;
}

static esy_err_t quiesce(void *user)
{
    assert(user == &s_ctx.identity);
    ++s_ctx.quiesce_calls;
    return s_ctx.quiesce_result;
}

static void event_sink(void *user, const esy_ble_event_t *event)
{
    assert(s_ctx.events < EVENT_CAPACITY);
    s_ctx.event_user = user;
    s_ctx.event[s_ctx.events++] = *event;
    if (event->data != NULL) {
        assert(event->data_length <= sizeof(s_ctx.event_payload));
        memcpy(s_ctx.event_payload, event->data, event->data_length);
    }
}

static uint8_t read_value(void *user, esy_ble_connection_t connection, uint16_t offset,
                          void *buffer, size_t capacity, size_t *length)
{
    ++s_ctx.read_calls;
    s_ctx.attribute_user = user;
    s_ctx.last_connection = connection;
    s_ctx.attribute_offset = offset;
    if (s_ctx.att_result != ESY_BLE_ATT_SUCCESS) {
        return s_ctx.att_result;
    }
    if (buffer == NULL && capacity == 0u) {
        assert(offset == 0u);
        *length = s_ctx.queried_length;
        return ESY_BLE_ATT_SUCCESS;
    }
    if (s_ctx.read_slice_error != ESY_BLE_ATT_SUCCESS) {
        return s_ctx.read_slice_error;
    }
    if (offset > s_ctx.queried_length) {
        return ESY_BLE_ATT_INVALID_OFFSET;
    }
    *length = s_ctx.queried_length - offset;
    if (*length > capacity) {
        *length = capacity;
    }
    for (size_t i = 0u; i < *length; ++i) {
        ((uint8_t *)buffer)[i] = k_value[(offset + i) % sizeof(k_value)];
    }
    if (s_ctx.invalid_read_length) {
        *length = capacity + 1u;
    }
    if (s_ctx.short_read_length && *length != 0u) {
        --*length;
    }
    return ESY_BLE_ATT_SUCCESS;
}

static uint8_t write_value(void *user, esy_ble_connection_t connection, uint16_t offset,
                           const void *data, size_t length, bool command)
{
    ++s_ctx.write_calls;
    s_ctx.attribute_user = user;
    s_ctx.last_connection = connection;
    s_ctx.attribute_offset = offset;
    s_ctx.command = command;
    assert(data != NULL || length == 0u);
    return s_ctx.att_result;
}

HCI_STATE hci_get_state(void) { return s_ctx.hci_state; }
void hci_add_event_handler(btstack_packet_callback_registration_t *handler)
{ ++s_ctx.hci_add_calls; s_ctx.hci_handler = handler; }
void hci_remove_event_handler(btstack_packet_callback_registration_t *handler)
{ assert(handler == s_ctx.hci_handler); ++s_ctx.hci_remove_calls; s_ctx.hci_handler = NULL; }
void sm_add_event_handler(btstack_packet_callback_registration_t *handler)
{ ++s_ctx.sm_add_calls; s_ctx.sm_handler = handler; }
void sm_remove_event_handler(btstack_packet_callback_registration_t *handler)
{ assert(handler == s_ctx.sm_handler); ++s_ctx.sm_remove_calls; s_ctx.sm_handler = NULL; }
void sm_set_io_capabilities(io_capability_t capability) { s_ctx.io_capability = (uint8_t)capability; }
void sm_set_authentication_requirements(uint8_t requirements) { s_ctx.auth_requirements = requirements; }
void sm_set_encryption_key_size_range(uint8_t minimum, uint8_t maximum)
{ s_ctx.key_min = minimum; s_ctx.key_max = maximum; }
void att_server_init(const uint8_t *database, att_read_callback_t read, att_write_callback_t write)
{
    ++s_ctx.att_init_calls;
    s_ctx.native_database = database;
    s_ctx.read = read;
    s_ctx.write = write;
#if defined(ESY_BLE_TEST_NATIVE_ATT)
    att_set_db(database);
    att_set_read_callback(read);
    att_set_write_callback(write);
#endif
}
void att_server_deinit(void)
{
    ++s_ctx.att_deinit_calls;
#if defined(ESY_BLE_TEST_NATIVE_ATT)
    s_ctx.native_database = NULL;
    s_ctx.read = NULL;
    s_ctx.write = NULL;
#endif
}
#if !defined(ESY_BLE_TEST_NATIVE_ATT)
void att_set_db(const uint8_t *database) { s_ctx.native_database = database; }
void att_set_read_callback(att_read_callback_t read) { s_ctx.read = read; }
void att_set_write_callback(att_write_callback_t write) { s_ctx.write = write; }
#endif
uint16_t att_server_get_mtu(hci_con_handle_t connection)
{ (void)connection; return s_ctx.mtu; }
void sm_set_secure_connections_only_mode(bool enabled) { s_ctx.secure_only = enabled; }
void hci_connections_get_iterator(btstack_linked_list_iterator_t *iterator) { (void)iterator; }
bool btstack_linked_list_iterator_has_next(btstack_linked_list_iterator_t *iterator)
{ (void)iterator; return s_ctx.preexisting_connection; }
gap_connection_type_t gap_get_connection_type(hci_con_handle_t connection)
{
    for (size_t i = 0u; i < LINK_CAPACITY; ++i) {
        if (s_ctx.native_connections[i] == connection) { return GAP_CONNECTION_LE; }
    }
    return GAP_CONNECTION_INVALID;
}
uint8_t gap_encryption_key_size(hci_con_handle_t connection)
{ (void)connection; return s_ctx.encryption_key_size; }
bool gap_authenticated(hci_con_handle_t connection)
{ (void)connection; return s_ctx.authenticated; }
bool gap_secure_connection(hci_con_handle_t connection)
{ (void)connection; return s_ctx.secure_connection; }
void sm_request_pairing(hci_con_handle_t connection)
{ ++s_ctx.security_requests; s_ctx.last_connection = connection; }
int sm_le_device_index(hci_con_handle_t connection)
{ (void)connection; return s_ctx.bonded_device_index; }
void gap_advertisements_set_params(uint16_t minimum, uint16_t maximum, uint8_t type,
                                  uint8_t address_type, bd_addr_t address, uint8_t channels, uint8_t filter)
{
    s_ctx.advertising_min = minimum; s_ctx.advertising_max = maximum; s_ctx.advertising_type = type;
    (void)address_type; (void)address; assert(channels == 7u); assert(filter == 0u);
}
void gap_advertisements_set_data(uint8_t length, uint8_t *data)
{ s_ctx.advertising_length = length; s_ctx.advertising_data = data; }
void gap_scan_response_set_data(uint8_t length, uint8_t *data)
{ s_ctx.scan_response_length = length; s_ctx.scan_response = data; }
void gap_advertisements_enable(int enabled)
{ ++s_ctx.advertising_calls; s_ctx.advertising_enabled = enabled; }
void gap_set_scan_params(uint8_t type, uint16_t interval, uint16_t window, uint8_t filter)
{ s_ctx.scan_type = type; s_ctx.scan_interval = interval; s_ctx.scan_window = window; assert(filter == 0u); }
void gap_set_scan_duplicate_filter(bool enabled) { s_ctx.duplicates = enabled; }
void gap_start_scan(void) { ++s_ctx.scan_start_calls; }
void gap_stop_scan(void) { ++s_ctx.scan_stop_calls; }
void gap_set_connection_parameters(uint16_t interval, uint16_t window, uint16_t minimum,
    uint16_t maximum, uint16_t latency, uint16_t timeout, uint16_t ce_min, uint16_t ce_max)
{
    (void)interval; (void)window; (void)ce_min; (void)ce_max;
    s_ctx.connection_min = minimum; s_ctx.connection_max = maximum;
    s_ctx.connection_latency = latency; s_ctx.connection_timeout = timeout;
}
uint8_t gap_connect(const bd_addr_t address, bd_addr_type_t type)
{
    ++s_ctx.connect_calls; memcpy(s_ctx.connect_address, address, sizeof(bd_addr_t));
    s_ctx.connect_address_type = type; return s_ctx.native_result;
}
uint8_t gap_connect_cancel(void) { ++s_ctx.cancel_calls; return s_ctx.native_result; }
uint8_t gap_disconnect(hci_con_handle_t connection)
{ ++s_ctx.disconnect_calls; s_ctx.last_connection = connection; return s_ctx.native_result; }
uint8_t att_server_notify(hci_con_handle_t connection, uint16_t handle, const uint8_t *data, uint16_t length)
{
    ++s_ctx.notify_calls; s_ctx.last_connection = connection; s_ctx.notify_handle = handle;
    s_ctx.notify_data = data; s_ctx.notify_length = length;
#if defined(ESY_BLE_TEST_NATIVE_ATT)
    if (s_ctx.native_result == ERROR_CODE_SUCCESS) {
        att_connection_t native_connection = {.con_handle = connection, .mtu = s_ctx.mtu};
        uint8_t packet[READ_BUFFER_SIZE] = {0u};
        assert(s_ctx.mtu <= sizeof(packet));
        assert(s_ctx.mtu >= 3u && length <= s_ctx.mtu - 3u);
        uint16_t packet_length = att_prepare_handle_value_notification(
            &native_connection, handle, data, length, packet);
        assert(packet_length == length + 3u && packet_length <= s_ctx.mtu);
        assert(packet[0] == ATT_HANDLE_VALUE_NOTIFICATION);
        assert(little_endian_read_16(packet, 1) == handle);
        assert(memcmp(packet + 3u, data, length) == 0);
    }
#endif
    return s_ctx.native_result;
}
void sm_passkey_input(hci_con_handle_t connection, uint32_t passkey)
{ ++s_ctx.passkey_calls; s_ctx.last_connection = connection; s_ctx.passkey = passkey; }
void sm_numeric_comparison_confirm(hci_con_handle_t connection)
{ ++s_ctx.numeric_calls; s_ctx.last_connection = connection; }
void sm_just_works_confirm(hci_con_handle_t connection)
{ ++s_ctx.confirm_calls; s_ctx.last_connection = connection; }
void sm_bonding_decline(hci_con_handle_t connection)
{ ++s_ctx.decline_calls; s_ctx.last_connection = connection; }
int le_device_db_max_count(void) { return DEVICE_CAPACITY; }
void le_device_db_info(int index, int *type, bd_addr_t address, sm_key_t irk)
{
    assert(index >= 0 && index < (int)DEVICE_CAPACITY); *type = s_ctx.device_type[index];
    if (address != NULL) { memcpy(address, s_ctx.device_address[index], sizeof(bd_addr_t)); }
    if (irk != NULL) { memset(irk, 0, sizeof(sm_key_t)); }
}
void gap_delete_bonding(bd_addr_type_t type, bd_addr_t address)
{
    ++s_ctx.deleted_bonds;
    for (size_t i = 0u; i < DEVICE_CAPACITY; ++i) {
        if (s_ctx.device_type[i] == (int)type &&
            memcmp(s_ctx.device_address[i], address, sizeof(bd_addr_t)) == 0) {
            s_ctx.device_type[i] = BD_ADDR_TYPE_UNKNOWN;
        }
    }
}

static void reset_fixture(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.resources = (esy_ble_btstack_static_resources_t){
        .attribute_database = s_ctx.database, .attribute_database_size = sizeof(s_ctx.database),
        .services = s_ctx.services, .service_capacity = SERVICE_CAPACITY,
        .links = s_ctx.links, .link_capacity = LINK_CAPACITY,
        .subscriptions = s_ctx.subscriptions, .subscription_capacity = SUBSCRIPTION_CAPACITY,
        .read_buffer = s_ctx.read_buffer, .read_buffer_size = sizeof(s_ctx.read_buffer)
    };
    s_ctx.config = (esy_ble_btstack_config_t){
        .static_resources = &s_ctx.resources,
        .persistence = {.restore = restore, .flush = flush, .user = &s_ctx.identity},
        .host_quiesce = quiesce, .host_user = &s_ctx.identity,
        .io_capability = 1u, .minimum_security = 2u, .bondable = true
    };
    s_ctx.hci_state = HCI_STATE_WORKING;
    s_ctx.encryption_key_size = 16u;
    s_ctx.bonded_device_index = -1;
    s_ctx.queried_length = sizeof(k_value);
    s_ctx.mtu = TEST_ATT_MTU;
    for (size_t i = 0u; i < LINK_CAPACITY; ++i) {
        s_ctx.native_connections[i] = ESY_BLE_CONNECTION_INVALID;
    }
    for (size_t i = 0u; i < DEVICE_CAPACITY; ++i) {
        s_ctx.device_type[i] = BD_ADDR_TYPE_UNKNOWN;
    }
    for (size_t i = 0u; i < 2u; ++i) {
        s_ctx.characteristics[i] = (esy_ble_gatt_characteristic_t){
            .uuid = {.length = 2u, .bytes = {(uint8_t)(0x37u + i), 0x2au}},
            .properties = ESY_BLE_GATT_READ | ESY_BLE_GATT_WRITE | ESY_BLE_GATT_NOTIFY,
            .read_security = 2u, .write_security = 2u, .notify_security = 2u,
            .read = read_value, .write = write_value, .user = &s_ctx.identity
        };
    }
    s_ctx.service = (esy_ble_gatt_service_t){
        .uuid = {.length = 2u, .bytes = {0x0du, 0x18u}}, .primary = true,
        .characteristics = s_ctx.characteristics, .characteristic_count = 2u
    };
}

static void bind(void)
{
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &s_ctx.config, &s_ctx.provider) == ESY_OK);
    assert(esy_ble_init(&s_ctx.ble, &s_ctx.provider, event_sink, &s_ctx.identity) == ESY_OK);
}

static void close_binding(void)
{
    s_ctx.flush_result = ESY_OK;
    s_ctx.quiesce_result = ESY_OK;
    s_ctx.native_result = ERROR_CODE_SUCCESS;
    assert(esy_ble_deinit(&s_ctx.ble) == ESY_OK);
}

static void emit_connection(uint16_t connection, uint8_t status)
{
    if (status == ERROR_CODE_SUCCESS) {
        for (size_t i = 0u; i < LINK_CAPACITY; ++i) {
            if (s_ctx.native_connections[i] == ESY_BLE_CONNECTION_INVALID) {
                s_ctx.native_connections[i] = connection;
                break;
            }
        }
    }
    uint8_t packet[36] = {HCI_EVENT_META_GAP, 34u, GAP_SUBEVENT_LE_CONNECTION_COMPLETE};
    packet[3] = status;
    little_endian_store_16(packet, 4, connection);
    packet[7] = BD_ADDR_TYPE_LE_RANDOM;
    for (size_t i = 0u; i < ESY_BLE_ADDRESS_SIZE; ++i) {
        packet[8u + i] = (uint8_t)(i + 1u);
    }
    assert(s_ctx.hci_handler != NULL);
    s_ctx.hci_handler->callback(HCI_EVENT_PACKET, 0u, packet, sizeof(packet));
}

static void emit_disconnection(uint16_t connection)
{
    uint8_t packet[] = {HCI_EVENT_DISCONNECTION_COMPLETE, 4u, 0u, 0u, 0u, 0x13u};
    little_endian_store_16(packet, 3, connection);
    assert(s_ctx.hci_handler != NULL);
    s_ctx.hci_handler->callback(HCI_EVENT_PACKET, 0u, packet, sizeof(packet));
    for (size_t i = 0u; i < LINK_CAPACITY; ++i) {
        if (s_ctx.native_connections[i] == connection) {
            s_ctx.native_connections[i] = ESY_BLE_CONNECTION_INVALID;
        }
    }
}

static void emit_security_event(uint8_t event, uint16_t connection, uint32_t passkey)
{
    uint8_t packet[17] = {0u};
    packet[0] = event;
    packet[1] = 15u;
    little_endian_store_16(packet, 2, connection);
    little_endian_store_32(packet, 12, passkey);
    assert(s_ctx.sm_handler != NULL);
    s_ctx.sm_handler->callback(HCI_EVENT_PACKET, 0u, packet, sizeof(packet));
}

static void test_factory_rejects_invalid_inputs_without_mutating_token(void)
{
    assert(esy_ble_btstack_provider(NULL, &s_ctx.config, &s_ctx.provider) == ESY_ERR_INVALID_ARG);
    assert(esy_ble_btstack_provider(&s_ctx.adapter, NULL, &s_ctx.provider) == ESY_ERR_INVALID_ARG);
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &s_ctx.config, NULL) == ESY_ERR_INVALID_ARG);
    esy_ble_btstack_config_t config = s_ctx.config;
    config.static_resources = NULL;
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &config, &s_ctx.provider) == ESY_ERR_INVALID_ARG);
    config = s_ctx.config;
    config.persistence.restore = NULL;
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &config, &s_ctx.provider) == ESY_ERR_INVALID_ARG);
    config = s_ctx.config;
    config.persistence.flush = NULL;
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &config, &s_ctx.provider) == ESY_ERR_INVALID_ARG);
    config = s_ctx.config;
    config.host_quiesce = NULL;
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &config, &s_ctx.provider) == ESY_ERR_INVALID_ARG);
    config = s_ctx.config;
    config.io_capability = 5u;
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &config, &s_ctx.provider) == ESY_ERR_INVALID_ARG);
    config = s_ctx.config;
    config.minimum_security = 1u;
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &config, &s_ctx.provider) == ESY_ERR_INVALID_ARG);
    config.minimum_security = 5u;
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &config, &s_ctx.provider) == ESY_ERR_INVALID_ARG);
    assert(s_ctx.adapter.owner == NULL);
    assert(s_ctx.provider.ops == NULL);
    bind();
    close_binding();
}

static void test_factory_requires_all_caller_owned_backing_storage(void)
{
    esy_ble_btstack_static_resources_t valid = s_ctx.resources;
    s_ctx.resources.attribute_database = NULL;
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &s_ctx.config, &s_ctx.provider) == ESY_ERR_INVALID_ARG);
    s_ctx.resources = valid;
    s_ctx.resources.attribute_database_size = 2u;
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &s_ctx.config, &s_ctx.provider) == ESY_ERR_INVALID_ARG);
    s_ctx.resources.attribute_database_size = (size_t)UINT16_MAX + 1u;
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &s_ctx.config, &s_ctx.provider) == ESY_ERR_INVALID_ARG);
    s_ctx.resources = valid;
    s_ctx.resources.services = NULL;
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &s_ctx.config, &s_ctx.provider) == ESY_ERR_INVALID_ARG);
    s_ctx.resources = valid;
    s_ctx.resources.service_capacity = 0u;
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &s_ctx.config, &s_ctx.provider) == ESY_ERR_INVALID_ARG);
    s_ctx.resources = valid;
    s_ctx.resources.links = NULL;
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &s_ctx.config, &s_ctx.provider) == ESY_ERR_INVALID_ARG);
    s_ctx.resources = valid;
    s_ctx.resources.link_capacity = 0u;
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &s_ctx.config, &s_ctx.provider) == ESY_ERR_INVALID_ARG);
    s_ctx.resources = valid;
    s_ctx.resources.subscriptions = NULL;
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &s_ctx.config, &s_ctx.provider) == ESY_ERR_INVALID_ARG);
    s_ctx.resources = valid;
    s_ctx.resources.subscription_capacity = 0u;
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &s_ctx.config, &s_ctx.provider) == ESY_ERR_INVALID_ARG);
    s_ctx.resources = valid;
    s_ctx.resources.read_buffer = NULL;
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &s_ctx.config, &s_ctx.provider) == ESY_ERR_INVALID_ARG);
    s_ctx.resources = valid;
    s_ctx.resources.read_buffer_size = 0u;
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &s_ctx.config, &s_ctx.provider) == ESY_ERR_INVALID_ARG);
    s_ctx.resources.read_buffer_size = (size_t)UINT16_MAX + 1u;
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &s_ctx.config, &s_ctx.provider) == ESY_ERR_INVALID_ARG);
    assert(s_ctx.adapter.owner == NULL);
    s_ctx.resources = valid;
    bind();
    close_binding();
}

static void test_lifecycle_copies_configuration_sets_policy_and_releases_native_callbacks(void)
{
    s_ctx.config.minimum_security = 4u;
    bind();
    assert(s_ctx.restore_calls == 1u);
    assert(s_ctx.att_init_calls == 1u);
    assert(s_ctx.hci_add_calls == 1u && s_ctx.sm_add_calls == 1u);
    assert(s_ctx.io_capability == 1u);
    assert(s_ctx.auth_requirements & SM_AUTHREQ_BONDING);
    assert(s_ctx.key_min == 16u && s_ctx.key_max == 16u);
    assert(s_ctx.auth_requirements & SM_AUTHREQ_MITM_PROTECTION);
    assert(s_ctx.auth_requirements & SM_AUTHREQ_SECURE_CONNECTION);
    s_ctx.config.persistence.flush = NULL;
    s_ctx.config.host_quiesce = NULL;
    close_binding();
    assert(s_ctx.flush_calls > 0u && s_ctx.quiesce_calls > 0u);
    assert(s_ctx.att_deinit_calls == 1u);
    assert(s_ctx.hci_handler == NULL && s_ctx.sm_handler == NULL);
    assert(s_ctx.hci_remove_calls == 1u && s_ctx.sm_remove_calls == 1u);
    assert(s_ctx.adapter.owner == NULL);
    assert(esy_ble_deinit(&s_ctx.ble) == ESY_OK);
    assert(s_ctx.att_deinit_calls == 1u);
}

static void test_factory_rejects_live_and_copied_tokens(void)
{
    bind();
    esy_ble_provider_t provider = {0};
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &s_ctx.config, &provider) == ESY_ERR_STATE);
    esy_ble_btstack_t copied = s_ctx.adapter;
    assert(esy_ble_btstack_provider(&copied, &s_ctx.config, &provider) == ESY_ERR_STATE);
    assert(provider.ops == NULL);
    close_binding();
}

static void test_restore_failure_is_cleanable_and_can_be_retried_after_deinit(void)
{
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &s_ctx.config, &s_ctx.provider) == ESY_OK);
    s_ctx.restore_result = ESY_ERR_IO;
    assert(esy_ble_init(&s_ctx.ble, &s_ctx.provider, event_sink, &s_ctx.identity) == ESY_ERR_IO);
    assert(s_ctx.restore_calls == 1u);
    assert(s_ctx.hci_add_calls == 0u && s_ctx.sm_add_calls == 0u);
    close_binding();
    assert(s_ctx.adapter.owner == NULL);
    s_ctx.restore_result = ESY_OK;
    bind();
    close_binding();
}

static void test_init_requires_working_native_host_and_cleanup_releases_token(void)
{
    s_ctx.hci_state = HCI_STATE_OFF;
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &s_ctx.config, &s_ctx.provider) == ESY_OK);
    assert(esy_ble_init(&s_ctx.ble, &s_ctx.provider, event_sink, &s_ctx.identity) == ESY_ERR_STATE);
    assert(s_ctx.restore_calls == 0u && s_ctx.att_init_calls == 0u);
    close_binding();
    assert(s_ctx.adapter.owner == NULL);
}

static void test_deinit_retains_owner_until_quiescence_and_durable_flush_succeed(void)
{
    bind();
    s_ctx.quiesce_result = ESY_ERR_BUSY;
    assert(esy_ble_deinit(&s_ctx.ble) == ESY_ERR_BUSY);
    assert(s_ctx.adapter.owner != NULL);
    assert(s_ctx.hci_handler != NULL && s_ctx.att_deinit_calls == 0u);
    assert(esy_ble_adv_stop(&s_ctx.ble) == ESY_ERR_STATE);
    s_ctx.quiesce_result = ESY_OK;
    s_ctx.flush_result = ESY_ERR_IO;
    assert(esy_ble_deinit(&s_ctx.ble) == ESY_ERR_IO);
    assert(s_ctx.adapter.owner != NULL && s_ctx.att_deinit_calls == 0u);
    close_binding();
    assert(s_ctx.adapter.owner == NULL && s_ctx.att_deinit_calls == 1u);
}

static void test_deinit_waits_for_active_link_disconnect_event(void)
{
    bind();
    emit_connection(7u, ERROR_CODE_SUCCESS);
    assert(s_ctx.events == 1u);
    assert(s_ctx.event[0].type == ESY_BLE_EVENT_CONNECTED);
    assert(esy_ble_deinit(&s_ctx.ble) == ESY_ERR_BUSY);
    assert(s_ctx.disconnect_calls == 1u && s_ctx.last_connection == 7u);
    assert(s_ctx.hci_handler != NULL);
    emit_disconnection(7u);
    close_binding();
    assert(s_ctx.adapter.owner == NULL);
}

static void test_gap_copies_advertising_payload_and_maps_scan_parameters(void)
{
    uint8_t data[] = {2u, 1u, 6u};
    uint8_t response[] = {2u, 9u, 'B'};
    esy_ble_adv_params_t advertising = {
        .connectable = true, .interval_min = 0x20u, .interval_max = 0x100u,
        .data = data, .data_length = sizeof(data),
        .scan_response = response, .scan_response_length = sizeof(response)
    };
    esy_ble_scan_params_t scan = {
        .active = true, .filter_duplicates = true, .interval = 16u, .window = 8u
    };
    bind();
    assert(esy_ble_adv_start(&s_ctx.ble, &advertising) == ESY_OK);
    assert(s_ctx.advertising_enabled == 1);
    assert(s_ctx.advertising_min == 0x20u && s_ctx.advertising_max == 0x100u);
    assert(s_ctx.advertising_type == 0u);
    assert(s_ctx.advertising_length == sizeof(data));
    assert(s_ctx.scan_response_length == sizeof(response));
    assert(s_ctx.advertising_data != data && s_ctx.scan_response != response);
    memset(data, 0xff, sizeof(data));
    memset(response, 0xff, sizeof(response));
    assert(s_ctx.advertising_data[2] == 6u && s_ctx.scan_response[2] == 'B');
    assert(esy_ble_adv_stop(&s_ctx.ble) == ESY_OK);
    assert(s_ctx.advertising_enabled == 0);
    assert(esy_ble_scan_start(&s_ctx.ble, &scan) == ESY_OK);
    assert(s_ctx.scan_start_calls == 1u && s_ctx.scan_type == 1u);
    assert(s_ctx.scan_interval == 16u && s_ctx.scan_window == 8u && s_ctx.duplicates);
    assert(esy_ble_scan_stop(&s_ctx.ble) == ESY_OK);
    assert(s_ctx.scan_stop_calls == 1u);
    close_binding();
    assert(s_ctx.advertising_length == 0u && s_ctx.scan_response_length == 0u);
}

static void test_connect_maps_address_units_and_result_events(void)
{
    esy_ble_connect_params_t params = {
        .peer = {.type = ESY_BLE_ADDRESS_RANDOM, .bytes = {1u, 2u, 3u, 4u, 5u, 6u}},
        .interval_min = 12u, .interval_max = 24u, .latency = 1u, .supervision_timeout = 100u
    };
    const uint8_t native_address[] = {6u, 5u, 4u, 3u, 2u, 1u};
    bind();
    assert(esy_ble_connect(&s_ctx.ble, &params) == ESY_OK);
    assert(s_ctx.connect_calls == 1u);
    assert(memcmp(s_ctx.connect_address, native_address, sizeof(native_address)) == 0);
    assert(s_ctx.connect_address_type == BD_ADDR_TYPE_LE_RANDOM);
    assert(s_ctx.connection_min == 12u && s_ctx.connection_max == 24u);
    assert(s_ctx.connection_latency == 1u && s_ctx.connection_timeout == 100u);
    emit_connection(7u, ERROR_CODE_SUCCESS);
    assert(s_ctx.event[0].connection == 7u && s_ctx.event[0].status == ESY_OK);
    assert(s_ctx.event[0].peer.type == ESY_BLE_ADDRESS_RANDOM);
    assert(memcmp(s_ctx.event[0].peer.bytes, params.peer.bytes, ESY_BLE_ADDRESS_SIZE) == 0);
    assert(s_ctx.event_user == &s_ctx.identity);
    assert(esy_ble_disconnect(&s_ctx.ble, 7u, 0x13u) == ESY_OK);
    emit_disconnection(7u);
    assert(s_ctx.event[1].type == ESY_BLE_EVENT_DISCONNECTED);
    assert(s_ctx.event[1].connection == 7u && s_ctx.event[1].reason == 0x13u);
    close_binding();
}

static void test_event_translation_preserves_scan_payload_and_rejects_truncation(void)
{
    uint8_t packet[] = {
        GAP_EVENT_ADVERTISING_REPORT, 13u, 0u, BD_ADDR_TYPE_LE_RANDOM,
        1u, 2u, 3u, 4u, 5u, 6u, (uint8_t)-42, 3u, 2u, 1u, 6u
    };
    bind();
    esy_ble_scan_params_t scan = {.interval = 16u, .window = 8u};
    assert(esy_ble_scan_start(&s_ctx.ble, &scan) == ESY_OK);
    s_ctx.hci_handler->callback(HCI_EVENT_PACKET, 0u, packet, 1u);
    assert(s_ctx.events == 0u);
    s_ctx.hci_handler->callback(HCI_EVENT_PACKET, 0u, packet, sizeof(packet) - 1u);
    assert(s_ctx.events == 0u);
    s_ctx.hci_handler->callback(HCI_EVENT_PACKET, 0u, packet, sizeof(packet));
    assert(s_ctx.events == 1u);
    assert(s_ctx.event[0].type == ESY_BLE_EVENT_SCAN_RESULT);
    assert(s_ctx.event[0].connection == ESY_BLE_CONNECTION_INVALID);
    assert(s_ctx.event[0].rssi == -42 && s_ctx.event[0].data_length == 3u);
    assert(memcmp(s_ctx.event[0].peer.bytes, packet + 4, ESY_BLE_ADDRESS_SIZE) == 0);
    assert(memcmp(s_ctx.event_payload, packet + 12, 3u) == 0);
    packet[1] = 1u;
    s_ctx.hci_handler->callback(HCI_EVENT_PACKET, 0u, packet, sizeof(packet));
    assert(s_ctx.events == 1u);
    close_binding();
}

static void test_failed_connection_event_does_not_create_live_link(void)
{
    bind();
    emit_connection(7u, ERROR_CODE_CONNECTION_TIMEOUT);
    assert(s_ctx.events == 1u);
    assert(s_ctx.event[0].type == ESY_BLE_EVENT_CONNECTED);
    assert(s_ctx.event[0].connection == ESY_BLE_CONNECTION_INVALID);
    assert(s_ctx.event[0].status != ESY_OK);
    assert(s_ctx.event[0].reason == ERROR_CODE_CONNECTION_TIMEOUT);
    close_binding();
    assert(s_ctx.disconnect_calls == 0u);
}

static void test_authentication_requests_require_matching_owner_reply_and_expire(void)
{
    bind();
    emit_connection(7u, ERROR_CODE_SUCCESS);
    assert(esy_ble_auth_reply(&s_ctx.ble, 7u, ESY_BLE_AUTH_PAIRING_CONFIRM, 0u) == ESY_ERR_STATE);
    emit_security_event(SM_EVENT_PASSKEY_INPUT_NUMBER, 7u, 0u);
    assert(s_ctx.event[1].type == ESY_BLE_EVENT_PASSKEY_ENTRY);
    assert(esy_ble_auth_reply(&s_ctx.ble, 7u, ESY_BLE_AUTH_PASSKEY_CONFIRM, 0u) == ESY_ERR_STATE);
    assert(esy_ble_auth_reply(&s_ctx.ble, 7u, ESY_BLE_AUTH_PASSKEY_ENTRY, 123456u) == ESY_OK);
    assert(s_ctx.passkey_calls == 1u && s_ctx.passkey == 123456u);
    assert(esy_ble_auth_reply(&s_ctx.ble, 7u, ESY_BLE_AUTH_PASSKEY_ENTRY, 123456u) == ESY_ERR_STATE);
    emit_security_event(SM_EVENT_NUMERIC_COMPARISON_REQUEST, 7u, 654321u);
    assert(s_ctx.event[2].passkey == 654321u);
    assert(esy_ble_auth_reply(&s_ctx.ble, 7u, ESY_BLE_AUTH_PASSKEY_CONFIRM, 0u) == ESY_OK);
    assert(s_ctx.numeric_calls == 1u);
    emit_security_event(SM_EVENT_JUST_WORKS_REQUEST, 7u, 0u);
    assert(esy_ble_auth_reply(&s_ctx.ble, 7u, ESY_BLE_AUTH_PAIRING_CONFIRM, 0u) == ESY_OK);
    assert(s_ctx.confirm_calls == 1u);
    emit_security_event(SM_EVENT_JUST_WORKS_REQUEST, 7u, 0u);
    assert(esy_ble_auth_reply(&s_ctx.ble, 7u, ESY_BLE_AUTH_CANCEL, 0u) == ESY_OK);
    assert(s_ctx.decline_calls == 1u);
    emit_security_event(SM_EVENT_PASSKEY_INPUT_NUMBER, 7u, 0u);
    emit_security_event(SM_EVENT_PASSKEY_DISPLAY_CANCEL, 7u, 0u);
    assert(esy_ble_auth_reply(&s_ctx.ble, 7u, ESY_BLE_AUTH_PASSKEY_ENTRY, 123456u) == ESY_ERR_STATE);
    emit_disconnection(7u);
    close_binding();
}

static const uint8_t *find_attribute(uint16_t handle)
{
    size_t offset = 1u;
    while (offset + 8u <= sizeof(s_ctx.database)) {
        uint16_t size = little_endian_read_16(s_ctx.database, (int)offset);
        if (size == 0u) { return NULL; }
        assert(size >= 8u && offset + size <= sizeof(s_ctx.database));
        if (little_endian_read_16(s_ctx.database, (int)offset + 4) == handle) {
            return s_ctx.database + offset;
        }
        offset += size;
    }
    return NULL;
}

static void test_gatt_database_uses_little_endian_uuid_values_and_native_security_flags(void)
{
    s_ctx.config.minimum_security = 4u;
    s_ctx.service.primary = false;
    s_ctx.service.uuid.length = 16u;
    s_ctx.characteristics[1].uuid.length = 16u;
    for (size_t i = 0u; i < 16u; ++i) {
        s_ctx.service.uuid.bytes[i] = (uint8_t)(0x80u + i);
        s_ctx.characteristics[1].uuid.bytes[i] = (uint8_t)i;
    }
    s_ctx.characteristics[1].read_security = 4u;
    s_ctx.characteristics[1].write_security = 4u;
    s_ctx.characteristics[1].notify_security = 4u;
    bind();
    assert(esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service) == ESY_OK);
    const uint8_t *service = find_attribute(s_ctx.service.handle);
    assert(service != NULL);
    assert(little_endian_read_16(service, 6) == GATT_SECONDARY_SERVICE_UUID);
    assert(memcmp(service + 8, s_ctx.service.uuid.bytes, 16u) == 0);
    const uint8_t *value = find_attribute(s_ctx.characteristics[1].value_handle);
    assert(value != NULL && memcmp(value + 6, s_ctx.characteristics[1].uuid.bytes, 16u) == 0);
    uint16_t flags = little_endian_read_16(value, 2);
    assert(flags & ATT_PROPERTY_UUID128);
    assert(flags & ATT_PROPERTY_DYNAMIC);
    assert(flags & ATT_PROPERTY_READ_PERMISSION_SC);
    assert(flags & ATT_PROPERTY_WRITE_PERMISSION_SC);
    assert((flags & 0xf000u) == 0xf000u);
    const uint8_t *ccc = find_attribute((uint16_t)(s_ctx.characteristics[1].value_handle + 1u));
    assert(ccc != NULL && little_endian_read_16(ccc, 6) == GATT_CLIENT_CHARACTERISTICS_CONFIGURATION);
    assert(little_endian_read_16(ccc, 2) & ATT_PROPERTY_WRITE_PERMISSION_SC);
    close_binding();
    assert(s_ctx.service.handle == 0u && s_ctx.characteristics[0].value_handle == 0u);
    assert(s_ctx.characteristics[1].value_handle == 0u);
    assert(s_ctx.native_database != s_ctx.database);
    assert(s_ctx.read == NULL && s_ctx.write == NULL);
}

static void test_gatt_read_query_offsets_write_context_and_att_errors(void)
{
    bind();
    assert(esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service) == ESY_OK);
    emit_connection(7u, ERROR_CODE_SUCCESS);
    uint16_t handle = s_ctx.characteristics[0].value_handle;
    uint8_t buffer[2] = {0u};
    assert(s_ctx.read(7u, handle, 0u, NULL, 0u) == sizeof(k_value));
    assert(s_ctx.read(7u, handle, 2u, NULL, 0u) == sizeof(k_value));
    assert(s_ctx.attribute_offset == 2u);
    assert(s_ctx.read(7u, handle, 1u, buffer, sizeof(buffer)) == 0u);
    assert(s_ctx.read(7u, handle, 1u, NULL, 0u) == sizeof(k_value));
    unsigned prefetched_calls = s_ctx.read_calls;
    assert(s_ctx.read(7u, handle, 1u, buffer, sizeof(buffer)) == sizeof(buffer));
    assert(memcmp(buffer, k_value + 1, sizeof(buffer)) == 0);
    assert(s_ctx.attribute_user == &s_ctx.identity && s_ctx.attribute_offset == 1u);
    assert(s_ctx.read_calls == prefetched_calls);
    assert(s_ctx.read(7u, handle, 1u, buffer, sizeof(buffer)) == 0u);
    assert(s_ctx.read(7u, handle, 5u, NULL, 0u) == sizeof(k_value));
    assert(s_ctx.read(7u, handle, 5u, buffer, sizeof(buffer)) == 0u);
    s_ctx.invalid_read_length = true;
    assert(s_ctx.read(7u, handle, 0u, NULL, 0u) ==
        (ATT_READ_ERROR_CODE_OFFSET | ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH));
    assert(s_ctx.read(7u, handle, 0u, buffer, sizeof(buffer)) == 0u);
    s_ctx.invalid_read_length = false;
    s_ctx.short_read_length = true;
    assert(s_ctx.read(7u, handle, 0u, NULL, 0u) ==
        (ATT_READ_ERROR_CODE_OFFSET | ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH));
    s_ctx.short_read_length = false;
    s_ctx.read_slice_error = ESY_BLE_ATT_UNLIKELY_ERROR;
    assert(s_ctx.read(7u, handle, 0u, NULL, 0u) ==
        (ATT_READ_ERROR_CODE_OFFSET | ATT_ERROR_UNLIKELY_ERROR));
    assert(s_ctx.read(7u, handle, 0u, buffer, sizeof(buffer)) == 0u);
    s_ctx.read_slice_error = ESY_BLE_ATT_SUCCESS;
    s_ctx.queried_length = ATT_READ_ERROR_CODE_OFFSET;
    assert(s_ctx.read(7u, handle, 0u, NULL, 0u) ==
        (ATT_READ_ERROR_CODE_OFFSET | ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH));
    s_ctx.queried_length = sizeof(k_value);
    s_ctx.att_result = ESY_BLE_ATT_INVALID_LENGTH;
    assert(s_ctx.write(7u, handle, ATT_TRANSACTION_MODE_NONE, 1u, buffer, sizeof(buffer)) ==
        ESY_BLE_ATT_INVALID_LENGTH);
    assert(s_ctx.write_calls == 1u && !s_ctx.command);
    assert(s_ctx.attribute_user == &s_ctx.identity && s_ctx.attribute_offset == 1u);
    assert(s_ctx.write(7u, handle, ATT_TRANSACTION_MODE_ACTIVE, 0u, buffer, sizeof(buffer)) ==
        ATT_ERROR_REQUEST_NOT_SUPPORTED);
    assert(s_ctx.write_calls == 1u);
    emit_disconnection(7u);
    close_binding();
}

static void test_security_four_requires_encryption_mitm_secure_connections_and_full_key(void)
{
    s_ctx.config.minimum_security = 4u;
    s_ctx.service.characteristic_count = 1u;
    s_ctx.characteristics[0].read_security = 4u;
    s_ctx.characteristics[0].write_security = 4u;
    s_ctx.characteristics[0].notify_security = 4u;
    bind();
    assert(s_ctx.secure_only);
    assert(esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service) == ESY_OK);
    emit_connection(7u, ERROR_CODE_SUCCESS);
    uint16_t handle = s_ctx.characteristics[0].value_handle;
    uint8_t value[] = {1u, 0u};
    s_ctx.encryption_key_size = 0u;
    assert(s_ctx.read(7u, handle, 0u, NULL, 0u) ==
        (ATT_READ_ERROR_CODE_OFFSET | ATT_ERROR_INSUFFICIENT_AUTHENTICATION));
    assert(s_ctx.write(7u, handle, 0u, 0u, value, sizeof(value)) == ATT_ERROR_INSUFFICIENT_AUTHENTICATION);
    s_ctx.encryption_key_size = 16u;
    assert(s_ctx.write(7u, (uint16_t)(handle + 1u), 0u, 0u, value, sizeof(value)) ==
        ATT_ERROR_INSUFFICIENT_AUTHENTICATION);
    s_ctx.authenticated = true;
    assert(s_ctx.read(7u, handle, 0u, NULL, 0u) ==
        (ATT_READ_ERROR_CODE_OFFSET | ATT_ERROR_INSUFFICIENT_AUTHENTICATION));
    s_ctx.secure_connection = true;
    s_ctx.encryption_key_size = 15u;
    assert(s_ctx.read(7u, handle, 0u, NULL, 0u) ==
        (ATT_READ_ERROR_CODE_OFFSET | ATT_ERROR_INSUFFICIENT_AUTHENTICATION));
    assert(s_ctx.read_calls == 0u && s_ctx.write_calls == 0u);
    s_ctx.encryption_key_size = 16u;
    assert(s_ctx.read(7u, handle, 0u, NULL, 0u) == sizeof(k_value));
    assert(s_ctx.write(7u, (uint16_t)(handle + 1u), 0u, 0u, value, sizeof(value)) == ATT_ERROR_SUCCESS);
    assert(esy_ble_notify(&s_ctx.ble, 7u, handle, k_value, sizeof(k_value)) == ESY_OK);
    s_ctx.secure_connection = false;
    assert(esy_ble_notify(&s_ctx.ble, 7u, handle, k_value, sizeof(k_value)) == ESY_ERR_STATE);
    assert(s_ctx.notify_calls == 1u);
    emit_disconnection(7u);
    close_binding();
}

static void test_ccc_is_per_link_and_disconnect_clears_subscription_before_handle_reuse(void)
{
    bind();
    assert(esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service) == ESY_OK);
    emit_connection(7u, ERROR_CODE_SUCCESS);
    emit_connection(8u, ERROR_CODE_SUCCESS);
    uint16_t handle = s_ctx.characteristics[0].value_handle;
    uint16_t ccc = (uint16_t)(handle + 1u);
    uint8_t enabled[] = {1u, 0u};
    uint8_t buffer[2];
    assert(s_ctx.write(7u, ccc, 0u, 0u, enabled, sizeof(enabled)) == ATT_ERROR_SUCCESS);
    assert(s_ctx.read(7u, ccc, 0u, buffer, sizeof(buffer)) == 2u && buffer[0] == 1u);
    assert(s_ctx.read(8u, ccc, 0u, buffer, sizeof(buffer)) == 2u && buffer[0] == 0u);
    assert(esy_ble_notify(&s_ctx.ble, 7u, handle, k_value, sizeof(k_value)) == ESY_OK);
    assert(s_ctx.notify_handle == handle && s_ctx.notify_length == sizeof(k_value));
    assert(s_ctx.notify_data == k_value);
    assert(esy_ble_notify(&s_ctx.ble, 8u, handle, k_value, sizeof(k_value)) == ESY_ERR_STATE);
    assert(s_ctx.write(8u, ccc, 0u, 0u, enabled, sizeof(enabled)) == ATT_ERROR_SUCCESS);
    assert(esy_ble_notify(&s_ctx.ble, 8u, handle, k_value, sizeof(k_value)) == ESY_OK);
    s_ctx.native_result = BTSTACK_ACL_BUFFERS_FULL;
    assert(esy_ble_notify(&s_ctx.ble, 8u, handle, k_value, sizeof(k_value)) == ESY_ERR_BUSY);
    s_ctx.native_result = ERROR_CODE_SUCCESS;
    emit_disconnection(7u);
    emit_connection(7u, ERROR_CODE_SUCCESS);
    assert(esy_ble_notify(&s_ctx.ble, 7u, handle, k_value, sizeof(k_value)) == ESY_ERR_STATE);
    assert(s_ctx.read(7u, ccc, 1u, buffer, 1u) == 1u && buffer[0] == 0u);
    assert(s_ctx.read(7u, ccc, 3u, buffer, sizeof(buffer)) == 0u);
    emit_disconnection(7u);
    emit_disconnection(8u);
    close_binding();
}

static void test_ccc_validates_offset_value_capacity_and_encryption_before_mutation(void)
{
    s_ctx.resources.subscription_capacity = 1u;
    bind();
    assert(esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service) == ESY_OK);
    emit_connection(7u, ERROR_CODE_SUCCESS);
    emit_connection(8u, ERROR_CODE_SUCCESS);
    uint16_t ccc = (uint16_t)(s_ctx.characteristics[0].value_handle + 1u);
    uint8_t enabled[] = {1u, 0u};
    uint8_t invalid[] = {2u, 0u};
    assert(s_ctx.write(7u, ccc, 0u, 1u, enabled, sizeof(enabled)) == ATT_ERROR_INVALID_OFFSET);
    assert(s_ctx.write(7u, ccc, 0u, 0u, invalid, sizeof(invalid)) == ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH);
    assert(s_ctx.write(7u, ccc, 0u, 0u, enabled, 1u) == ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH);
    s_ctx.encryption_key_size = 0u;
    assert(s_ctx.write(7u, ccc, 0u, 0u, enabled, sizeof(enabled)) == ATT_ERROR_INSUFFICIENT_ENCRYPTION);
    s_ctx.encryption_key_size = 16u;
    assert(s_ctx.write(7u, ccc, 0u, 0u, enabled, sizeof(enabled)) == ATT_ERROR_SUCCESS);
    assert(s_ctx.write(8u, ccc, 0u, 0u, enabled, sizeof(enabled)) == ATT_ERROR_INSUFFICIENT_RESOURCES);
    enabled[0] = 0u;
    assert(s_ctx.write(7u, ccc, 0u, 0u, enabled, sizeof(enabled)) == ATT_ERROR_SUCCESS);
    enabled[0] = 1u;
    assert(s_ctx.write(8u, ccc, 0u, 0u, enabled, sizeof(enabled)) == ATT_ERROR_SUCCESS);
    emit_disconnection(7u);
    emit_disconnection(8u);
    close_binding();
}

static void test_gatt_register_capacity_and_unsupported_writes_fail_without_partial_schema(void)
{
    s_ctx.resources.attribute_database_size = 3u;
    bind();
    assert(esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service) == ESY_ERR_NO_MEM);
    assert(s_ctx.service.handle == 0u && s_ctx.characteristics[0].value_handle == 0u);
    assert(s_ctx.database[1] == 0u && s_ctx.database[2] == 0u);
    close_binding();
    reset_fixture();
    s_ctx.characteristics[0].properties |= ESY_BLE_GATT_WRITE_COMMAND;
    bind();
    assert(esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service) == ESY_ERR_NOT_SUPPORTED);
    assert(s_ctx.service.handle == 0u && s_ctx.characteristics[0].value_handle == 0u);
    assert(s_ctx.database[1] == 0u && s_ctx.database[2] == 0u);
    close_binding();
}

static void test_gatt_unregister_requires_same_identity_and_lifo_and_clears_removed_handles(void)
{
    bind();
    assert(esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service) == ESY_OK);
    esy_ble_gatt_characteristic_t characteristic = s_ctx.characteristics[0];
    characteristic.value_handle = 0u;
    esy_ble_gatt_service_t second = s_ctx.service;
    second.handle = 0u;
    second.characteristics = &characteristic;
    second.characteristic_count = 1u;
    assert(esy_ble_gatt_register(&s_ctx.ble, &second) == ESY_OK);
    assert(esy_ble_gatt_unregister(&s_ctx.ble, &s_ctx.service) == ESY_ERR_BUSY);
    esy_ble_gatt_service_t copy = second;
    assert(esy_ble_gatt_unregister(&s_ctx.ble, &copy) == ESY_ERR_STATE);
    assert(esy_ble_gatt_unregister(&s_ctx.ble, &second) == ESY_OK);
    assert(second.handle == 0u && characteristic.value_handle == 0u);
    assert(s_ctx.service.handle != 0u);
    assert(esy_ble_gatt_unregister(&s_ctx.ble, &s_ctx.service) == ESY_OK);
    assert(s_ctx.database[1] == 0u && s_ctx.database[2] == 0u);
    close_binding();
}

static void visit_bond(void *user, const esy_ble_address_t *peer)
{
    assert(user == &s_ctx.identity);
    assert(s_ctx.visited_bonds < DEVICE_CAPACITY);
    s_ctx.visited[s_ctx.visited_bonds++] = *peer;
}

static void set_native_bond(size_t index, uint8_t type)
{
    assert(index < DEVICE_CAPACITY);
    s_ctx.device_type[index] = type;
    for (size_t i = 0u; i < ESY_BLE_ADDRESS_SIZE; ++i) {
        s_ctx.device_address[index][i] = (uint8_t)(ESY_BLE_ADDRESS_SIZE - i + index);
    }
}

static void test_bond_enumeration_unpair_uses_native_cache_api_and_reports_durable_failure(void)
{
    set_native_bond(0u, BD_ADDR_TYPE_LE_PUBLIC);
    set_native_bond(2u, BD_ADDR_TYPE_LE_RANDOM);
    bind();
    assert(esy_ble_bonds_foreach(&s_ctx.ble, visit_bond, &s_ctx.identity) == ESY_OK);
    assert(s_ctx.visited_bonds == 2u);
    assert(s_ctx.visited[0].type == ESY_BLE_ADDRESS_PUBLIC && s_ctx.visited[1].type == ESY_BLE_ADDRESS_RANDOM);
    assert(s_ctx.visited[0].bytes[0] == 1u && s_ctx.visited[0].bytes[5] == 6u);
    s_ctx.flush_result = ESY_ERR_IO;
    assert(esy_ble_unpair(&s_ctx.ble, &s_ctx.visited[0]) == ESY_ERR_IO);
    assert(s_ctx.deleted_bonds == 1u);
    assert(s_ctx.device_type[0] == BD_ADDR_TYPE_UNKNOWN);
    assert(s_ctx.device_type[2] == BD_ADDR_TYPE_LE_RANDOM);
    s_ctx.flush_result = ESY_OK;
    assert(esy_ble_unpair(&s_ctx.ble, NULL) == ESY_OK);
    assert(s_ctx.deleted_bonds == 2u);
    assert(s_ctx.device_type[2] == BD_ADDR_TYPE_UNKNOWN);
    close_binding();
}

static void test_pairing_completion_checks_durable_flush_before_reporting_bond(void)
{
    bind();
    emit_connection(7u, ERROR_CODE_SUCCESS);
    s_ctx.bonded_device_index = 0;
    s_ctx.flush_result = ESY_ERR_IO;
    uint8_t packet[13] = {SM_EVENT_PAIRING_COMPLETE, 11u};
    little_endian_store_16(packet, 2, 7u);
    s_ctx.sm_handler->callback(HCI_EVENT_PACKET, 0u, packet, sizeof(packet));
    assert(s_ctx.event[1].type == ESY_BLE_EVENT_PAIRING_COMPLETE);
    assert(s_ctx.event[1].status == ESY_ERR_IO && !s_ctx.event[1].bonded);
    assert(s_ctx.flush_calls == 1u);
    s_ctx.flush_result = ESY_OK;
    s_ctx.sm_handler->callback(HCI_EVENT_PACKET, 0u, packet, sizeof(packet));
    assert(s_ctx.event[2].status == ESY_OK && s_ctx.event[2].bonded);
    packet[11] = ERROR_CODE_AUTHENTICATION_FAILURE;
    packet[12] = 5u;
    s_ctx.sm_handler->callback(HCI_EVENT_PACKET, 0u, packet, sizeof(packet));
    assert(s_ctx.event[3].type == ESY_BLE_EVENT_PAIRING_FAILED);
    assert(s_ctx.event[3].status != ESY_OK && s_ctx.event[3].reason == 5u);
    emit_disconnection(7u);
    close_binding();
}

static void test_schema_and_bond_mutation_wait_for_native_quiescence_after_gap_stop(void)
{
    esy_ble_adv_params_t advertising = {.interval_min = 0x20u, .interval_max = 0x40u};
    set_native_bond(0u, BD_ADDR_TYPE_LE_PUBLIC);
    bind();
    assert(esy_ble_adv_start(&s_ctx.ble, &advertising) == ESY_OK);
    assert(esy_ble_adv_stop(&s_ctx.ble) == ESY_OK);
    s_ctx.quiesce_result = ESY_ERR_BUSY;
    assert(esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service) == ESY_ERR_BUSY);
    assert(s_ctx.service.handle == 0u && s_ctx.database[1] == 0u);
    s_ctx.quiesce_result = ESY_ERR_IO;
    assert(esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service) == ESY_ERR_IO);
    s_ctx.quiesce_result = ESY_OK;
    assert(esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service) == ESY_OK);
    uint16_t service_handle = s_ctx.service.handle;
    uint16_t characteristic_handle = s_ctx.characteristics[0].value_handle;
    s_ctx.quiesce_result = ESY_ERR_BUSY;
    assert(esy_ble_gatt_unregister(&s_ctx.ble, &s_ctx.service) == ESY_ERR_BUSY);
    assert(s_ctx.service.handle == service_handle && s_ctx.characteristics[0].value_handle == characteristic_handle);
    assert(esy_ble_unpair(&s_ctx.ble, NULL) == ESY_ERR_BUSY);
    assert(s_ctx.deleted_bonds == 0u && s_ctx.flush_calls == 0u);
    s_ctx.quiesce_result = ESY_ERR_IO;
    assert(esy_ble_unpair(&s_ctx.ble, NULL) == ESY_ERR_IO);
    assert(s_ctx.deleted_bonds == 0u);
    s_ctx.quiesce_result = ESY_OK;
    assert(esy_ble_gatt_unregister(&s_ctx.ble, &s_ctx.service) == ESY_OK);
    assert(esy_ble_unpair(&s_ctx.ble, NULL) == ESY_OK);
    close_binding();
}

static void test_deinit_retries_failed_connect_cancel_until_completion_event(void)
{
    esy_ble_connect_params_t params = {
        .peer = {.type = ESY_BLE_ADDRESS_PUBLIC},
        .interval_min = 6u, .interval_max = 12u, .supervision_timeout = 10u
    };
    bind();
    assert(esy_ble_connect(&s_ctx.ble, &params) == ESY_OK);
    s_ctx.native_result = ERROR_CODE_COMMAND_DISALLOWED;
    assert(esy_ble_deinit(&s_ctx.ble) == ESY_ERR_BUSY);
    assert(s_ctx.cancel_calls == 1u && s_ctx.hci_handler != NULL);
    s_ctx.native_result = ERROR_CODE_SUCCESS;
    assert(esy_ble_deinit(&s_ctx.ble) == ESY_ERR_BUSY);
    assert(s_ctx.cancel_calls == 2u && s_ctx.adapter.owner != NULL);
    emit_connection(7u, ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER);
    close_binding();
    assert(s_ctx.cancel_calls == 2u && s_ctx.adapter.owner == NULL);
}

static void test_conflicting_adapter_cleanup_preserves_original_native_owner(void)
{
    uint8_t database[DATABASE_SIZE];
    uint8_t read_buffer[READ_BUFFER_SIZE];
    esy_ble_gatt_service_t *services[SERVICE_CAPACITY];
    esy_ble_btstack_link_t links[LINK_CAPACITY];
    esy_ble_btstack_subscription_t subscriptions[SUBSCRIPTION_CAPACITY];
    esy_ble_btstack_static_resources_t resources = {
        .attribute_database = database, .attribute_database_size = sizeof(database),
        .services = services, .service_capacity = SERVICE_CAPACITY,
        .links = links, .link_capacity = LINK_CAPACITY,
        .subscriptions = subscriptions, .subscription_capacity = SUBSCRIPTION_CAPACITY,
        .read_buffer = read_buffer, .read_buffer_size = sizeof(read_buffer)
    };
    esy_ble_btstack_t adapter = {0};
    esy_ble_t ble = {0};
    esy_ble_provider_t provider = {0};
    esy_ble_btstack_config_t config = s_ctx.config;
    config.static_resources = &resources;
    bind();
    btstack_packet_callback_registration_t *original_handler = s_ctx.hci_handler;
    assert(esy_ble_btstack_provider(&adapter, &config, &provider) == ESY_OK);
    assert(esy_ble_init(&ble, &provider, event_sink, &s_ctx.identity) == ESY_ERR_STATE);
    assert(esy_ble_deinit(&ble) == ESY_OK);
    assert(adapter.owner == NULL && s_ctx.adapter.owner != NULL);
    assert(s_ctx.hci_handler == original_handler && s_ctx.att_deinit_calls == 0u);
    assert(esy_ble_adv_stop(&s_ctx.ble) == ESY_OK);
    close_binding();
}

static void test_init_rejects_preexisting_host_connections_without_claiming_native_callbacks(void)
{
    s_ctx.preexisting_connection = true;
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &s_ctx.config, &s_ctx.provider) == ESY_OK);
    assert(esy_ble_init(&s_ctx.ble, &s_ctx.provider, event_sink, &s_ctx.identity) == ESY_ERR_BUSY);
    assert(s_ctx.restore_calls == 0u && s_ctx.att_init_calls == 0u);
    close_binding();
    assert(s_ctx.adapter.owner == NULL);
}

static void test_reinitialization_resets_native_key_policy_after_secure_connections_only_binding(void)
{
    s_ctx.config.minimum_security = 4u;
    bind();
    assert(s_ctx.key_min == 16u && s_ctx.key_max == 16u && s_ctx.secure_only);
    close_binding();
    s_ctx.config.minimum_security = 2u;
    bind();
    assert(s_ctx.key_min == 7u && s_ctx.key_max == 16u && !s_ctx.secure_only);
    assert((s_ctx.auth_requirements & SM_AUTHREQ_MITM_PROTECTION) == 0u);
    close_binding();
    s_ctx.config.minimum_security = 3u;
    bind();
    assert(s_ctx.key_min == 7u && s_ctx.key_max == 16u && !s_ctx.secure_only);
    assert(s_ctx.auth_requirements & SM_AUTHREQ_MITM_PROTECTION);
    close_binding();
}

static void test_failed_restore_cleanup_failure_retains_token_until_flush_retry(void)
{
    assert(esy_ble_btstack_provider(&s_ctx.adapter, &s_ctx.config, &s_ctx.provider) == ESY_OK);
    s_ctx.restore_result = ESY_ERR_IO;
    assert(esy_ble_init(&s_ctx.ble, &s_ctx.provider, event_sink, &s_ctx.identity) == ESY_ERR_IO);
    s_ctx.flush_result = ESY_ERR_IO;
    assert(esy_ble_deinit(&s_ctx.ble) == ESY_ERR_IO);
    assert(s_ctx.adapter.owner != NULL && s_ctx.flush_calls == 1u);
    assert(s_ctx.att_init_calls == 0u && s_ctx.hci_handler == NULL);
    assert(esy_ble_adv_stop(&s_ctx.ble) == ESY_ERR_STATE);
    close_binding();
    assert(s_ctx.adapter.owner == NULL && s_ctx.flush_calls == 2u);
    s_ctx.restore_result = ESY_OK;
    bind();
    close_binding();
}

static void test_security_requests_respect_immutable_binding_policy_and_live_connection(void)
{
    bind();
    emit_connection(7u, ERROR_CODE_SUCCESS);
    assert(esy_ble_security(&s_ctx.ble, 7u, 1u) == ESY_OK);
    assert(esy_ble_security(&s_ctx.ble, 7u, 2u) == ESY_OK);
    assert(s_ctx.security_requests == 0u);
    assert(esy_ble_security(&s_ctx.ble, 7u, 3u) == ESY_ERR_NOT_SUPPORTED);
    assert(esy_ble_security(&s_ctx.ble, 8u, 2u) == ESY_ERR_STATE);
    s_ctx.encryption_key_size = 0u;
    assert(esy_ble_security(&s_ctx.ble, 7u, 2u) == ESY_OK);
    assert(s_ctx.security_requests == 1u && s_ctx.last_connection == 7u);
    emit_disconnection(7u);
    close_binding();
}

static void test_native_connection_errors_are_mapped_without_leaving_pending_connection(void)
{
    static const struct {
        uint8_t native;
        esy_err_t expected;
    } k_errors[] = {
        {ERROR_CODE_MEMORY_CAPACITY_EXCEEDED, ESY_ERR_NO_MEM},
        {ERROR_CODE_COMMAND_DISALLOWED, ESY_ERR_BUSY},
        {ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER, ESY_ERR_STATE},
        {ERROR_CODE_INVALID_HCI_COMMAND_PARAMETERS, ESY_ERR_INVALID_ARG},
        {ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE, ESY_ERR_NOT_SUPPORTED},
        {ERROR_CODE_CONNECTION_TIMEOUT, ESY_ERR_TIMEOUT},
        {ERROR_CODE_AUTHENTICATION_FAILURE, ESY_ERR_IO}
    };
    esy_ble_connect_params_t params = {
        .peer = {.type = ESY_BLE_ADDRESS_PUBLIC},
        .interval_min = 6u, .interval_max = 12u, .supervision_timeout = 10u
    };
    bind();
    for (size_t i = 0u; i < sizeof(k_errors) / sizeof(k_errors[0]); ++i) {
        s_ctx.native_result = k_errors[i].native;
        assert(esy_ble_connect(&s_ctx.ble, &params) == k_errors[i].expected);
    }
    close_binding();
    assert(s_ctx.cancel_calls == 0u);
}

static void test_att_callbacks_enforce_property_permissions_and_write_command_semantics(void)
{
    s_ctx.characteristics[0].properties = ESY_BLE_GATT_READ;
    s_ctx.characteristics[1].properties = ESY_BLE_GATT_WRITE_COMMAND;
    bind();
    assert(esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service) == ESY_OK);
    emit_connection(7u, ERROR_CODE_SUCCESS);
    uint16_t read_handle = s_ctx.characteristics[0].value_handle;
    uint16_t write_handle = s_ctx.characteristics[1].value_handle;
    uint8_t data[] = {1u};
    assert(s_ctx.write(7u, read_handle, 0u, 0u, data, sizeof(data)) == ATT_ERROR_WRITE_NOT_PERMITTED);
    assert(s_ctx.read(7u, write_handle, 0u, NULL, 0u) ==
        (ATT_READ_ERROR_CODE_OFFSET | ATT_ERROR_READ_NOT_PERMITTED));
    assert(s_ctx.read(8u, read_handle, 0u, NULL, 0u) ==
        (ATT_READ_ERROR_CODE_OFFSET | ATT_ERROR_UNLIKELY_ERROR));
    assert(s_ctx.read(7u, 0xfffeu, 0u, NULL, 0u) ==
        (ATT_READ_ERROR_CODE_OFFSET | ATT_ERROR_INVALID_HANDLE));
    assert(s_ctx.write(7u, write_handle, 0u, 1u, data, sizeof(data)) == ATT_ERROR_SUCCESS);
    assert(s_ctx.write_calls == 1u && s_ctx.command && s_ctx.attribute_offset == 1u);
    assert(esy_ble_notify(&s_ctx.ble, 7u, read_handle, data, sizeof(data)) == ESY_ERR_INVALID_ARG);
    emit_disconnection(7u);
    close_binding();
}

static void test_read_snapshot_capacity_and_copy_failures_are_bounded(void)
{
    s_ctx.resources.read_buffer_size = 2u;
    bind();
    assert(esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service) == ESY_OK);
    emit_connection(7u, ERROR_CODE_SUCCESS);
    uint16_t handle = s_ctx.characteristics[0].value_handle;
    uint8_t buffer[2] = {0xa5u, 0xa5u};
    assert(s_ctx.read(7u, handle, 0u, NULL, 0u) ==
        (ATT_READ_ERROR_CODE_OFFSET | ATT_ERROR_INSUFFICIENT_RESOURCES));
    assert(s_ctx.read(7u, handle, 0u, buffer, sizeof(buffer)) == 0u);
    assert(buffer[0] == 0xa5u && buffer[1] == 0xa5u);
    assert(s_ctx.read_calls == 1u);
    emit_disconnection(7u);
    close_binding();

    reset_fixture();
    bind();
    assert(esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service) == ESY_OK);
    emit_connection(7u, ERROR_CODE_SUCCESS);
    handle = s_ctx.characteristics[0].value_handle;
    assert(s_ctx.read(7u, handle, 0u, NULL, 0u) == sizeof(k_value));
    s_ctx.encryption_key_size = 0u;
    assert(s_ctx.read(7u, handle, 0u, buffer, sizeof(buffer)) == 0u);
    s_ctx.encryption_key_size = 16u;
    assert(s_ctx.read(7u, handle, 0u, buffer, sizeof(buffer)) == 0u);
    assert(s_ctx.read(7u, handle, 0u, NULL, 0u) == sizeof(k_value));
    assert(s_ctx.read(8u, handle, 0u, buffer, sizeof(buffer)) == 0u);
    assert(s_ctx.read(7u, handle, 0u, NULL, 0u) == sizeof(k_value));
    assert(s_ctx.read(7u, 0xfffeu, 0u, buffer, sizeof(buffer)) == 0u);
    assert(s_ctx.read(7u, handle, 0u, NULL, 0u) == sizeof(k_value));
    att_read_callback_t detached_read = s_ctx.read;
    emit_disconnection(7u);
    assert(detached_read(7u, handle, 0u, buffer, sizeof(buffer)) == 0u);
    close_binding();
    assert(detached_read(7u, handle, 0u, buffer, sizeof(buffer)) == 0u);
}

static void test_notifications_validate_negotiated_mtu_before_native_submission(void)
{
    bind();
    assert(esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service) == ESY_OK);
    emit_connection(7u, ERROR_CODE_SUCCESS);
    uint16_t handle = s_ctx.characteristics[0].value_handle;
    uint8_t enabled[] = {1u, 0u};
    uint8_t payload[TEST_ATT_MTU - 2u];
    memset(payload, 0x42, sizeof(payload));
    assert(s_ctx.write(7u, (uint16_t)(handle + 1u), ATT_TRANSACTION_MODE_NONE,
                        0u, enabled, sizeof(enabled)) == ATT_ERROR_SUCCESS);
    assert(esy_ble_notify(&s_ctx.ble, 7u, handle, payload, TEST_ATT_MTU - 3u) == ESY_OK);
    assert(s_ctx.notify_calls == 1u && s_ctx.notify_length == TEST_ATT_MTU - 3u);
    assert(esy_ble_notify(&s_ctx.ble, 7u, handle, payload, sizeof(payload)) == ESY_ERR_INVALID_ARG);
    assert(s_ctx.notify_calls == 1u);
    assert(esy_ble_notify(&s_ctx.ble, 7u, handle, NULL, 0u) == ESY_OK);
    assert(s_ctx.notify_calls == 2u && s_ctx.notify_length == 0u);
    s_ctx.mtu = 3u;
    assert(esy_ble_notify(&s_ctx.ble, 7u, handle, NULL, 0u) == ESY_OK);
    assert(esy_ble_notify(&s_ctx.ble, 7u, handle, payload, 1u) == ESY_ERR_INVALID_ARG);
    assert(s_ctx.notify_calls == 3u);
    s_ctx.mtu = 0u;
    assert(esy_ble_notify(&s_ctx.ble, 7u, handle, NULL, 0u) == ESY_ERR_STATE);
    s_ctx.mtu = 2u;
    assert(esy_ble_notify(&s_ctx.ble, 7u, handle, NULL, 0u) == ESY_ERR_STATE);
    assert(s_ctx.notify_calls == 3u);
    emit_disconnection(7u);
    close_binding();
}

static void test_lifo_unregister_reclaims_handles_without_moving_retained_service(void)
{
    bind();
    assert(esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service) == ESY_OK);
    uint16_t retained_handle = s_ctx.service.handle;
    uint16_t retained_value = s_ctx.characteristics[0].value_handle;
    esy_ble_gatt_characteristic_t characteristics[2] = {
        s_ctx.characteristics[0], s_ctx.characteristics[1]
    };
    for (size_t i = 0u; i < 2u; ++i) {
        characteristics[i].value_handle = 0u;
    }
    esy_ble_gatt_service_t service = s_ctx.service;
    service.handle = 0u;
    service.characteristics = characteristics;
    /* Two notifying characteristics consume seven handles per registration. */
    for (size_t i = 0u; i < GATT_REUSE_ITERATIONS; ++i) {
        assert(esy_ble_gatt_register(&s_ctx.ble, &service) == ESY_OK);
        assert(service.handle == 8u && characteristics[0].value_handle == 10u);
        assert(esy_ble_gatt_unregister(&s_ctx.ble, &service) == ESY_OK);
        assert(service.handle == 0u && characteristics[0].value_handle == 0u);
        assert(s_ctx.service.handle == retained_handle);
        assert(s_ctx.characteristics[0].value_handle == retained_value);
    }
    assert(esy_ble_gatt_unregister(&s_ctx.ble, &s_ctx.service) == ESY_OK);
    assert(s_ctx.database[1] == 0u && s_ctx.database[2] == 0u);
    assert(esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service) == ESY_OK);
    assert(s_ctx.service.handle == retained_handle);
    close_binding();
}

#if defined(ESY_BLE_TEST_NATIVE_ATT)
static uint16_t native_request(uint8_t *request, size_t request_length,
                                uint8_t response[TEST_ATT_MTU + 1u])
{
    assert(request_length <= UINT16_MAX);
    att_connection_t connection = {
        .con_handle = 7u, .mtu = TEST_ATT_MTU, .max_mtu = TEST_ATT_MTU,
        .encryption_key_size = s_ctx.encryption_key_size,
        .authenticated = s_ctx.authenticated, .secure_connection = s_ctx.secure_connection
    };
    memset(response, 0xa5, TEST_ATT_MTU + 1u);
    uint16_t length = att_handle_request(&connection, request, (uint16_t)request_length, response);
    assert(length <= TEST_ATT_MTU);
    assert(response[TEST_ATT_MTU] == 0xa5u);
    return length;
}

static void assert_native_error(uint8_t *request, size_t request_length, uint8_t error)
{
    uint8_t response[TEST_ATT_MTU + 1u];
    assert(native_request(request, request_length, response) == 5u);
    assert(response[0] == ATT_ERROR_RESPONSE && response[1] == request[0]);
    assert(little_endian_read_16(response, 2) == little_endian_read_16(request, 1));
    assert(response[4] == error);
}

static void test_native_att_read_and_blob_preserve_snapshot_bytes_and_offsets(void)
{
    bind();
    assert(esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service) == ESY_OK);
    emit_connection(7u, ERROR_CODE_SUCCESS);
    uint16_t handle = s_ctx.characteristics[0].value_handle;
    uint8_t request[] = {ATT_READ_REQUEST, 0u, 0u, 0u, 0u};
    little_endian_store_16(request, 1, handle);
    uint8_t response[TEST_ATT_MTU + 1u];
    assert(native_request(request, 3u, response) == 1u + sizeof(k_value));
    assert(response[0] == ATT_READ_RESPONSE);
    assert(memcmp(response + 1u, k_value, sizeof(k_value)) == 0);
    request[0] = ATT_READ_BLOB_REQUEST;
    little_endian_store_16(request, 3, 2u);
    assert(native_request(request, sizeof(request), response) == 3u);
    assert(response[0] == ATT_READ_BLOB_RESPONSE);
    assert(memcmp(response + 1u, k_value + 2u, 2u) == 0);
    little_endian_store_16(request, 3, sizeof(k_value));
    assert(native_request(request, sizeof(request), response) == 1u);
    assert(response[0] == ATT_READ_BLOB_RESPONSE);
    little_endian_store_16(request, 3, sizeof(k_value) + 1u);
    assert_native_error(request, sizeof(request), ATT_ERROR_INVALID_OFFSET);

    s_ctx.queried_length = TEST_ATT_MTU + 4u;
    request[0] = ATT_READ_REQUEST;
    assert(native_request(request, 3u, response) == TEST_ATT_MTU);
    for (size_t i = 0u; i < TEST_ATT_MTU - 1u; ++i) {
        assert(response[i + 1u] == k_value[i % sizeof(k_value)]);
    }
    request[0] = ATT_READ_BLOB_REQUEST;
    little_endian_store_16(request, 3, TEST_ATT_MTU - 1u);
    assert(native_request(request, sizeof(request), response) == 6u);
    for (size_t i = 0u; i < 5u; ++i) {
        assert(response[i + 1u] == k_value[(TEST_ATT_MTU - 1u + i) % sizeof(k_value)]);
    }
    emit_disconnection(7u);
    close_binding();
}

static void test_native_att_slice_errors_and_invalid_lengths_are_error_pdus(void)
{
    bind();
    assert(esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service) == ESY_OK);
    emit_connection(7u, ERROR_CODE_SUCCESS);
    uint8_t request[] = {ATT_READ_REQUEST, 0u, 0u, 1u, 0u};
    little_endian_store_16(request, 1, s_ctx.characteristics[0].value_handle);
    s_ctx.read_slice_error = ESY_BLE_ATT_UNLIKELY_ERROR;
    assert_native_error(request, 3u, ATT_ERROR_UNLIKELY_ERROR);
    request[0] = ATT_READ_BLOB_REQUEST;
    assert_native_error(request, sizeof(request), ATT_ERROR_UNLIKELY_ERROR);
    s_ctx.read_slice_error = ESY_BLE_ATT_SUCCESS;
    s_ctx.invalid_read_length = true;
    assert_native_error(request, sizeof(request), ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH);
    s_ctx.invalid_read_length = false;
    s_ctx.short_read_length = true;
    assert_native_error(request, sizeof(request), ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH);
    s_ctx.short_read_length = false;
    s_ctx.queried_length = ATT_READ_ERROR_CODE_OFFSET;
    assert_native_error(request, sizeof(request), ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH);
    emit_disconnection(7u);
    close_binding();
}

static void test_native_att_by_type_and_multiple_use_each_attribute_snapshot(void)
{
    s_ctx.characteristics[1].uuid = s_ctx.characteristics[0].uuid;
    bind();
    assert(esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service) == ESY_OK);
    emit_connection(7u, ERROR_CODE_SUCCESS);
    uint16_t first = s_ctx.characteristics[0].value_handle;
    uint16_t second = s_ctx.characteristics[1].value_handle;
    uint8_t request[] = {ATT_READ_BY_TYPE_REQUEST, 1u, 0u, 0xffu, 0xffu, 0u, 0u};
    memcpy(request + 5u, s_ctx.characteristics[0].uuid.bytes, 2u);
    uint8_t response[TEST_ATT_MTU + 1u];
    assert(native_request(request, sizeof(request), response) == 14u);
    assert(response[0] == ATT_READ_BY_TYPE_RESPONSE && response[1] == 6u);
    assert(little_endian_read_16(response, 2) == first);
    assert(little_endian_read_16(response, 8) == second);
    assert(memcmp(response + 4u, k_value, sizeof(k_value)) == 0);
    assert(memcmp(response + 10u, k_value, sizeof(k_value)) == 0);
    request[0] = ATT_READ_MULTIPLE_REQUEST;
    little_endian_store_16(request, 1, first);
    little_endian_store_16(request, 3, second);
    assert(native_request(request, 5u, response) == 9u);
    assert(response[0] == ATT_READ_MULTIPLE_RESPONSE);
    assert(memcmp(response + 1u, k_value, sizeof(k_value)) == 0);
    assert(memcmp(response + 5u, k_value, sizeof(k_value)) == 0);
    s_ctx.read_slice_error = ESY_BLE_ATT_UNLIKELY_ERROR;
    assert_native_error(request, 5u, ATT_ERROR_UNLIKELY_ERROR);
    request[0] = ATT_READ_BY_TYPE_REQUEST;
    little_endian_store_16(request, 1, first);
    little_endian_store_16(request, 3, UINT16_MAX);
    memcpy(request + 5u, s_ctx.characteristics[0].uuid.bytes, 2u);
    assert_native_error(request, sizeof(request), ATT_ERROR_UNLIKELY_ERROR);
    s_ctx.read_slice_error = ESY_BLE_ATT_SUCCESS;
    s_ctx.queried_length = TEST_ATT_MTU + 4u;
    assert(native_request(request, sizeof(request), response) == TEST_ATT_MTU);
    assert(response[0] == ATT_READ_BY_TYPE_RESPONSE);
    assert(response[1] == TEST_ATT_MTU - 2u);
    assert(little_endian_read_16(response, 2) == first);
    for (size_t i = 0u; i < TEST_ATT_MTU - 4u; ++i) {
        assert(response[i + 4u] == k_value[i % sizeof(k_value)]);
    }
    emit_disconnection(7u);
    close_binding();
}

static void test_native_att_small_snapshot_capacity_reports_resources_error(void)
{
    s_ctx.resources.read_buffer_size = sizeof(k_value) - 1u;
    bind();
    assert(esy_ble_gatt_register(&s_ctx.ble, &s_ctx.service) == ESY_OK);
    emit_connection(7u, ERROR_CODE_SUCCESS);
    uint8_t request[] = {ATT_READ_REQUEST, 0u, 0u};
    little_endian_store_16(request, 1, s_ctx.characteristics[0].value_handle);
    assert_native_error(request, sizeof(request), ATT_ERROR_INSUFFICIENT_RESOURCES);
    emit_disconnection(7u);
    close_binding();
}
#endif

int main(void)
{
    static void (*const k_tests[])(void) = {
        test_factory_rejects_invalid_inputs_without_mutating_token,
        test_factory_requires_all_caller_owned_backing_storage,
        test_lifecycle_copies_configuration_sets_policy_and_releases_native_callbacks,
        test_factory_rejects_live_and_copied_tokens,
        test_restore_failure_is_cleanable_and_can_be_retried_after_deinit,
        test_init_requires_working_native_host_and_cleanup_releases_token,
        test_deinit_retains_owner_until_quiescence_and_durable_flush_succeed,
        test_deinit_waits_for_active_link_disconnect_event,
        test_gap_copies_advertising_payload_and_maps_scan_parameters,
        test_connect_maps_address_units_and_result_events,
        test_event_translation_preserves_scan_payload_and_rejects_truncation,
        test_failed_connection_event_does_not_create_live_link,
        test_authentication_requests_require_matching_owner_reply_and_expire,
        test_gatt_database_uses_little_endian_uuid_values_and_native_security_flags,
        test_gatt_read_query_offsets_write_context_and_att_errors,
        test_security_four_requires_encryption_mitm_secure_connections_and_full_key,
        test_ccc_is_per_link_and_disconnect_clears_subscription_before_handle_reuse,
        test_ccc_validates_offset_value_capacity_and_encryption_before_mutation,
        test_gatt_register_capacity_and_unsupported_writes_fail_without_partial_schema,
        test_gatt_unregister_requires_same_identity_and_lifo_and_clears_removed_handles,
        test_bond_enumeration_unpair_uses_native_cache_api_and_reports_durable_failure,
        test_pairing_completion_checks_durable_flush_before_reporting_bond,
        test_schema_and_bond_mutation_wait_for_native_quiescence_after_gap_stop,
        test_deinit_retries_failed_connect_cancel_until_completion_event,
        test_conflicting_adapter_cleanup_preserves_original_native_owner,
        test_init_rejects_preexisting_host_connections_without_claiming_native_callbacks,
        test_reinitialization_resets_native_key_policy_after_secure_connections_only_binding,
        test_failed_restore_cleanup_failure_retains_token_until_flush_retry,
        test_security_requests_respect_immutable_binding_policy_and_live_connection,
        test_native_connection_errors_are_mapped_without_leaving_pending_connection,
        test_att_callbacks_enforce_property_permissions_and_write_command_semantics,
        test_read_snapshot_capacity_and_copy_failures_are_bounded,
        test_notifications_validate_negotiated_mtu_before_native_submission,
        test_lifo_unregister_reclaims_handles_without_moving_retained_service,
#if defined(ESY_BLE_TEST_NATIVE_ATT)
        test_native_att_read_and_blob_preserve_snapshot_bytes_and_offsets,
        test_native_att_slice_errors_and_invalid_lengths_are_error_pdus,
        test_native_att_by_type_and_multiple_use_each_attribute_snapshot,
        test_native_att_small_snapshot_capacity_reports_resources_error,
#endif
    };
    for (size_t i = 0u; i < sizeof(k_tests) / sizeof(k_tests[0]); ++i) {
        reset_fixture();
        k_tests[i]();
    }
#if defined(ESY_BLE_TEST_NATIVE_ATT)
    printf("BTstack adapter native ATT contracts: %zu passed\n", sizeof(k_tests) / sizeof(k_tests[0]));
#else
    printf("BTstack adapter runtime contracts: %zu passed\n", sizeof(k_tests) / sizeof(k_tests[0]));
#endif
    return 0;
}
