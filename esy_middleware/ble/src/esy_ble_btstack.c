/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/ble/esy_ble_btstack.h"
#include "btstack.h"
#include <limits.h>
#include <string.h>

#if !defined(ENABLE_BLE) || !defined(ENABLE_LE_CENTRAL) || \
    !defined(ENABLE_LE_PERIPHERAL) || !defined(ENABLE_LE_SECURE_CONNECTIONS)
#error "BTstack BLE adapter requires central, peripheral and Secure Connections support"
#endif
#if !defined(MAX_NR_HCI_CONNECTIONS) || MAX_NR_HCI_CONNECTIONS < 1
#error "BTstack BLE adapter requires a finite positive MAX_NR_HCI_CONNECTIONS"
#endif
#if !defined(NVN_NUM_GATT_SERVER_CCC) || NVN_NUM_GATT_SERVER_CCC != 0
#error "BTstack adapter requires NVN_NUM_GATT_SERVER_CCC=0 in the native provider configuration"
#endif
#if defined(ENABLE_GATT_OVER_EATT)
#error "BTstack adapter does not support EATT bearers"
#endif

#define BLE_DB_HEADER_SIZE 1u
#define BLE_DB_END_SIZE 2u
#define BLE_DB_ATTRIBUTE_HEADER_SIZE 6u
#define BLE_DB_UUID16_SIZE 2u
#define BLE_DB_DECLARATION_SIZE 3u
#define BLE_CCC_SIZE 2u
#define BLE_SECURITY_ENCRYPTED 2u
#define BLE_SECURITY_AUTHENTICATED 3u
#define BLE_SECURITY_SECURE_CONNECTION 4u
#define BLE_SECURITY_KEY_SIZE 16u
#define BLE_SECURITY_KEY_SIZE_MIN 7u
#define BLE_SCAN_INTERVAL 0x0060u
#define BLE_SCAN_WINDOW 0x0030u
#define BLE_ADV_CHANNEL_MAP 0x07u
#define BLE_ADV_SCANNABLE 0x02u
#define BLE_ADV_NONCONNECTABLE 0x03u
#define BLE_PASSKEY_MAX 999999u
#define BLE_ATT_READ_HEADER_SIZE 1u
#define BLE_ATT_NOTIFICATION_HEADER_SIZE 3u

typedef struct {
    esy_ble_btstack_config_t config;
    esy_ble_btstack_static_resources_t resources;
    esy_ble_event_fn event;
    void *event_user;
    btstack_packet_callback_registration_t hci_callback;
    btstack_packet_callback_registration_t sm_callback;
    size_t service_count;
    size_t database_used;
    uint32_t next_handle;
    size_t read_length;
    esy_ble_connection_t read_connection;
    uint16_t read_handle;
    uint16_t read_offset;
    uint8_t advertising_data[ESY_BLE_LEGACY_DATA_MAX];
    uint8_t scan_response[ESY_BLE_LEGACY_DATA_MAX];
    bool claimed;
    bool ready;
    bool stopping;
    bool advertising;
    bool scanning;
    bool connecting;
    bool attached;
    bool read_valid;
} ble_btstack_context_t;

typedef struct {
    ble_btstack_context_t *active;
} ble_btstack_registry_t;

static ble_btstack_registry_t s_ctx;
static const uint8_t k_empty_database[] = { ATT_DB_VERSION, 0u, 0u };
static const uint8_t k_empty_payload[] = {0u};

_Static_assert(sizeof(ble_btstack_context_t) <= ESY_BLE_BTSTACK_STORAGE_SIZE,
               "BTstack adapter storage is too small");
_Static_assert(_Alignof(ble_btstack_context_t) <= _Alignof(max_align_t),
               "BTstack adapter storage alignment is insufficient");

static ble_btstack_context_t *context(void *arg)
{
    esy_ble_btstack_t *token = arg;
    if (token == NULL || token->owner != token) {
        return NULL;
    }
    return (ble_btstack_context_t *)(void *)token->storage.bytes;
}

static esy_err_t native_status(uint8_t status)
{
    switch (status) {
    case ERROR_CODE_SUCCESS: return ESY_OK;
    case ERROR_CODE_MEMORY_CAPACITY_EXCEEDED:
    case BTSTACK_MEMORY_ALLOC_FAILED: return ESY_ERR_NO_MEM;
    case ERROR_CODE_COMMAND_DISALLOWED:
    case BTSTACK_ACL_BUFFERS_FULL: return ESY_ERR_BUSY;
    case ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER: return ESY_ERR_STATE;
    case ERROR_CODE_INVALID_HCI_COMMAND_PARAMETERS: return ESY_ERR_INVALID_ARG;
    case ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE: return ESY_ERR_NOT_SUPPORTED;
    case ERROR_CODE_CONNECTION_TIMEOUT: return ESY_ERR_TIMEOUT;
    default: return ESY_ERR_IO;
    }
}

static void address_from_native(esy_ble_address_t *out, uint8_t type,
                                const bd_addr_t address)
{
    out->type = type & 1u;
    for (size_t i = 0u; i < ESY_BLE_ADDRESS_SIZE; ++i) {
        out->bytes[i] = address[ESY_BLE_ADDRESS_SIZE - 1u - i];
    }
}

static void address_to_native(bd_addr_t out, const esy_ble_address_t *address)
{
    for (size_t i = 0u; i < ESY_BLE_ADDRESS_SIZE; ++i) {
        out[i] = address->bytes[ESY_BLE_ADDRESS_SIZE - 1u - i];
    }
}

static esy_ble_btstack_link_t *find_link(ble_btstack_context_t *ctx,
                                        esy_ble_connection_t connection)
{
    if (connection == ESY_BLE_CONNECTION_INVALID) {
        return NULL;
    }
    for (size_t i = 0u; i < ctx->resources.link_capacity; ++i) {
        if (ctx->resources.links[i].connection == connection) {
            return &ctx->resources.links[i];
        }
    }
    return NULL;
}

static bool has_links(const ble_btstack_context_t *ctx)
{
    for (size_t i = 0u; i < ctx->resources.link_capacity; ++i) {
        if (ctx->resources.links[i].connection != ESY_BLE_CONNECTION_INVALID) {
            return true;
        }
    }
    return false;
}

