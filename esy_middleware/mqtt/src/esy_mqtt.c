/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/mqtt/esy_mqtt.h"

#include <limits.h>
#include <string.h>

#include "core_mqtt.h"
#include "core_mqtt_config_defaults.h"

#define MQTT_PACKET_KIND_MASK 0xF0u
#define MQTT_UTF8_CONTINUATION_MASK 0xC0u
#define MQTT_UTF8_CONTINUATION_VALUE 0x80u

typedef struct esy_mqtt_ctx esy_mqtt_ctx_t;

/* coreMQTT owns the incomplete type name; its contents belong to this adapter. */
struct NetworkContext {
  esy_network_stream_t transport;
  esy_mqtt_ctx_t *owner;
};

typedef struct {
  bool active;
  uint16_t packet_id;
  esy_mqtt_event_type_t event;
  esy_mqtt_qos_t requested_qos;
  uint32_t started_ms;
} esy_mqtt_pending_t;

struct esy_mqtt_ctx {
  bool initialized;
  bool busy;
  bool connected;
  MQTTContext_t mqtt;
  struct NetworkContext network;
  esy_mqtt_static_resources_t resources;
  uint32_t (*get_time_ms)(void);
  uint32_t acknowledgement_timeout_ms;
  esy_mqtt_event_cb_t event_cb;
  void *event_context;
  esy_err_t event_error;
  MQTTPubAckInfo_t outgoing[ESY_MQTT_MAX_PENDING_OPERATIONS];
  MQTTPubAckInfo_t incoming[ESY_MQTT_MAX_PENDING_OPERATIONS];
  esy_mqtt_pending_t pending[ESY_MQTT_MAX_PENDING_OPERATIONS];
};

_Static_assert(sizeof(esy_mqtt_ctx_t) <= ESY_MQTT_CLIENT_STORAGE_BYTES,
               "MQTT client storage is too small");
_Static_assert(_Alignof(esy_mqtt_ctx_t) <= _Alignof(esy_mqtt_t),
               "MQTT client storage is misaligned");
_Static_assert(MQTT_SEND_TIMEOUT_MS < INT32_MAX,
               "MQTT send timeout must be less than INT32_MAX milliseconds");
_Static_assert(MQTT_RECV_POLLING_TIMEOUT_MS <= INT32_MAX,
               "MQTT receive timeout exceeds the supported clock range");
_Static_assert(MQTT_PINGRESP_TIMEOUT_MS <= INT32_MAX,
               "MQTT ping timeout exceeds the supported clock range");
_Static_assert(PACKET_TX_TIMEOUT_MS <= INT32_MAX &&
                   PACKET_RX_TIMEOUT_MS <= INT32_MAX,
               "MQTT inactivity timeout exceeds the supported clock range");

static esy_mqtt_ctx_t *client_context(esy_mqtt_t *client) {
  return (esy_mqtt_ctx_t *)(void *)client->priv.bytes;
}

static const esy_mqtt_ctx_t *const_client_context(const esy_mqtt_t *client) {
  return (const esy_mqtt_ctx_t *)(const void *)client->priv.bytes;
}

static esy_err_t map_status(MQTTStatus_t status) {
  switch (status) {
  case MQTTSuccess:
    return ESY_OK;
  case MQTTBadParameter:
    return ESY_ERR_INVALID_ARG;
  case MQTTNoMemory:
    return ESY_ERR_NO_MEM;
  case MQTTNoDataAvailable:
  case MQTTKeepAliveTimeout:
    return ESY_ERR_TIMEOUT;
  case MQTTServerRefused:
    return ESY_ERR_NOT_SUPPORTED;
  case MQTTIllegalState:
    return ESY_ERR_STATE;
  case MQTTStateCollision:
    return ESY_ERR_BUSY;
  default:
    return ESY_ERR_IO;
  }
}

static bool valid_qos(esy_mqtt_qos_t qos) {
  return qos == ESY_MQTT_QOS_0 || qos == ESY_MQTT_QOS_1;
}

