<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Cryptographic Randomness

## Purpose

Provide caller-owned cryptographic random generation for TLS through a Mbed TLS
3.6.x CTR_DRBG adapter. The consumer explicitly supplies an assessed cryptographic
entropy source. The module never selects a hardware source, seeds from a clock or
device identifier, or silently treats generic SDK random bytes as secure entropy.

## Public Surface

- `esy_crypto.h`: the borrowed random-provider callback, configuration, opaque
  generator type, initialization, generation, callback bridge and cleanup.
- `esy_crypto_mbedtls.h`: completes the caller-owned generator type using
  `mbedtls_ctr_drbg_context`. This provider-specific API intentionally couples its
  layout to Mbed TLS **3.6.x** and the exact provider build configuration.
- `esy_crypto` / `Esy::crypto`: the CMake library target, publicly propagating
  `esy_common_defs` and `esy_mbedtls_provider` so consumers inherit native headers
  and matching build definitions.

## Integration

Enable `ENABLE_ESY_CRYPTO` in the consumer's selected feature file. Before module
assembly, supply a compatible `esy_mbedtls_provider` target, or call
`esy_configure_mbedtls_provider()` from `cmake/esy_mbedtls_provider.cmake` with an
explicit `ESY_MBEDTLS_PATH` pointing to a complete Mbed TLS 3.6.x release.

