/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/bootloader/esy_bootloader.h"

#include <string.h>

#include "esy_common/utils/integrity/esy_sha256.h"
#include "fake_esy_hal_boot.h"
#include "fake_esy_hal_flash.h"
#include "fake_esy_hal_system.h"
#include "unity.h"

#define TEST_FLASH_BASE 0x10000000u
#define TEST_SLOT_B 0x10010000u
#define TEST_SLOT_SIZE 0xF000u
#define TEST_META_A 0x1000F000u
#define TEST_META_B 0x1001F000u
#define TEST_IMAGE_BYTES 1024u
#define TEST_FLASH_BYTES 0x20000u
#define TEST_NONUNIFORM_FLASH_BYTES 0x80000u
#define TEST_NONUNIFORM_META_A (TEST_FLASH_BASE + 0x10000u)
#define TEST_NONUNIFORM_META_B (TEST_FLASH_BASE + 0x20000u)
#define TEST_NONUNIFORM_META_A_BYTES 0x10000u
#define TEST_NONUNIFORM_META_B_BYTES 0x20000u
#define TEST_NONUNIFORM_SLOT_A (TEST_FLASH_BASE + 0x40000u)
#define TEST_NONUNIFORM_SLOT_B (TEST_FLASH_BASE + 0x60000u)
#define TEST_NONUNIFORM_SLOT_BYTES 0x20000u

typedef struct {
  uint8_t flash[TEST_NONUNIFORM_FLASH_BYTES];
  bool verify_result;
  uint32_t verify_calls;
  uint8_t signed_digest[ESY_FIRMWARE_SHA256_BYTES];
} bootloader_test_ctx_t;
static bootloader_test_ctx_t s_ctx;

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.verify_result = true;
  fake_esy_hal_boot_reset();
  fake_esy_hal_flash_reset();
  fake_esy_hal_system_reset();
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  flash->storage_base_addr = TEST_FLASH_BASE;
  flash->size_bytes = TEST_FLASH_BYTES;
  flash->program_size = 0x100u;
  flash->sector_size = 0x1000u;
  flash->backing_storage = s_ctx.flash;
  flash->backing_storage_size = sizeof(s_ctx.flash);
  fake_esy_hal_boot_get_state()->layout = (esy_bootloader_layout_t){
      .slot_addr = {TEST_FLASH_BASE, TEST_SLOT_B},
      .meta_addr = {TEST_META_A, TEST_META_B},
      .slot_size = TEST_SLOT_SIZE, .meta_size = {0x1000u, 0x1000u}};
  fake_esy_hal_boot_get_state()->runtime.running_address = TEST_FLASH_BASE + 16u;
}

void tearDown(void) {}

static bool verify_signature(void *context, const uint8_t *digest,
                              const uint8_t *signature) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx, context);
  TEST_ASSERT_NOT_NULL(digest);
  TEST_ASSERT_NOT_NULL(signature);
  memcpy(s_ctx.signed_digest, digest, sizeof(s_ctx.signed_digest));
  s_ctx.verify_calls++;
  return s_ctx.verify_result;
}

static esy_bootloader_config_t make_bootloader_config(void) {
  const esy_bootloader_config_t config = {
      .verify_signature = verify_signature,
      .verify_context = &s_ctx,
  };
  return config;
}

static esy_firmware_header_t make_header(uint32_t version, uint8_t state) {
  uint8_t image[1024] = {0};
  esy_firmware_header_t header = {
      .version = version,
      .firmware_size = 1024u,
      .state = state,
  };
  esy_sha256(image, sizeof(image), header.image_sha256);
  memset(header.signature, 0x5Au, sizeof(header.signature));
  return header;
}

static void use_nonuniform_metadata_layout(void) {
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  flash->size_bytes = TEST_NONUNIFORM_FLASH_BYTES;
  flash->program_size = 4u;
  flash->sector_size = TEST_NONUNIFORM_META_A_BYTES;
  flash->sector_size_after_address = TEST_NONUNIFORM_META_B;
  flash->sector_size_after = TEST_NONUNIFORM_META_B_BYTES;
  fake_esy_hal_boot_get_state()->layout = (esy_bootloader_layout_t){
      .slot_addr = {TEST_NONUNIFORM_SLOT_A, TEST_NONUNIFORM_SLOT_B},
      .meta_addr = {TEST_NONUNIFORM_META_A, TEST_NONUNIFORM_META_B},
      .slot_size = TEST_NONUNIFORM_SLOT_BYTES,
      .meta_size = {TEST_NONUNIFORM_META_A_BYTES, TEST_NONUNIFORM_META_B_BYTES}};
}

