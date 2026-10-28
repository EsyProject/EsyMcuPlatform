/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_BLE_BTSTACK_H
#define ESY_BLE_BTSTACK_H

#include "esy_middleware/ble/esy_ble.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Private adapter storage capacity; native types are deliberately hidden. */
#define ESY_BLE_BTSTACK_STORAGE_SIZE 512u

/** Caller-owned link slot, implementation-owned between init and deinit. */
typedef struct {
    esy_ble_connection_t connection; /**< Private native HCI handle. */
    esy_ble_address_t peer; /**< Private peer identity. */
    uint8_t auth_request; /**< Private pending authentication request. */
} esy_ble_btstack_link_t;

/** Caller-owned CCC slot; subscriptions are connection-local and not restored. */
typedef struct {
    esy_ble_connection_t connection; /**< Private owning connection. */
    uint16_t value_handle; /**< Private characteristic value handle. */
} esy_ble_btstack_subscription_t;

/** Aggregate caller-owned backing storage; contents need not be initialized. */
typedef struct {
    uint8_t *attribute_database; /**< Required byte buffer; >=3 and <=UINT16_MAX. */
    size_t attribute_database_size; /**< Capacity including version/end marker. */
    esy_ble_gatt_service_t **services; /**< Required service pointer slots. */
    size_t service_capacity; /**< Nonzero service registration capacity. */
    esy_ble_btstack_link_t *links; /**< Required native link tracking slots. */
    size_t link_capacity; /**< >= positive native MAX_NR_HCI_CONNECTIONS. */
    esy_ble_btstack_subscription_t *subscriptions; /**< Required CCC slots. */
    size_t subscription_capacity; /**< Nonzero simultaneous enabled CCC capacity. */
    uint8_t *read_buffer; /**< Required shared dynamic-read snapshot buffer. */
    size_t read_buffer_size; /**< 1..UINT16_MAX; at least the largest negotiated
        fixed ATT MTU minus one supports a full response. A smaller buffer makes
        reads requiring more snapshot space fail with ATT INSUFFICIENT_RESOURCES. */
} esy_ble_btstack_static_resources_t;

/**
 * Explicit native persistence boundary; neither callback is a generic allocator.
 * restore populates the already initialized native LE device DB before init
 * succeeds. flush commits/checks all native key changes and returns OK only when
 * durable, including deletions. Native TLV backends may persist eagerly; flush
 * then reports any latched backend error. Both callbacks are required. They run
 * synchronously in the serialized host owner, must not call BLE APIs, and may
 * perform bounded storage I/O under the consumer's Flash/OTA policy. Their worst
 * latency belongs to the consumer's native/HIL budget. No key bytes enter the
 * generic BLE API. user remains borrowed until successful core deinit.
 */
typedef struct {
    esy_err_t (*restore)(void *user); /**< Required native DB restore. */
    esy_err_t (*flush)(void *user); /**< Required durable commit/error check. */
    void *user; /**< Borrowed storage context. */
} esy_ble_btstack_persistence_t;

/** Configuration copied by provider(); pointed-to storage remains borrowed. */
typedef struct {
    const esy_ble_btstack_static_resources_t *static_resources; /**< Required. */
    esy_ble_btstack_persistence_t persistence; /**< Required durable key store. */
    esy_err_t (*host_quiesce)(void *user); /**< Required confirmation hook: OK only
        when native GAP is idle, pending commands are complete and no new link
        can appear. Called before init, GATT schema/key mutations, and deinit.
        BUSY asks the owner to pump BTstack and retry; other errors retain the
        binding. This hook must not initiate unrelated GAP or disable HCI. */
    void *host_user; /**< Borrowed quiescence context. */
    uint8_t io_capability; /**< Native Bluetooth IO capability, 0..4. */
    uint8_t minimum_security; /**< Immutable SM policy, 2 encrypted, 3 MITM,
        4 authenticated Secure Connections with a 16-byte encryption key. */
    bool bondable; /**< Request durable bonding when true. */
} esy_ble_btstack_config_t;