For library host tests, `./setup.sh` prepares seven pinned official providers;
the Python/Ruby test helpers discover their cache automatically. Firmware CMake
uses the consumer's explicit providers, independently of this test cache and
lock. See [software provider setup](../../tools/README.md#software-provider-setup).

Mbed TLS 2.x, 3.5 and 3.7+ are outside this adapter's contract. The
provider must enable AES-256 CTR_DRBG and its AES implementation, permit requests of at
least 1024 bytes, and permit seed input of at least 176 bytes. OS facilities are
not required by this module; any configured provider threading or hardware port
must respect the platform's OS and hardware integration boundaries.

### Usage Example

The caller supplies a configuration with a qualified entropy callback/context
and optional personalization, plus a disjoint output buffer. This synchronous
example owns a local generator for one request, so no TLS consumer borrows it.
Keep the entropy context alive until the function returns and use the bytes only
on `ESY_OK`; the request must fit `ESY_CRYPTO_RANDOM_MAX_REQUEST_BYTES`.

```c
#include "esy_middleware/crypto/esy_crypto_mbedtls.h"

esy_err_t crypto_example_bytes(const esy_crypto_random_config_t *config,
                              uint8_t *output, size_t length)
{
    esy_crypto_random_t random = {0};
    esy_err_t result = esy_crypto_random_init(&random, config);
    if (result == ESY_OK) {
        result = esy_crypto_random_get(&random, output, length);
    }
    esy_err_t cleanup = esy_crypto_random_deinit(&random);
    return cleanup == ESY_OK ? result : cleanup;
}
```

For TLS, keep the generator in the owner's persistent context and provide an
`esy_crypto_random_provider_t` with `fill = esy_crypto_random_fill` and
`context = &random`. Stop all borrowers before deinit; retain their generator
and entropy source until cleanup succeeds. An entropy callback that merely
returns `esy_hal_system_random_bytes()` is valid only after the consumer
independently establishes that its selected source and operating state satisfy
the stronger entropy contract below.

## Contracts

First generator storage is `{0}`. Initialization on a live object returns
`ESY_ERR_STATE` without replacing it. Each seed and automatic reseed requests
exactly `ESY_CRYPTO_RANDOM_ENTROPY_BYTES` (48) bytes, incorporating sufficient nonce
material for the supported native AES-256 DRBG. A successful entropy callback
must fill the entire request with independent cryptographic entropy. The adapter
cannot inspect entropy quality or detect a dishonest callback. Optional nonsecret
personalization is at most 128 bytes and is borrowed only during initialization.

Generation accepts at most 1024 bytes. A zero-length request succeeds only on an
initialized generator and permits NULL output. Other invalid pointers or lengths
return `ESY_ERR_INVALID_ARG`; generation on a stopped object returns
`ESY_ERR_STATE`. Argument/state errors preserve output. Output must not overlap
the generator or entropy state. Provider success is authoritative only on
`ESY_OK`.

One externally serialized owner performs every operation in normal startup/task
context. No API is ISR-safe, fault-safe or reentrant. Entropy callbacks run
synchronously during initialization or automatic reseeding; the consumer budgets
their complete blocking and execution time. The module adds no task, timer,
deadline, lock or scheduling dependency. No timeout-counter or wraparound
contract applies to its own operations.

Seed failure frees native resources and clears the complete handle so explicit
retry is legal. An entropy error propagates as the same negative status; an
invalid positive provider status maps to `ESY_ERR_FAIL`. Other native failures
map to `ESY_ERR_FAIL`. A generation/reseed failure additionally clears the whole
requested output through `mbedtls_platform_zeroize()` and deinitializes the
generator. Failed generators never silently resume. Successful deinitialization
is idempotent, clears native state and permits initialization again. Stop every
borrowing consumer, including all TLS sessions, before deinitialization.

## Resource Model

The caller owns the complete generator and entropy-source lifetime. No caller
output buffers or personalization pointers are retained. Entropy callback/context
are borrowed until failure or deinitialization. No hidden singleton, task,
hardware initialization or persistence write is introduced.

Native DRBG initialization, seeding, generation and destruction run through the
selected Mbed TLS provider. A provider configured with alternative AES, PSA or
threading implementations can allocate or block internally. The adapter does not
promise whole-provider heap freedom, hard real-time execution or hardware-safe
behavior. Native resource creation/destruction belongs to initialization,
failure cleanup and deinitialization; resource and timing budgets depend on the
consumer's provider configuration and target.

## Verification

The Ceedling suite compiles real Mbed TLS AES, CTR_DRBG and platform zeroization
sources with a minimal test configuration. A deterministic entropy fixture is
used only for tests. The suite covers a published AES-256 CTR_DRBG known answer,
argument and length limits, duplicate initialization, source error propagation,
seed-failure retry, automatic reseeding, failure output clearing and explicit
restart. It also verifies the callback bridge used by TLS.

From the platform repository root, run `./setup.sh` once to prepare the cached
software providers, then run:

```sh
ruby tools/quality/run_module_tests.rb esy_middleware/crypto/tests \
  /tmp/esy-crypto-tests test:all
ruby tools/quality/run_module_tests.rb esy_middleware/crypto/tests \
  /tmp/esy-crypto-sanitizer test:all --sanitizer
python3 -B esy_middleware/crypto/tests/test_cmake_consumer.py
```

Optionally set the `ESY_MBEDTLS_PATH` environment variable to test another
compatible Mbed TLS 3.6.x checkout.

The sanitizer run instruments the real software provider sources as well as the
adapter. CMake consumer tests check public header propagation and C/C++ callers
against a normally configured real Mbed TLS provider. Host tests establish API
behavior; they do not measure physical entropy, certify cryptographic quality or
prove an embedded provider's resource/timing budget.

## Known Limits

- This module adapts random generation only. It does not provide general HMAC,
  AEAD, key derivation, signing or boot-verification algorithm APIs.
- Entropy health testing, source readiness, persistent seed/rollback protection
  and hardware source configuration belong to the supplied entropy provider.
- No implicit default entropy provider exists. In particular, STM32F4 generic
  system random bytes currently return `ESY_ERR_NOT_SUPPORTED` for nonempty
  requests, and other SDK random-source contracts do not promise cryptographic
  suitability.
- There is no internal synchronization, asynchronous operation, reseed-control
  API, credential persistence or secure-element key management.
- No FIPS validation or side-channel certification is implied by using CTR_DRBG
  or by passing known-answer tests.
