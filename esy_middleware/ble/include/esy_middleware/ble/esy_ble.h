/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_MIDDLEWARE_BLE_H
#define ESY_MIDDLEWARE_BLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_ble.h
 * @brief Provider-independent Bluetooth LE GAP, GATT server and security control.
 *
 * A single non-ISR owner serializes all calls and lifecycle operations. No task,
 * timer, allocator, lock or controller driver is created by this core. Providers
 * schedule their own stack and deliver events in their documented contexts;
 * callbacks must be short, nonblocking, and must not call core control/lifecycle
 * APIs. Hand events to the owner using consumer-owned synchronization.
 *
 * A successful control return means the provider accepted the operation, not
 * that a connection, pairing or radio procedure completed. Provider errors pass
 * through unchanged. Unsupported optional operations return NOT_SUPPORTED.
 * Provider timing, native resource allocation and blocking apply to every call.
 * Control APIs first reject NULL handles with INVALID_ARG and detached/failed
 * init/stopping handles with STATE, then validate operation arguments (INVALID_ARG).
 */

/** Maximum payload size for each legacy advertising or scan-response packet. */
#define ESY_BLE_LEGACY_DATA_MAX 31u
/** Address size in bytes, least significant octet first. */
#define ESY_BLE_ADDRESS_SIZE 6u
/** Maximum UUID width; only 2-byte and 16-byte UUIDs are accepted. */
#define ESY_BLE_UUID_MAX 16u
/** Sentinel used for events that have no live connection. */
#define ESY_BLE_CONNECTION_INVALID UINT16_MAX

/** LE address type; identity addresses use their underlying public/random type. */
typedef enum {
    ESY_BLE_ADDRESS_PUBLIC = 0, /**< Public device address. */
    ESY_BLE_ADDRESS_RANDOM = 1  /**< Random device address. */
} esy_ble_address_type_t;

/** Peer address; bytes use Bluetooth little-endian order, not display order. */
typedef struct {
    uint8_t type; /**< An esy_ble_address_type_t value. */
    uint8_t bytes[ESY_BLE_ADDRESS_SIZE]; /**< Address octets. */
} esy_ble_address_t;

/**
 * Provider connection identifier, valid only from CONNECTED until DISCONNECTED.
 * IDs can be reused after disconnect; consumers must discard stale requests and
 * must not use them as persistent peer identities. No native pointer is exposed.
 */
typedef uint16_t esy_ble_connection_t;

/** Bluetooth UUID encoded least significant octet first, including 128-bit UUIDs. */
typedef struct {
    uint8_t length; /**< Exactly 2 or 16; unused bytes are ignored. */
    uint8_t bytes[ESY_BLE_UUID_MAX]; /**< UUID octets. */
} esy_ble_uuid_t;

/** Supported characteristic properties using Bluetooth-assigned bit values. */
typedef enum {
    ESY_BLE_GATT_READ = 0x02, /**< Read requests accepted. */
    ESY_BLE_GATT_WRITE_COMMAND = 0x04, /**< Write without response accepted. */
    ESY_BLE_GATT_WRITE = 0x08, /**< Write requests accepted. */
    ESY_BLE_GATT_NOTIFY = 0x10 /**< Notifications with provider-managed CCC. */
} esy_ble_gatt_property_t;

/** Common ATT callback results; other Bluetooth-assigned ATT errors are allowed. */
typedef enum {
    ESY_BLE_ATT_SUCCESS = 0x00, /**< Request completed. */
    ESY_BLE_ATT_READ_NOT_PERMITTED = 0x02, /**< Attribute does not allow reads. */
    ESY_BLE_ATT_WRITE_NOT_PERMITTED = 0x03, /**< Attribute does not allow writes. */
    ESY_BLE_ATT_INVALID_OFFSET = 0x07, /**< Offset outside the value. */
    ESY_BLE_ATT_INVALID_LENGTH = 0x0d, /**< Value length invalid. */
    ESY_BLE_ATT_UNLIKELY_ERROR = 0x0e /**< Application could not complete request. */
} esy_ble_att_status_t;