static void assert_no_flash_io(void) {
  const fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  TEST_ASSERT_EQUAL_UINT32(0u, flash->erase_count);
  TEST_ASSERT_EQUAL_UINT32(0u, flash->write_count);
  TEST_ASSERT_EQUAL_UINT32(0u, flash->read_count);
}

void test_esy_bootloader_rejects_invalid_metadata_args(void) {
  esy_firmware_header_t header = make_header(1u, ESY_FIRMWARE_STATE_CONFIRMED);

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_bootloader_read_meta_data(-1, &header));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_bootloader_read_meta_data(0, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_bootloader_write_meta_data(2, &header));
  header.firmware_size = 0u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_bootloader_write_meta_data(0, &header));
}

void test_esy_bootloader_rejects_invalid_config(void) {
  const esy_bootloader_config_t missing_callback = {0};

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_bootloader_init(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_bootloader_init(&missing_callback));
}

void test_esy_bootloader_promotes_try_boot_slot(void) {
  const esy_bootloader_config_t config = make_bootloader_config();
  esy_firmware_header_t slot0 = make_header(1u, ESY_FIRMWARE_STATE_CONFIRMED);
  esy_firmware_header_t slot1 = make_header(2u, ESY_FIRMWARE_STATE_TRY_BOOT);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_bootloader_write_meta_data(0, &slot0));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_bootloader_write_meta_data(1, &slot1));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_bootloader_init(&config));
  TEST_ASSERT_FALSE(esy_bootloader_jump_to_app());

  const fake_esy_hal_boot_state_t *state = fake_esy_hal_boot_get_state();
  TEST_ASSERT_EQUAL_UINT32(TEST_SLOT_B, state->last_jump_address);

  esy_firmware_header_t promoted = {0};
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_bootloader_read_meta_data(1, &promoted));
  TEST_ASSERT_EQUAL_UINT8(ESY_FIRMWARE_STATE_UNCONFIRMED, promoted.state);
}

void test_esy_bootloader_selects_higher_confirmed_version(void) {
  const esy_bootloader_config_t config = make_bootloader_config();
  esy_firmware_header_t slot0 = make_header(1u, ESY_FIRMWARE_STATE_CONFIRMED);
  esy_firmware_header_t slot1 = make_header(3u, ESY_FIRMWARE_STATE_CONFIRMED);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_bootloader_write_meta_data(0, &slot0));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_bootloader_write_meta_data(1, &slot1));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_bootloader_init(&config));
  TEST_ASSERT_FALSE(esy_bootloader_jump_to_app());
  TEST_ASSERT_EQUAL_UINT32(TEST_SLOT_B,
                           fake_esy_hal_boot_get_state()->last_jump_address);
}

void test_esy_bootloader_promotes_fallback_trial_after_preferred_promotion_fails(void) {
  const esy_bootloader_config_t config = make_bootloader_config();
  esy_firmware_header_t first = make_header(1u, ESY_FIRMWARE_STATE_TRY_BOOT);
  esy_firmware_header_t preferred = make_header(2u, ESY_FIRMWARE_STATE_TRY_BOOT);
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_write_meta_data(0, &first));
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_write_meta_data(1, &preferred));
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  const uint32_t erases = flash->erase_count;
  const uint32_t writes = flash->write_count;
  flash->erase_failures_remaining = 3u;

  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_init(&config));
  TEST_ASSERT_EQUAL_INT(0, esy_bootloader_get_selected_slot());
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.verify_calls);
  TEST_ASSERT_EQUAL_UINT32(erases + 4u, flash->erase_count);
  TEST_ASSERT_EQUAL_UINT32(writes + 1u, flash->write_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_read_meta_data(0, &first));
  TEST_ASSERT_EQUAL_UINT8(ESY_FIRMWARE_STATE_UNCONFIRMED, first.state);
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_read_meta_data(1, &preferred));
  TEST_ASSERT_EQUAL_UINT8(ESY_FIRMWARE_STATE_TRY_BOOT, preferred.state);
  TEST_ASSERT_FALSE(esy_bootloader_jump_to_app());
  TEST_ASSERT_EQUAL_UINT32(TEST_FLASH_BASE,
                         fake_esy_hal_boot_get_state()->last_jump_address);
}

