/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_middleware/credentials/esy_credentials.h"

#include <string.h>

static const uint8_t k_der[] = {0x30u, 0x00u, 0x01u};
static const uint8_t k_pem[] = "test PEM bytes";
static const uint8_t k_new_der[] = {0x30u, 0x02u};

static esy_credentials_config_t make_config(void)
{
    esy_credentials_config_t config = {0};
    config.trust_anchors.data = k_der;
    config.trust_anchors.size = sizeof(k_der);
    config.trust_anchors.format = ESY_CREDENTIALS_FORMAT_DER;
    return config;
}

void setUp(void)
{
}

void tearDown(void)
{
}

void test_lifecycle_retains_borrowed_bytes_and_permits_reinitialization(void)
{
    esy_credentials_t credentials = {0};
    esy_credentials_lease_t lease = {0};
    esy_credentials_config_t config = make_config();
    esy_credentials_config_t snapshot = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_deinit(&credentials));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_release(&lease));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_init(&credentials, &config));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_init(&credentials, &config));
    config.trust_anchors.data = k_new_der;
    config.trust_anchors.size = sizeof(k_new_der);
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_acquire(&credentials, &lease, &snapshot));
    TEST_ASSERT_EQUAL_PTR(k_der, snapshot.trust_anchors.data);
    TEST_ASSERT_EQUAL(sizeof(k_der), snapshot.trust_anchors.size);
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_release(&lease));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_release(&lease));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_deinit(&credentials));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_deinit(&credentials));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_init(&credentials, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_deinit(&credentials));
}

void test_all_leases_block_replacement_and_deinit_until_the_last_release(void)
{
    esy_credentials_t credentials = {0};
    esy_credentials_lease_t first = {0}, second = {0};
    esy_credentials_config_t config = make_config(), snapshot = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_init(&credentials, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_acquire(&credentials, &first, &snapshot));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_acquire(&credentials, &second, &snapshot));
    config.trust_anchors.data = k_new_der;
    config.trust_anchors.size = sizeof(k_new_der);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_credentials_replace(&credentials, &config));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_credentials_deinit(&credentials));
    TEST_ASSERT_EQUAL_PTR(k_der, snapshot.trust_anchors.data);
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_release(&first));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_credentials_replace(&credentials, &config));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_credentials_deinit(&credentials));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_release(&second));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_replace(&credentials, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_acquire(&credentials, &first, &snapshot));
    TEST_ASSERT_EQUAL_PTR(k_new_der, snapshot.trust_anchors.data);
    TEST_ASSERT_EQUAL(sizeof(k_new_der), snapshot.trust_anchors.size);
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_release(&first));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_deinit(&credentials));
}

void test_mixed_formats_and_optional_client_pair_are_preserved(void)
{
    esy_credentials_t credentials = {0};
    esy_credentials_lease_t lease = {0};
    esy_credentials_config_t config = make_config(), snapshot = {0};
    config.client_certificate.data = k_pem;
    config.client_certificate.size = sizeof(k_pem);
    config.client_certificate.format = ESY_CREDENTIALS_FORMAT_PEM;
    config.private_key.data = k_der;
    config.private_key.size = sizeof(k_der);
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_init(&credentials, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_acquire(&credentials, &lease, &snapshot));
    TEST_ASSERT_EQUAL_PTR(k_pem, snapshot.client_certificate.data);
    TEST_ASSERT_EQUAL(ESY_CREDENTIALS_FORMAT_PEM, snapshot.client_certificate.format);
    TEST_ASSERT_EQUAL_PTR(k_der, snapshot.private_key.data);
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_release(&lease));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_deinit(&credentials));
}

void test_missing_trust_anchor_or_unpaired_client_credentials_fail_atomically(void)
{
    esy_credentials_t credentials = {0}, zero = {0};
    esy_credentials_config_t config = {0};
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_init(&credentials, &config));
    TEST_ASSERT_EQUAL_MEMORY(&zero, &credentials, sizeof(zero));
    config = make_config();
    config.client_certificate = config.trust_anchors;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_init(&credentials, &config));
    config = make_config();
    config.private_key = config.trust_anchors;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_init(&credentials, &config));
    config = make_config();
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_init(&credentials, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_deinit(&credentials));
}

void test_invalid_blob_presence_format_and_pointer_wrap_are_rejected(void)
{
    esy_credentials_t credentials = {0};
    esy_credentials_config_t config = make_config();
    config.trust_anchors.data = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_init(&credentials, &config));
    config = make_config();
    config.trust_anchors.size = 0u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_init(&credentials, &config));
    config = make_config();
    config.client_certificate.format = (esy_credentials_format_t)42;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_init(&credentials, &config));
    config = make_config();
    config.trust_anchors.data = (const uint8_t *)(UINTPTR_MAX - 1u);
    config.trust_anchors.size = 3u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_init(&credentials, &config));
}

