/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/credentials/esy_credentials.h"

#include <stdbool.h>
#include <string.h>

#define CREDENTIALS_MAGIC UINT32_C(0x43524544)
#define CREDENTIALS_LEASE_MAGIC UINT32_C(0x434c4541)

typedef struct {
    uint32_t magic;
    const esy_credentials_t *owner;
    esy_credentials_config_t config;
    size_t leases;
} credentials_ctx_t;

typedef struct {
    uint32_t magic;
    const esy_credentials_lease_t *owner;
    esy_credentials_t *credentials;
} credentials_lease_ctx_t;

_Static_assert(sizeof(credentials_ctx_t) <= ESY_CREDENTIALS_STORAGE_BYTES,
               "Credentials handle storage is too small");
_Static_assert(sizeof(credentials_lease_ctx_t) <= ESY_CREDENTIALS_LEASE_STORAGE_BYTES,
               "Credentials lease storage is too small");

static bool bytes_zero(const uint8_t *bytes, size_t size)
{
    for (size_t i = 0u; i < size; ++i) {
        if (bytes[i] != 0u) {
            return false;
        }
    }
    return true;
}

static bool spans_overlap(const void *left, size_t left_size,
                          const void *right, size_t right_size)
{
    uintptr_t a = (uintptr_t)left;
    uintptr_t b = (uintptr_t)right;
    if (left_size == 0u || right_size == 0u) {
        return false;
    }
    if (left_size - 1u > UINTPTR_MAX - a ||
        right_size - 1u > UINTPTR_MAX - b) {
        return true;
    }
    return a <= b + right_size - 1u && b <= a + left_size - 1u;
}

static bool blob_valid(const esy_credentials_blob_t *blob)
{
    if (blob->format != ESY_CREDENTIALS_FORMAT_DER &&
        blob->format != ESY_CREDENTIALS_FORMAT_PEM) {
        return false;
    }
    if (blob->data == NULL || blob->size == 0u) {
        return blob->data == NULL && blob->size == 0u;
    }
    if (blob->size - 1u > UINTPTR_MAX - (uintptr_t)blob->data) {
        return false;
    }
    if (blob->format == ESY_CREDENTIALS_FORMAT_PEM) {
        if (blob->size < 2u || blob->data[blob->size - 1u] != 0u) {
            return false;
        }
        return memchr(blob->data, 0, blob->size - 1u) == NULL;
    }
    return true;
}

static bool config_overlaps(const esy_credentials_config_t *config,
                            const void *span, size_t size)
{
    return spans_overlap(config->trust_anchors.data, config->trust_anchors.size,
                         span, size) ||
           spans_overlap(config->client_certificate.data,
                         config->client_certificate.size, span, size) ||
           spans_overlap(config->private_key.data, config->private_key.size,
                         span, size);
}

static bool config_valid(const esy_credentials_config_t *config,
                         const esy_credentials_t *credentials)
{
    if (config == NULL ||
        spans_overlap(config, sizeof(*config), credentials, sizeof(*credentials))) {
        return false;
    }
    return blob_valid(&config->trust_anchors) &&
           blob_valid(&config->client_certificate) &&
           blob_valid(&config->private_key) &&
           config->trust_anchors.size != 0u &&
           ((config->client_certificate.size == 0u) ==
            (config->private_key.size == 0u)) &&
           !config_overlaps(config, credentials, sizeof(*credentials));
}