/**
 * Read a value slice at offset. Provider supplies buffer/capacity and length
 * output; on success callback sets *length <= capacity, including zero at EOF.
 * A NULL buffer with capacity=0 and offset=0 instead queries total value length;
 * set *length <= UINT16_MAX in that case without copying any bytes. The length
 * output is always non-NULL. Other NULL buffer combinations are invalid.
 * Return an ATT status, not esy_err_t. Provider-native non-ISR callback context;
 * no core control/lifecycle calls or blocking. Borrowed buffers expire on return.
 */
typedef uint8_t (*esy_ble_gatt_read_fn)(void *user, esy_ble_connection_t connection,
    uint16_t offset, void *buffer, size_t capacity, size_t *length);
/**
 * Write one value slice; command means no ATT response. Callback returns ATT
 * status; commands cannot report failure to peer. Prepared/reliable writes are
 * outside this contract. Same context/lifetime constraints as the read callback.
 */
typedef uint8_t (*esy_ble_gatt_write_fn)(void *user, esy_ble_connection_t connection,
    uint16_t offset, const void *data, size_t length, bool command);

/** Caller-owned characteristic definition, borrowed until unregister/deinit. */
typedef struct {
    esy_ble_uuid_t uuid; /**< Characteristic UUID. */
    uint8_t properties; /**< Nonzero combination of supported property bits. */
    uint8_t read_security; /**< Minimum 1..4 security level when READ is set. */
    uint8_t write_security; /**< Minimum 1..4 security level for either write bit. */
    uint8_t notify_security; /**< Minimum 1..4 for NOTIFY, including CCC writes/sends. */
    esy_ble_gatt_read_fn read; /**< Required for READ. */
    esy_ble_gatt_write_fn write; /**< Required for either write bit. */
    void *user; /**< Borrowed application callback context, NULL allowed. */
    uint16_t value_handle; /**< Zero before registration; provider assigns on success. */
} esy_ble_gatt_characteristic_t;

/**
 * Portable GATT server definition. Keep service, characteristic array, UUIDs and
 * callback contexts at stable addresses; do not mutate while registered.
 * Providers generate declarations and CCC for NOTIFY, enforce requested read/
 * write security, and reject unsupported combinations without partial binding.
 */
typedef struct {
    esy_ble_uuid_t uuid; /**< Service UUID. */
    bool primary; /**< Primary service when true, secondary otherwise. */
    esy_ble_gatt_characteristic_t *characteristics; /**< Borrowed non-NULL array. */
    size_t characteristic_count; /**< 1..UINT16_MAX; provider capacity may be lower. */
    uint16_t handle; /**< Zero before registration; provider assigns on success. */
} esy_ble_gatt_service_t;

/** Owner-side response to the matching authentication event. */
typedef enum {
    ESY_BLE_AUTH_PASSKEY_ENTRY = 0, /**< Submit passkey in 0..999999. */
    ESY_BLE_AUTH_PASSKEY_CONFIRM, /**< Accept numeric comparison. */
    ESY_BLE_AUTH_PAIRING_CONFIRM, /**< Accept an incoming pairing request. */
    ESY_BLE_AUTH_CANCEL /**< Reject or cancel authentication. */
} esy_ble_auth_reply_t;

/** Legacy advertising parameters; buffers are borrowed only during adv_start. */
typedef struct {
    bool connectable; /**< Connectable/scannable, or nonconnectable advertising. */
    uint16_t interval_min; /**< Minimum interval, 0x0020..0x4000, in 0.625 ms units. */
    uint16_t interval_max; /**< Maximum interval, at least interval_min. */
    const uint8_t *data; /**< Encoded [length, type, value...] AD structures. */
    size_t data_length; /**< 0..31 bytes; NULL is allowed only for zero length. */
    const uint8_t *scan_response; /**< Encoded scan-response AD structures. */
    size_t scan_response_length; /**< 0..31 bytes; same NULL rule as data. */
} esy_ble_adv_params_t;

/** Continuous legacy scan parameters; stop explicitly, no core timeout exists. */
typedef struct {
    bool active; /**< Request scan responses when true. */
    bool filter_duplicates; /**< Ask the provider to filter duplicate reports. */
    uint16_t interval; /**< 0x0004..0x4000 in 0.625 ms units. */
    uint16_t window; /**< 0x0004..interval in 0.625 ms units. */
} esy_ble_scan_params_t;