static bool live_link(ble_btstack_context_t *ctx, esy_ble_connection_t connection)
{
    return connection != ESY_BLE_CONNECTION_INVALID &&
        find_link(ctx, connection) != NULL &&
        gap_get_connection_type(connection) == GAP_CONNECTION_LE;
}

static uint8_t security_level(esy_ble_connection_t connection)
{
    if (gap_encryption_key_size(connection) == 0u) {
        return 1u;
    }
    if (!gap_authenticated(connection)) {
        return BLE_SECURITY_ENCRYPTED;
    }
    return gap_secure_connection(connection) &&
        gap_encryption_key_size(connection) == BLE_SECURITY_KEY_SIZE ?
        BLE_SECURITY_SECURE_CONNECTION : BLE_SECURITY_AUTHENTICATED;
}

static uint8_t security_error(esy_ble_connection_t connection, uint8_t required)
{
    if (security_level(connection) >= required) {
        return ESY_BLE_ATT_SUCCESS;
    }
    return required == BLE_SECURITY_ENCRYPTED ?
        ATT_ERROR_INSUFFICIENT_ENCRYPTION : ATT_ERROR_INSUFFICIENT_AUTHENTICATION;
}

static esy_ble_gatt_characteristic_t *find_characteristic(ble_btstack_context_t *ctx,
                                                         uint16_t handle,
                                                         bool *ccc)
{
    for (size_t i = 0u; i < ctx->service_count; ++i) {
        esy_ble_gatt_service_t *service = ctx->resources.services[i];
        for (size_t j = 0u; j < service->characteristic_count; ++j) {
            esy_ble_gatt_characteristic_t *ch = &service->characteristics[j];
            if (ch->value_handle == handle) {
                *ccc = false;
                return ch;
            }
            if ((ch->properties & ESY_BLE_GATT_NOTIFY) != 0u &&
                (uint32_t)ch->value_handle + 1u == handle) {
                *ccc = true;
                return ch;
            }
        }
    }
    return NULL;
}

static esy_ble_btstack_subscription_t *find_subscription(ble_btstack_context_t *ctx,
    esy_ble_connection_t connection, uint16_t value_handle)
{
    for (size_t i = 0u; i < ctx->resources.subscription_capacity; ++i) {
        esy_ble_btstack_subscription_t *slot = &ctx->resources.subscriptions[i];
        if (slot->connection == connection && slot->value_handle == value_handle) {
            return slot;
        }
    }
    return NULL;
}

static uint16_t read_attribute(hci_con_handle_t connection, uint16_t handle,
                               uint16_t offset, uint8_t *buffer, uint16_t capacity)
{
    ble_btstack_context_t *ctx = s_ctx.active;
    bool read_valid = ctx != NULL && ctx->read_valid;
    if (ctx != NULL) {
        ctx->read_valid = false;
    }
    if (ctx == NULL || !ctx->ready || !live_link(ctx, connection)) {
        return buffer == NULL ? ATT_READ_ERROR_CODE_OFFSET | ATT_ERROR_UNLIKELY_ERROR : 0u;
    }
    bool ccc = false;
    esy_ble_gatt_characteristic_t *ch = find_characteristic(ctx, handle, &ccc);
    if (ch == NULL) {
        return buffer == NULL ? ATT_READ_ERROR_CODE_OFFSET | ATT_ERROR_INVALID_HANDLE : 0u;
    }
    if (ccc) {
        if (buffer == NULL) {
            return BLE_CCC_SIZE;
        }
        if (offset > BLE_CCC_SIZE) {
            return 0u;
        }
        uint8_t value[BLE_CCC_SIZE] = {0u, 0u};
        if (find_subscription(ctx, connection, ch->value_handle) != NULL) {
            value[0] = 1u;
        }
        size_t length = BLE_CCC_SIZE - offset;
        if (length > capacity) {
            length = capacity;
        }
        memcpy(buffer, &value[offset], length);
        return (uint16_t)length;
    }
    if ((ch->properties & ESY_BLE_GATT_READ) == 0u) {
        return buffer == NULL ? ATT_READ_ERROR_CODE_OFFSET | ATT_ERROR_READ_NOT_PERMITTED : 0u;
    }
    uint8_t status = security_error(connection, ch->read_security);
    if (status != ESY_BLE_ATT_SUCCESS) {
        return buffer == NULL ? ATT_READ_ERROR_CODE_OFFSET | status : 0u;
    }
    if (buffer != NULL) {
        /* Native copy returns are byte counts; only length probes decode errors. */
        if (!read_valid || ctx->read_connection != connection ||
            ctx->read_handle != handle || ctx->read_offset != offset) {
            return 0u;
        }
        size_t copied = ctx->read_length < capacity ? ctx->read_length : capacity;
        memcpy(buffer, ctx->resources.read_buffer, copied);
        return (uint16_t)copied;
    }
    /* Stage application work while the native length probe can report errors. */
    size_t length = 0u;
    status = ch->read(ch->user, connection, 0u, NULL, 0u, &length);
    if (status != ESY_BLE_ATT_SUCCESS) {
        return ATT_READ_ERROR_CODE_OFFSET | status;
    }
    if (length >= ATT_READ_ERROR_CODE_OFFSET) {
        return ATT_READ_ERROR_CODE_OFFSET | ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH;
    }
    if (offset > length) {
        return (uint16_t)length;
    }
    uint16_t mtu = att_server_get_mtu(connection);
    if (mtu <= BLE_ATT_READ_HEADER_SIZE) {
        return ATT_READ_ERROR_CODE_OFFSET | ATT_ERROR_UNLIKELY_ERROR;
    }
    size_t requested = length - offset;
    if (requested > mtu - BLE_ATT_READ_HEADER_SIZE) {
        requested = mtu - BLE_ATT_READ_HEADER_SIZE;
    }
    if (requested > ctx->resources.read_buffer_size) {
        return ATT_READ_ERROR_CODE_OFFSET | ATT_ERROR_INSUFFICIENT_RESOURCES;
    }
    size_t copied = 0u;
    if (requested != 0u) {
        status = ch->read(ch->user, connection, offset, ctx->resources.read_buffer,
                          requested, &copied);
        if (status != ESY_BLE_ATT_SUCCESS) {
            return ATT_READ_ERROR_CODE_OFFSET | status;
        }
        if (copied != requested) {
            return ATT_READ_ERROR_CODE_OFFSET | ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH;
        }
    }
    ctx->read_length = copied;
    ctx->read_connection = connection;
    ctx->read_handle = handle;
    ctx->read_offset = offset;
    ctx->read_valid = true;
    return (uint16_t)length;
}

