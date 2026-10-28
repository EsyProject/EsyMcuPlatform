/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_WIFI_DEFS_H
#define ESY_WIFI_DEFS_H

/**
 * @file esy_wifi_defs.h
 * @brief Platform-agnostic Wi-Fi type definitions.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum SSID byte length, excluding the terminating NUL. */
#define ESY_WIFI_SSID_MAX_LENGTH 32u
/** @brief Maximum passphrase/hex PSK byte length, excluding the terminating NUL. */
#define ESY_WIFI_PASSWORD_MAX_LENGTH 64u

/**
 * @brief Wi-Fi station authentication mode.
 */
typedef enum {
  ESY_WIFI_AUTH_OPEN = 0,      /**< Open network without authentication. */
  ESY_WIFI_AUTH_WPA2_AES_PSK,  /**< WPA2 personal authentication. */
  ESY_WIFI_AUTH_WPA2_MIXED_PSK /**< WPA/WPA2 mixed personal authentication. */
} esy_wifi_auth_t;

/**
 * @brief IPv4 address assigned to a Wi-Fi station interface.
 */
typedef struct {
  uint8_t octets[4]; /**< Address octets in presentation order. */
} esy_wifi_ipv4_addr_t;

/**
 * @brief Borrowed native network execution hooks supplied by the Wi-Fi port.
 *
 * One serialized, non-ISR owner invokes these hooks only while the native
 * provider is alive. They do not own or initialize that provider. Progress
 * dispatches one provider iteration and may synchronously invoke its callbacks;
 * its latency follows the provider contract. Enter/leave must be paired around
 * native network calls on the same owner; they do not make a borrower safe for
 * concurrent use by multiple tasks. Do not stop or reinitialize the provider
 * from a hook or from callbacks dispatched by progress. Quiesce every borrower
 * before provider shutdown. Context is borrowed and may be NULL.
 */
typedef struct {
  void (*progress)(void *context); /**< Advance the native polling provider. */
  void (*enter)(void *context); /**< Enter the native network execution region. */
  void (*leave)(void *context); /**< Leave the matching execution region. */
  void *context; /**< Shared borrowed context for all three hooks. */
} esy_wifi_network_runtime_t;

/**
 * @brief Wi-Fi station connection configuration.
 *
 * SSID and password are readable NUL-terminated strings, at most the named
 * maximum lengths. SSID must be nonempty; OPEN uses an empty, non-NULL password.
 * This type itself owns no strings. Manager init copies them; direct HAL or
 * HAL backend start/init borrows them until backend deinit, including
 * failure/retry.
 * A zero timeout requests an immediate deadline after the first backend poll.
 */
typedef struct {
  const char *ssid;            /**< Network SSID. */
  const char *password;        /**< Network password. */
  esy_wifi_auth_t auth;        /**< Authentication mode. */
  uint32_t connect_timeout_ms; /**< Connection timeout in milliseconds. */
} esy_wifi_config_t;

#ifdef __cplusplus
}
#endif

#endif /* ESY_WIFI_DEFS_H */
