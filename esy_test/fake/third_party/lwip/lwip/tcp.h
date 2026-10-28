/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_TEST_FAKE_THIRD_PARTY_LWIP_LWIP_TCP_H
#define ESY_TEST_FAKE_THIRD_PARTY_LWIP_LWIP_TCP_H
#include <stdbool.h>
#include "ip_addr.h"
#include "pbuf.h"
typedef int err_t;
#define ERR_OK              0
#define ERR_MEM             -1
#define ERR_ABRT            -13
#define TCP_WRITE_FLAG_COPY 1
#define TCP_SND_BUF         4096
struct tcp_pcb;
typedef err_t (*tcp_recv_fn)(void*, struct tcp_pcb*, struct pbuf*, err_t);
typedef err_t (*tcp_sent_fn)(void*, struct tcp_pcb*, u16_t);
typedef err_t (*tcp_accept_fn)(void*, struct tcp_pcb*, err_t);
typedef void (*tcp_err_fn)(void*, err_t);
struct tcp_pcb {
  void* arg;
  tcp_recv_fn recv;
  tcp_sent_fn sent;
  tcp_accept_fn accept, connected;
  tcp_err_fn error;
  struct pbuf* refused_data; /**< Provider-owned RX awaiting explicit redelivery. */
  bool used, listening;
};
struct tcp_pcb* tcp_new_ip_type(int type);
void tcp_arg(struct tcp_pcb* p, void* arg);
void tcp_recv(struct tcp_pcb* p, tcp_recv_fn fn);
void tcp_sent(struct tcp_pcb* p, tcp_sent_fn fn);
void tcp_accept(struct tcp_pcb* p, tcp_accept_fn fn);
void tcp_err(struct tcp_pcb* p, tcp_err_fn fn);
err_t tcp_close(struct tcp_pcb* p);
void tcp_abort(struct tcp_pcb* p);
void tcp_nagle_disable(struct tcp_pcb* p);
u16_t tcp_sndbuf(const struct tcp_pcb* p);
err_t tcp_write(struct tcp_pcb* p, const void* data, u16_t len, int flags);
err_t tcp_output(struct tcp_pcb* p);
void tcp_recved(struct tcp_pcb* p, u16_t len);
err_t tcp_bind(struct tcp_pcb* p, const ip_addr_t* addr, u16_t port);
struct tcp_pcb* tcp_listen_with_backlog(struct tcp_pcb* p, unsigned backlog);
err_t tcp_connect(struct tcp_pcb* p, const ip_addr_t* addr, u16_t port,
                  tcp_accept_fn fn);

#endif