static int write_attribute(hci_con_handle_t connection, uint16_t handle,
    uint16_t transaction, uint16_t offset, uint8_t *buffer, uint16_t length)
{
    ble_btstack_context_t *ctx = s_ctx.active;
    if (ctx == NULL || !ctx->ready || !live_link(ctx, connection)) {
        return ATT_ERROR_UNLIKELY_ERROR;
    }
    if (transaction != ATT_TRANSACTION_MODE_NONE) {
        return ATT_ERROR_REQUEST_NOT_SUPPORTED;
    }
    bool ccc = false;
    esy_ble_gatt_characteristic_t *ch = find_characteristic(ctx, handle, &ccc);
    if (ch == NULL) {
        return ATT_ERROR_INVALID_HANDLE;
    }
    if (ccc) {
        if (offset != 0u) {
            return ATT_ERROR_INVALID_OFFSET;
        }
        if (length != BLE_CCC_SIZE || buffer == NULL || buffer[1] != 0u || buffer[0] > 1u) {
            return ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH;
        }
        uint8_t status = security_error(connection, ch->notify_security);
        if (status != ESY_BLE_ATT_SUCCESS) {
            return status;
        }
        esy_ble_btstack_subscription_t *slot = find_subscription(ctx, connection, ch->value_handle);
        if (buffer[0] == 0u) {
            if (slot != NULL) {
                slot->connection = ESY_BLE_CONNECTION_INVALID;
                slot->value_handle = 0u;
            }
            return ATT_ERROR_SUCCESS;
        }
        if (slot == NULL) {
            for (size_t i = 0u; i < ctx->resources.subscription_capacity; ++i) {
                if (ctx->resources.subscriptions[i].connection == ESY_BLE_CONNECTION_INVALID) {
                    slot = &ctx->resources.subscriptions[i];
                    break;
                }
            }
        }
        if (slot == NULL) {
            return ATT_ERROR_INSUFFICIENT_RESOURCES;
        }
        slot->connection = connection;
        slot->value_handle = ch->value_handle;
        return ATT_ERROR_SUCCESS;
    }
    if ((ch->properties & (ESY_BLE_GATT_WRITE | ESY_BLE_GATT_WRITE_COMMAND)) == 0u) {
        return ATT_ERROR_WRITE_NOT_PERMITTED;
    }
    uint8_t status = security_error(connection, ch->write_security);
    if (status != ESY_BLE_ATT_SUCCESS) {
        return status;
    }
    return ch->write(ch->user, connection, offset, buffer, length,
                     (ch->properties & ESY_BLE_GATT_WRITE_COMMAND) != 0u);
}

static void emit_event(ble_btstack_context_t *ctx, esy_ble_event_t *event)
{
    esy_ble_btstack_link_t *link = find_link(ctx, event->connection);
    if (link != NULL) {
        event->peer = link->peer;
    }
    ctx->event(ctx->event_user, event);
}

static void connection_event(ble_btstack_context_t *ctx, const uint8_t *packet)
{
    esy_ble_event_t event = {0};
    event.type = ESY_BLE_EVENT_CONNECTED;
    event.reason = gap_subevent_le_connection_complete_get_status(packet);
    event.status = native_status(event.reason);
    event.connection = ESY_BLE_CONNECTION_INVALID;
    uint8_t role = gap_subevent_le_connection_complete_get_role(packet);
    if (role == HCI_ROLE_MASTER || event.reason != 0u) {
        ctx->connecting = false;
    }
    if (role == HCI_ROLE_SLAVE) {
        ctx->advertising = false;
    }
    bd_addr_t address;
    gap_subevent_le_connection_complete_get_peer_address(packet, address);
    address_from_native(&event.peer,
        gap_subevent_le_connection_complete_get_peer_address_type(packet), address);
    if (event.reason == 0u) {
        event.connection = gap_subevent_le_connection_complete_get_connection_handle(packet);
        esy_ble_btstack_link_t *slot = NULL;
        for (size_t i = 0u; i < ctx->resources.link_capacity; ++i) {
            if (ctx->resources.links[i].connection == ESY_BLE_CONNECTION_INVALID) {
                slot = &ctx->resources.links[i];
                break;
            }
        }
        if (slot == NULL) {
            (void)gap_disconnect(event.connection);
            event.connection = ESY_BLE_CONNECTION_INVALID;
            event.status = ESY_ERR_NO_MEM;
        } else {
            slot->connection = event.connection;
            slot->peer = event.peer;
            slot->auth_request = 0u;
            if (ctx->stopping) {
                (void)gap_disconnect(event.connection);
            }
        }
    }
    emit_event(ctx, &event);
}

static void authentication_event(ble_btstack_context_t *ctx, const uint8_t *packet,
                                  uint8_t native_type)
{
    esy_ble_event_t event = {0};
    event.connection = little_endian_read_16(packet, 2u);
    esy_ble_btstack_link_t *link = find_link(ctx, event.connection);
    if (link == NULL) {
        return;
    }
    switch (native_type) {
    case SM_EVENT_JUST_WORKS_REQUEST: event.type = ESY_BLE_EVENT_PAIRING_CONFIRM; break;
    case SM_EVENT_PASSKEY_INPUT_NUMBER: event.type = ESY_BLE_EVENT_PASSKEY_ENTRY; break;
    case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
        event.type = ESY_BLE_EVENT_PASSKEY_DISPLAY;
        event.passkey = sm_event_passkey_display_number_get_passkey(packet);
        break;
    case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
        event.type = ESY_BLE_EVENT_PASSKEY_CONFIRM;
        event.passkey = sm_event_numeric_comparison_request_get_passkey(packet);
        break;
    default: event.type = ESY_BLE_EVENT_AUTH_CANCELLED; break;
    }
    link->auth_request = event.type == ESY_BLE_EVENT_AUTH_CANCELLED ||
        event.type == ESY_BLE_EVENT_PASSKEY_DISPLAY ? 0u : (uint8_t)event.type + 1u;
    emit_event(ctx, &event);
}