/** Connection parameters; controller owns procedure and supervision timing. */
typedef struct {
    esy_ble_address_t peer; /**< Target address (public or random). */
    uint16_t interval_min; /**< 0x0006..0x0c80 in 1.25 ms units. */
    uint16_t interval_max; /**< At least interval_min, at most 0x0c80. */
    uint16_t latency; /**< Maximum peripheral latency, 0..499 events. */
    uint16_t supervision_timeout; /**< 0x000a..0x0c80 in 10 ms units; must
        exceed twice (latency + 1) times interval_max. */
} esy_ble_connect_params_t;

/** Event kinds; radio procedure failures use status and provider-native reason. */
typedef enum {
    ESY_BLE_EVENT_CONNECTED, /**< status reports connection success/failure. */
    ESY_BLE_EVENT_DISCONNECTED, /**< Connection identifier expires after callback. */
    ESY_BLE_EVENT_SCAN_RESULT, /**< peer, RSSI and raw AD payload are valid. */
    ESY_BLE_EVENT_SECURITY_CHANGED, /**< security_level and status are valid. */
    ESY_BLE_EVENT_PAIRING_COMPLETE, /**< bonded reports whether keys were stored. */
    ESY_BLE_EVENT_PAIRING_FAILED, /**< reason identifies the provider security error. */
    ESY_BLE_EVENT_PASSKEY_DISPLAY, /**< Display passkey without requesting a reply. */
    ESY_BLE_EVENT_PASSKEY_ENTRY, /**< Owner replies with PASSKEY_ENTRY or CANCEL. */
    ESY_BLE_EVENT_PASSKEY_CONFIRM, /**< Owner confirms passkey or cancels. */
    ESY_BLE_EVENT_PAIRING_CONFIRM, /**< Owner accepts pairing or cancels. */
    ESY_BLE_EVENT_AUTH_CANCELLED /**< Pending authentication request has expired. */
} esy_ble_event_type_t;

/**
 * Borrowed event; copy any needed fields/payload before returning. Unused fields
 * are zero, connection is INVALID for scan reports or failed connections.
 * Scan data may exceed legacy payload size if the provider reports extended AD.
 */
typedef struct {
    esy_ble_event_type_t type; /**< Event discriminator. */
    esy_ble_connection_t connection; /**< Live connection ID, or INVALID. */
    esy_ble_address_t peer; /**< Peer when known, otherwise zero. */
    esy_err_t status; /**< ESY_OK or a mapped provider error. */
    uint8_t reason; /**< Native HCI/SMP reason, interpreted by event kind. */
    uint8_t security_level; /**< 1: clear, 2: encrypted, 3: MITM, 4: LE SC MITM. */
    bool bonded; /**< Pairing produced persistent bond keys when true. */
    int8_t rssi; /**< Scan RSSI in dBm. */
    const uint8_t *data; /**< Borrowed scan payload, otherwise NULL. */
    size_t data_length; /**< Number of payload bytes. */
    uint32_t passkey; /**< 0..999999 for display/numeric confirmation events. */
} esy_ble_event_t;

/** Event sink; provider context, non-ISR; no reentrant control/lifecycle calls. */
typedef void (*esy_ble_event_fn)(void *user, const esy_ble_event_t *event);
/** Synchronous bond visitor; peer pointer expires on return; do not modify bonds. */
typedef void (*esy_ble_bond_fn)(void *user, const esy_ble_address_t *peer);

/**
 * Software-provider contract. Only init/deinit are mandatory. All methods are
 * owner-only and non-ISR. Other NULL methods mean NOT_SUPPORTED. Control buffers
 * and parameter structs expire on return, except registered GATT definitions and
 * callback contexts; a provider must consume/copy other transient inputs.
 *
 * init borrows event/user until successful deinit and must leave deinit legal
 * after any failure. deinit must stop radio activity, release native resources
 * and quiesce ALL sink callbacks before returning OK; on failure it retains the
 * binding and borrowed resources for retry. init must not dispatch events until
 * its own setup is complete. Each core binding attempt needs an EXCLUSIVE
 * provider context until successful deinit, including failed init. Never attach
 * two handles to the same context or use another handle to clean it up. Shared
 * native stacks need distinct binding contexts which reject conflicting stack
 * ownership without touching the active owner's resources. The core cannot
 * detect aliases of arbitrary provider contexts. Stateless NULL contexts are
 * shareable only when init/deinit own no resources or callback state.
 * The provider translates the portable GATT schema, ATT callbacks and pairing
 * events. Persistence and IO capabilities are configured by the consumer-owned
 * provider. Restore keys before init succeeds, and report storage errors rather
 * than claiming a durable bond. No key bytes cross this public interface.
 *
 * gatt_register succeeds atomically and assigns service/characteristic handles;
 * failure leaves definitions unchanged and releases temporary native resources.
 * gatt_unregister validates the SAME service identity and quiesces its callbacks
 * before success. Successful deinit unregisters remaining services and zeros all
 * assigned handles, releasing their borrowed storage. Native resource allocation
 * is permitted only in documented lifecycle/control entry points, never in ATT
 * callbacks. Providers must document finite capacity, native timing and storage.
 */
