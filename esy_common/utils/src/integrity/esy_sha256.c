/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/utils/integrity/esy_sha256.h"

#include <string.h>

#define SHA256_ROTR(value, bits) \
  (((value) >> (bits)) | ((value) << (32u - (bits))))

static const uint32_t s_round_constants[64] = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu,
    0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u, 0xD807AA98u, 0x12835B01u,
    0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u,
    0xC19BF174u, 0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu,
    0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu, 0x983E5152u,
    0xA831C66Du, 0xB00327C8u, 0xBF597FC7u, 0xC6E00BF3u, 0xD5A79147u,
    0x06CA6351u, 0x14292967u, 0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu,
    0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u, 0xD192E819u,
    0xD6990624u, 0xF40E3585u, 0x106AA070u, 0x19A4C116u, 0x1E376C08u,
    0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu,
    0x682E6FF3u, 0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u,
    0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u,
};

static uint32_t load_be32(const uint8_t *data) {
  return ((uint32_t)data[0] << 24u) | ((uint32_t)data[1] << 16u) |
         ((uint32_t)data[2] << 8u) | (uint32_t)data[3];
}

static void store_be32(uint8_t *data, uint32_t value) {
  data[0] = (uint8_t)(value >> 24u);
  data[1] = (uint8_t)(value >> 16u);
  data[2] = (uint8_t)(value >> 8u);
  data[3] = (uint8_t)value;
}

static void sha256_transform(esy_sha256_ctx_t *ctx, const uint8_t *block) {
  uint32_t words[64];
  uint32_t a;
  uint32_t b;
  uint32_t c;
  uint32_t d;
  uint32_t e;
  uint32_t f;
  uint32_t g;
  uint32_t h;

  for (size_t i = 0u; i < 16u; ++i) {
    words[i] = load_be32(&block[i * 4u]);
  }
  for (size_t i = 16u; i < 64u; ++i) {
    uint32_t s0 = SHA256_ROTR(words[i - 15u], 7u) ^
                  SHA256_ROTR(words[i - 15u], 18u) ^ (words[i - 15u] >> 3u);
    uint32_t s1 = SHA256_ROTR(words[i - 2u], 17u) ^
                  SHA256_ROTR(words[i - 2u], 19u) ^ (words[i - 2u] >> 10u);
    words[i] = words[i - 16u] + s0 + words[i - 7u] + s1;
  }

  a = ctx->state[0];
  b = ctx->state[1];
  c = ctx->state[2];
  d = ctx->state[3];
  e = ctx->state[4];
  f = ctx->state[5];
  g = ctx->state[6];
  h = ctx->state[7];

  for (size_t i = 0u; i < 64u; ++i) {
    uint32_t sum1 =
        SHA256_ROTR(e, 6u) ^ SHA256_ROTR(e, 11u) ^ SHA256_ROTR(e, 25u);
    uint32_t choice = (e & f) ^ ((~e) & g);
    uint32_t temp1 = h + sum1 + choice + s_round_constants[i] + words[i];
    uint32_t sum0 =
        SHA256_ROTR(a, 2u) ^ SHA256_ROTR(a, 13u) ^ SHA256_ROTR(a, 22u);
    uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    uint32_t temp2 = sum0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
  ctx->state[5] += f;
  ctx->state[6] += g;
  ctx->state[7] += h;
}

void esy_sha256_init(esy_sha256_ctx_t *ctx) {
  if (ctx == NULL) return;

  memset(ctx, 0, sizeof(*ctx));
  ctx->state[0] = 0x6A09E667u;
  ctx->state[1] = 0xBB67AE85u;
  ctx->state[2] = 0x3C6EF372u;
  ctx->state[3] = 0xA54FF53Au;
  ctx->state[4] = 0x510E527Fu;
  ctx->state[5] = 0x9B05688Cu;
  ctx->state[6] = 0x1F83D9ABu;
  ctx->state[7] = 0x5BE0CD19u;
}

void esy_sha256_update(esy_sha256_ctx_t *ctx, const void *data, size_t length) {
  const uint8_t *bytes = (const uint8_t *)data;

  if (ctx == NULL || (bytes == NULL && length != 0u)) return;
  ctx->total_bytes += (uint64_t)length;

  while (length > 0u) {
    size_t available = ESY_SHA256_BLOCK_BYTES - ctx->block_len;
    size_t take = length < available ? length : available;
    memcpy(&ctx->block[ctx->block_len], bytes, take);
    ctx->block_len += take;
    bytes += take;
    length -= take;

    if (ctx->block_len == ESY_SHA256_BLOCK_BYTES) {
      sha256_transform(ctx, ctx->block);
      ctx->block_len = 0u;
    }
  }
}

void esy_sha256_final(esy_sha256_ctx_t *ctx,
                      uint8_t digest[ESY_SHA256_DIGEST_BYTES]) {
  uint64_t total_bits;

  if (ctx == NULL || digest == NULL) return;

  total_bits = ctx->total_bytes * 8u;
  ctx->block[ctx->block_len++] = 0x80u;
  if (ctx->block_len > 56u) {
    memset(&ctx->block[ctx->block_len], 0,
           ESY_SHA256_BLOCK_BYTES - ctx->block_len);
    sha256_transform(ctx, ctx->block);
    ctx->block_len = 0u;
  }
  memset(&ctx->block[ctx->block_len], 0, 56u - ctx->block_len);
  for (size_t i = 0u; i < 8u; ++i) {
    ctx->block[63u - i] = (uint8_t)(total_bits >> (i * 8u));
  }
  sha256_transform(ctx, ctx->block);

  for (size_t i = 0u; i < 8u; ++i) {
    store_be32(&digest[i * 4u], ctx->state[i]);
  }
  memset(ctx, 0, sizeof(*ctx));
}

void esy_sha256(const void *data, size_t length,
                uint8_t digest[ESY_SHA256_DIGEST_BYTES]) {
  esy_sha256_ctx_t ctx;

  if (digest == NULL || (data == NULL && length != 0u)) return;
  esy_sha256_init(&ctx);
  esy_sha256_update(&ctx, data, length);
  esy_sha256_final(&ctx, digest);
}
