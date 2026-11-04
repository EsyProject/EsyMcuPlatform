/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

typedef void (*native_probe_t)(void);
void esy_native_probe_core(void);
#ifdef ESY_TEST_NATIVE_PERIPHERALS
void esy_native_probe_peripherals(void);
#endif
#ifdef ESY_TEST_NATIVE_MANAGED
void esy_native_probe_managed(void);
#endif
#ifdef ESY_TEST_NATIVE_RADIO
void esy_native_probe_radio(void);
#endif
#ifdef ESY_TEST_NATIVE_POWER
void esy_native_probe_power(void);
#endif

typedef struct { native_probe_t volatile retained_probe; } native_consumer_ctx_t;
static native_consumer_ctx_t s_ctx;

/* Preserve actual public-API call closures in the final ELF without executing
 * this compile/link consumer's peripheral or image-management operations. */
void app_main(void) {
  s_ctx.retained_probe = esy_native_probe_core;
#ifdef ESY_TEST_NATIVE_PERIPHERALS
  s_ctx.retained_probe = esy_native_probe_peripherals;
#endif
#ifdef ESY_TEST_NATIVE_MANAGED
  s_ctx.retained_probe = esy_native_probe_managed;
#endif
#ifdef ESY_TEST_NATIVE_RADIO
  s_ctx.retained_probe = esy_native_probe_radio;
#endif
#ifdef ESY_TEST_NATIVE_POWER
  s_ctx.retained_probe = esy_native_probe_power;
#endif
}