static esy_err_t read_context(const esy_credentials_t *credentials,
                              credentials_ctx_t *ctx)
{
    if (credentials == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    memcpy(ctx, credentials->storage.bytes, sizeof(*ctx));
    return ctx->magic == CREDENTIALS_MAGIC && ctx->owner == credentials ?
           ESY_OK : ESY_ERR_STATE;
}

esy_err_t esy_credentials_init(esy_credentials_t *credentials,
                               const esy_credentials_config_t *config)
{
    credentials_ctx_t ctx;
    if (credentials == NULL || config == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    if (!bytes_zero(credentials->storage.bytes,
                    sizeof(credentials->storage.bytes))) {
        return ESY_ERR_STATE;
    }
    if (!config_valid(config, credentials)) {
        return ESY_ERR_INVALID_ARG;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.magic = CREDENTIALS_MAGIC;
    ctx.owner = credentials;
    ctx.config = *config;
    memcpy(credentials->storage.bytes, &ctx, sizeof(ctx));
    return ESY_OK;
}

esy_err_t esy_credentials_deinit(esy_credentials_t *credentials)
{
    credentials_ctx_t ctx;
    esy_err_t result;
    if (credentials == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    if (bytes_zero(credentials->storage.bytes,
                   sizeof(credentials->storage.bytes))) {
        return ESY_OK;
    }
    result = read_context(credentials, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    if (ctx.leases != 0u) {
        return ESY_ERR_BUSY;
    }
    memset(credentials, 0, sizeof(*credentials));
    return ESY_OK;
}

esy_err_t esy_credentials_acquire(esy_credentials_t *credentials,
                                  esy_credentials_lease_t *lease,
                                  esy_credentials_config_t *config)
{
    credentials_ctx_t ctx;
    credentials_lease_ctx_t lease_ctx;
    esy_err_t result;
    if (credentials == NULL || lease == NULL || config == NULL ||
        spans_overlap(credentials, sizeof(*credentials), lease, sizeof(*lease)) ||
        spans_overlap(credentials, sizeof(*credentials), config, sizeof(*config)) ||
        spans_overlap(lease, sizeof(*lease), config, sizeof(*config))) {
        return ESY_ERR_INVALID_ARG;
    }
    result = read_context(credentials, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    if (!bytes_zero(lease->storage.bytes, sizeof(lease->storage.bytes))) {
        return ESY_ERR_STATE;
    }
    if (config_overlaps(&ctx.config, lease, sizeof(*lease)) ||
        config_overlaps(&ctx.config, config, sizeof(*config))) {
        return ESY_ERR_INVALID_ARG;
    }
    if (ctx.leases == SIZE_MAX) {
        return ESY_ERR_NO_MEM;
    }
    memset(&lease_ctx, 0, sizeof(lease_ctx));
    lease_ctx.magic = CREDENTIALS_LEASE_MAGIC;
    lease_ctx.owner = lease;
    lease_ctx.credentials = credentials;
    ++ctx.leases;
    memcpy(credentials->storage.bytes, &ctx, sizeof(ctx));
    memcpy(lease->storage.bytes, &lease_ctx, sizeof(lease_ctx));
    *config = ctx.config;
    return ESY_OK;
}

esy_err_t esy_credentials_release(esy_credentials_lease_t *lease)
{
    credentials_lease_ctx_t lease_ctx;
    credentials_ctx_t ctx;
    esy_err_t result;
    if (lease == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    if (bytes_zero(lease->storage.bytes, sizeof(lease->storage.bytes))) {
        return ESY_OK;
    }
    memcpy(&lease_ctx, lease->storage.bytes, sizeof(lease_ctx));
    if (lease_ctx.magic != CREDENTIALS_LEASE_MAGIC || lease_ctx.owner != lease ||
        lease_ctx.credentials == NULL) {
        return ESY_ERR_STATE;
    }
    result = read_context(lease_ctx.credentials, &ctx);
    if (result != ESY_OK || ctx.leases == 0u) {
        return ESY_ERR_STATE;
    }
    --ctx.leases;
    memcpy(lease_ctx.credentials->storage.bytes, &ctx, sizeof(ctx));
    memset(lease, 0, sizeof(*lease));
    return ESY_OK;
}

esy_err_t esy_credentials_replace(esy_credentials_t *credentials,
                                  const esy_credentials_config_t *config)
{
    credentials_ctx_t ctx;
    esy_err_t result = read_context(credentials, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    if (ctx.leases != 0u) {
        return ESY_ERR_BUSY;
    }
    if (!config_valid(config, credentials)) {
        return ESY_ERR_INVALID_ARG;
    }
    ctx.config = *config;
    memcpy(credentials->storage.bytes, &ctx, sizeof(ctx));
    return ESY_OK;
}
