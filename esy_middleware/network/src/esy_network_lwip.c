/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/network/esy_network_lwip.h"

#include <limits.h>
#include <string.h>

#include "esy_common/ds/esy_ring_buffer.h"
#include "lwip/opt.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#if LWIP_DNS
#include "lwip/dns.h"
#endif

typedef struct {
  esy_ring_buffer_t rx_ring;
  struct tcp_pcb *pcb;
  struct tcp_pcb *listener;
  uint64_t session_id;
  uint32_t unacked_bytes;
  struct pbuf *rx_pending;
  uint16_t rx_pending_offset;
  uint16_t port;
  esy_network_state_t state;
  bool initialized;
  bool dns_pending;
  bool in_progress;
  void (*progress)(void *context);
  void *progress_context;
  void (*owner_enter)(void *context);
  void (*owner_leave)(void *context);
  void *owner_context;
} esy_network_lwip_ctx_t;

_Static_assert(sizeof(esy_network_lwip_ctx_t) <= ESY_NETWORK_LWIP_STORAGE_BYTES,
               "ESY_NETWORK_LWIP_STORAGE_BYTES is too small");
_Static_assert(_Alignof(esy_network_lwip_t) >= _Alignof(esy_network_lwip_ctx_t),
               "Network lwIP handle alignment is insufficient");

static esy_network_lwip_ctx_t *get_ctx(esy_network_lwip_t *handle) {
  return handle == NULL ? NULL :
      (esy_network_lwip_ctx_t *)(void *)handle->priv.bytes;
}

static void enter_owner(esy_network_lwip_ctx_t *ctx) {
  if (ctx->owner_enter != NULL) ctx->owner_enter(ctx->owner_context);
}

static void leave_owner(esy_network_lwip_ctx_t *ctx) {
  if (ctx->owner_leave != NULL) ctx->owner_leave(ctx->owner_context);
}

static err_t sent_callback(void *argument, struct tcp_pcb *pcb, u16_t size) {
  esy_network_lwip_ctx_t *ctx = argument;
  (void)pcb;
  ctx->unacked_bytes = size >= ctx->unacked_bytes ?
      0u : ctx->unacked_bytes - size;
  return ERR_OK;
}

static void free_pending(esy_network_lwip_ctx_t *ctx) {
  if (ctx->rx_pending != NULL) {
    pbuf_free(ctx->rx_pending);
    ctx->rx_pending = NULL;
  }
  ctx->rx_pending_offset = 0u;
}

static void discard_rx(esy_network_lwip_ctx_t *ctx) {
  free_pending(ctx);
  esy_ring_buffer_reset(&ctx->rx_ring);
}

static bool fill_rx(esy_network_lwip_ctx_t *ctx) {
  while (ctx->rx_pending != NULL) {
    uint8_t *destination;
    uint32_t span = esy_ring_buffer_get_write_ptr(&ctx->rx_ring, &destination);
    const uint16_t remaining =
        (uint16_t)(ctx->rx_pending->tot_len - ctx->rx_pending_offset);
    if (remaining == 0u) {
      free_pending(ctx);
      continue;
    }
    if (span == 0u) break;
    if (span > remaining) span = remaining;
    if (pbuf_copy_partial(ctx->rx_pending, destination, (u16_t)span,
                          ctx->rx_pending_offset) != span) {
      discard_rx(ctx);
      ctx->state = ESY_NETWORK_ERROR;
      return false;
    }
    esy_ring_buffer_commit_write(&ctx->rx_ring, span);
    ctx->rx_pending_offset = (uint16_t)(ctx->rx_pending_offset + span);
    if (ctx->rx_pending_offset == ctx->rx_pending->tot_len) free_pending(ctx);
  }
  return true;
}