void test_esy_bootloader_never_selects_a_trial_when_all_promotions_fail(void) {
  const esy_bootloader_config_t config = make_bootloader_config();
  esy_firmware_header_t first = make_header(1u, ESY_FIRMWARE_STATE_TRY_BOOT);
  esy_firmware_header_t preferred = make_header(2u, ESY_FIRMWARE_STATE_TRY_BOOT);
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_write_meta_data(0, &first));
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_write_meta_data(1, &preferred));
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  const uint32_t erases = flash->erase_count;
  const uint32_t writes = flash->write_count;
  flash->erase_result = ESY_ERR_IO;

  for (uint32_t reset = 0u; reset < 2u; ++reset) {
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_init(&config));
    TEST_ASSERT_EQUAL_INT(-1, esy_bootloader_get_selected_slot());
    TEST_ASSERT_FALSE(esy_bootloader_jump_to_app());
    TEST_ASSERT_EQUAL_UINT32(0u,
                           fake_esy_hal_boot_get_state()->last_jump_address);
    TEST_ASSERT_EQUAL_UINT32(erases + 6u * (reset + 1u), flash->erase_count);
    TEST_ASSERT_EQUAL_UINT32(writes, flash->write_count);
    TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_read_meta_data(0, &first));
    TEST_ASSERT_EQUAL_UINT8(ESY_FIRMWARE_STATE_TRY_BOOT, first.state);
    TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_read_meta_data(1, &preferred));
    TEST_ASSERT_EQUAL_UINT8(ESY_FIRMWARE_STATE_TRY_BOOT, preferred.state);
  }
}

void test_esy_bootloader_keeps_nontrial_fallbacks_when_trial_promotion_fails(void) {
  const uint8_t fallback_states[] = {ESY_FIRMWARE_STATE_UNCONFIRMED,
                                     ESY_FIRMWARE_STATE_CONFIRMED};
  const esy_bootloader_config_t config = make_bootloader_config();
  for (uint32_t index = 0u; index < sizeof(fallback_states); ++index) {
    esy_firmware_header_t fallback = make_header(1u, fallback_states[index]);
    esy_firmware_header_t trial = make_header(2u, ESY_FIRMWARE_STATE_TRY_BOOT);
    fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
    flash->erase_result = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_write_meta_data(0, &fallback));
    TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_write_meta_data(1, &trial));
    const uint32_t writes = flash->write_count;
    flash->erase_result = ESY_ERR_IO;

    TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_init(&config));
    TEST_ASSERT_EQUAL_INT(0, esy_bootloader_get_selected_slot());
    TEST_ASSERT_EQUAL_UINT32(writes, flash->write_count);
    TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_read_meta_data(0, &fallback));
    TEST_ASSERT_EQUAL_UINT8(fallback_states[index], fallback.state);
  }
}

void test_esy_bootloader_reports_running_and_flash_slot_from_runtime(void) {
  const esy_bootloader_config_t config = make_bootloader_config();
  esy_firmware_header_t slot0 = make_header(1u, ESY_FIRMWARE_STATE_CONFIRMED);
  esy_firmware_header_t slot1 = make_header(3u, ESY_FIRMWARE_STATE_CONFIRMED);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_bootloader_write_meta_data(0, &slot0));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_bootloader_write_meta_data(1, &slot1));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_bootloader_init(&config));

  fake_esy_hal_boot_get_state()->runtime.running_address = TEST_FLASH_BASE + 16u;
  TEST_ASSERT_EQUAL_INT(1, esy_bootloader_get_selected_slot());
  TEST_ASSERT_EQUAL_INT(0, esy_bootloader_get_running_slot());
  TEST_ASSERT_EQUAL_INT(1, esy_bootloader_get_flash_slot());
  TEST_ASSERT_EQUAL_UINT32(TEST_SLOT_B,
                           esy_bootloader_get_flash_slot_addr());
  TEST_ASSERT_EQUAL_UINT32(TEST_SLOT_SIZE,
                           esy_bootloader_get_flash_slot_size());

  fake_esy_hal_boot_get_state()->runtime.running_address = 0u;
  TEST_ASSERT_EQUAL_INT(1, esy_bootloader_get_selected_slot());
  TEST_ASSERT_EQUAL_INT(-1, esy_bootloader_get_running_slot());
  TEST_ASSERT_EQUAL_INT(-1, esy_bootloader_get_flash_slot());
  TEST_ASSERT_EQUAL_UINT32(0u, esy_bootloader_get_flash_slot_addr());
  TEST_ASSERT_EQUAL_UINT32(0u, esy_bootloader_get_flash_slot_size());
}

