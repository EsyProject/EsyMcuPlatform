/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TLS_TEST_SERVER_H
#define TLS_TEST_SERVER_H

/* Public test identity. This private key must never provision a real device. */
static const uint8_t k_test_server_certificate[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBlTCCATugAwIBAgIUXw8z26JtPbRyT2I1seafIAdj5iAwCgYIKoZIzj0EAwIw\n"
    "FDESMBAGA1UEAwwJbG9jYWxob3N0MCAXDTI2MTAyNjE5MjYzN1oYDzIxMjYxMDAy\n"
    "MTkyNjM3WjAUMRIwEAYDVQQDDAlsb2NhbGhvc3QwWTATBgcqhkjOPQIBBggqhkjO\n"
    "PQMBBwNCAARfEHD/eSKYtoXCjcP419FYQ8Jlx4AriyHnt9yD9AIveCu2vZ3/8qPM\n"
    "A3c6KsMfzktuHn17G/BjhMsAiyZ1Gd4oo2kwZzAdBgNVHQ4EFgQUOEyLQ59Tgu6N\n"
    "BatqJU4BPDNo2ygwHwYDVR0jBBgwFoAUOEyLQ59Tgu6NBatqJU4BPDNo2ygwDwYD\n"
    "VR0TAQH/BAUwAwEB/zAUBgNVHREEDTALgglsb2NhbGhvc3QwCgYIKoZIzj0EAwID\n"
    "SAAwRQIhAP6gSwk5ANpuSQEgDj7AzVn4yEk2A7htb2FRXwWyVYE2AiAbAOVcFHv9\n"
    "ZJW+SarcULNyxCdERML+P2d9O7TM0OF+lA==\n"
    "-----END CERTIFICATE-----\n";

static const uint8_t k_test_server_key[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgkYTWeCBJTeNdeAXq\n"
    "Q6aB5lfFklEf4RXvHv2Q6uhlNbOhRANCAARfEHD/eSKYtoXCjcP419FYQ8Jlx4Ar\n"
    "iyHnt9yD9AIveCu2vZ3/8qPMA3c6KsMfzktuHn17G/BjhMsAiyZ1Gd4o\n"
    "-----END PRIVATE KEY-----\n";

#endif /* TLS_TEST_SERVER_H */