static void error_callback(void *argument, err_t error) {
  esy_network_lwip_ctx_t *ctx = argument;
  (void)error;
  /* lwIP has already retired the PCB; only accepted pbufs belong to us. */
  ctx->pcb = NULL;
  ctx->unacked_bytes = 0u;
  discard_rx(ctx);
  ctx->state = ctx->state == ESY_NETWORK_CLOSING ?
      (ctx->listener == NULL ? ESY_NETWORK_IDLE : ESY_NETWORK_CLOSING) :
      ESY_NETWORK_ERROR;
}

static err_t receive_callback(void *argument, struct tcp_pcb *pcb,
                               struct pbuf *packet, err_t error) {
  esy_network_lwip_ctx_t *ctx = argument;
  (void)pcb;
  if (ctx->state == ESY_NETWORK_CLOSING) {
    /* Refusal retains provider ownership until a successful close/redelivery. */
    return packet == NULL ? ERR_OK : ERR_MEM;
  }
  if (error != ERR_OK) {
    if (packet != NULL) pbuf_free(packet);
    discard_rx(ctx);
    ctx->state = ESY_NETWORK_ERROR;
    return ERR_OK;
  }
  if (packet == NULL) {
    ctx->state = ESY_NETWORK_CLOSED;
    return ERR_OK;
  }
  if (ctx->rx_pending != NULL) return ERR_MEM;
  ctx->rx_pending = packet;
  ctx->rx_pending_offset = 0u;
  (void)fill_rx(ctx);
  return ERR_OK;
}

static err_t connect_callback(void *argument, struct tcp_pcb *pcb,
                               err_t error) {
  esy_network_lwip_ctx_t *ctx = argument;
  (void)pcb;
  if (ctx->state != ESY_NETWORK_CLOSING) {
    if (error == ERR_OK && ctx->session_id != UINT64_MAX) {
      ++ctx->session_id;
      tcp_nagle_disable(pcb);
      ctx->state = ESY_NETWORK_CONNECTED;
    } else {
      ctx->state = ESY_NETWORK_ERROR;
    }
  }
  return ERR_OK;
}

