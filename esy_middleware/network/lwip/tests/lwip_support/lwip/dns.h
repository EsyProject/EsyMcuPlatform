/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_Network_TEST_LWIP_DNS_H
#define ESY_Network_TEST_LWIP_DNS_H

#include "lwip/tcp.h"

#define ERR_INPROGRESS -5

typedef void (*dns_found_callback)(const char *name, const ip_addr_t *address,
                                    void *argument);

err_t dns_gethostbyname(const char *name, ip_addr_t *address,
                        dns_found_callback callback, void *argument);

#endif