static void pairing_event(ble_btstack_context_t *ctx, const uint8_t *packet,
                           bool pairing)
{
    esy_ble_event_t event = {0};
    event.connection = pairing ? sm_event_pairing_complete_get_handle(packet) :
        sm_event_reencryption_complete_get_handle(packet);
    esy_ble_btstack_link_t *link = find_link(ctx, event.connection);
    if (link == NULL) {
        return;
    }
    link->auth_request = 0u;
    uint8_t status = pairing ? sm_event_pairing_complete_get_status(packet) :
        sm_event_reencryption_complete_get_status(packet);
    event.type = pairing ? (status == 0u ? ESY_BLE_EVENT_PAIRING_COMPLETE :
                                         ESY_BLE_EVENT_PAIRING_FAILED) :
                           ESY_BLE_EVENT_SECURITY_CHANGED;
    event.reason = pairing ? sm_event_pairing_complete_get_reason(packet) : status;
    event.status = native_status(status);
    event.security_level = security_level(event.connection);
    if (pairing && status == 0u) {
        event.status = ctx->config.persistence.flush(ctx->config.persistence.user);
        event.bonded = event.status == ESY_OK && ctx->config.bondable &&
            sm_le_device_index(event.connection) >= 0;
    }
    emit_event(ctx, &event);
}

static void packet_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void)channel;
    ble_btstack_context_t *ctx = s_ctx.active;
    if (ctx == NULL || !ctx->ready || type != HCI_EVENT_PACKET || packet == NULL ||
        size < 2u || (size_t)packet[1] + 2u > size) {
        return;
    }
    size = (uint16_t)packet[1] + 2u;
    uint8_t event_type = hci_event_packet_get_type(packet);
    if (event_type == HCI_EVENT_META_GAP && size >= 36u &&
        packet[2] == GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
        connection_event(ctx, packet);
    } else if (event_type == HCI_EVENT_DISCONNECTION_COMPLETE && size >= 6u) {
        esy_ble_event_t event = {0};
        event.type = ESY_BLE_EVENT_DISCONNECTED;
        event.connection = hci_event_disconnection_complete_get_connection_handle(packet);
        esy_ble_btstack_link_t *link = find_link(ctx, event.connection);
        if (link == NULL) {
            return;
        }
        event.reason = hci_event_disconnection_complete_get_reason(packet);
        event.status = native_status(hci_event_disconnection_complete_get_status(packet));
        emit_event(ctx, &event);
        link->connection = ESY_BLE_CONNECTION_INVALID;
        link->auth_request = 0u;
        if (ctx->read_connection == event.connection) {
            ctx->read_valid = false;
        }
        for (size_t i = 0u; i < ctx->resources.subscription_capacity; ++i) {
            if (ctx->resources.subscriptions[i].connection == event.connection) {
                ctx->resources.subscriptions[i].connection = ESY_BLE_CONNECTION_INVALID;
                ctx->resources.subscriptions[i].value_handle = 0u;
            }
        }
    } else if (event_type == GAP_EVENT_ADVERTISING_REPORT && size >= 12u && ctx->scanning) {
        size_t data_length = gap_event_advertising_report_get_data_length(packet);
        if (data_length > size - 12u) {
            return;
        }
        esy_ble_event_t event = {0};
        event.type = ESY_BLE_EVENT_SCAN_RESULT;
        event.connection = ESY_BLE_CONNECTION_INVALID;
        bd_addr_t address;
        gap_event_advertising_report_get_address(packet, address);
        address_from_native(&event.peer, gap_event_advertising_report_get_address_type(packet), address);
        event.rssi = (int8_t)gap_event_advertising_report_get_rssi(packet);
        event.data = gap_event_advertising_report_get_data(packet);
        event.data_length = data_length;
        emit_event(ctx, &event);
    } else if (event_type == HCI_EVENT_ENCRYPTION_CHANGE && size >= 6u) {
        esy_ble_event_t event = {0};
        event.type = ESY_BLE_EVENT_SECURITY_CHANGED;
        event.connection = hci_event_encryption_change_get_connection_handle(packet);
        if (find_link(ctx, event.connection) == NULL) {
            return;
        }
        event.reason = hci_event_encryption_change_get_status(packet);
        event.status = native_status(event.reason);
        event.security_level = security_level(event.connection);
        emit_event(ctx, &event);
    } else if (event_type == SM_EVENT_PAIRING_COMPLETE && size >= 13u) {
        pairing_event(ctx, packet, true);
    } else if (event_type == SM_EVENT_REENCRYPTION_COMPLETE && size >= 12u) {
        pairing_event(ctx, packet, false);
    } else if ((event_type == SM_EVENT_JUST_WORKS_REQUEST ||
                event_type == SM_EVENT_PASSKEY_INPUT_NUMBER) && size >= 12u) {
        authentication_event(ctx, packet, event_type);
    } else if ((event_type == SM_EVENT_PASSKEY_DISPLAY_NUMBER ||
                event_type == SM_EVENT_NUMERIC_COMPARISON_REQUEST) && size >= 16u) {
        authentication_event(ctx, packet, event_type);
    } else if (event_type == SM_EVENT_PASSKEY_DISPLAY_CANCEL && size >= 11u) {
        authentication_event(ctx, packet, event_type);
    }
}

static bool idle(const ble_btstack_context_t *ctx)
{
    return !ctx->advertising && !ctx->scanning && !ctx->connecting && !has_links(ctx);
}

static esy_err_t require_idle(ble_btstack_context_t *ctx)
{
    return idle(ctx) ? ctx->config.host_quiesce(ctx->config.host_user) : ESY_ERR_BUSY;
}

static esy_err_t check_ready(ble_btstack_context_t *ctx)
{
    return ctx != NULL && ctx == s_ctx.active && ctx->ready && !ctx->stopping ?
        ESY_OK : ESY_ERR_STATE;
}