void test_esy_bootloader_rejects_slot_with_wrong_image_hash(void) {
  const esy_bootloader_config_t config = make_bootloader_config();
  esy_firmware_header_t slot0 = make_header(1u, ESY_FIRMWARE_STATE_CONFIRMED);
  esy_firmware_header_t slot1 = make_header(3u, ESY_FIRMWARE_STATE_CONFIRMED);

  slot1.image_sha256[0] ^= 1u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_bootloader_write_meta_data(0, &slot0));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_bootloader_write_meta_data(1, &slot1));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_bootloader_init(&config));
  TEST_ASSERT_EQUAL_INT(0, esy_bootloader_get_selected_slot());
}

void test_esy_bootloader_fails_closed_when_p256_rejects(void) {
  const esy_bootloader_config_t config = make_bootloader_config();
  esy_firmware_header_t slot0 = make_header(1u, ESY_FIRMWARE_STATE_CONFIRMED);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_bootloader_write_meta_data(0, &slot0));
  s_ctx.verify_result = false;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_FAIL, esy_bootloader_init(&config));
  TEST_ASSERT_EQUAL_INT(-1, esy_bootloader_get_selected_slot());
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.verify_calls);
}

void test_esy_bootloader_rejects_missing_layout_without_flash_io(void) {
  const esy_bootloader_config_t config = make_bootloader_config();
  fake_esy_hal_boot_get_state()->result = ESY_ERR_NOT_SUPPORTED;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_FAIL, esy_bootloader_init(&config));
  TEST_ASSERT_EQUAL_INT(-1, esy_bootloader_get_selected_slot());
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_flash_get_state()->read_count);
  TEST_ASSERT_FALSE(esy_bootloader_jump_to_app());
}

void test_esy_bootloader_preserves_metadata_wire_format_and_signed_digest(void) {
  esy_firmware_header_t header = make_header(0x12345678u, ESY_FIRMWARE_STATE_CONFIRMED);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_bootloader_write_meta_data(0, &header));
  esy_firmware_header_t stored = {0};
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_bootloader_read_meta_data(0, &stored));
  TEST_ASSERT_EQUAL_UINT32(256u, sizeof(stored));
  TEST_ASSERT_EQUAL_UINT32(0x4553594Du, stored.magic);
  TEST_ASSERT_EQUAL_UINT32(1u, stored.format_version);
  TEST_ASSERT_EQUAL_MEMORY(header.image_sha256, stored.image_sha256, 32u);
  TEST_ASSERT_EQUAL_MEMORY(header.signature, stored.signature, 64u);
  const esy_bootloader_config_t config = make_bootloader_config();
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_bootloader_init(&config));
  uint8_t canonical[40] = {0x78u, 0x56u, 0x34u, 0x12u, 0u, 4u, 0u, 0u};
  uint8_t digest[32];
  memcpy(&canonical[8], header.image_sha256, 32u);
  esy_sha256(canonical, sizeof(canonical), digest);
  TEST_ASSERT_EQUAL_MEMORY(digest, s_ctx.signed_digest, sizeof(digest));
}

void test_esy_bootloader_rejects_corrupt_crc_and_propagates_flash_errors(void) {
  esy_firmware_header_t header = make_header(1u, ESY_FIRMWARE_STATE_CONFIRMED);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_bootloader_write_meta_data(0, &header));
  s_ctx.flash[TEST_META_A - TEST_FLASH_BASE + 8u] ^= 1u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_CRC_FAIL, esy_bootloader_read_meta_data(0, &header));
  fake_esy_hal_flash_get_state()->erase_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_bootloader_write_meta_data(0, &header));
}

void test_esy_bootloader_confirmation_preserves_authenticated_metadata(void) {
  esy_firmware_header_t header = make_header(0x12345678u, ESY_FIRMWARE_STATE_UNCONFIRMED);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_bootloader_write_meta_data(0, &header));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_bootloader_mark_success());
  esy_firmware_header_t confirmed = {0};
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_bootloader_read_meta_data(0, &confirmed));
  TEST_ASSERT_EQUAL_UINT8(ESY_FIRMWARE_STATE_CONFIRMED, confirmed.state);
  TEST_ASSERT_EQUAL_UINT32(header.version, confirmed.version);
  TEST_ASSERT_EQUAL_UINT32(header.firmware_size, confirmed.firmware_size);
  TEST_ASSERT_EQUAL_MEMORY(header.image_sha256, confirmed.image_sha256, 32u);
  TEST_ASSERT_EQUAL_MEMORY(header.signature, confirmed.signature, 64u);
  uint32_t writes = fake_esy_hal_flash_get_state()->write_count;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_bootloader_mark_success());
  TEST_ASSERT_EQUAL_UINT32(writes, fake_esy_hal_flash_get_state()->write_count);
}