void test_pem_requires_nonempty_text_and_exactly_one_final_nul(void)
{
    const uint8_t no_nul[] = {'x', 'y'};
    const uint8_t inner_nul[] = {'x', 0u, 'y', 0u};
    const uint8_t empty[] = {0u};
    esy_credentials_t credentials = {0};
    esy_credentials_config_t config = make_config();
    config.trust_anchors.format = ESY_CREDENTIALS_FORMAT_PEM;
    config.trust_anchors.data = no_nul;
    config.trust_anchors.size = sizeof(no_nul);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_init(&credentials, &config));
    config.trust_anchors.data = inner_nul;
    config.trust_anchors.size = sizeof(inner_nul);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_init(&credentials, &config));
    config.trust_anchors.data = empty;
    config.trust_anchors.size = sizeof(empty);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_init(&credentials, &config));
    config.trust_anchors.data = k_pem;
    config.trust_anchors.size = sizeof(k_pem);
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_init(&credentials, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_deinit(&credentials));
}

void test_failed_replace_preserves_the_old_complete_bundle(void)
{
    esy_credentials_t credentials = {0};
    esy_credentials_lease_t lease = {0};
    esy_credentials_config_t config = make_config(), snapshot = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_init(&credentials, &config));
    config.trust_anchors.size = 0u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_replace(&credentials, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_acquire(&credentials, &lease, &snapshot));
    TEST_ASSERT_EQUAL_PTR(k_der, snapshot.trust_anchors.data);
    TEST_ASSERT_EQUAL(sizeof(k_der), snapshot.trust_anchors.size);
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_release(&lease));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_deinit(&credentials));
}

void test_copied_handle_and_lease_cannot_consume_an_existing_borrow(void)
{
    esy_credentials_t credentials = {0};
    esy_credentials_lease_t lease = {0};
    esy_credentials_config_t config = make_config(), snapshot = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_init(&credentials, &config));
    esy_credentials_t copied = credentials;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_deinit(&copied));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_replace(&copied, &config));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_acquire(&copied, &lease, &snapshot));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_acquire(&credentials, &lease, &snapshot));
    esy_credentials_lease_t copied_lease = lease;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_release(&copied_lease));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_credentials_deinit(&credentials));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_acquire(&credentials, &lease, &snapshot));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_release(&lease));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_deinit(&credentials));
}

void test_uninitialized_zero_objects_and_null_arguments_have_no_side_effects(void)
{
    esy_credentials_t credentials = {0};
    esy_credentials_lease_t lease = {0};
    esy_credentials_config_t config = make_config(), snapshot = make_config();
    esy_credentials_config_t expected = snapshot;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_init(NULL, &config));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_init(&credentials, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_deinit(NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_release(NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_replace(NULL, &config));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_replace(&credentials, &config));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_acquire(&credentials, &lease, &snapshot));
    TEST_ASSERT_EQUAL_MEMORY(&expected, &snapshot, sizeof(snapshot));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_init(&credentials, &config));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_replace(&credentials, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_acquire(NULL, &lease, &snapshot));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_acquire(&credentials, NULL, &snapshot));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_acquire(&credentials, &lease, NULL));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_deinit(&credentials));
}

void test_handle_lease_output_and_credential_aliases_are_rejected(void)
{
    esy_credentials_t credentials = {0};
    esy_credentials_lease_t lease = {0};
    esy_credentials_config_t config = make_config(), snapshot = {0};
    config.trust_anchors.data = credentials.storage.bytes;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_init(&credentials, &config));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_init(
        &credentials, (const esy_credentials_config_t *)&credentials));
    config = make_config();
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_init(&credentials, &config));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_acquire(
        &credentials, (esy_credentials_lease_t *)&credentials, &snapshot));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_acquire(
        &credentials, &lease, (esy_credentials_config_t *)&credentials));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_acquire(
        &credentials, &lease, (esy_credentials_config_t *)&lease));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_deinit(&credentials));
}

void test_outputs_cannot_overwrite_credential_bytes(void)
{
    esy_credentials_t credentials = {0};
    esy_credentials_lease_t lease = {0};
    esy_credentials_config_t snapshot = {0}, config = make_config();
    union {
        max_align_t alignment;
        uint8_t bytes[sizeof(esy_credentials_config_t)];
    } data = {0};
    config.trust_anchors.data = data.bytes;
    config.trust_anchors.size = sizeof(data.bytes);
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_init(&credentials, &config));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_acquire(
        &credentials, &lease, (esy_credentials_config_t *)data.bytes));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_acquire(
        &credentials, (esy_credentials_lease_t *)data.bytes, &snapshot));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_deinit(&credentials));
}
