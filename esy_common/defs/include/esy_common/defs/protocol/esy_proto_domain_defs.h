/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_PROTO_DOMAIN_DEFS_H
#define ESY_PROTO_DOMAIN_DEFS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_proto_domain_defs.h
 * @brief Shared payload domain identifiers, independent of transport framing.
 *
 * Every routed payload starts with a domain byte followed by its domain-specific
 * header. ProtoBin DATA frames carry the same payload unchanged.
 */

/** @brief Number of bytes occupied by the payload domain. */
#define ESY_PROTO_DOMAIN_HEADER_BYTES 1u

/** @brief Stable payload domain values shared by platform and product messages. */
typedef enum {
  ESY_PROTO_DOMAIN_REMOTE_MANAGER = 0x02u, /**< Remote management payload. */
  ESY_PROTO_DOMAIN_PRODUCT = 0x80u, /**< Consumer-defined product payload. */
} esy_proto_domain_t;

#ifdef __cplusplus
}
#endif

#endif /* ESY_PROTO_DOMAIN_DEFS_H */
