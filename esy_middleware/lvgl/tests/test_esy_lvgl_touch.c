/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/lvgl/esy_lvgl_touch.h"
#include "unity.h"

#include <string.h>

typedef struct {
  esy_touch_t touch;
  esy_lvgl_pointer_input_t input;
  esy_err_t poll_result;
} touch_input_test_ctx_t;
static touch_input_test_ctx_t s_ctx;

static esy_err_t poll(void *arg) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx.touch, arg);
  return s_ctx.poll_result;
}

static esy_err_t sample(esy_touch_data_t *value, void *arg) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx.touch, arg);
  *value = (esy_touch_data_t){.x=12u, .y=34u, .is_pressed=true};
  return ESY_OK;
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.touch.ctx = &s_ctx;
  s_ctx.touch.poll = poll;
  s_ctx.touch.get_sample = sample;
}

void tearDown(void) {}

void test_touch_descriptor_preserves_output_on_invalid_or_unattached_handle(void) {
  s_ctx.input.arg = &s_ctx;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_lvgl_touch_input_init(NULL, &s_ctx.touch));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_lvgl_touch_input_init(&s_ctx.input, NULL));
  s_ctx.touch.ctx = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_lvgl_touch_input_init(&s_ctx.input, &s_ctx.touch));
  TEST_ASSERT_EQUAL_PTR(&s_ctx, s_ctx.input.arg);
  TEST_ASSERT_NULL(s_ctx.input.read);
}

void test_touch_descriptor_reads_facade_and_propagates_errors(void) {
  uint16_t x=0u, y=0u;
  bool pressed=false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lvgl_touch_input_init(&s_ctx.input, &s_ctx.touch));
  TEST_ASSERT_EQUAL(ESY_OK, s_ctx.input.read(&x, &y, &pressed, s_ctx.input.arg));
  TEST_ASSERT_EQUAL_UINT16(12u, x);
  TEST_ASSERT_EQUAL_UINT16(34u, y);
  TEST_ASSERT_TRUE(pressed);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, s_ctx.input.read(NULL, &y, &pressed, s_ctx.input.arg));
  s_ctx.poll_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, s_ctx.input.read(&x, &y, &pressed, s_ctx.input.arg));
}