/* Reject invalid encodings, NUL, surrogates and Unicode noncharacters. */
static bool valid_utf8(esy_mqtt_string_t text, bool allow_empty) {
  if (text.length == 0u) {
    return allow_empty;
  }
  if (text.data == NULL) {
    return false;
  }
  size_t index = 0u;
  while (index < text.length) {
    uint32_t codepoint;
    uint32_t minimum;
    size_t continuation;
    uint8_t first = (uint8_t)text.data[index++];
    if (first < 0x80u) {
      codepoint = first;
      minimum = 0u;
      continuation = 0u;
    } else if (first >= 0xC2u && first <= 0xDFu) {
      codepoint = first & 0x1Fu;
      minimum = 0x80u;
      continuation = 1u;
    } else if (first >= 0xE0u && first <= 0xEFu) {
      codepoint = first & 0x0Fu;
      minimum = 0x800u;
      continuation = 2u;
    } else if (first >= 0xF0u && first <= 0xF4u) {
      codepoint = first & 0x07u;
      minimum = 0x10000u;
      continuation = 3u;
    } else {
      return false;
    }
    if (continuation > (size_t)text.length - index) {
      return false;
    }
    for (size_t byte = 0u; byte < continuation; ++byte) {
      uint8_t next = (uint8_t)text.data[index++];
      if ((next & MQTT_UTF8_CONTINUATION_MASK) != MQTT_UTF8_CONTINUATION_VALUE) {
        return false;
      }
      codepoint = (codepoint << 6u) | (next & 0x3Fu);
    }
    if (codepoint < minimum || codepoint == 0u || codepoint > 0x10FFFFu ||
        (codepoint >= 0xD800u && codepoint <= 0xDFFFu) ||
        (codepoint >= 0xFDD0u && codepoint <= 0xFDEFu) ||
        (codepoint & 0xFFFFu) >= 0xFFFEu) {
      return false;
    }
  }
  return true;
}

static bool valid_topic(esy_mqtt_string_t topic, bool filter) {
  if (!valid_utf8(topic, false)) {
    return false;
  }
  for (size_t index = 0u; index < topic.length; ++index) {
    char value = topic.data[index];
    if (value != '+' && value != '#') {
      continue;
    }
    if (!filter || (index > 0u && topic.data[index - 1u] != '/')) {
      return false;
    }
    if (value == '#' && index + 1u != topic.length) {
      return false;
    }
    if (value == '+' && index + 1u < topic.length &&
        topic.data[index + 1u] != '/') {
      return false;
    }
  }
  return true;
}

static bool valid_message(const esy_mqtt_message_t *message) {
  return message != NULL && valid_qos(message->qos) && !message->duplicate &&
         valid_topic(message->topic, false) &&
         (message->payload != NULL || message->payload_size == 0u);
}

static MQTTPublishInfo_t publish_info(const esy_mqtt_message_t *message) {
  MQTTPublishInfo_t info = {0};
  info.qos = (MQTTQoS_t)message->qos;
  info.retain = message->retain;
  info.dup = message->duplicate;
  info.pTopicName = message->topic.data;
  info.topicNameLength = message->topic.length;
  info.pPayload = message->payload;
  info.payloadLength = message->payload_size;
  return info;
}

static int32_t transport_send(NetworkContext_t *network, const void *data,
                              size_t size) {
  if (network->transport.progress != NULL &&
      network->transport.progress(network->transport.context) != ESY_OK) return -1;
  int32_t result = network->transport.send(network->transport.context, data, size);
  return result < 0 || (size_t)result > size ? -1 : result;
}

static bool incoming_packet_too_large(esy_mqtt_ctx_t *ctx, size_t available) {
  MQTTPacketInfo_t packet = {0};
  MQTTStatus_t status = MQTT_ProcessIncomingPacketTypeAndLength(
      ctx->resources.network_buffer, &available, &packet);
  return status == MQTTSuccess &&
         (packet.headerLength > ctx->resources.network_buffer_size ||
          packet.remainingLength >
              ctx->resources.network_buffer_size - packet.headerLength);
}

