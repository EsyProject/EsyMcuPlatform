/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_NETWORK_DNS_FIXTURE_H
#define ESY_NETWORK_DNS_FIXTURE_H

#include "lwip/dns.h"

void network_lwip_dns_reset(void);
void network_lwip_dns_result(err_t result);
void network_lwip_dns_complete(bool success);
bool network_lwip_dns_pending(void);

#endif
