/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "network_lwip_dns_fixture.h"

#include <string.h>

typedef struct {
  dns_found_callback callback;
  void *argument;
  err_t result;
} network_lwip_dns_fixture_ctx_t;

static network_lwip_dns_fixture_ctx_t s_ctx;

void network_lwip_dns_reset(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.result = ERR_INPROGRESS;
}

void network_lwip_dns_result(err_t result) { s_ctx.result = result; }

bool network_lwip_dns_pending(void) { return s_ctx.callback != NULL; }

void network_lwip_dns_complete(bool success) {
  ip_addr_t address = {.addr = 1u};
  dns_found_callback callback = s_ctx.callback;
  void *argument = s_ctx.argument;
  s_ctx.callback = NULL;
  s_ctx.argument = NULL;
  if (callback != NULL) callback("!broker.invalid", success ? &address : NULL,
                                argument);
}

err_t dns_gethostbyname(const char *name, ip_addr_t *address,
                        dns_found_callback callback, void *argument) {
  (void)name;
  if (s_ctx.result == ERR_INPROGRESS) {
    s_ctx.callback = callback;
    s_ctx.argument = argument;
  } else if (s_ctx.result == ERR_OK) {
    address->addr = 1u;
  }
  return s_ctx.result;
}