void test_esy_bootloader_confirmation_rejects_unknown_image_and_io_failure(void) {
  fake_esy_hal_boot_get_state()->runtime.running_address = 0u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_FAIL, esy_bootloader_mark_success());
  fake_esy_hal_boot_get_state()->runtime.running_address = TEST_FLASH_BASE;
  fake_esy_hal_flash_get_state()->read_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_bootloader_mark_success());
}

void test_layout_rejects_geometry_errors_and_out_of_flash_without_io(void) {
  esy_firmware_header_t header = make_header(1u, ESY_FIRMWARE_STATE_CONFIRMED);
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  flash->geometry_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_write_meta_data(0, &header));
  flash->geometry_result = ESY_OK;
  flash->size_bytes = 0x10000u;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_write_meta_data(0, &header));
  flash->size_bytes = TEST_FLASH_BYTES;
  flash->program_size = 512u;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_write_meta_data(0, &header));
  TEST_ASSERT_EQUAL_UINT32(0u, flash->erase_count);
  TEST_ASSERT_EQUAL_UINT32(0u, flash->write_count);
  TEST_ASSERT_EQUAL_UINT32(0u, flash->read_count);
}

void test_layout_rejects_every_region_overlap_and_overflow_before_io(void) {
  esy_bootloader_layout_t *layout = &fake_esy_hal_boot_get_state()->layout;
  esy_bootloader_layout_t original = *layout;
  esy_firmware_header_t header = make_header(1u, ESY_FIRMWARE_STATE_CONFIRMED);
  layout->slot_size = 0x10000u;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_write_meta_data(0, &header));
  *layout = original;
  layout->slot_addr[1] = layout->slot_addr[0];
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_write_meta_data(0, &header));
  *layout = original;
  layout->meta_addr[1] = layout->meta_addr[0];
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_write_meta_data(0, &header));
  *layout = original;
  layout->slot_addr[1] = UINT32_MAX - 255u;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_write_meta_data(0, &header));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_flash_get_state()->erase_count);
}

void test_layout_rejects_partial_metadata_sector_instead_of_rounding_up(void) {
  esy_firmware_header_t header = make_header(1u, ESY_FIRMWARE_STATE_CONFIRMED);
  esy_bootloader_layout_t *layout = &fake_esy_hal_boot_get_state()->layout;
  layout->meta_size[0] = ESY_FIRMWARE_METADATA_SIZE;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_write_meta_data(0, &header));
  layout->meta_size[0] = 0x1000u;
  layout->meta_addr[0] += 0x100u;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_write_meta_data(0, &header));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_flash_get_state()->erase_count);
}

void test_metadata_erases_exact_owned_region_on_nonuniform_geometry(void) {
  esy_firmware_header_t header = make_header(1u, ESY_FIRMWARE_STATE_CONFIRMED);
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  esy_bootloader_layout_t *layout = &fake_esy_hal_boot_get_state()->layout;
  flash->sector_size_after_address = TEST_SLOT_B;
  flash->sector_size_after = 0x2000u;
  layout->slot_size = 0xE000u;
  layout->meta_addr[0] = TEST_FLASH_BASE + 0xE000u;
  layout->meta_addr[1] = TEST_SLOT_B + 0xE000u;
  layout->meta_size[0] = 0x2000u;
  layout->meta_size[1] = 0x2000u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_write_meta_data(1, &header));
  TEST_ASSERT_EQUAL_UINT32(layout->meta_addr[1], flash->last_erase_addr);
  TEST_ASSERT_EQUAL_UINT32(0x2000u, flash->last_erase_len);
}

