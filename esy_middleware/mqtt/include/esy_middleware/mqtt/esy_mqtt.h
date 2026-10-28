/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_MQTT_H
#define ESY_MQTT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_middleware/network/esy_network_stream.h"

/**
 * @file
 * @brief Caller-owned MQTT 3.1.1 client with an injected TCP/TLS byte stream.
 *
 * One task or bare-metal loop serializes every operation on a client. No API is
 * ISR-safe. Callbacks execute synchronously in that owner and may query the
 * connection, but must not mutate the client or recursively drive it. Different
 * clients may run independently if their providers support it.
 *
 * The consumer opens/authenticates the TCP/TLS connection before connect, and
 * closes it after disconnect, abort, or any connection-fatal error. This module
 * borrows that connection; it never creates, closes or frees provider resources.
 * Each connect starts a clean session. The consumer reconnects with a fresh
 * transport and restores subscriptions; no old publish is replayed implicitly.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Fixed private storage capacity; rebuild consumers when it changes. */
#define ESY_MQTT_CLIENT_STORAGE_BYTES 2048u
/** @brief Maximum outstanding QoS 1 publishes and subscription commands. */
#define ESY_MQTT_MAX_PENDING_OPERATIONS 8u
/** @brief Minimum caller-owned packet buffer capacity, in bytes. */
#define ESY_MQTT_MIN_BUFFER_SIZE 8u

/** @brief Opaque client. Initialize with {0}; never copy an initialized client. */
typedef struct {
  ESY_ALIGNED_STORAGE(priv, ESY_MQTT_CLIENT_STORAGE_BYTES);
} esy_mqtt_t;

/** @brief Supported delivery levels. QoS 2 is not supported. */
typedef enum {
  ESY_MQTT_QOS_0 = 0, /**< At most once. */
  ESY_MQTT_QOS_1 = 1  /**< At least once; consumers must tolerate duplicates. */
} esy_mqtt_qos_t;

/** @brief Borrowed UTF-8 string; length excludes any optional terminator. */
typedef struct {
  const char *data;  /**< NULL is allowed only with zero length. */
  uint16_t length;  /**< Number of bytes, not code points. */
} esy_mqtt_string_t;

/** @brief Packet storage, exclusively borrowed until successful deinit. */
typedef struct {
  uint8_t *network_buffer; /**< Writable buffer; no initial contents required. */
  size_t network_buffer_size; /**< At least ESY_MQTT_MIN_BUFFER_SIZE bytes. */
} esy_mqtt_static_resources_t;

/** @brief Publish parameters and callback view; data is borrowed, not copied. */
typedef struct {
  esy_mqtt_string_t topic; /**< Nonempty MQTT topic name, without wildcards. */
  const void *payload; /**< NULL is allowed only when payload_size is zero. */
  size_t payload_size; /**< Payload bytes. */
  esy_mqtt_qos_t qos; /**< QoS 0 or 1. */
  bool retain; /**< Retained-message flag. */
  bool duplicate; /**< RX DUP flag; must be false for new outbound messages. */
} esy_mqtt_message_t;

/** @brief Events emitted synchronously from the client owner context. */
typedef enum {
  ESY_MQTT_EVENT_MESSAGE, /**< Incoming publish; message expires on return. */
  ESY_MQTT_EVENT_PUBLISHED, /**< QoS 1 PUBACK, or cancellation/failure. */
  ESY_MQTT_EVENT_SUBSCRIBED, /**< SUBACK, including broker rejection. */
  ESY_MQTT_EVENT_UNSUBSCRIBED, /**< UNSUBACK, or cancellation/failure. */
  ESY_MQTT_EVENT_DISCONNECTED /**< Connection lost or explicitly stopped. */
} esy_mqtt_event_type_t;

/** @brief Event fields; only MESSAGE events contain a valid message view. */
typedef struct {
  esy_mqtt_event_type_t type; /**< Event kind. */
  esy_err_t result; /**< ESY_OK or an error/cancellation reason. */
  uint16_t packet_id; /**< Matching operation ID, or zero for connection events. */
  esy_mqtt_qos_t granted_qos; /**< Valid on successful SUBSCRIBED events. */
  esy_mqtt_message_t message; /**< Valid only during a MESSAGE callback. */
} esy_mqtt_event_t;

/**
 * @brief Receive an event. Copy any needed RX data before returning.
 * @param context Borrowed application context.
 * @param event Borrowed event; never retain it or block in this callback.
 */
typedef void (*esy_mqtt_event_cb_t)(void *context,
                                    const esy_mqtt_event_t *event);

/** @brief Initialization inputs, copied by init except borrowed references. */
typedef struct {
  esy_network_stream_t transport; /**< Base send/recv/progress/context only. */
  const esy_mqtt_static_resources_t *static_resources; /**< Copied descriptor. */
  uint32_t (*get_time_ms)(void); /**< Continuously advancing uint32_t ms clock. */
  uint32_t acknowledgement_timeout_ms; /**< 1..INT32_MAX per pending command. */
  esy_mqtt_event_cb_t event_cb; /**< Mandatory callback. */
  void *event_context; /**< Borrowed application context; may be NULL. */
} esy_mqtt_config_t;

/** @brief Borrowed inputs needed only for the synchronous connect call. */
typedef struct {
  esy_mqtt_string_t client_id; /**< Nonempty client identifier. */
  esy_mqtt_string_t username; /**< Optional username. */
  esy_mqtt_string_t password; /**< Optional binary password; requires username. */
  uint16_t keep_alive_seconds; /**< 0 disables keep-alive; otherwise 1..65535. */
  uint32_t timeout_ms; /**< CONNACK wait, 1..INT32_MAX milliseconds. */
  const esy_mqtt_message_t *will; /**< Optional QoS 0/1 last will. */
} esy_mqtt_connect_config_t;