static esy_err_t start_tcp(esy_network_lwip_ctx_t *ctx, const ip_addr_t *address) {
  err_t result;
  if (ctx->session_id == UINT64_MAX) return ESY_ERR_STATE;
  struct tcp_pcb *pcb = tcp_new_ip_type(IP_GET_TYPE(address));
  if (pcb == NULL) {
    ctx->state = ESY_NETWORK_ERROR;
    return ESY_ERR_NO_MEM;
  }
  ctx->pcb = pcb;
  ctx->state = ESY_NETWORK_CONNECTING;
  tcp_arg(pcb, ctx);
  tcp_err(pcb, error_callback);
  tcp_recv(pcb, receive_callback);
  tcp_sent(pcb, sent_callback);
  result = tcp_connect(pcb, address, ctx->port, connect_callback);
  if (result != ERR_OK) {
    /* No connection was established. Retire the failed native control object. */
    tcp_arg(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_abort(pcb);
    ctx->pcb = NULL;
    ctx->state = ESY_NETWORK_ERROR;
    return result == ERR_MEM ? ESY_ERR_NO_MEM : ESY_ERR_IO;
  }
  return ESY_OK;
}

#if LWIP_DNS
/* Continue the explicit begin lifecycle on the same serialized network owner. */
static void dns_callback(const char *hostname, const ip_addr_t *address,
                          void *argument) {
  esy_network_lwip_ctx_t *ctx = argument;
  (void)hostname;
  ctx->dns_pending = false;
  if (ctx->state == ESY_NETWORK_CLOSING) {
    ctx->state = ESY_NETWORK_IDLE;
    return;
  }
  if (address == NULL) {
    ctx->state = ESY_NETWORK_ERROR;
    return;
  }
  (void)start_tcp(ctx, address);
}
#endif

static esy_err_t progress_io(esy_network_lwip_ctx_t *ctx) {
  if (ctx == NULL || !ctx->initialized) return ESY_ERR_STATE;
  if (ctx->in_progress) return ESY_ERR_BUSY;
  if (ctx->progress != NULL) {
    ctx->in_progress = true;
    ctx->progress(ctx->progress_context);
    ctx->in_progress = false;
  }
  return ESY_OK;
}

static int32_t write_bytes(esy_network_lwip_ctx_t *ctx, const void *data,
                           size_t size, bool all) {
  size_t count;
  err_t result;
  if ((ctx->state != ESY_NETWORK_CONNECTED && ctx->state != ESY_NETWORK_CLOSED) ||
      ctx->pcb == NULL) {
    return ESY_ERR_STATE;
  }
  if (data == NULL && size != 0u) return ESY_ERR_INVALID_ARG;
  if (size == 0u) return 0;
  if (all && size > UINT16_MAX) return ESY_ERR_INVALID_ARG;
  count = tcp_sndbuf(ctx->pcb);
  if (all && count < size) return 0;
  if (count > size) count = size;
  if (count > UINT16_MAX) count = UINT16_MAX;
  if (count == 0u) return 0;
  if (count > UINT32_MAX - ctx->unacked_bytes) return ESY_ERR_BUSY;
  result = tcp_write(ctx->pcb, data, (u16_t)count, TCP_WRITE_FLAG_COPY);
  if (result == ERR_MEM) return 0;
  if (result != ERR_OK) {
    ctx->state = ESY_NETWORK_ERROR;
    return ESY_ERR_IO;
  }
  ctx->unacked_bytes += (uint32_t)count;
  /* Accepted bytes are queued even if immediate output cannot progress. lwIP
   * timers/ACK handling retry output; reporting failure here risks duplication. */
  (void)tcp_output(ctx->pcb);
  return (int32_t)count;
}

static int32_t read_bytes(esy_network_lwip_ctx_t *ctx, void *data, size_t size) {
  size_t count = 0u;
  if ((ctx->state != ESY_NETWORK_CONNECTED &&
       ctx->state != ESY_NETWORK_CLOSED) || ctx->pcb == NULL) {
    return ESY_ERR_STATE;
  }
  if (data == NULL && size != 0u) return ESY_ERR_INVALID_ARG;
  if (size > INT32_MAX) size = INT32_MAX;
  if (!fill_rx(ctx)) return ESY_ERR_IO;
  while (count < size) {
    uint8_t *source;
    uint32_t span = esy_ring_buffer_get_read_ptr(&ctx->rx_ring, &source);
    if (span == 0u) break;
    if (span > size - count) span = (uint32_t)(size - count);
    memcpy((uint8_t *)data + count, source, span);
    esy_ring_buffer_commit_read(&ctx->rx_ring, span);
    count += span;
    /* tcp_recved uses a 16-bit length even for a larger caller receive. */
    for (uint32_t acknowledged = 0u; acknowledged < span;) {
      uint32_t chunk = span - acknowledged;
      if (chunk > UINT16_MAX) chunk = UINT16_MAX;
      tcp_recved(ctx->pcb, (u16_t)chunk);
      acknowledged += chunk;
    }
    if (!fill_rx(ctx)) return ESY_ERR_IO;
  }
  if (count == 0u && size != 0u && ctx->state == ESY_NETWORK_CLOSED) {
    return ESY_ERR_IO;
  }
  return (int32_t)count;
}

static int32_t send_common(void *argument, const void *data, size_t size,
                            bool all) {
  esy_network_lwip_ctx_t *ctx = argument;
  if (ctx == NULL || !ctx->initialized) return ESY_ERR_STATE;
  if (ctx->in_progress) return ESY_ERR_BUSY;
  enter_owner(ctx);
  int32_t result = write_bytes(ctx, data, size, all);
  leave_owner(ctx);
  return result;
}

static int32_t send_bytes(void *argument, const void *data, size_t size) {
  return send_common(argument, data, size, false);
}

static int32_t send_all_bytes(void *argument, const void *data, size_t size) {
  return send_common(argument, data, size, true);
}

static int32_t receive_bytes(void *argument, void *data, size_t size) {
  esy_network_lwip_ctx_t *ctx = argument;
  if (ctx == NULL || !ctx->initialized) return ESY_ERR_STATE;
  if (ctx->in_progress) return ESY_ERR_BUSY;
  enter_owner(ctx);
  int32_t result = read_bytes(ctx, data, size);
  leave_owner(ctx);
  return result;
}

static esy_err_t stream_progress(void *argument) {
  return progress_io(argument);
}

static esy_err_t stream_status(void *argument,
                               esy_network_connection_status_t *status) {
  esy_network_lwip_ctx_t *ctx = argument;
  if (ctx == NULL || status == NULL) return ESY_ERR_INVALID_ARG;
  if (!ctx->initialized) return ESY_ERR_STATE;
  if (ctx->in_progress) return ESY_ERR_BUSY;
  enter_owner(ctx);
  status->state = ctx->state == ESY_NETWORK_CONNECTED ? ESY_NETWORK_CONNECTION_OPEN :
      ctx->state == ESY_NETWORK_CLOSED ? ESY_NETWORK_CONNECTION_EOF :
      ESY_NETWORK_CONNECTION_INACTIVE;
  status->session_id = ctx->session_id;
  status->unacked_bytes = ctx->unacked_bytes;
  status->send_capacity =
      (ctx->state == ESY_NETWORK_CONNECTED || ctx->state == ESY_NETWORK_CLOSED) &&
      ctx->pcb != NULL ?
      tcp_sndbuf(ctx->pcb) : 0u;
  status->rx_available = esy_ring_buffer_used(&ctx->rx_ring);
  if (ctx->rx_pending != NULL) {
    status->rx_available += ctx->rx_pending->tot_len - ctx->rx_pending_offset;
  }
  leave_owner(ctx);
  return ESY_OK;
}

static err_t accept_callback(void *argument, struct tcp_pcb *pcb, err_t error) {
  esy_network_lwip_ctx_t *ctx = argument;
  if (pcb == NULL) return ERR_ABRT;
  if (error != ERR_OK || ctx->state != ESY_NETWORK_LISTENING ||
      ctx->pcb != NULL || ctx->session_id == UINT64_MAX) {
    tcp_abort(pcb);
    return ERR_ABRT;
  }
  ctx->pcb = pcb;
  ++ctx->session_id;
  ctx->state = ESY_NETWORK_CONNECTED;
  ctx->unacked_bytes = 0u;
  tcp_arg(pcb, ctx);
  tcp_recv(pcb, receive_callback);
  tcp_sent(pcb, sent_callback);
  tcp_err(pcb, error_callback);
  tcp_nagle_disable(pcb);
  return ERR_OK;
}

esy_err_t esy_network_lwip_init(esy_network_lwip_t *handle,
                            const esy_network_lwip_config_t *config) {
  esy_network_lwip_ctx_t *ctx = get_ctx(handle);
  const esy_network_lwip_static_resources_t *resources;
  if (ctx == NULL || config == NULL || config->static_resources == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  resources = config->static_resources;
  if (resources->rx_buffer == NULL || resources->rx_buffer_size == 0u ||
      resources->rx_buffer_size > INT32_MAX ||
      (resources->rx_buffer_size & (resources->rx_buffer_size - 1u)) != 0u) {
    return ESY_ERR_INVALID_ARG;
  }
  if ((config->owner_enter == NULL) != (config->owner_leave == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (ctx->initialized) return ESY_ERR_STATE;
  memset(ctx, 0, sizeof(*ctx));
  (void)esy_ring_buffer_init(&ctx->rx_ring, resources->rx_buffer,
                             resources->rx_buffer_size);
  ctx->initialized = true;
  ctx->progress = config->progress;
  ctx->progress_context = config->progress_context;
  ctx->owner_enter = config->owner_enter;
  ctx->owner_leave = config->owner_leave;
  ctx->owner_context = config->owner_context;
  ctx->state = ESY_NETWORK_IDLE;
  return ESY_OK;
}

static esy_err_t begin_connection(esy_network_lwip_t *handle, const char *host,
                                   uint16_t port) {
  esy_network_lwip_ctx_t *ctx = get_ctx(handle);
  ip_addr_t address;
  if (ctx == NULL || host == NULL || host[0] == '\0' || port == 0u) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!ctx->initialized || ctx->state != ESY_NETWORK_IDLE) {
    return ESY_ERR_STATE;
  }
  if (ctx->in_progress) return ESY_ERR_BUSY;
  ctx->port = port;
  if (ipaddr_aton(host, &address)) return start_tcp(ctx, &address);
#if LWIP_DNS
  {
    err_t result;
    ctx->state = ESY_NETWORK_RESOLVING;
    ctx->dns_pending = true;
    result = dns_gethostbyname(host, &address, dns_callback, ctx);
    if (result == ERR_INPROGRESS) return ESY_OK;
    ctx->dns_pending = false;
    if (result == ERR_OK) return start_tcp(ctx, &address);
    ctx->state = ESY_NETWORK_ERROR;
    return result == ERR_MEM ? ESY_ERR_NO_MEM : ESY_ERR_IO;
  }
#else
  ctx->state = ESY_NETWORK_ERROR;
  return ESY_ERR_NOT_SUPPORTED;
#endif
}

esy_err_t esy_network_lwip_begin(esy_network_lwip_t *handle, const char *host,
                                uint16_t port) {
  esy_network_lwip_ctx_t *ctx = get_ctx(handle);
  if (ctx == NULL) return ESY_ERR_INVALID_ARG;
  if (!ctx->initialized) return ESY_ERR_STATE;
  if (ctx->in_progress) return ESY_ERR_BUSY;
  enter_owner(ctx);
  esy_err_t result = begin_connection(handle, host, port);
  leave_owner(ctx);
  return result;
}

esy_err_t esy_network_lwip_listen(esy_network_lwip_t *handle, uint16_t port,
                                 uint8_t backlog) {
  esy_network_lwip_ctx_t *ctx = get_ctx(handle);
  if (ctx == NULL || port == 0u || backlog == 0u) return ESY_ERR_INVALID_ARG;
  if (!ctx->initialized || ctx->state != ESY_NETWORK_IDLE) return ESY_ERR_STATE;
  if (ctx->in_progress) return ESY_ERR_BUSY;
  enter_owner(ctx);
  struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
  esy_err_t result = ESY_OK;
  if (pcb == NULL) {
    result = ESY_ERR_NO_MEM;
  } else {
    err_t error = tcp_bind(pcb, IP_ANY_TYPE, port);
    if (error == ERR_OK) {
      ctx->listener = tcp_listen_with_backlog(pcb, backlog);
      if (ctx->listener == NULL) error = ERR_MEM;
    }
    if (error != ERR_OK) {
      tcp_abort(pcb);
      result = error == ERR_MEM ? ESY_ERR_NO_MEM : ESY_ERR_IO;
    } else {
      tcp_arg(ctx->listener, ctx);
      tcp_accept(ctx->listener, accept_callback);
    }
  }
  ctx->state = result == ESY_OK ? ESY_NETWORK_LISTENING : ESY_NETWORK_ERROR;
  leave_owner(ctx);
  return result;
}

esy_network_state_t esy_network_lwip_get_state(const esy_network_lwip_t *handle) {
  const esy_network_lwip_ctx_t *ctx = handle == NULL ? NULL :
      (const esy_network_lwip_ctx_t *)(const void *)handle->priv.bytes;
  return ctx == NULL || !ctx->initialized ?
      ESY_NETWORK_UNINITIALIZED : ctx->state;
}

esy_err_t esy_network_lwip_get_stream(esy_network_lwip_t *handle,
                                     esy_network_stream_t *transport) {
  esy_network_lwip_ctx_t *ctx = get_ctx(handle);
  if (ctx == NULL || transport == NULL) return ESY_ERR_INVALID_ARG;
  if (!ctx->initialized) return ESY_ERR_STATE;
  transport->progress = stream_progress;
  transport->send = send_bytes;
  transport->recv = receive_bytes;
  transport->context = ctx;
  return ESY_OK;
}

esy_err_t esy_network_lwip_get_connection(esy_network_lwip_t *handle,
                                         esy_network_connection_t *connection) {
  if (connection == NULL) return ESY_ERR_INVALID_ARG;
  esy_network_connection_t result = {0};
  esy_err_t error = esy_network_lwip_get_stream(handle, &result.stream);
  if (error != ESY_OK) return error;
  result.send_all = send_all_bytes;
  result.get_status = stream_status;
  *connection = result;
  return ESY_OK;
}

static esy_err_t retire_peer(esy_network_lwip_ctx_t *ctx) {
  ctx->state = ESY_NETWORK_CLOSING;
  if (ctx->dns_pending) return ESY_ERR_BUSY;
  if (ctx->pcb != NULL) {
    tcp_arg(ctx->pcb, NULL);
    tcp_recv(ctx->pcb, NULL);
    tcp_sent(ctx->pcb, NULL);
    tcp_err(ctx->pcb, NULL);
    err_t result = tcp_close(ctx->pcb);
    if (result != ERR_OK) {
      tcp_arg(ctx->pcb, ctx);
      tcp_recv(ctx->pcb, receive_callback);
      tcp_sent(ctx->pcb, sent_callback);
      tcp_err(ctx->pcb, error_callback);
      return result == ERR_MEM ? ESY_ERR_BUSY : ESY_ERR_IO;
    }
    ctx->pcb = NULL;
  }
  ctx->unacked_bytes = 0u;
  discard_rx(ctx);
  ctx->state = ctx->listener != NULL ? ESY_NETWORK_LISTENING : ESY_NETWORK_IDLE;
  return ESY_OK;
}

esy_err_t esy_network_lwip_close_peer(esy_network_lwip_t *handle) {
  esy_network_lwip_ctx_t *ctx = get_ctx(handle);
  if (ctx == NULL) return ESY_ERR_INVALID_ARG;
  if (!ctx->initialized) return ESY_ERR_STATE;
  if (ctx->in_progress) return ESY_ERR_BUSY;
  enter_owner(ctx);
  esy_err_t result = retire_peer(ctx);
  leave_owner(ctx);
  return result;
}

esy_err_t esy_network_lwip_close(esy_network_lwip_t *handle) {
  esy_network_lwip_ctx_t *ctx = get_ctx(handle);
  if (ctx == NULL) return ESY_ERR_INVALID_ARG;
  if (!ctx->initialized) return ESY_ERR_STATE;
  if (ctx->in_progress) return ESY_ERR_BUSY;
  enter_owner(ctx);
  esy_err_t result = retire_peer(ctx);
  if (result == ESY_OK && ctx->listener != NULL) {
    tcp_arg(ctx->listener, NULL);
    tcp_accept(ctx->listener, NULL);
    err_t error = tcp_close(ctx->listener);
    if (error == ERR_OK) {
      ctx->listener = NULL;
      ctx->state = ESY_NETWORK_IDLE;
    } else {
      tcp_arg(ctx->listener, ctx);
      tcp_accept(ctx->listener, accept_callback);
      ctx->state = ESY_NETWORK_CLOSING;
      result = error == ERR_MEM ? ESY_ERR_BUSY : ESY_ERR_IO;
    }
  }
  leave_owner(ctx);
  return result;
}

esy_err_t esy_network_lwip_deinit(esy_network_lwip_t *handle) {
  esy_err_t result = esy_network_lwip_close(handle);
  if (result == ESY_OK) memset(handle, 0, sizeof(*handle));
  return result;
}
