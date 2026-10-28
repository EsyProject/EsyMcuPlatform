/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_lwip.h"
#include <string.h>
#define FAKE_LWIP_PCB_POOL_SIZE 16u
/* Cover a full 16-bit receive chain plus refused data without host allocation. */
#define FAKE_LWIP_PBUF_POOL_SIZE 132u
typedef struct {
  struct tcp_pcb pcbs[FAKE_LWIP_PCB_POOL_SIZE];
  struct pbuf pbufs[FAKE_LWIP_PBUF_POOL_SIZE];
  bool pbuf_used[FAKE_LWIP_PBUF_POOL_SIZE];
  struct tcp_pcb* last;
  unsigned aborts, writes, invalid_pbuf_frees;
  err_t allocation_result, connect_result, close_result, output_result,
      write_result, listen_result;
} fake_lwip_ctx_t;
static fake_lwip_ctx_t s_ctx;
void fake_lwip_reset(void) { memset(&s_ctx, 0, sizeof(s_ctx)); }
void fake_lwip_fail(err_t a, err_t c, err_t d, err_t o) {
  s_ctx.allocation_result = a;
  s_ctx.connect_result = c;
  s_ctx.close_result = d;
  s_ctx.output_result = o;
}
void fake_lwip_set_write_result(err_t r) { s_ctx.write_result = r; }
void fake_lwip_set_listen_result(err_t r) { s_ctx.listen_result = r; }
struct tcp_pcb* fake_lwip_last_pcb(void) { return s_ctx.last; }
struct tcp_pcb* tcp_new_ip_type(int type) {
  (void)type;
  if (s_ctx.allocation_result) return NULL;
  for (unsigned i = 0; i < FAKE_LWIP_PCB_POOL_SIZE; ++i)
    if (!s_ctx.pcbs[i].used) {
      struct tcp_pcb* p = &s_ctx.pcbs[i];
      memset(p, 0, sizeof(*p));
      p->used = true;
      s_ctx.last = p;
      return p;
    }
  return NULL;
}
void tcp_arg(struct tcp_pcb* p, void* a) { p->arg = a; }
void tcp_recv(struct tcp_pcb* p, tcp_recv_fn f) { p->recv = f; }
void tcp_sent(struct tcp_pcb* p, tcp_sent_fn f) { p->sent = f; }
void tcp_accept(struct tcp_pcb* p, tcp_accept_fn f) { p->accept = f; }
void tcp_err(struct tcp_pcb* p, tcp_err_fn f) { p->error = f; }
static void release_refused_data(struct tcp_pcb* p) {
  struct pbuf* refused = p->refused_data;
  p->refused_data = NULL;
  if (refused) pbuf_free(refused);
}
err_t tcp_close(struct tcp_pcb* p) {
  if (!p->listening && s_ctx.close_result) return s_ctx.close_result;
  release_refused_data(p);
  p->used = false;
  return ERR_OK;
}
void tcp_abort(struct tcp_pcb* p) {
  s_ctx.aborts++;
  release_refused_data(p);
  p->used = false;
  if (p->error) p->error(p->arg, ERR_ABRT);
}
void tcp_nagle_disable(struct tcp_pcb* p) { (void)p; }
u16_t tcp_sndbuf(const struct tcp_pcb* p) {
  (void)p;
  return TCP_SND_BUF;
}
err_t tcp_write(struct tcp_pcb* p, const void* d, u16_t n, int flags) {
  (void)p;
  (void)d;
  (void)n;
  (void)flags;
  if (s_ctx.write_result) return s_ctx.write_result;
  s_ctx.writes++;
  return ERR_OK;
}
err_t tcp_output(struct tcp_pcb* p) {
  (void)p;
  return s_ctx.output_result;
}
void tcp_recved(struct tcp_pcb* p, u16_t n) {
  (void)p;
  (void)n;
}
err_t tcp_bind(struct tcp_pcb* p, const ip_addr_t* a, u16_t port) {
  (void)p;
  (void)a;
  (void)port;
  return ERR_OK;
}
struct tcp_pcb* tcp_listen_with_backlog(struct tcp_pcb* p, unsigned n) {
  (void)n;
  if (!p || !p->used || s_ctx.listen_result) return NULL;
  struct tcp_pcb* listener = tcp_new_ip_type(0);
  if (!listener) return NULL;
  listener->arg = p->arg;
  listener->listening = true;
  p->used = false;
  return listener;
}
err_t tcp_connect(struct tcp_pcb* p, const ip_addr_t* a, u16_t port,
                  tcp_accept_fn f) {
  (void)a;
  (void)port;
  p->connected = f;
  return s_ctx.connect_result;
}
int ipaddr_aton(const char* text, ip_addr_t* addr) {
  if (!text || !text[0] || text[0] == '!') return 0;
  addr->addr = 1;
  return 1;
}
struct tcp_pcb* fake_lwip_accept(void) {
  for (unsigned i = 0; i < FAKE_LWIP_PCB_POOL_SIZE; ++i) {
    struct tcp_pcb* l = &s_ctx.pcbs[i];
    if (l->used && l->listening && l->accept) {
      struct tcp_pcb* p = tcp_new_ip_type(0);
      if (!p || l->accept(l->arg, p, ERR_OK) != ERR_OK) return NULL;
      return p;
    }
  }
  return NULL;
}
err_t fake_lwip_connect(void) {
  struct tcp_pcb* p = s_ctx.last;
  return p && p->connected ? p->connected(p->arg, p, ERR_OK) : ERR_MEM;
}
unsigned pbuf_free(struct pbuf* p) {
  unsigned freed = 0;
  while (p) {
    unsigned index = FAKE_LWIP_PBUF_POOL_SIZE;
    for (unsigned i = 0; i < FAKE_LWIP_PBUF_POOL_SIZE; ++i)
      if (p == &s_ctx.pbufs[i]) {
        index = i;
        break;
      }
    /* Validate ownership before reading a possibly foreign or retired node. */
    if (index == FAKE_LWIP_PBUF_POOL_SIZE || !s_ctx.pbuf_used[index]) {
      ++s_ctx.invalid_pbuf_frees;
      break;
    }
    struct pbuf* next = p->next;
    s_ctx.pbuf_used[index] = false;
    ++freed;
    p = next;
  }
  return freed;
}
void pbuf_cat(struct pbuf* head, struct pbuf* tail) {
  for (;;) {
    head->tot_len += tail->tot_len;
    if (!head->next) break;
    head = head->next;
  }
  head->next = tail;
}
u16_t pbuf_copy_partial(const struct pbuf* p, void* dst, u16_t len,
                        u16_t offset) {
  unsigned done = 0;
  while (p && offset >= p->len) {
    offset -= p->len;
    p = p->next;
  }
  while (p && done < len) {
    unsigned n = p->len - offset;
    if (n > len - done) n = len - done;
    memcpy((uint8_t*)dst + done, p->data + offset, n);
    done += n;
    offset = 0;
    p = p->next;
  }
  return (u16_t)done;
}
static err_t deliver_pbuf(struct tcp_pcb* pcb, struct pbuf* p) {
  err_t result = pcb->recv(pcb->arg, pcb, p, ERR_OK);
  if (result != ERR_OK && result != ERR_ABRT) pcb->refused_data = p;
  return result;
}
err_t fake_lwip_receive(struct tcp_pcb* pcb, const uint8_t* data, u16_t len) {
  if (!pcb || !pcb->used || !pcb->recv || pcb->refused_data ||
      (len != 0u && !data) || len > sizeof(s_ctx.pbufs[0].data)) return ERR_MEM;
  for (unsigned i = 0; i < FAKE_LWIP_PBUF_POOL_SIZE; ++i)
    if (!s_ctx.pbuf_used[i]) {
      struct pbuf* p = &s_ctx.pbufs[i];
      memset(p, 0, sizeof(*p));
      s_ctx.pbuf_used[i] = true;
      p->len = p->tot_len = len;
      if (len != 0u) memcpy(p->data, data, len);
      return deliver_pbuf(pcb, p);
    }
  return ERR_MEM;
}
err_t fake_lwip_redeliver(struct tcp_pcb* pcb) {
  if (!pcb || !pcb->used || !pcb->recv || !pcb->refused_data) return ERR_MEM;
  struct pbuf* p = pcb->refused_data;
  pcb->refused_data = NULL;
  return deliver_pbuf(pcb, p);
}
err_t fake_lwip_fin(struct tcp_pcb* p) {
  return p->recv(p->arg, p, NULL, ERR_OK);
}
void fake_lwip_ack(struct tcp_pcb* p) {
  if (p->sent) p->sent(p->arg, p, UINT16_MAX);
}
void fake_lwip_error(struct tcp_pcb* p) {
  release_refused_data(p);
  p->used = false;
  if (p->error) p->error(p->arg, ERR_ABRT);
}
unsigned fake_lwip_live_pcbs(void) {
  unsigned n = 0;
  for (unsigned i = 0; i < FAKE_LWIP_PCB_POOL_SIZE; ++i) n += s_ctx.pcbs[i].used;
  return n;
}
unsigned fake_lwip_live_pbufs(void) {
  unsigned n = 0;
  for (unsigned i = 0; i < FAKE_LWIP_PBUF_POOL_SIZE; ++i) n += s_ctx.pbuf_used[i];
  return n;
}
unsigned fake_lwip_aborts(void) { return s_ctx.aborts; }
unsigned fake_lwip_writes(void) { return s_ctx.writes; }
unsigned fake_lwip_invalid_pbuf_frees(void) { return s_ctx.invalid_pbuf_frees; }