static int32_t transport_recv(NetworkContext_t *network, void *data, size_t size) {
  esy_mqtt_ctx_t *ctx = network->owner;
  /* Check the current packet only; subsequent packets advance on later calls.
   * Reject before coreMQTT can enter its synchronous oversized-packet drain.
   * Connect uses separate temporary buffers and has no connected receive loop.
   */
  if (ctx->connected && incoming_packet_too_large(ctx, ctx->mqtt.index)) {
    ctx->event_error = ESY_ERR_NO_MEM;
    return -1;
  }
  if (network->transport.progress != NULL &&
      network->transport.progress(network->transport.context) != ESY_OK) return -1;
  int32_t result = network->transport.recv(network->transport.context, data, size);
  if (result < 0 || (size_t)result > size) {
    return -1;
  }
  if (ctx->connected &&
      incoming_packet_too_large(ctx, ctx->mqtt.index + (size_t)result)) {
    ctx->event_error = ESY_ERR_NO_MEM;
    return -1;
  }
  return result;
}

static void emit_event(esy_mqtt_ctx_t *ctx, const esy_mqtt_event_t *event) {
  ctx->event_cb(ctx->event_context, event);
}

static esy_mqtt_pending_t *find_pending(esy_mqtt_ctx_t *ctx, uint16_t packet_id) {
  for (size_t index = 0u; index < ESY_MQTT_MAX_PENDING_OPERATIONS; ++index) {
    if (ctx->pending[index].active &&
        ctx->pending[index].packet_id == packet_id) {
      return &ctx->pending[index];
    }
  }
  return NULL;
}

static esy_mqtt_pending_t *free_pending(esy_mqtt_ctx_t *ctx) {
  for (size_t index = 0u; index < ESY_MQTT_MAX_PENDING_OPERATIONS; ++index) {
    if (!ctx->pending[index].active) {
      return &ctx->pending[index];
    }
  }
  return NULL;
}

static uint16_t next_packet_id(esy_mqtt_ctx_t *ctx) {
  /* At most eight identifiers are occupied, even across identifier wrap. */
  uint16_t packet_id;
  do {
    packet_id = MQTT_GetPacketId(&ctx->mqtt);
  } while (find_pending(ctx, packet_id) != NULL);
  return packet_id;
}

static void record_pending(esy_mqtt_ctx_t *ctx, esy_mqtt_pending_t *pending,
                            uint16_t packet_id, esy_mqtt_event_type_t event,
                            esy_mqtt_qos_t qos) {
  pending->packet_id = packet_id;
  pending->event = event;
  pending->requested_qos = qos;
  pending->started_ms = ctx->get_time_ms();
  pending->active = true;
}

static void finish_pending(esy_mqtt_ctx_t *ctx, esy_mqtt_pending_t *pending,
                            esy_err_t result, esy_mqtt_qos_t granted_qos) {
  esy_mqtt_event_t event = {0};
  event.type = pending->event;
  event.packet_id = pending->packet_id;
  event.result = result;
  event.granted_qos = granted_qos;
  pending->active = false;
  emit_event(ctx, &event);
}

static void retire_connection(esy_mqtt_ctx_t *ctx, esy_err_t reason,
                               esy_err_t cancellation_reason) {
  bool was_connected = ctx->connected;
  ctx->connected = false;
  ctx->mqtt.connectStatus = MQTTNotConnected;
  ctx->mqtt.waitingForPingResp = false;
  ctx->mqtt.index = 0u;
  memset(ctx->outgoing, 0, sizeof(ctx->outgoing));
  memset(ctx->incoming, 0, sizeof(ctx->incoming));
  for (size_t index = 0u; index < ESY_MQTT_MAX_PENDING_OPERATIONS; ++index) {
    if (ctx->pending[index].active) {
      finish_pending(ctx, &ctx->pending[index], cancellation_reason,
                     ESY_MQTT_QOS_0);
    }
  }
  if (was_connected) {
    esy_mqtt_event_t event = {0};
    event.type = ESY_MQTT_EVENT_DISCONNECTED;
    event.result = reason;
    emit_event(ctx, &event);
  }
}