void test_metadata_uses_each_slots_complete_64k_and_128k_sector(void) {
  use_nonuniform_metadata_layout();
  memset(s_ctx.flash, 0xA5, sizeof(s_ctx.flash));
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  esy_firmware_header_t header_a = make_header(1u, ESY_FIRMWARE_STATE_CONFIRMED);
  esy_firmware_header_t header_b = make_header(2u, ESY_FIRMWARE_STATE_TRY_BOOT);
  const size_t meta_a_offset = TEST_NONUNIFORM_META_A - TEST_FLASH_BASE;
  const size_t meta_b_offset = TEST_NONUNIFORM_META_B - TEST_FLASH_BASE;

  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_write_meta_data(0, &header_a));
  TEST_ASSERT_EQUAL_UINT32(TEST_NONUNIFORM_META_A, flash->last_erase_addr);
  TEST_ASSERT_EQUAL_UINT32(TEST_NONUNIFORM_META_A_BYTES, flash->last_erase_len);
  TEST_ASSERT_EQUAL_HEX8(0xA5u, s_ctx.flash[meta_a_offset - 1u]);
  TEST_ASSERT_EACH_EQUAL_HEX8(0xFFu,
      &s_ctx.flash[meta_a_offset + ESY_FIRMWARE_METADATA_SIZE],
      TEST_NONUNIFORM_META_A_BYTES - ESY_FIRMWARE_METADATA_SIZE);
  TEST_ASSERT_EQUAL_HEX8(0xA5u, s_ctx.flash[meta_b_offset]);

  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_write_meta_data(1, &header_b));
  TEST_ASSERT_EQUAL_UINT32(TEST_NONUNIFORM_META_B, flash->last_erase_addr);
  TEST_ASSERT_EQUAL_UINT32(TEST_NONUNIFORM_META_B_BYTES, flash->last_erase_len);
  TEST_ASSERT_EQUAL_UINT32(2u, flash->erase_count);
  TEST_ASSERT_EACH_EQUAL_HEX8(0xFFu,
      &s_ctx.flash[meta_b_offset + ESY_FIRMWARE_METADATA_SIZE],
      TEST_NONUNIFORM_META_B_BYTES - ESY_FIRMWARE_METADATA_SIZE);
  TEST_ASSERT_EQUAL_HEX8(0xA5u,
      s_ctx.flash[meta_b_offset + TEST_NONUNIFORM_META_B_BYTES]);
  TEST_ASSERT_EQUAL_HEX8(0xA5u, s_ctx.flash[meta_a_offset - 1u]);

  esy_firmware_header_t stored_a = {0};
  esy_firmware_header_t stored_b = {0};
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_read_meta_data(0, &stored_a));
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_read_meta_data(1, &stored_b));
  TEST_ASSERT_EQUAL_UINT32(header_a.version, stored_a.version);
  TEST_ASSERT_EQUAL_UINT32(header_b.version, stored_b.version);
  TEST_ASSERT_EQUAL_UINT8(header_a.state, stored_a.state);
  TEST_ASSERT_EQUAL_UINT8(header_b.state, stored_b.state);
}

void test_invalid_slot_b_metadata_capacity_rejects_the_layout_before_io(void) {
  static const uint32_t k_invalid_sizes[] = {
      0u, ESY_FIRMWARE_METADATA_SIZE - 4u, 0x4000u,
      TEST_NONUNIFORM_META_B_BYTES - 1u};
  use_nonuniform_metadata_layout();
  esy_bootloader_layout_t *layout = &fake_esy_hal_boot_get_state()->layout;
  esy_firmware_header_t header = make_header(1u, ESY_FIRMWARE_STATE_CONFIRMED);
  const esy_bootloader_config_t config = make_bootloader_config();

  for (size_t i = 0u; i < sizeof(k_invalid_sizes) / sizeof(k_invalid_sizes[0]); ++i) {
    layout->meta_size[1] = k_invalid_sizes[i];
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_write_meta_data(0, &header));
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_write_meta_data(1, &header));
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_read_meta_data(1, &header));
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_init(&config));
    assert_no_flash_io();
  }
}

void test_distinct_metadata_capacities_reject_overlap_and_overflow_before_io(void) {
  use_nonuniform_metadata_layout();
  esy_bootloader_layout_t *layout = &fake_esy_hal_boot_get_state()->layout;
  const esy_bootloader_layout_t original = *layout;
  esy_firmware_header_t header = make_header(1u, ESY_FIRMWARE_STATE_CONFIRMED);

  layout->meta_size[0] = TEST_NONUNIFORM_META_A_BYTES + TEST_NONUNIFORM_META_B_BYTES;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_write_meta_data(0, &header));
  *layout = original;
  layout->meta_addr[1] = TEST_FLASH_BASE;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_write_meta_data(0, &header));
  *layout = original;
  layout->meta_size[1] = 2u * TEST_NONUNIFORM_META_B_BYTES;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_write_meta_data(0, &header));
  *layout = original;
  layout->meta_size[1] = 0xF0000000u;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_write_meta_data(0, &header));
  *layout = original;
  layout->meta_addr[1] = UINT32_MAX - 0xFFFFu;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_write_meta_data(0, &header));
  assert_no_flash_io();
}

void test_metadata_propagates_program_and_verify_failures(void) {
  esy_firmware_header_t header = make_header(1u, ESY_FIRMWARE_STATE_CONFIRMED);
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  flash->write_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_bootloader_write_meta_data(0, &header));
  flash->write_result = ESY_OK;
  flash->read_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_bootloader_write_meta_data(0, &header));
  flash->read_result = ESY_OK;
  flash->sector_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_write_meta_data(0, &header));
}