typedef struct {
    esy_err_t (*init)(void *ctx, esy_ble_event_fn event, void *user); /**< Bind/start. */
    esy_err_t (*deinit)(void *ctx); /**< Stop/unbind; safe after failed init. */
    esy_err_t (*adv_start)(void *ctx, const esy_ble_adv_params_t *params); /**< Start legacy AD. */
    esy_err_t (*adv_stop)(void *ctx); /**< Stop advertising. */
    esy_err_t (*scan_start)(void *ctx, const esy_ble_scan_params_t *params); /**< Start scan. */
    esy_err_t (*scan_stop)(void *ctx); /**< Stop scan. */
    esy_err_t (*connect)(void *ctx, const esy_ble_connect_params_t *params); /**< Initiate link. */
    esy_err_t (*disconnect)(void *ctx, esy_ble_connection_t connection, uint8_t reason); /**< End link. */
    esy_err_t (*security)(void *ctx, esy_ble_connection_t connection, uint8_t level); /**< Request 1..4. */
    esy_err_t (*bonds_foreach)(void *ctx, esy_ble_bond_fn visitor, void *user); /**< Enumerate identity bonds. */
    esy_err_t (*unpair)(void *ctx, const esy_ble_address_t *peer); /**< NULL erases all bonds. */
    esy_err_t (*notify)(void *ctx, esy_ble_connection_t connection,
                        uint16_t value_handle, const void *data, size_t length); /**< Notify subscribed peer. */
    esy_err_t (*gatt_register)(void *ctx, esy_ble_gatt_service_t *service); /**< Register schema atomically. */
    esy_err_t (*gatt_unregister)(void *ctx, esy_ble_gatt_service_t *service); /**< Remove owned schema. */
    esy_err_t (*auth_reply)(void *ctx, esy_ble_connection_t connection,
                          esy_ble_auth_reply_t reply, uint32_t passkey); /**< Reply to authentication. */
} esy_ble_ops_t;

/**
 * Borrowed provider descriptor; ops and exclusive ctx must remain alive until
 * deinit succeeds. Copying a descriptor does not create another binding context.
 */
typedef struct {
    const esy_ble_ops_t *ops; /**< Immutable operation table. */
    void *ctx; /**< Provider-owned context; NULL allowed for stateless providers. */
} esy_ble_provider_t;

/**
 * Caller-owned handle; initialize with {0} before FIRST use. Fields are private;
 * do not copy, reset or move a bound handle. Keep at a stable address until
 * deinit succeeds. A failed init remains bound for mandatory deinit cleanup.
 */
typedef struct {
    esy_ble_provider_t provider; /**< Private borrowed provider binding. */
    uint8_t state; /**< Private lifecycle state; zero means detached. */
} esy_ble_t;

/**
 * Bind/init a provider. Non-NULL event required, user may be NULL. Descriptor is
 * copied; its ops/context and event/user are borrowed. INVALID_ARG leaves handle
 * unchanged, repeated init returns STATE. A provider failure requires deinit
 * before retry, retaining resources if cleanup fails. Not ISR/callback safe.
 * @return OK, INVALID_ARG, STATE, or provider error.
 */
esy_err_t esy_ble_init(esy_ble_t *ble, const esy_ble_provider_t *provider,
                       esy_ble_event_fn event, void *user);