static void receive_publish(esy_mqtt_ctx_t *ctx,
                             const MQTTDeserializedInfo_t *deserialized) {
  const MQTTPublishInfo_t *info = deserialized->pPublishInfo;
  if (info == NULL) {
    ctx->event_error = ESY_ERR_IO;
    return;
  }
  esy_mqtt_event_t event = {0};
  event.type = ESY_MQTT_EVENT_MESSAGE;
  event.packet_id = deserialized->packetIdentifier;
  event.message.topic.data = info->pTopicName;
  event.message.topic.length = info->topicNameLength;
  event.message.payload = info->pPayload;
  event.message.payload_size = info->payloadLength;
  event.message.qos = (esy_mqtt_qos_t)info->qos;
  event.message.retain = info->retain;
  event.message.duplicate = info->dup;
  if (!valid_qos(event.message.qos) ||
      !valid_topic(event.message.topic, false)) {
    ctx->event_error = ESY_ERR_IO;
    return;
  }
  emit_event(ctx, &event);
}

static void receive_ack(esy_mqtt_ctx_t *ctx, const MQTTPacketInfo_t *packet,
                         const MQTTDeserializedInfo_t *deserialized) {
  esy_mqtt_pending_t *pending =
      find_pending(ctx, deserialized->packetIdentifier);
  uint8_t kind = packet->type & MQTT_PACKET_KIND_MASK;
  if (pending == NULL ||
      (kind == MQTT_PACKET_TYPE_PUBACK &&
       pending->event != ESY_MQTT_EVENT_PUBLISHED) ||
      (kind == MQTT_PACKET_TYPE_SUBACK &&
       pending->event != ESY_MQTT_EVENT_SUBSCRIBED) ||
      (kind == MQTT_PACKET_TYPE_UNSUBACK &&
       pending->event != ESY_MQTT_EVENT_UNSUBSCRIBED)) {
    ctx->event_error = ESY_ERR_IO;
    return;
  }
  if ((uint32_t)(ctx->get_time_ms() - pending->started_ms) >=
      ctx->acknowledgement_timeout_ms) {
    ctx->event_error = ESY_ERR_TIMEOUT;
    return;
  }
  esy_err_t result = ESY_OK;
  esy_mqtt_qos_t granted = ESY_MQTT_QOS_0;
  if (kind == MQTT_PACKET_TYPE_SUBACK) {
    uint8_t *codes = NULL;
    size_t count = 0u;
    if (MQTT_GetSubAckStatusCodes(packet, &codes, &count) != MQTTSuccess ||
        count != 1u) {
      ctx->event_error = ESY_ERR_IO;
      return;
    }
    if (codes[0] == MQTTSubAckFailure) {
      result = ESY_ERR_NOT_SUPPORTED;
    } else if (codes[0] > (uint8_t)pending->requested_qos) {
      ctx->event_error = ESY_ERR_IO;
      return;
    } else {
      granted = (esy_mqtt_qos_t)codes[0];
    }
  }
  finish_pending(ctx, pending, result, granted);
}

static bool ping_response_expired(esy_mqtt_ctx_t *ctx) {
  return ctx->mqtt.keepAliveIntervalSec != 0u &&
         ctx->mqtt.waitingForPingResp &&
         (uint32_t)(ctx->get_time_ms() - ctx->mqtt.pingReqSendTimeMs) >
             MQTT_PINGRESP_TIMEOUT_MS;
}

static void mqtt_event(MQTTContext_t *mqtt, MQTTPacketInfo_t *packet,
                        MQTTDeserializedInfo_t *deserialized) {
  esy_mqtt_ctx_t *ctx = mqtt->transportInterface.pNetworkContext->owner;
  switch (packet->type & MQTT_PACKET_KIND_MASK) {
  case MQTT_PACKET_TYPE_PUBLISH:
    receive_publish(ctx, deserialized);
    break;
  case MQTT_PACKET_TYPE_PUBACK:
  case MQTT_PACKET_TYPE_SUBACK:
  case MQTT_PACKET_TYPE_UNSUBACK:
    receive_ack(ctx, packet, deserialized);
    break;
  case MQTT_PACKET_TYPE_PINGRESP:
    if (ping_response_expired(ctx)) {
      ctx->event_error = ESY_ERR_TIMEOUT;
    } else {
      mqtt->waitingForPingResp = false;
    }
    break;
  default:
    break;
  }
}

