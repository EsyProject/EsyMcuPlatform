/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/ble/esy_ble.h"
#include <string.h>

#define BLE_ADV_INTERVAL_MIN 0x0020u
#define BLE_INTERVAL_MAX 0x4000u
#define BLE_SCAN_INTERVAL_MIN 0x0004u
#define BLE_CONN_INTERVAL_MIN 0x0006u
#define BLE_CONN_INTERVAL_MAX 0x0c80u
#define BLE_CONN_LATENCY_MAX 499u
#define BLE_SUPERVISION_MIN 0x000au
#define BLE_SUPERVISION_MAX 0x0c80u
#define BLE_SUPERVISION_RATIO 4u
#define BLE_SECURITY_MIN 1u
#define BLE_SECURITY_MAX 4u
#define BLE_UUID16_LENGTH 2u
#define BLE_UUID128_LENGTH 16u
#define BLE_GATT_PROPERTIES 0x1eu
#define BLE_PASSKEY_MAX 999999u

typedef enum {
    BLE_DETACHED = 0,
    BLE_ATTACHED,
    BLE_READY
} ble_state_t;

static esy_err_t check_ready(const esy_ble_t *ble)
{
    if (ble == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    return ble->state == BLE_READY ? ESY_OK : ESY_ERR_STATE;
}

static bool valid_address(const esy_ble_address_t *address)
{
    return address->type <= ESY_BLE_ADDRESS_RANDOM;
}

static bool valid_uuid(const esy_ble_uuid_t *uuid)
{
    return uuid->length == BLE_UUID16_LENGTH || uuid->length == BLE_UUID128_LENGTH;
}

static bool valid_ad(const uint8_t *data, size_t length)
{
    if (length > ESY_BLE_LEGACY_DATA_MAX || (data == NULL && length != 0u)) {
        return false;
    }
    size_t offset = 0u;
    while (offset < length) {
        size_t field_length = data[offset++];
        if (field_length > length - offset) {
            return false;
        }
        if (field_length == 0u) {
            /* A zero-length terminator may only be followed by padding. */
            while (offset < length) {
                if (data[offset++] != 0u) {
                    return false;
                }
            }
            break;
        }
        offset += field_length;
    }
    return true;
}

esy_err_t esy_ble_init(esy_ble_t *ble, const esy_ble_provider_t *provider,
                       esy_ble_event_fn event, void *user)
{
    if (ble == NULL || provider == NULL || provider->ops == NULL ||
        provider->ops->init == NULL || provider->ops->deinit == NULL || event == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    if (ble->state != BLE_DETACHED) {
        return ESY_ERR_STATE;
    }
    ble->provider = *provider;
    ble->state = BLE_ATTACHED;
    esy_err_t result = ble->provider.ops->init(ble->provider.ctx, event, user);
    if (result == ESY_OK) {
        ble->state = BLE_READY;
    }
    return result;
}

esy_err_t esy_ble_deinit(esy_ble_t *ble)
{
    if (ble == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    if (ble->state == BLE_DETACHED) {
        return ESY_OK;
    }
    /* Partial provider teardown makes further control operations unsafe. */
    ble->state = BLE_ATTACHED;
    esy_err_t result = ble->provider.ops->deinit(ble->provider.ctx);
    if (result == ESY_OK) {
        memset(ble, 0, sizeof(*ble));
    }
    return result;
}

esy_err_t esy_ble_adv_start(esy_ble_t *ble, const esy_ble_adv_params_t *params)
{
    esy_err_t result = check_ready(ble);
    if (result != ESY_OK) {
        return result;
    }
    if (params == NULL || params->interval_min < BLE_ADV_INTERVAL_MIN ||
        params->interval_max < params->interval_min || params->interval_max > BLE_INTERVAL_MAX ||
        !valid_ad(params->data, params->data_length) ||
        !valid_ad(params->scan_response, params->scan_response_length)) {
        return ESY_ERR_INVALID_ARG;
    }
    return ble->provider.ops->adv_start != NULL ?
        ble->provider.ops->adv_start(ble->provider.ctx, params) : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_ble_adv_stop(esy_ble_t *ble)
{
    esy_err_t result = check_ready(ble);
    if (result != ESY_OK) {
        return result;
    }
    return ble->provider.ops->adv_stop != NULL ?
        ble->provider.ops->adv_stop(ble->provider.ctx) : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_ble_scan_start(esy_ble_t *ble, const esy_ble_scan_params_t *params)
{
    esy_err_t result = check_ready(ble);
    if (result != ESY_OK) {
        return result;
    }
    if (params == NULL || params->interval < BLE_SCAN_INTERVAL_MIN ||
        params->interval > BLE_INTERVAL_MAX || params->window < BLE_SCAN_INTERVAL_MIN ||
        params->window > params->interval) {
        return ESY_ERR_INVALID_ARG;
    }
    return ble->provider.ops->scan_start != NULL ?
        ble->provider.ops->scan_start(ble->provider.ctx, params) : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_ble_scan_stop(esy_ble_t *ble)
{
    esy_err_t result = check_ready(ble);
    if (result != ESY_OK) {
        return result;
    }
    return ble->provider.ops->scan_stop != NULL ?
        ble->provider.ops->scan_stop(ble->provider.ctx) : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_ble_connect(esy_ble_t *ble, const esy_ble_connect_params_t *params)
{
    esy_err_t result = check_ready(ble);
    if (result != ESY_OK) {
        return result;
    }
    if (params == NULL || !valid_address(&params->peer) ||
        params->interval_min < BLE_CONN_INTERVAL_MIN ||
        params->interval_max < params->interval_min ||
        params->interval_max > BLE_CONN_INTERVAL_MAX || params->latency > BLE_CONN_LATENCY_MAX ||
        params->supervision_timeout < BLE_SUPERVISION_MIN ||
        params->supervision_timeout > BLE_SUPERVISION_MAX ||
        (uint32_t)params->supervision_timeout * BLE_SUPERVISION_RATIO <=
            (uint32_t)(params->latency + 1u) * params->interval_max) {
        return ESY_ERR_INVALID_ARG;
    }
    return ble->provider.ops->connect != NULL ?
        ble->provider.ops->connect(ble->provider.ctx, params) : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_ble_disconnect(esy_ble_t *ble, esy_ble_connection_t connection, uint8_t reason)
{
    esy_err_t result = check_ready(ble);
    if (result != ESY_OK) {
        return result;
    }
    if (connection == ESY_BLE_CONNECTION_INVALID) {
        return ESY_ERR_INVALID_ARG;
    }
    return ble->provider.ops->disconnect != NULL ?
        ble->provider.ops->disconnect(ble->provider.ctx, connection, reason) : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_ble_security(esy_ble_t *ble, esy_ble_connection_t connection, uint8_t level)
{
    esy_err_t result = check_ready(ble);
    if (result != ESY_OK) {
        return result;
    }
    if (connection == ESY_BLE_CONNECTION_INVALID || level < BLE_SECURITY_MIN || level > BLE_SECURITY_MAX) {
        return ESY_ERR_INVALID_ARG;
    }
    return ble->provider.ops->security != NULL ?
        ble->provider.ops->security(ble->provider.ctx, connection, level) : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_ble_bonds_foreach(esy_ble_t *ble, esy_ble_bond_fn visitor, void *user)
{
    esy_err_t result = check_ready(ble);
    if (result != ESY_OK) {
        return result;
    }
    if (visitor == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    return ble->provider.ops->bonds_foreach != NULL ?
        ble->provider.ops->bonds_foreach(ble->provider.ctx, visitor, user) : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_ble_unpair(esy_ble_t *ble, const esy_ble_address_t *peer)
{
    esy_err_t result = check_ready(ble);
    if (result != ESY_OK) {
        return result;
    }
    if (peer != NULL && !valid_address(peer)) {
        return ESY_ERR_INVALID_ARG;
    }
    return ble->provider.ops->unpair != NULL ?
        ble->provider.ops->unpair(ble->provider.ctx, peer) : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_ble_notify(esy_ble_t *ble, esy_ble_connection_t connection,
                         uint16_t value_handle, const void *data, size_t length)
{
    esy_err_t result = check_ready(ble);
    if (result != ESY_OK) {
        return result;
    }
    if (connection == ESY_BLE_CONNECTION_INVALID || value_handle == 0u ||
        length > UINT16_MAX || (data == NULL && length != 0u)) {
        return ESY_ERR_INVALID_ARG;
    }
    return ble->provider.ops->notify != NULL ?
        ble->provider.ops->notify(ble->provider.ctx, connection, value_handle, data, length) : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_ble_gatt_register(esy_ble_t *ble, esy_ble_gatt_service_t *service)
{
    esy_err_t result = check_ready(ble);
    if (result != ESY_OK) {
        return result;
    }
    if (service == NULL || !valid_uuid(&service->uuid) || service->characteristics == NULL ||
        service->characteristic_count == 0u || service->characteristic_count > UINT16_MAX) {
        return ESY_ERR_INVALID_ARG;
    }
    if (service->handle != 0u) {
        return ESY_ERR_STATE;
    }
    for (size_t i = 0u; i < service->characteristic_count; ++i) {
        const esy_ble_gatt_characteristic_t *characteristic = &service->characteristics[i];
        if (!valid_uuid(&characteristic->uuid) || characteristic->properties == 0u ||
            (characteristic->properties & ~BLE_GATT_PROPERTIES) != 0u ||
            ((characteristic->properties & ESY_BLE_GATT_READ) != 0u &&
                (characteristic->read == NULL || characteristic->read_security < BLE_SECURITY_MIN ||
                 characteristic->read_security > BLE_SECURITY_MAX)) ||
            ((characteristic->properties & (ESY_BLE_GATT_WRITE | ESY_BLE_GATT_WRITE_COMMAND)) != 0u &&
                (characteristic->write == NULL || characteristic->write_security < BLE_SECURITY_MIN ||
                 characteristic->write_security > BLE_SECURITY_MAX)) ||
            ((characteristic->properties & ESY_BLE_GATT_NOTIFY) != 0u &&
                (characteristic->notify_security < BLE_SECURITY_MIN ||
                 characteristic->notify_security > BLE_SECURITY_MAX))) {
            return ESY_ERR_INVALID_ARG;
        }
        if (characteristic->value_handle != 0u) {
            return ESY_ERR_STATE;
        }
    }
    return ble->provider.ops->gatt_register != NULL ?
        ble->provider.ops->gatt_register(ble->provider.ctx, service) : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_ble_gatt_unregister(esy_ble_t *ble, esy_ble_gatt_service_t *service)
{
    esy_err_t result = check_ready(ble);
    if (result != ESY_OK) {
        return result;
    }
    if (service == NULL || service->characteristics == NULL ||
        service->characteristic_count == 0u || service->characteristic_count > UINT16_MAX) {
        return ESY_ERR_INVALID_ARG;
    }
    if (service->handle == 0u) {
        return ESY_ERR_STATE;
    }
    if (ble->provider.ops->gatt_unregister == NULL) {
        return ESY_ERR_NOT_SUPPORTED;
    }
    result = ble->provider.ops->gatt_unregister(ble->provider.ctx, service);
    if (result == ESY_OK) {
        service->handle = 0u;
        for (size_t i = 0u; i < service->characteristic_count; ++i) {
            service->characteristics[i].value_handle = 0u;
        }
    }
    return result;
}

esy_err_t esy_ble_auth_reply(esy_ble_t *ble, esy_ble_connection_t connection,
                            esy_ble_auth_reply_t reply, uint32_t passkey)
{
    esy_err_t result = check_ready(ble);
    if (result != ESY_OK) {
        return result;
    }
    if (connection == ESY_BLE_CONNECTION_INVALID || (unsigned)reply > (unsigned)ESY_BLE_AUTH_CANCEL ||
        (reply == ESY_BLE_AUTH_PASSKEY_ENTRY && passkey > BLE_PASSKEY_MAX)) {
        return ESY_ERR_INVALID_ARG;
    }
    return ble->provider.ops->auth_reply != NULL ?
        ble->provider.ops->auth_reply(ble->provider.ctx, connection, reply, passkey) : ESY_ERR_NOT_SUPPORTED;
}
