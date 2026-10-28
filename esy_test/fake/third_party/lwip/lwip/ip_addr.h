/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_TEST_FAKE_THIRD_PARTY_LWIP_LWIP_IP_ADDR_H
#define ESY_TEST_FAKE_THIRD_PARTY_LWIP_LWIP_IP_ADDR_H
#include <stdint.h>
typedef struct {
  uint32_t addr;
} ip_addr_t;
#define IPADDR_TYPE_ANY   0
#define IP_ANY_TYPE       ((const ip_addr_t*)0)
#define IP_GET_TYPE(addr) 0
int ipaddr_aton(const char* text, ip_addr_t* addr);

#endif