static esy_err_t initialize_protocol(esy_mqtt_ctx_t *ctx) {
  TransportInterface_t transport = {0};
  transport.send = transport_send;
  transport.recv = transport_recv;
  transport.pNetworkContext = &ctx->network;
  MQTTFixedBuffer_t buffer = {ctx->resources.network_buffer,
                              ctx->resources.network_buffer_size};
  memset(ctx->outgoing, 0, sizeof(ctx->outgoing));
  memset(ctx->incoming, 0, sizeof(ctx->incoming));
  memset(ctx->pending, 0, sizeof(ctx->pending));
  MQTTStatus_t status =
      MQTT_Init(&ctx->mqtt, &transport, ctx->get_time_ms, mqtt_event, &buffer);
  if (status == MQTTSuccess) {
    status = MQTT_InitStatefulQoS(&ctx->mqtt, ctx->outgoing,
                                  ESY_MQTT_MAX_PENDING_OPERATIONS, ctx->incoming,
                                  ESY_MQTT_MAX_PENDING_OPERATIONS);
  }
  return map_status(status);
}

static esy_err_t enter_client(esy_mqtt_t *client, bool require_connection) {
  if (client == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  esy_mqtt_ctx_t *ctx = client_context(client);
  if (!ctx->initialized) {
    return ESY_ERR_STATE;
  }
  if (ctx->busy) {
    return ESY_ERR_BUSY;
  }
  if (require_connection && !ctx->connected) {
    return ESY_ERR_STATE;
  }
  ctx->busy = true;
  return ESY_OK;
}

static esy_err_t finish_send(esy_mqtt_ctx_t *ctx, MQTTStatus_t status) {
  esy_err_t result = map_status(status);
  if (status != MQTTSuccess && status != MQTTNoMemory &&
      status != MQTTBadParameter) {
    retire_connection(ctx, result, result);
  }
  ctx->busy = false;
  return result;
}

esy_err_t esy_mqtt_init(esy_mqtt_t *client, const esy_mqtt_config_t *config) {
  if (client == NULL || config == NULL || config->static_resources == NULL ||
      config->static_resources->network_buffer == NULL ||
      config->static_resources->network_buffer_size < ESY_MQTT_MIN_BUFFER_SIZE ||
      config->transport.send == NULL || config->transport.recv == NULL ||
      config->get_time_ms == NULL || config->event_cb == NULL ||
      config->acknowledgement_timeout_ms == 0u ||
      config->acknowledgement_timeout_ms > INT32_MAX) {
    return ESY_ERR_INVALID_ARG;
  }
  esy_mqtt_ctx_t *ctx = client_context(client);
  if (ctx->initialized) {
    return ctx->busy ? ESY_ERR_BUSY : ESY_ERR_STATE;
  }
  memset(client, 0, sizeof(*client));
  ctx->network.transport = config->transport;
  ctx->network.owner = ctx;
  ctx->resources = *config->static_resources;
  ctx->get_time_ms = config->get_time_ms;
  ctx->acknowledgement_timeout_ms = config->acknowledgement_timeout_ms;
  ctx->event_cb = config->event_cb;
  ctx->event_context = config->event_context;
  ctx->initialized = true;
  return ESY_OK;
}

esy_err_t esy_mqtt_connect(esy_mqtt_t *client,
                           const esy_mqtt_connect_config_t *config) {
  if (config == NULL || !valid_utf8(config->client_id, false) ||
      !valid_utf8(config->username, true) ||
      (config->password.length > 0u && config->password.data == NULL) ||
      (config->password.data != NULL && config->username.data == NULL) ||
      config->timeout_ms == 0u || config->timeout_ms > INT32_MAX ||
      (config->will != NULL && !valid_message(config->will))) {
    return ESY_ERR_INVALID_ARG;
  }
  esy_err_t result = enter_client(client, false);
  if (result != ESY_OK) {
    return result;
  }
  esy_mqtt_ctx_t *ctx = client_context(client);
  if (ctx->connected) {
    ctx->busy = false;
    return ESY_ERR_STATE;
  }
  result = initialize_protocol(ctx);
  if (result == ESY_OK) {
    MQTTConnectInfo_t info = {0};
    info.cleanSession = true;
    info.keepAliveSeconds = config->keep_alive_seconds;
    info.pClientIdentifier = config->client_id.data;
    info.clientIdentifierLength = config->client_id.length;
    info.pUserName = config->username.data;
    info.userNameLength = config->username.length;
    info.pPassword = config->password.data;
    info.passwordLength = config->password.length;
    MQTTPublishInfo_t will = {0};
    if (config->will != NULL) {
      will = publish_info(config->will);
    }
    bool session_present = false;
    result = map_status(MQTT_Connect(&ctx->mqtt, &info,
                                     config->will == NULL ? NULL : &will,
                                     config->timeout_ms, &session_present));
    if (result == ESY_OK && session_present) {
      result = ESY_ERR_IO;
    }
    ctx->connected = result == ESY_OK;
    if (ctx->connected) {
      ctx->mqtt.lastPacketRxTime = ctx->get_time_ms();
    }
  }
  ctx->busy = false;
  return result;
}

esy_err_t esy_mqtt_publish(esy_mqtt_t *client,
                           const esy_mqtt_message_t *message,
                           uint16_t *packet_id) {
  if (!valid_message(message) || packet_id == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  esy_err_t result = enter_client(client, true);
  if (result != ESY_OK) {
    return result;
  }
  esy_mqtt_ctx_t *ctx = client_context(client);
  esy_mqtt_pending_t *pending = NULL;
  uint16_t id = 0u;
  if (message->qos == ESY_MQTT_QOS_1) {
    pending = free_pending(ctx);
    if (pending == NULL) {
      ctx->busy = false;
      return ESY_ERR_NO_MEM;
    }
    id = next_packet_id(ctx);
  }
  MQTTPublishInfo_t info = publish_info(message);
  MQTTStatus_t status = MQTT_Publish(&ctx->mqtt, &info, id);
  if (status == MQTTSuccess) {
    if (pending != NULL) {
      record_pending(ctx, pending, id, ESY_MQTT_EVENT_PUBLISHED, message->qos);
    }
    *packet_id = id;
  }
  return finish_send(ctx, status);
}

static esy_err_t subscription_command(esy_mqtt_t *client,
                                       esy_mqtt_string_t filter,
                                       esy_mqtt_qos_t qos, uint16_t *packet_id,
                                       bool subscribe) {
  if (!valid_topic(filter, true) || !valid_qos(qos) || packet_id == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  esy_err_t result = enter_client(client, true);
  if (result != ESY_OK) {
    return result;
  }
  esy_mqtt_ctx_t *ctx = client_context(client);
  esy_mqtt_pending_t *pending = free_pending(ctx);
  if (pending == NULL) {
    ctx->busy = false;
    return ESY_ERR_NO_MEM;
  }
  MQTTSubscribeInfo_t info = {0};
  info.pTopicFilter = filter.data;
  info.topicFilterLength = filter.length;
  info.qos = (MQTTQoS_t)qos;
  uint16_t id = next_packet_id(ctx);
  MQTTStatus_t status = subscribe
                            ? MQTT_Subscribe(&ctx->mqtt, &info, 1u, id)
                            : MQTT_Unsubscribe(&ctx->mqtt, &info, 1u, id);
  if (status == MQTTSuccess) {
    record_pending(ctx, pending, id,
                    subscribe ? ESY_MQTT_EVENT_SUBSCRIBED
                              : ESY_MQTT_EVENT_UNSUBSCRIBED,
                    qos);
    *packet_id = id;
  }
  return finish_send(ctx, status);
}

esy_err_t esy_mqtt_subscribe(esy_mqtt_t *client, esy_mqtt_string_t filter,
                             esy_mqtt_qos_t qos, uint16_t *packet_id) {
  return subscription_command(client, filter, qos, packet_id, true);
}

esy_err_t esy_mqtt_unsubscribe(esy_mqtt_t *client, esy_mqtt_string_t filter,
                               uint16_t *packet_id) {
  return subscription_command(client, filter, ESY_MQTT_QOS_0, packet_id, false);
}

static esy_err_t check_acknowledgement_deadlines(esy_mqtt_ctx_t *ctx) {
  uint32_t now = ctx->get_time_ms();
  for (size_t index = 0u; index < ESY_MQTT_MAX_PENDING_OPERATIONS; ++index) {
    const esy_mqtt_pending_t *pending = &ctx->pending[index];
    if (pending->active &&
        (uint32_t)(now - pending->started_ms) >=
            ctx->acknowledgement_timeout_ms) {
      return ESY_ERR_TIMEOUT;
    }
  }
  return ESY_OK;
}

static esy_err_t maintain_keepalive(esy_mqtt_ctx_t *ctx) {
  if (ctx->mqtt.keepAliveIntervalSec == 0u) {
    return ESY_OK;
  }
  if (ctx->mqtt.waitingForPingResp) {
    return ping_response_expired(ctx) ? ESY_ERR_TIMEOUT : ESY_OK;
  }
  uint32_t now = ctx->get_time_ms();
  uint32_t tx_timeout_ms = 1000u * (uint32_t)ctx->mqtt.keepAliveIntervalSec;
  if (PACKET_TX_TIMEOUT_MS < tx_timeout_ms) {
    tx_timeout_ms = PACKET_TX_TIMEOUT_MS;
  }
  uint32_t rx_elapsed_ms = (uint32_t)(now - ctx->mqtt.lastPacketRxTime);
  if ((tx_timeout_ms != 0u &&
       (uint32_t)(now - ctx->mqtt.lastPacketTxTime) >= tx_timeout_ms) ||
      (rx_elapsed_ms != 0u && rx_elapsed_ms >= PACKET_RX_TIMEOUT_MS)) {
    return map_status(MQTT_Ping(&ctx->mqtt));
  }
  return ESY_OK;
}

esy_err_t esy_mqtt_process(esy_mqtt_t *client) {
  esy_err_t result = enter_client(client, true);
  if (result != ESY_OK) {
    return result;
  }
  esy_mqtt_ctx_t *ctx = client_context(client);
  result = check_acknowledgement_deadlines(ctx);
  if (result == ESY_OK && ping_response_expired(ctx)) {
    result = ESY_ERR_TIMEOUT;
  }
  if (result != ESY_OK) {
    retire_connection(ctx, result, result);
    ctx->busy = false;
    return result;
  }
  ctx->event_error = ESY_OK;
  /* The upstream process loop checks keep-alive only when recv returns zero. */
  MQTTStatus_t status = MQTT_ReceiveLoop(&ctx->mqtt);
  if (ctx->event_error != ESY_OK) {
    result = ctx->event_error;
  } else if (status == MQTTSuccess || status == MQTTNoDataAvailable ||
             status == MQTTNeedMoreBytes || status == MQTTServerRefused) {
    result = ESY_OK;
  } else {
    result = map_status(status);
  }
  if (result == ESY_OK) {
    result = check_acknowledgement_deadlines(ctx);
  }
  if (result == ESY_OK) {
    result = maintain_keepalive(ctx);
  }
  if (result != ESY_OK) {
    retire_connection(ctx, result, result);
  }
  ctx->busy = false;
  return result;
}

esy_err_t esy_mqtt_disconnect(esy_mqtt_t *client) {
  esy_err_t result = enter_client(client, false);
  if (result != ESY_OK) {
    return result;
  }
  esy_mqtt_ctx_t *ctx = client_context(client);
  if (ctx->connected) {
    result = map_status(MQTT_Disconnect(&ctx->mqtt));
    retire_connection(ctx, result, ESY_ERR_STATE);
  }
  ctx->busy = false;
  return result;
}

esy_err_t esy_mqtt_abort(esy_mqtt_t *client) {
  esy_err_t result = enter_client(client, false);
  if (result != ESY_OK) {
    return result;
  }
  esy_mqtt_ctx_t *ctx = client_context(client);
  retire_connection(ctx, ESY_ERR_STATE, ESY_ERR_STATE);
  ctx->busy = false;
  return ESY_OK;
}

esy_err_t esy_mqtt_deinit(esy_mqtt_t *client) {
  esy_err_t result = enter_client(client, false);
  if (result != ESY_OK) {
    return result;
  }
  esy_mqtt_ctx_t *ctx = client_context(client);
  if (ctx->connected) {
    ctx->busy = false;
    return ESY_ERR_STATE;
  }
  memset(client, 0, sizeof(*client));
  return ESY_OK;
}

bool esy_mqtt_is_connected(const esy_mqtt_t *client) {
  return client != NULL && const_client_context(client)->initialized &&
         const_client_context(client)->connected;
}
