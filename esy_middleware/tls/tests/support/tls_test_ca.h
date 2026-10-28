/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TLS_TEST_CA_H
#define TLS_TEST_CA_H

/* Public test certificate only; no private key is retained. */
static const uint8_t k_test_ca[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBlTCCATugAwIBAgIUUglsnc+D/rgH7Fov4x/OTEglHScwCgYIKoZIzj0EAwIw\n"
    "HzEdMBsGA1UEAwwURXN5IFRMUyB1bml0IHRlc3QgQ0EwIBcNMjYxMDI2MTkyMTM3\n"
    "WhgPMjEyNjEwMDIxOTIxMzdaMB8xHTAbBgNVBAMMFEVzeSBUTFMgdW5pdCB0ZXN0\n"
    "IENBMFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEKVqy9z8W5O0tGl72USZ8sN4c\n"
    "G1+3gALkIx0XyLs1FVPy1o4H+qd+IJHf4fJ2lhxK6e6N2GXv/5oTpEAs3FfOoqNT\n"
    "MFEwHQYDVR0OBBYEFK13g1eFES48bxhr76ZnB+0IxrFyMB8GA1UdIwQYMBaAFK13\n"
    "g1eFES48bxhr76ZnB+0IxrFyMA8GA1UdEwEB/wQFMAMBAf8wCgYIKoZIzj0EAwID\n"
    "SAAwRQIgY4umPw0tvZZUHt+2njVK0+p/bNG9UoPSdJ1+oov+ASUCIQDRqjQ8rFDE\n"
    "9jx8hUXpMa/RFVlJ/vdA7MpMLzAw9IyV6w==\n"
    "-----END CERTIFICATE-----\n";

#endif /* TLS_TEST_CA_H */