void test_managed_init_preserves_native_running_selection_and_other_trial_metadata(void) {
  const esy_bootloader_config_t config = make_bootloader_config();
  esy_firmware_header_t current = make_header(1u, ESY_FIRMWARE_STATE_CONFIRMED);
  esy_firmware_header_t candidate = make_header(9u, ESY_FIRMWARE_STATE_TRY_BOOT);
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_write_meta_data(0, &current));
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_write_meta_data(1, &candidate));
  fake_esy_hal_boot_get_state()->runtime.mode = ESY_BOOT_MODE_MANAGED;
  const uint32_t writes = fake_esy_hal_flash_get_state()->write_count;
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_init(&config));
  TEST_ASSERT_EQUAL_INT(0, esy_bootloader_get_selected_slot());
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.verify_calls);
  TEST_ASSERT_EQUAL_UINT32(writes, fake_esy_hal_flash_get_state()->write_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_read_meta_data(1, &candidate));
  TEST_ASSERT_EQUAL_UINT8(ESY_FIRMWARE_STATE_TRY_BOOT, candidate.state);
}

void test_managed_pending_trial_keeps_running_slot_and_protects_rollback_image(void) {
  const esy_bootloader_config_t config = make_bootloader_config();
  esy_firmware_header_t fallback = make_header(1u, ESY_FIRMWARE_STATE_CONFIRMED);
  esy_firmware_header_t running = make_header(2u, ESY_FIRMWARE_STATE_UNCONFIRMED);
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_write_meta_data(0, &fallback));
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_write_meta_data(1, &running));
  fake_esy_hal_boot_state_t *boot = fake_esy_hal_boot_get_state();
  boot->runtime.mode = ESY_BOOT_MODE_MANAGED;
  boot->runtime.running_address = TEST_SLOT_B;
  boot->runtime.update_allowed = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_init(&config));
  TEST_ASSERT_EQUAL_INT(1, esy_bootloader_get_selected_slot());
  TEST_ASSERT_EQUAL_INT(1, esy_bootloader_get_running_slot());
  TEST_ASSERT_EQUAL_INT(-1, esy_bootloader_get_flash_slot());
  TEST_ASSERT_EQUAL_UINT32(0u, esy_bootloader_get_flash_slot_addr());
  TEST_ASSERT_EQUAL_UINT32(0u, esy_bootloader_get_flash_slot_size());
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_bootloader_activate_image(0));
  TEST_ASSERT_EQUAL_UINT32(0u, boot->pending_count);
}

void test_runtime_flash_offset_identifies_image_independently_of_mapped_pc(void) {
  esy_bootloader_layout_t *layout = &fake_esy_hal_boot_get_state()->layout;
  for (uint32_t slot = 0u; slot < ESY_BOOTLOADER_SLOT_COUNT; ++slot) {
    layout->slot_addr[slot] -= TEST_FLASH_BASE;
    layout->meta_addr[slot] -= TEST_FLASH_BASE;
  }
  fake_esy_hal_flash_get_state()->storage_base_addr = 0u;
  fake_esy_hal_system_get_ctx()->pc = 0x400D1234u;
  fake_esy_hal_boot_get_state()->runtime = (esy_boot_runtime_t){
      .mode = ESY_BOOT_MODE_MANAGED,
      .running_address = 0x10000u,
      .update_allowed = true};
  TEST_ASSERT_EQUAL_INT(1, esy_bootloader_get_running_slot());
  TEST_ASSERT_EQUAL_INT(0, esy_bootloader_get_flash_slot());
  fake_esy_hal_boot_get_state()->runtime.running_address = 0u;
  TEST_ASSERT_EQUAL_INT(0, esy_bootloader_get_running_slot());
  TEST_ASSERT_EQUAL_INT(1, esy_bootloader_get_flash_slot());
}

void test_runtime_query_failure_rejects_selection_and_mutating_operations(void) {
  const esy_bootloader_config_t config = make_bootloader_config();
  fake_esy_hal_boot_state_t *boot = fake_esy_hal_boot_get_state();
  boot->runtime_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_bootloader_init(&config));
  TEST_ASSERT_EQUAL_INT(-1, esy_bootloader_get_selected_slot());
  TEST_ASSERT_EQUAL_INT(-1, esy_bootloader_get_running_slot());
  TEST_ASSERT_EQUAL_INT(-1, esy_bootloader_get_flash_slot());
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_bootloader_activate_image(1));
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_bootloader_mark_success());
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_flash_get_state()->read_count);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_flash_get_state()->write_count);
  TEST_ASSERT_EQUAL_UINT32(0u, boot->pending_count);
  TEST_ASSERT_EQUAL_UINT32(0u, boot->confirm_count);
}

