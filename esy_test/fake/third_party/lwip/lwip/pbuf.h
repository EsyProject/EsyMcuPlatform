/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_TEST_FAKE_THIRD_PARTY_LWIP_LWIP_PBUF_H
#define ESY_TEST_FAKE_THIRD_PARTY_LWIP_LWIP_PBUF_H
#include <stdint.h>
typedef uint16_t u16_t;
struct pbuf {
  struct pbuf* next;
  u16_t tot_len, len;
  uint8_t data[512];
};
unsigned pbuf_free(struct pbuf* p);
void pbuf_cat(struct pbuf* head, struct pbuf* tail);
u16_t pbuf_copy_partial(const struct pbuf* p, void* dst, u16_t len,
                        u16_t offset);

#endif