/** Caller-owned adapter token; initialize with {0}, never copy/move while bound. */
typedef struct esy_ble_btstack_s {
    const struct esy_ble_btstack_s *owner; /**< Private identity guard. */
    union {
        max_align_t alignment; /**< Enforces native context alignment. */
        unsigned char bytes[ESY_BLE_BTSTACK_STORAGE_SIZE]; /**< Private context. */
    } storage; /**< Implementation-owned opaque storage. */
} esy_ble_btstack_t;

/**
 * @file esy_ble_btstack.h
 * @brief BTstack 1.6.2 adapter over a consumer-owned software host runtime.
 *
 * The consumer initializes the native run loop, memory pools, HCI transport,
 * L2CAP, SM and LE device DB, then powers HCI to HCI_STATE_WORKING before core
 * init. Native central, peripheral and Secure Connections support and a finite
 * positive MAX_NR_HCI_CONNECTIONS are required. The adapter exclusively owns
 * the native ATT server and GAP/SM control
 * while attached; no other module may change them. It borrows the native host
 * without creating tasks, touching hardware, choosing Flash regions or powering
 * down HCI. Native provider allocations/timing still apply. All APIs, native
 * event/ATT callbacks, persistence hooks and lifecycle operations execute in ONE
 * serialized non-ISR BTstack context; no multithread or callback reentry is legal.
 * All native provider sources, including att_server.c, must use the same
 * btstack_config.h with explicit NVN_NUM_GATT_SERVER_CCC=0. Native CCC persistence
 * and restoration are disabled; adding this macro only to the adapter is invalid.
 * EATT bearers are unsupported and ENABLE_GATT_OVER_EATT must remain undefined.
 *
 * The host owner keeps driving its run loop during shutdown. Deinit requests
 * GAP stop/cancel/disconnect and returns BUSY until links are gone and the
 * host_quiesce hook confirms controller quiescence. Successful deinit unregisters
 * adapter callbacks, detaches ATT application state, clears service handles and
 * restores adapter storage to {0}. Consumer HCI/SM/L2CAP/TLV remain initialized.
 * Failed init remains cleanable, including conflicting singleton ownership.
 * The configured native SM policy remains installed after detach; a subsequent
 * native owner must install its own IO, authentication and key-size policy.
 *
 * GATT registration appends attributes to caller storage while GAP is idle and
 * no link exists. Unregister is LIFO, also while idle, and reclaims removed
 * handle numbers without changing retained handles. Capacity errors leave
 * schemas unchanged. ATT callbacks do no allocation;
 * prepared/reliable/signed writes, indications and client procedures are excluded.
 * CCC slots are per connection, cleared on disconnect and checked before notify.
 * Simultaneous WRITE and WRITE_COMMAND on one characteristic is unsupported:
 * the native write callback cannot distinguish their opcodes. Dynamic read
 * values must be shorter than 0xfe00 bytes due to native error encoding. The
 * adapter queries total length, then snapshots the requested slice in read_buffer
 * during the native length probe, where ATT errors can still be reported. A read
 * callback must fill exactly min(capacity, total_length - offset) bytes, consistent
 * with its preceding length query. Probes may be discarded without copying;
 * application reads must have no destructive side effects. The native copy step
 * only copies the bounded snapshot and never invokes the application callback.
 * Notifications exceeding the negotiated fixed ATT MTU minus three return
 * INVALID_ARG; no truncation or fragmentation is performed. The
 * native disconnect operation supports only reason 0x13. Service Changed and
 * bonded-peer cache invalidation remain consumer schema-version policy.
 */

/**
 * @brief Prepare an exclusive provider binding without initializing native HCI.
 * @param adapter Exclusive {0} storage retained until successful core deinit.
 * @param config Valid storage, persistence, IO and quiescence policy.
 * @param provider Required descriptor output, passed to esy_ble_init().
 * @return OK, INVALID_ARG for invalid configuration, or STATE for live/copied
 * storage. On failure neither adapter nor provider changes. On success init and
 * subsequent deinit must use the same adapter; config itself may then expire.
 * Each core binding needs its own token, even when an init attempt will conflict.
 */
esy_err_t esy_ble_btstack_provider(esy_ble_btstack_t *adapter,
                                  const esy_ble_btstack_config_t *config,
                                  esy_ble_provider_t *provider);

#ifdef __cplusplus
}
#endif
#endif /* ESY_BLE_BTSTACK_H */