/**
 * @brief Initialize a zero-initialized client without network I/O or allocation.
 * @return OK, INVALID_ARG for invalid inputs, STATE for an active client, or
 * BUSY during callback reentry.
 * Failed init retains no resources and preserves the input client. Repeated init
 * returns STATE without replacing an active configuration. The packet buffer,
 * provider and callback contexts remain borrowed until successful deinit.
 */
esy_err_t esy_mqtt_init(esy_mqtt_t *client, const esy_mqtt_config_t *config);

/**
 * @brief Establish a clean MQTT session on an already opened transport.
 * @return OK, INVALID_ARG, STATE, BUSY on callback reentry, or a protocol/I/O
 * error. A failure requires closing the transport before another connect.
 * Success does not mean old subscriptions or unacknowledged messages survived.
 */
esy_err_t esy_mqtt_connect(esy_mqtt_t *client,
                           const esy_mqtt_connect_config_t *config);

/**
 * @brief Send a publish. Output packet_id is written only on success.
 * @return OK after transport acceptance; QoS 1 completion arrives by event.
 * QoS 0 writes packet ID zero and has no completion event. NO_MEM means pending
 * command capacity is exhausted; no bytes were sent. Inputs are borrowed only
 * during this call. An I/O error invalidates the connection and cancels pending
 * commands; close the transport. An outgoing DUP flag is rejected.
 */
esy_err_t esy_mqtt_publish(esy_mqtt_t *client,
                           const esy_mqtt_message_t *message,
                           uint16_t *packet_id);

/**
 * @brief Subscribe to one valid MQTT topic filter at QoS 0 or 1.
 * @return OK after transport acceptance, with packet_id written; the broker's
 * SUBACK event reports the granted QoS or NOT_SUPPORTED on rejection. NO_MEM
 * means all pending command slots are occupied. The filter is borrowed for this
 * call only; the consumer owns subscription tracking and reconnect restoration.
 */
esy_err_t esy_mqtt_subscribe(esy_mqtt_t *client, esy_mqtt_string_t filter,
                             esy_mqtt_qos_t qos, uint16_t *packet_id);

/** @brief Unsubscribe one filter; completion and output rules match subscribe. */
esy_err_t esy_mqtt_unsubscribe(esy_mqtt_t *client, esy_mqtt_string_t filter,
                               uint16_t *packet_id);

/**
 * @brief Process at most one incoming packet and check pending acknowledgments.
 *
 * Invoke from the owner at least every min(1000, acknowledgement_timeout_ms)
 * milliseconds, and often enough for the provider's own progress requirements.
 * The clock must keep advancing during synchronous calls and must not reset or
 * stop in a connected session. Its uint32_t millisecond period is about 49.7 days;
 * each configured wait is at most INT32_MAX and must be observed within that
 * range. Disconnect/abort before an unbounded pause or clock discontinuity.
 * Completed/cancelled commands are removed, so later wraps cannot reactivate
 * them. Broker keep-alive limits may require a shorter processing interval.
 * Keep-alive advances even while receiving data. A pending PINGRESP wait expires
 * only after MQTT_PINGRESP_TIMEOUT_MS; a response dispatched at that boundary is
 * accepted, but one dispatched later is fatal. This provider configuration uses
 * the same advancing millisecond clock and observation limits described above.
 * An ACK dispatched at or after its deadline is expired, even if its bytes were
 * buffered earlier. Expired commands are checked before consuming new input.
 *
 * No-data/partial-packet progress returns OK. Acknowledgment timeout, malformed
 * packets, keep-alive failure or I/O error invalidates the connection and emits
 * failure events for pending commands, followed by DISCONNECTED. Local buffer
 * exhaustion returns NO_MEM and is also fatal. During connected processing, an
 * oversized current packet is rejected once its header is available, without
 * draining its remaining payload. Close the transport before reconnecting.
 *
 * Work includes upstream synchronous transport calls and user callbacks; timeout
 * configuration is not a hard wall-clock bound. MQTT packet size and provider
 * latency must be budgeted by the consumer. No background task/timer is created.
 */
esy_err_t esy_mqtt_process(esy_mqtt_t *client);

/**
 * @brief Send DISCONNECT, cancel pending commands and stop MQTT processing.
 * Cancellation events use STATE. Even if sending fails, local processing stops;
 * always close the transport afterwards. Calling again while stopped is OK.
 */
esy_err_t esy_mqtt_disconnect(esy_mqtt_t *client);

/**
 * @brief Stop without network I/O, e.g. after network loss or before OTA/suspend.
 * Pending commands receive STATE, followed by DISCONNECTED when connected.
 * Close the borrowed transport after return. Repeated abort is OK.
 */
esy_err_t esy_mqtt_abort(esy_mqtt_t *client);

/**
 * @brief Release borrowed references after disconnect/abort.
 * @return OK and zero the client, STATE if uninitialized/still connected, BUSY
 * during callback reentry, or INVALID_ARG for NULL. Failure retains resources.
 * Repeated deinit returns STATE; successful deinit permits a new init.
 */
esy_err_t esy_mqtt_deinit(esy_mqtt_t *client);

/** @brief Owner-context query; NULL/uninitialized clients return false. */
bool esy_mqtt_is_connected(const esy_mqtt_t *client);

#ifdef __cplusplus
}
#endif

#endif /* ESY_MQTT_H */