/**
 * Stop and release provider, including failed-init resources. NULL is invalid;
 * detached handles return OK. On provider failure retain handle for cleanup
 * retry only; all control APIs then return STATE, even if previously ready. On
 * success clear it to {0}, allowing reinit and releasing borrowed resources.
 * Serialize with control calls and satisfy provider callback-drain prerequisites.
 * @return OK, INVALID_ARG, or provider error. Not ISR/callback safe.
 */
esy_err_t esy_ble_deinit(esy_ble_t *ble);
/** Start legacy advertising; validate AD encoding/ranges, then call provider. */
esy_err_t esy_ble_adv_start(esy_ble_t *ble, const esy_ble_adv_params_t *params);
/** Stop advertising; provider defines repeated-stop status. */
esy_err_t esy_ble_adv_stop(esy_ble_t *ble);
/** Start continuous scanning with validated window/interval. */
esy_err_t esy_ble_scan_start(esy_ble_t *ble, const esy_ble_scan_params_t *params);
/** Stop scanning; provider defines repeated-stop status. */
esy_err_t esy_ble_scan_stop(esy_ble_t *ble);
/** Initiate connection; validate peer and Bluetooth timing constraints. */
esy_err_t esy_ble_connect(esy_ble_t *ble, const esy_ble_connect_params_t *params);
/** Disconnect a live ID; reason is a provider-supported HCI disconnect reason. */
esy_err_t esy_ble_disconnect(esy_ble_t *ble, esy_ble_connection_t connection, uint8_t reason);
/** Request security level 1..4; pairing/authentication may finish asynchronously. */
esy_err_t esy_ble_security(esy_ble_t *ble, esy_ble_connection_t connection, uint8_t level);
/** Synchronously visit bonds; non-NULL visitor required, user may be NULL. */
esy_err_t esy_ble_bonds_foreach(esy_ble_t *ble, esy_ble_bond_fn visitor, void *user);
/** Remove peer bond or all bonds (NULL); persistence follows provider contract. */
esy_err_t esy_ble_unpair(esy_ble_t *ble, const esy_ble_address_t *peer);
/**
 * Notify a live connection at a nonzero LOCAL characteristic value handle.
 * data may be NULL only for length=0; length <= UINT16_MAX, further MTU and CCC
 * checks belong to provider. Data is consumed/copied before return. Acceptance
 * does not confirm peer receipt. No implicit fragmentation or broadcast.
 */
esy_err_t esy_ble_notify(esy_ble_t *ble, esy_ble_connection_t connection,
                         uint16_t value_handle, const void *data, size_t length);
/**
 * Register a valid GATT server service. Definitions must have zero handles before
 * first registration; repeated registration returns STATE. Provider errors leave
 * the definitions unchanged. Read/write properties require matching callbacks
 * and minimum security in 1..4; NOTIFY requires notify_security in 1..4, enforced
 * for CCC writes and notifications. No include services, descriptors beyond generated
 * CCC, indications, prepared writes or GATT client procedures are provided.
 * @return OK, INVALID_ARG, STATE, NOT_SUPPORTED or provider error.
 */
esy_err_t esy_ble_gatt_register(esy_ble_t *ble, esy_ble_gatt_service_t *service);
/**
 * Unregister the same live service object; provider must quiesce ATT callbacks.
 * On success handles are cleared and storage can be released/reused. On failure
 * retain definitions and callback state for retry. No reentry/ISR use permitted.
 * @return OK, INVALID_ARG, STATE, NOT_SUPPORTED or provider error.
 */
esy_err_t esy_ble_gatt_unregister(esy_ble_t *ble, esy_ble_gatt_service_t *service);
/**
 * Reply to a pending authentication event from the owner context. Validate live
 * connection ID, action and passkey range (only for PASSKEY_ENTRY); the provider
 * validates request identity/state. Ignore stale queued requests after disconnect
 * or AUTH_CANCELLED and after any pairing completion/failure. The owner must
 * invalidate queued UI responses before accepting a new request on the same
 * connection; no authentication generation token is provided. Connection IDs
 * may be reused. No implicit auto-accept policy.
 * @return OK, INVALID_ARG, STATE, NOT_SUPPORTED or provider error.
 */
esy_err_t esy_ble_auth_reply(esy_ble_t *ble, esy_ble_connection_t connection,
                            esy_ble_auth_reply_t reply, uint32_t passkey);

#ifdef __cplusplus
}
#endif
#endif