static esy_err_t provider_init(void *arg, esy_ble_event_fn event, void *user)
{
    ble_btstack_context_t *ctx = context(arg);
    if (ctx == NULL || event == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    if (ctx->claimed || s_ctx.active != NULL) {
        return ESY_ERR_STATE;
    }
    if (hci_get_state() != HCI_STATE_WORKING) {
        return ESY_ERR_STATE;
    }
    btstack_linked_list_iterator_t iterator;
    hci_connections_get_iterator(&iterator);
    if (btstack_linked_list_iterator_has_next(&iterator)) {
        return ESY_ERR_BUSY;
    }
    esy_err_t result = ctx->config.host_quiesce(ctx->config.host_user);
    if (result != ESY_OK) {
        return result;
    }
    s_ctx.active = ctx;
    ctx->claimed = true;
    ctx->event = event;
    ctx->event_user = user;
    result = ctx->config.persistence.restore(ctx->config.persistence.user);
    if (result != ESY_OK) {
        return result;
    }
    for (size_t i = 0u; i < ctx->resources.link_capacity; ++i) {
        ctx->resources.links[i].connection = ESY_BLE_CONNECTION_INVALID;
        ctx->resources.links[i].auth_request = 0u;
    }
    for (size_t i = 0u; i < ctx->resources.subscription_capacity; ++i) {
        ctx->resources.subscriptions[i].connection = ESY_BLE_CONNECTION_INVALID;
        ctx->resources.subscriptions[i].value_handle = 0u;
    }
    memset(ctx->resources.services, 0,
           ctx->resources.service_capacity * sizeof(*ctx->resources.services));
    memcpy(ctx->resources.attribute_database, k_empty_database, sizeof(k_empty_database));
    ctx->database_used = BLE_DB_HEADER_SIZE;
    ctx->next_handle = 1u;
    sm_set_io_capabilities((io_capability_t)ctx->config.io_capability);
    uint8_t auth = ctx->config.bondable ? SM_AUTHREQ_BONDING : SM_AUTHREQ_NO_BONDING;
    if (ctx->config.minimum_security >= BLE_SECURITY_AUTHENTICATED) {
        auth |= SM_AUTHREQ_MITM_PROTECTION;
    }
    auth |= SM_AUTHREQ_SECURE_CONNECTION;
    sm_set_authentication_requirements(auth);
    sm_set_secure_connections_only_mode(ctx->config.minimum_security == BLE_SECURITY_SECURE_CONNECTION);
    sm_set_encryption_key_size_range(ctx->config.minimum_security == BLE_SECURITY_SECURE_CONNECTION ?
        BLE_SECURITY_KEY_SIZE : BLE_SECURITY_KEY_SIZE_MIN, BLE_SECURITY_KEY_SIZE);
    att_server_init(ctx->resources.attribute_database, read_attribute, write_attribute);
    ctx->hci_callback.callback = packet_handler;
    ctx->sm_callback.callback = packet_handler;
    hci_add_event_handler(&ctx->hci_callback);
    sm_add_event_handler(&ctx->sm_callback);
    ctx->attached = true;
    ctx->ready = true;
    return ESY_OK;
}

static void clear_service(esy_ble_gatt_service_t *service)
{
    service->handle = 0u;
    for (size_t i = 0u; i < service->characteristic_count; ++i) {
        service->characteristics[i].value_handle = 0u;
    }
}

static esy_err_t provider_deinit(void *arg)
{
    esy_ble_btstack_t *token = arg;
    ble_btstack_context_t *ctx = context(arg);
    if (ctx == NULL) {
        return ESY_ERR_STATE;
    }
    if (!ctx->claimed) {
        memset(token, 0, sizeof(*token));
        return ESY_OK;
    }
    if (ctx != s_ctx.active) {
        return ESY_ERR_STATE;
    }
    if (ctx->attached) {
        if (!ctx->stopping) {
            ctx->stopping = true;
            gap_advertisements_enable(0);
            gap_stop_scan();
            ctx->advertising = false;
            ctx->scanning = false;
        }
        if (ctx->connecting) {
            esy_err_t cancel_result = native_status(gap_connect_cancel());
            if (cancel_result != ESY_OK && cancel_result != ESY_ERR_BUSY) {
                return cancel_result;
            }
        }
        for (size_t i = 0u; i < ctx->resources.link_capacity; ++i) {
            if (ctx->resources.links[i].connection != ESY_BLE_CONNECTION_INVALID) {
                (void)gap_disconnect(ctx->resources.links[i].connection);
            }
        }
        esy_err_t result = ctx->config.host_quiesce(ctx->config.host_user);
        if (result != ESY_OK) {
            return result;
        }
        if (has_links(ctx) || ctx->connecting) {
            return ESY_ERR_BUSY;
        }
    }
    esy_err_t result = ctx->config.persistence.flush(ctx->config.persistence.user);
    if (result != ESY_OK) {
        return result;
    }
    if (ctx->attached) {
        ctx->ready = false;
        hci_remove_event_handler(&ctx->hci_callback);
        sm_remove_event_handler(&ctx->sm_callback);
        att_server_deinit();
        att_set_db(k_empty_database);
        att_set_read_callback(NULL);
        att_set_write_callback(NULL);
        /* Native GAP retains these pointers beyond disable; detach our storage. */
        gap_advertisements_set_data(0u, (uint8_t *)(void *)k_empty_payload);
        gap_scan_response_set_data(0u, (uint8_t *)(void *)k_empty_payload);
    }
    for (size_t i = 0u; i < ctx->service_count; ++i) {
        clear_service(ctx->resources.services[i]);
        ctx->resources.services[i] = NULL;
    }
    s_ctx.active = NULL;
    memset(token, 0, sizeof(*token));
    return ESY_OK;
}

static esy_err_t advertising_start(void *arg, const esy_ble_adv_params_t *params)
{
    ble_btstack_context_t *ctx = context(arg);
    esy_err_t result = check_ready(ctx);
    if (result != ESY_OK) { return result; }
    if (ctx->advertising) { return ESY_ERR_BUSY; }
    if (params->data_length != 0u) {
        memcpy(ctx->advertising_data, params->data, params->data_length);
    }
    if (params->scan_response_length != 0u) {
        memcpy(ctx->scan_response, params->scan_response, params->scan_response_length);
    }
    bd_addr_t direct_address = {0};
    uint8_t type = params->connectable ? 0u :
        (params->scan_response_length != 0u ? BLE_ADV_SCANNABLE : BLE_ADV_NONCONNECTABLE);
    gap_advertisements_set_params(params->interval_min, params->interval_max,
                                  type, 0u, direct_address, BLE_ADV_CHANNEL_MAP, 0u);
    gap_advertisements_set_data((uint8_t)params->data_length, ctx->advertising_data);
    gap_scan_response_set_data((uint8_t)params->scan_response_length, ctx->scan_response);
    ctx->advertising = true;
    gap_advertisements_enable(1);
    return ESY_OK;
}

static esy_err_t advertising_stop(void *arg)
{
    ble_btstack_context_t *ctx = context(arg);
    esy_err_t result = check_ready(ctx);
    if (result != ESY_OK) { return result; }
    gap_advertisements_enable(0);
    ctx->advertising = false;
    return ESY_OK;
}

static esy_err_t scanning_start(void *arg, const esy_ble_scan_params_t *params)
{
    ble_btstack_context_t *ctx = context(arg);
    esy_err_t result = check_ready(ctx);
    if (result != ESY_OK) { return result; }
    if (ctx->scanning) { return ESY_ERR_BUSY; }
    gap_set_scan_params(params->active ? 1u : 0u, params->interval, params->window, 0u);
    gap_set_scan_duplicate_filter(params->filter_duplicates);
    ctx->scanning = true;
    gap_start_scan();
    return ESY_OK;
}

static esy_err_t scanning_stop(void *arg)
{
    ble_btstack_context_t *ctx = context(arg);
    esy_err_t result = check_ready(ctx);
    if (result != ESY_OK) { return result; }
    gap_stop_scan();
    ctx->scanning = false;
    return ESY_OK;
}

static esy_err_t connecting_start(void *arg, const esy_ble_connect_params_t *params)
{
    ble_btstack_context_t *ctx = context(arg);
    esy_err_t result = check_ready(ctx);
    if (result != ESY_OK) { return result; }
    if (ctx->connecting) { return ESY_ERR_BUSY; }
    bd_addr_t address;
    address_to_native(address, &params->peer);
    gap_set_connection_parameters(BLE_SCAN_INTERVAL, BLE_SCAN_WINDOW, params->interval_min,
        params->interval_max, params->latency, params->supervision_timeout, 0u, 0u);
    ctx->connecting = true;
    result = native_status(gap_connect(address, (bd_addr_type_t)params->peer.type));
    if (result != ESY_OK) { ctx->connecting = false; }
    return result;
}

static esy_err_t disconnecting(void *arg, esy_ble_connection_t connection, uint8_t reason)
{
    ble_btstack_context_t *ctx = context(arg);
    esy_err_t result = check_ready(ctx);
    if (result != ESY_OK) { return result; }
    if (!live_link(ctx, connection)) { return ESY_ERR_STATE; }
    if (reason != ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION) { return ESY_ERR_NOT_SUPPORTED; }
    return native_status(gap_disconnect(connection));
}

static esy_err_t request_security(void *arg, esy_ble_connection_t connection, uint8_t level)
{
    ble_btstack_context_t *ctx = context(arg);
    esy_err_t result = check_ready(ctx);
    if (result != ESY_OK) { return result; }
    if (!live_link(ctx, connection)) { return ESY_ERR_STATE; }
    if (security_level(connection) >= level) { return ESY_OK; }
    if (level > ctx->config.minimum_security) { return ESY_ERR_NOT_SUPPORTED; }
    sm_request_pairing(connection);
    return ESY_OK;
}

static esy_err_t visit_bonds(void *arg, esy_ble_bond_fn visitor, void *user)
{
    ble_btstack_context_t *ctx = context(arg);
    esy_err_t result = check_ready(ctx);
    if (result != ESY_OK) { return result; }
    for (int i = 0; i < le_device_db_max_count(); ++i) {
        int type;
        bd_addr_t address;
        le_device_db_info(i, &type, address, NULL);
        if (type == BD_ADDR_TYPE_LE_PUBLIC || type == BD_ADDR_TYPE_LE_RANDOM) {
            esy_ble_address_t peer;
            address_from_native(&peer, (uint8_t)type, address);
            visitor(user, &peer);
        }
    }
    return ESY_OK;
}

static esy_err_t erase_bonds(void *arg, const esy_ble_address_t *peer)
{
    ble_btstack_context_t *ctx = context(arg);
    esy_err_t result = check_ready(ctx);
    if (result != ESY_OK) { return result; }
    result = require_idle(ctx);
    if (result != ESY_OK) { return result; }
    bd_addr_t target = {0};
    if (peer != NULL) { address_to_native(target, peer); }
    for (int i = 0; i < le_device_db_max_count(); ++i) {
        int type;
        bd_addr_t address;
        le_device_db_info(i, &type, address, NULL);
        if (type == BD_ADDR_TYPE_LE_PUBLIC || type == BD_ADDR_TYPE_LE_RANDOM) {
            if (peer == NULL || (type == peer->type && memcmp(address, target, sizeof(target)) == 0)) {
                gap_delete_bonding((bd_addr_type_t)type, address);
            }
        }
    }
    return ctx->config.persistence.flush(ctx->config.persistence.user);
}

static esy_err_t send_notification(void *arg, esy_ble_connection_t connection,
                                   uint16_t handle, const void *data, size_t length)
{
    ble_btstack_context_t *ctx = context(arg);
    esy_err_t result = check_ready(ctx);
    if (result != ESY_OK) { return result; }
    if (!live_link(ctx, connection)) { return ESY_ERR_STATE; }
    bool ccc;
    esy_ble_gatt_characteristic_t *ch = find_characteristic(ctx, handle, &ccc);
    if (ch == NULL || ccc || (ch->properties & ESY_BLE_GATT_NOTIFY) == 0u) { return ESY_ERR_INVALID_ARG; }
    if (find_subscription(ctx, connection, handle) == NULL ||
        security_error(connection, ch->notify_security) != ESY_BLE_ATT_SUCCESS) { return ESY_ERR_STATE; }
    uint16_t mtu = att_server_get_mtu(connection);
    if (mtu < BLE_ATT_NOTIFICATION_HEADER_SIZE) { return ESY_ERR_STATE; }
    if (length > mtu - BLE_ATT_NOTIFICATION_HEADER_SIZE) { return ESY_ERR_INVALID_ARG; }
    return native_status(att_server_notify(connection, handle,
        data != NULL ? data : k_empty_payload, (uint16_t)length));
}

static void store16(uint8_t *buffer, size_t offset, uint16_t value)
{
    buffer[offset] = (uint8_t)value;
    buffer[offset + 1u] = (uint8_t)(value >> 8u);
}

static uint16_t attribute_flags(uint8_t properties, uint8_t read_security,
                                uint8_t write_security)
{
    uint16_t flags = (properties & (ESY_BLE_GATT_READ | ESY_BLE_GATT_WRITE | ESY_BLE_GATT_WRITE_COMMAND)) |
        ATT_PROPERTY_DYNAMIC;
    if (read_security > 1u) {
        uint8_t permission = read_security == BLE_SECURITY_SECURE_CONNECTION ? 2u : read_security - 1u;
        if ((permission & 1u) != 0u) { flags |= ATT_PROPERTY_READ_PERMISSION_BIT_0; }
        if ((permission & 2u) != 0u) { flags |= ATT_PROPERTY_READ_PERMISSION_BIT_1; }
    }
    if (write_security > 1u) {
        uint8_t permission = write_security == BLE_SECURITY_SECURE_CONNECTION ? 2u : write_security - 1u;
        if ((permission & 1u) != 0u) { flags |= ATT_PROPERTY_WRITE_PERMISSION_BIT_0; }
        if ((permission & 2u) != 0u) { flags |= ATT_PROPERTY_WRITE_PERMISSION_BIT_1; }
    }
    if (read_security == BLE_SECURITY_SECURE_CONNECTION) { flags |= ATT_PROPERTY_READ_PERMISSION_SC | 0xf000u; }
    if (write_security == BLE_SECURITY_SECURE_CONNECTION) { flags |= ATT_PROPERTY_WRITE_PERMISSION_SC | 0xf000u; }
    return flags;
}

static void append_attribute(ble_btstack_context_t *ctx, uint16_t flags, uint16_t handle,
                             const uint8_t *uuid, size_t uuid_size,
                             const uint8_t *value, size_t value_size)
{
    uint8_t *buffer = ctx->resources.attribute_database + ctx->database_used;
    size_t size = BLE_DB_ATTRIBUTE_HEADER_SIZE + uuid_size + value_size;
    store16(buffer, 0u, (uint16_t)size);
    store16(buffer, 2u, flags | (uuid_size == ESY_BLE_UUID_MAX ? ATT_PROPERTY_UUID128 : 0u));
    store16(buffer, 4u, handle);
    memcpy(buffer + BLE_DB_ATTRIBUTE_HEADER_SIZE, uuid, uuid_size);
    if (value_size != 0u) {
        memcpy(buffer + BLE_DB_ATTRIBUTE_HEADER_SIZE + uuid_size, value, value_size);
    }
    ctx->database_used += size;
    ctx->resources.attribute_database[ctx->database_used] = 0u;
    ctx->resources.attribute_database[ctx->database_used + 1u] = 0u;
}

static esy_err_t register_service(void *arg, esy_ble_gatt_service_t *service)
{
    ble_btstack_context_t *ctx = context(arg);
    esy_err_t result = check_ready(ctx);
    if (result != ESY_OK) { return result; }
    result = require_idle(ctx);
    if (result != ESY_OK) { return result; }
    if (ctx->service_count == ctx->resources.service_capacity) { return ESY_ERR_NO_MEM; }
    size_t bytes = BLE_DB_ATTRIBUTE_HEADER_SIZE + BLE_DB_UUID16_SIZE + service->uuid.length;
    uint32_t handles = 1u;
    for (size_t i = 0u; i < service->characteristic_count; ++i) {
        esy_ble_gatt_characteristic_t *ch = &service->characteristics[i];
        if ((ch->properties & (ESY_BLE_GATT_WRITE | ESY_BLE_GATT_WRITE_COMMAND)) ==
            (ESY_BLE_GATT_WRITE | ESY_BLE_GATT_WRITE_COMMAND)) { return ESY_ERR_NOT_SUPPORTED; }
        if (((ch->properties & ESY_BLE_GATT_READ) != 0u && ch->read_security > ctx->config.minimum_security) ||
            ((ch->properties & (ESY_BLE_GATT_WRITE | ESY_BLE_GATT_WRITE_COMMAND)) != 0u && ch->write_security > ctx->config.minimum_security) ||
            ((ch->properties & ESY_BLE_GATT_NOTIFY) != 0u && ch->notify_security > ctx->config.minimum_security)) {
            return ESY_ERR_NOT_SUPPORTED;
        }
        bytes += 2u * BLE_DB_ATTRIBUTE_HEADER_SIZE + BLE_DB_UUID16_SIZE +
            BLE_DB_DECLARATION_SIZE + 2u * ch->uuid.length;
        handles += 2u;
        if ((ch->properties & ESY_BLE_GATT_NOTIFY) != 0u) {
            bytes += BLE_DB_ATTRIBUTE_HEADER_SIZE + BLE_DB_UUID16_SIZE + BLE_CCC_SIZE;
            ++handles;
        }
    }
    if (bytes > ctx->resources.attribute_database_size - ctx->database_used - BLE_DB_END_SIZE ||
        ctx->next_handle + handles - 1u > UINT16_MAX) { return ESY_ERR_NO_MEM; }
    uint8_t uuid16[BLE_DB_UUID16_SIZE];
    store16(uuid16, 0u, service->primary ? GATT_PRIMARY_SERVICE_UUID : GATT_SECONDARY_SERVICE_UUID);
    service->handle = (uint16_t)ctx->next_handle++;
    append_attribute(ctx, ATT_PROPERTY_READ, service->handle, uuid16, sizeof(uuid16),
                      service->uuid.bytes, service->uuid.length);
    for (size_t i = 0u; i < service->characteristic_count; ++i) {
        esy_ble_gatt_characteristic_t *ch = &service->characteristics[i];
        uint8_t declaration[BLE_DB_DECLARATION_SIZE + ESY_BLE_UUID_MAX];
        declaration[0] = ch->properties;
        store16(declaration, 1u, (uint16_t)(ctx->next_handle + 1u));
        memcpy(declaration + BLE_DB_DECLARATION_SIZE, ch->uuid.bytes, ch->uuid.length);
        store16(uuid16, 0u, GATT_CHARACTERISTICS_UUID);
        append_attribute(ctx, ATT_PROPERTY_READ, (uint16_t)ctx->next_handle++, uuid16,
                          sizeof(uuid16), declaration, BLE_DB_DECLARATION_SIZE + ch->uuid.length);
        ch->value_handle = (uint16_t)ctx->next_handle++;
        append_attribute(ctx, attribute_flags(ch->properties, ch->read_security, ch->write_security),
                          ch->value_handle, ch->uuid.bytes, ch->uuid.length, NULL, 0u);
        if ((ch->properties & ESY_BLE_GATT_NOTIFY) != 0u) {
            uint8_t value[BLE_CCC_SIZE] = {0u, 0u};
            store16(uuid16, 0u, GATT_CLIENT_CHARACTERISTICS_CONFIGURATION);
            append_attribute(ctx, attribute_flags(ESY_BLE_GATT_READ | ESY_BLE_GATT_WRITE,
                              1u, ch->notify_security), (uint16_t)ctx->next_handle++,
                              uuid16, sizeof(uuid16), value, sizeof(value));
        }
    }
    ctx->resources.services[ctx->service_count++] = service;
    return ESY_OK;
}

static esy_err_t unregister_service(void *arg, esy_ble_gatt_service_t *service)
{
    ble_btstack_context_t *ctx = context(arg);
    esy_err_t result = check_ready(ctx);
    if (result != ESY_OK) { return result; }
    result = require_idle(ctx);
    if (result != ESY_OK) { return result; }
    if (ctx->service_count == 0u) { return ESY_ERR_STATE; }
    if (ctx->resources.services[ctx->service_count - 1u] != service) {
        for (size_t i = 0u; i < ctx->service_count; ++i) {
            if (ctx->resources.services[i] == service) { return ESY_ERR_BUSY; }
        }
        return ESY_ERR_STATE;
    }
    size_t offset = BLE_DB_HEADER_SIZE;
    while (offset < ctx->database_used) {
        if (little_endian_read_16(ctx->resources.attribute_database, (int)offset + 4) == service->handle) {
            break;
        }
        offset += little_endian_read_16(ctx->resources.attribute_database, (int)offset);
    }
    ctx->database_used = offset;
    ctx->resources.attribute_database[offset] = 0u;
    ctx->resources.attribute_database[offset + 1u] = 0u;
    ctx->resources.services[--ctx->service_count] = NULL;
    ctx->next_handle = service->handle;
    ctx->read_valid = false;
    clear_service(service);
    return ESY_OK;
}

static esy_err_t authentication_reply(void *arg, esy_ble_connection_t connection,
                                      esy_ble_auth_reply_t reply, uint32_t passkey)
{
    ble_btstack_context_t *ctx = context(arg);
    esy_err_t result = check_ready(ctx);
    if (result != ESY_OK) { return result; }
    if (!live_link(ctx, connection)) { return ESY_ERR_STATE; }
    esy_ble_btstack_link_t *link = find_link(ctx, connection);
    if (link->auth_request == 0u) { return ESY_ERR_STATE; }
    switch (reply) {
    case ESY_BLE_AUTH_PASSKEY_ENTRY:
        if (link->auth_request != ESY_BLE_EVENT_PASSKEY_ENTRY + 1u || passkey > BLE_PASSKEY_MAX) { return ESY_ERR_STATE; }
        link->auth_request = 0u;
        sm_passkey_input(connection, passkey);
        break;
    case ESY_BLE_AUTH_PASSKEY_CONFIRM:
        if (link->auth_request != ESY_BLE_EVENT_PASSKEY_CONFIRM + 1u) { return ESY_ERR_STATE; }
        link->auth_request = 0u;
        sm_numeric_comparison_confirm(connection);
        break;
    case ESY_BLE_AUTH_PAIRING_CONFIRM:
        if (link->auth_request != ESY_BLE_EVENT_PAIRING_CONFIRM + 1u) { return ESY_ERR_STATE; }
        link->auth_request = 0u;
        sm_just_works_confirm(connection);
        break;
    case ESY_BLE_AUTH_CANCEL:
        link->auth_request = 0u;
        sm_bonding_decline(connection);
        break;
    default: return ESY_ERR_INVALID_ARG;
    }
    return ESY_OK;
}

static const esy_ble_ops_t k_ops = {
    .init = provider_init, .deinit = provider_deinit,
    .adv_start = advertising_start, .adv_stop = advertising_stop,
    .scan_start = scanning_start, .scan_stop = scanning_stop,
    .connect = connecting_start, .disconnect = disconnecting,
    .security = request_security, .bonds_foreach = visit_bonds, .unpair = erase_bonds,
    .notify = send_notification, .gatt_register = register_service,
    .gatt_unregister = unregister_service, .auth_reply = authentication_reply
};

esy_err_t esy_ble_btstack_provider(esy_ble_btstack_t *adapter,
    const esy_ble_btstack_config_t *config, esy_ble_provider_t *provider)
{
    if (adapter == NULL || config == NULL || provider == NULL ||
        config->static_resources == NULL || config->persistence.restore == NULL ||
        config->persistence.flush == NULL || config->host_quiesce == NULL ||
        config->io_capability > 4u || config->minimum_security < BLE_SECURITY_ENCRYPTED ||
        config->minimum_security > BLE_SECURITY_SECURE_CONNECTION) { return ESY_ERR_INVALID_ARG; }
    if (adapter->owner != NULL) { return ESY_ERR_STATE; }
    const esy_ble_btstack_static_resources_t *resources = config->static_resources;
    if (resources->attribute_database == NULL || resources->attribute_database_size < sizeof(k_empty_database) ||
        resources->attribute_database_size > UINT16_MAX || resources->services == NULL ||
        resources->service_capacity == 0u || resources->service_capacity > UINT16_MAX ||
        resources->links == NULL || resources->link_capacity < MAX_NR_HCI_CONNECTIONS ||
        resources->link_capacity > UINT16_MAX ||
        resources->subscriptions == NULL || resources->subscription_capacity == 0u ||
        resources->subscription_capacity > UINT16_MAX || resources->read_buffer == NULL ||
        resources->read_buffer_size == 0u || resources->read_buffer_size > UINT16_MAX) { return ESY_ERR_INVALID_ARG; }
    memset(adapter, 0, sizeof(*adapter));
    adapter->owner = adapter;
    ble_btstack_context_t *ctx = context(adapter);
    ctx->config = *config;
    ctx->resources = *resources;
    ctx->config.static_resources = &ctx->resources;
    provider->ops = &k_ops;
    provider->ctx = adapter;
    return ESY_OK;
}