void test_managed_activation_submits_exact_inactive_image_and_propagates_native_failure(void) {
  esy_firmware_header_t candidate = make_header(9u, ESY_FIRMWARE_STATE_TRY_BOOT);
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_write_meta_data(1, &candidate));
  fake_esy_hal_boot_state_t *boot = fake_esy_hal_boot_get_state();
  boot->runtime.mode = ESY_BOOT_MODE_MANAGED;
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_activate_image(1));
  TEST_ASSERT_EQUAL_UINT32(1u, boot->pending_count);
  TEST_ASSERT_EQUAL_UINT32(TEST_SLOT_B, boot->pending_address);
  TEST_ASSERT_EQUAL_UINT32(candidate.firmware_size, boot->pending_size);
  boot->pending_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_bootloader_activate_image(1));
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_read_meta_data(1, &candidate));
  TEST_ASSERT_EQUAL_UINT8(ESY_FIRMWARE_STATE_TRY_BOOT, candidate.state);
}

void test_activation_rejects_active_invalid_or_nontrial_images_before_native_mutation(void) {
  fake_esy_hal_boot_state_t *boot = fake_esy_hal_boot_get_state();
  boot->runtime.mode = ESY_BOOT_MODE_MANAGED;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_bootloader_activate_image(-1));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_bootloader_activate_image(2));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_bootloader_activate_image(0));
  TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_bootloader_activate_image(1));
  esy_firmware_header_t candidate = make_header(9u, ESY_FIRMWARE_STATE_CONFIRMED);
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_write_meta_data(1, &candidate));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_bootloader_activate_image(1));
  TEST_ASSERT_EQUAL_UINT32(0u, boot->pending_count);
}

void test_direct_activation_uses_trial_metadata_without_native_submission(void) {
  esy_firmware_header_t candidate = make_header(9u, ESY_FIRMWARE_STATE_TRY_BOOT);
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_write_meta_data(1, &candidate));
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_activate_image(1));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_boot_get_state()->pending_count);
}

void test_managed_confirmation_retries_native_failure_after_metadata_is_confirmed(void) {
  esy_firmware_header_t current = make_header(1u, ESY_FIRMWARE_STATE_UNCONFIRMED);
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_write_meta_data(0, &current));
  fake_esy_hal_boot_state_t *boot = fake_esy_hal_boot_get_state();
  boot->runtime.mode = ESY_BOOT_MODE_MANAGED;
  boot->confirm_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_bootloader_mark_success());
  TEST_ASSERT_EQUAL_UINT32(1u, boot->confirm_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_read_meta_data(0, &current));
  TEST_ASSERT_EQUAL_UINT8(ESY_FIRMWARE_STATE_CONFIRMED, current.state);
  const uint32_t writes = fake_esy_hal_flash_get_state()->write_count;
  boot->confirm_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_mark_success());
  TEST_ASSERT_EQUAL_UINT32(2u, boot->confirm_count);
  TEST_ASSERT_EQUAL_UINT32(writes, fake_esy_hal_flash_get_state()->write_count);
}

void test_managed_confirmation_preserves_native_rollback_on_invalid_metadata_or_write_failure(void) {
  fake_esy_hal_boot_state_t *boot = fake_esy_hal_boot_get_state();
  boot->runtime.mode = ESY_BOOT_MODE_MANAGED;
  TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_bootloader_mark_success());
  esy_firmware_header_t current = make_header(1u, ESY_FIRMWARE_STATE_UNCONFIRMED);
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_write_meta_data(0, &current));
  fake_esy_hal_flash_get_state()->write_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_bootloader_mark_success());
  TEST_ASSERT_EQUAL_UINT32(0u, boot->confirm_count);
}

void test_managed_init_rejects_unidentified_or_unauthenticated_running_image(void) {
  const esy_bootloader_config_t config = make_bootloader_config();
  fake_esy_hal_boot_state_t *boot = fake_esy_hal_boot_get_state();
  boot->runtime.mode = ESY_BOOT_MODE_MANAGED;
  boot->runtime.running_address = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_init(&config));
  boot->runtime.running_address = TEST_FLASH_BASE;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_init(&config));
  esy_firmware_header_t current = make_header(1u, ESY_FIRMWARE_STATE_CONFIRMED);
  TEST_ASSERT_EQUAL(ESY_OK, esy_bootloader_write_meta_data(0, &current));
  s_ctx.verify_result = false;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_bootloader_init(&config));
  TEST_ASSERT_EQUAL_INT(-1, esy_bootloader_get_selected_slot());
}
