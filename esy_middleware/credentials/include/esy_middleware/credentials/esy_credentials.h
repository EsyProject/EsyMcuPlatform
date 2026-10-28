/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_CREDENTIALS_H
#define ESY_CREDENTIALS_H

/**
 * @file esy_credentials.h
 * @brief Borrowed certificate/key bundles with explicit rotation leases.
 *
 * One startup/main-loop/task owner serializes all APIs and users of the borrowed
 * bytes. No API is ISR/fault safe. There is no allocation, storage access, task,
 * clock or crypto operation. This module validates descriptors, not certificates,
 * key matching, trust, expiry or secret protection. Configurations contain
 * exportable software key bytes; they are not protected key identifiers.
 */

#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Encoding of one certificate chain or private key. */
typedef enum {
    ESY_CREDENTIALS_FORMAT_DER = 0, /**< Binary DER; size counts every byte. */
    ESY_CREDENTIALS_FORMAT_PEM = 1  /**< PEM text; size includes its final NUL. */
} esy_credentials_format_t;

/** @brief Immutable borrowed bytes; NULL/zero means an absent optional value. */
typedef struct {
    const uint8_t *data; /**< Borrowed bytes, never copied or erased. */
    size_t size; /**< Readable bytes; PEM includes exactly one final NUL. */
    esy_credentials_format_t format; /**< DER or PEM, including absent blobs. */
} esy_credentials_blob_t;

/** @brief Complete bundle copied as descriptors; the bytes remain borrowed. */
typedef struct {
    esy_credentials_blob_t trust_anchors; /**< Required nonempty CA certificate(s). */
    esy_credentials_blob_t client_certificate; /**< Optional client certificate chain. */
    esy_credentials_blob_t private_key; /**< Required exactly when client cert exists. */
} esy_credentials_config_t;

/** @brief Private handle capacity, not an ABI or persistence format promise. */
#define ESY_CREDENTIALS_STORAGE_BYTES 128u
/** @brief Private lease capacity, not an ABI or persistence format promise. */
#define ESY_CREDENTIALS_LEASE_STORAGE_BYTES 32u

/**
 * @brief Caller-owned bundle handle; initialize with `{0}` before first init.
 * @note Do not copy, move, modify or clear a live handle. Borrowed bytes remain
 * immutable and alive until successful replacement or deinit, including between
 * leases. The original config descriptor need not remain alive. Every supplied
 * span must be accessible, and credential bytes must not overlap handles or API
 * outputs. Pointer accessibility and modification by other code cannot be checked.
 */
typedef struct {
    ESY_ALIGNED_STORAGE(storage, ESY_CREDENTIALS_STORAGE_BYTES);
} esy_credentials_t;

/**
 * @brief Caller-owned rotation lease; initialize with `{0}` before first use.
 * @note Do not copy, move, modify or clear an acquired lease. The lease and its
 * bundle handle remain alive until release succeeds. Successful release restores
 * zero state. A TLS adapter can hold a lease from its init through deinit.
 */
typedef struct {
    ESY_ALIGNED_STORAGE(storage, ESY_CREDENTIALS_LEASE_STORAGE_BYTES);
} esy_credentials_lease_t;

/**
 * @brief Initialize a zero handle with one complete borrowed bundle.
 * @param[in,out] credentials Zero-initialized or successfully deinitialized handle.
 * @param[in] config Required descriptors, disjoint from the handle. Trust anchors
 * must be present; client certificate/private key must both be present or absent.
 * PEM is nonempty text followed by one NUL; DER is a nonempty binary span.
 * @return ESY_OK, INVALID_ARG for invalid descriptors/spans, or STATE for a
 * nonzero handle. Failure leaves the handle unchanged and acquires no bytes.
 */
esy_err_t esy_credentials_init(esy_credentials_t *credentials,
                               const esy_credentials_config_t *config);

/**
 * @brief Release the bundle borrow without erasing credential bytes.
 * @param[in,out] credentials Initialized handle or its zero initial state.
 * @return ESY_OK including repeated deinit, INVALID_ARG for NULL, STATE for an
 * invalid/copied handle, or BUSY while any lease exists. Failure retains the
 * complete live state. Success restores zero state and permits init again.
 */
esy_err_t esy_credentials_deinit(esy_credentials_t *credentials);

/**
 * @brief Acquire a lease and copy a descriptor snapshot for a consumer.
 * @param[in,out] credentials Initialized bundle handle.
 * @param[in,out] lease Required zero lease, disjoint from handle/output/bytes.
 * @param[out] config Required descriptor output, disjoint from handle/lease/bytes.
 * @return ESY_OK, INVALID_ARG for NULL/overlapping spans, STATE for invalid/copied
 * handle or nonzero lease, or NO_MEM if the lease count is exhausted. Failure
 * leaves all objects and output unchanged. The snapshot is usable only until
 * release; replacement/deinit are BUSY while any acquired lease remains live.
 */
esy_err_t esy_credentials_acquire(esy_credentials_t *credentials,
                                  esy_credentials_lease_t *lease,
                                  esy_credentials_config_t *config);

/**
 * @brief Release exactly this lease and invalidate its descriptor snapshot.
 * @param[in,out] lease Acquired lease or its zero initial state.
 * @return ESY_OK including repeated release of a zero lease, INVALID_ARG for
 * NULL, or STATE for invalid/copied lease/handle. Failure preserves the lease
 * and bundle. Successful release restores zero state; re-acquisition is legal.
 */
esy_err_t esy_credentials_release(esy_credentials_lease_t *lease);

/**
 * @brief Publish a complete replacement when no consumer holds a lease.
 * @param[in,out] credentials Initialized bundle handle.
 * @param[in] config New descriptors, with the same validation as init. New bytes
 * remain borrowed until the next successful replacement/deinit. After success,
 * old bytes may be reclaimed unless also referenced by the new configuration.
 * @return ESY_OK, INVALID_ARG for invalid descriptors/spans, STATE for invalid
 * handle, or BUSY while leased. Failure retains the old complete bundle. Atomic
 * publication relies on the documented single owner; this is not a thread lock
 * or persistent transaction. No certificate/key parsing or storage I/O occurs.
 */
esy_err_t esy_credentials_replace(esy_credentials_t *credentials,
                                  const esy_credentials_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* ESY_CREDENTIALS_H */
