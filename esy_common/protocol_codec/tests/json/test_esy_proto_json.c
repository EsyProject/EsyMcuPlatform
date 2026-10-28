/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"

#include <limits.h>
#include <string.h>

#include "esy_common/protocol/esy_proto_json.h"

void setUp(void) {}
void tearDown(void) {}

static void parse_document(const char *document, esy_json_value_t *value) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_parse(document, strlen(document), value));
}

static void init_writer(esy_json_writer_t *writer, char *buffer, size_t capacity,
                        uint8_t *frames, size_t depth) {
    esy_json_writer_resources_t resources = {buffer, capacity, frames, depth};
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_init(writer, &resources));
}

static void assert_view_unchanged(const esy_json_value_t *before,
                                 const esy_json_value_t *after) {
    TEST_ASSERT_EQUAL_PTR(before->data, after->data);
    TEST_ASSERT_EQUAL_size_t(before->length, after->length);
    TEST_ASSERT_EQUAL(before->type, after->type);
}

void test_parse_borrows_one_complete_root_and_trims_json_whitespace(void) {
    const char input[] = {' ', '\t', '[', '1', ']', '\r', '\n'};
    esy_json_value_t value;
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_parse(input, sizeof(input), &value));
    TEST_ASSERT_EQUAL_PTR(input + 2, value.data);
    TEST_ASSERT_EQUAL_size_t(3, value.length);
    TEST_ASSERT_EQUAL(ESY_JSON_ARRAY, value.type);
}

void test_parse_identifies_all_root_types_without_requiring_a_terminator(void) {
    const char *documents[] = {"null", "true", "false", "-1.25e+2", "\"x\"", "[]", "{}"};
    const esy_json_type_t types[] = {ESY_JSON_NULL, ESY_JSON_BOOL, ESY_JSON_BOOL,
        ESY_JSON_NUMBER, ESY_JSON_STRING, ESY_JSON_ARRAY, ESY_JSON_OBJECT};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        esy_json_value_t value;
        parse_document(documents[i], &value);
        TEST_ASSERT_EQUAL(types[i], value.type);
        TEST_ASSERT_EQUAL_size_t(strlen(documents[i]), value.length);
    }
}

void test_parse_rejects_truncation_trailing_data_and_invalid_syntax_without_output(void) {
    const char *documents[] = {" ", "[", "{\"a\":", "{\"a\":1}junk", "{} {}",
        "[1,]", "01", "+1", "NaN", "\"\\q\"", "true false"};
    const esy_json_value_t sentinel = {"sentinel", 8, ESY_JSON_STRING};
    for (size_t i = 0; i < sizeof(documents) / sizeof(documents[0]); ++i) {
        esy_json_value_t value = sentinel;
        TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_json_parse(documents[i], strlen(documents[i]), &value));
        assert_view_unchanged(&sentinel, &value);
    }
    esy_json_value_t value = sentinel;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_parse(NULL, 1, &value));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_parse("", 0, &value));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_parse("null", 4, NULL));
    assert_view_unchanged(&sentinel, &value);
}

void test_parse_reports_provider_depth_exhaustion(void) {
    char document[67];
    memset(document, '[', 33);
    document[33] = '0';
    memset(document + 34, ']', 33);
    esy_json_value_t value = {0};
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_json_parse(document, sizeof(document), &value));
}

void test_iterator_visits_only_direct_children_and_preserves_string_quotes(void) {
    esy_json_value_t root, key, value;
    esy_json_iterator_t iterator;
    parse_document("{\"a\": [1,{\"b\":true}], \"s\":\"x\\ny\"}", &root);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_iterator_init(&iterator, &root));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_iterator_next(&iterator, &key, &value));
    TEST_ASSERT_EQUAL(ESY_JSON_STRING, key.type);
    TEST_ASSERT_EQUAL_size_t(3, key.length);
    TEST_ASSERT_EQUAL_MEMORY("\"a\"", key.data, key.length);
    TEST_ASSERT_EQUAL(ESY_JSON_ARRAY, value.type);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_iterator_next(&iterator, &key, &value));
    TEST_ASSERT_EQUAL_MEMORY("\"s\"", key.data, key.length);
    TEST_ASSERT_EQUAL(ESY_JSON_STRING, value.type);
    TEST_ASSERT_EQUAL_size_t(6, value.length);
    TEST_ASSERT_EQUAL_MEMORY("\"x\\ny\"", value.data, value.length);
    const esy_json_value_t key_before = key, value_before = value;
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_json_iterator_next(&iterator, &key, &value));
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_json_iterator_next(&iterator, &key, &value));
    assert_view_unchanged(&key_before, &key);
    assert_view_unchanged(&value_before, &value);
}

void test_iterator_returns_null_key_for_arrays_and_supports_reset(void) {
    esy_json_value_t root, key, value;
    esy_json_iterator_t iterator;
    parse_document("[false,{},[]]", &root);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_iterator_init(&iterator, &root));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_iterator_next(&iterator, &key, &value));
    TEST_ASSERT_NULL(key.data);
    TEST_ASSERT_EQUAL_size_t(0, key.length);
    TEST_ASSERT_EQUAL(ESY_JSON_NULL, key.type);
    TEST_ASSERT_EQUAL(ESY_JSON_BOOL, value.type);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_iterator_next(&iterator, &key, &value));
    TEST_ASSERT_EQUAL(ESY_JSON_OBJECT, value.type);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_iterator_next(&iterator, &key, &value));
    TEST_ASSERT_EQUAL(ESY_JSON_ARRAY, value.type);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_iterator_init(&iterator, &value));
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_json_iterator_next(&iterator, &key, &value));
}

void test_iterator_rejects_zero_handle_wrong_type_and_invalid_arguments(void) {
    esy_json_iterator_t iterator = {0};
    esy_json_value_t root, key, value;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_json_iterator_next(&iterator, &key, &value));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_iterator_next(NULL, &key, &value));
    parse_document("1", &root);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_iterator_init(&iterator, &root));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_iterator_init(NULL, &root));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_iterator_init(&iterator, NULL));
    parse_document("[]", &root);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_iterator_init(&iterator, &root));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_iterator_next(&iterator, NULL, &value));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_iterator_next(&iterator, &key, NULL));
}

void test_object_get_matches_decoded_keys_including_empty_unicode_and_embedded_nul(void) {
    esy_json_value_t root, value;
    uint64_t integer;
    parse_document("{\"\\u0061\":1,\"\":2,\"\\u00e9\":3,\"x\\u0000y\":4}", &root);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_object_get(&root, "a", 1, &value));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_as_u64(&value, &integer));
    TEST_ASSERT_EQUAL_UINT64(1, integer);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_object_get(&root, NULL, 0, &value));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_as_u64(&value, &integer));
    TEST_ASSERT_EQUAL_UINT64(2, integer);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_object_get(&root, "\xc3\xa9", 2, &value));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_as_u64(&value, &integer));
    TEST_ASSERT_EQUAL_UINT64(3, integer);
    const char binary_key[] = {'x', 0, 'y'};
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_object_get(&root, binary_key, sizeof(binary_key), &value));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_as_u64(&value, &integer));
    TEST_ASSERT_EQUAL_UINT64(4, integer);
}

void test_object_get_rejects_duplicate_decoded_matches_and_preserves_output(void) {
    esy_json_value_t root;
    const esy_json_value_t sentinel = {"sentinel", 8, ESY_JSON_STRING};
    esy_json_value_t value = sentinel;
    parse_document("{\"a\":1,\"\\u0061\":2,\"b\":3}", &root);
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_json_object_get(&root, "a", 1, &value));
    assert_view_unchanged(&sentinel, &value);
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_json_object_get(&root, "A", 1, &value));
    assert_view_unchanged(&sentinel, &value);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_object_get(&root, "b", 1, &value));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_object_get(&root, NULL, 1, &value));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_object_get(NULL, "a", 1, &value));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_object_get(&root, "a", 1, NULL));
    parse_document("[]", &root);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_object_get(&root, "a", 1, &value));
}

void test_array_get_borrows_nested_element_and_reports_out_of_range(void) {
    esy_json_value_t root, value;
    parse_document("[null,[2],{\"x\":3}]", &root);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_array_get(&root, 2, &value));
    TEST_ASSERT_EQUAL(ESY_JSON_OBJECT, value.type);
    const esy_json_value_t before = value;
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_json_array_get(&root, 3, &value));
    assert_view_unchanged(&before, &value);
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_json_array_get(&root, SIZE_MAX, &value));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_array_get(&root, 0, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_array_get(NULL, 0, &value));
    parse_document("{}", &root);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_array_get(&root, 0, &value));
}

void test_boolean_conversion_checks_type_and_preserves_failed_output(void) {
    esy_json_value_t value;
    bool result = false;
    parse_document("true", &value);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_as_bool(&value, &result));
    TEST_ASSERT_TRUE(result);
    parse_document("false", &value);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_as_bool(&value, &result));
    TEST_ASSERT_FALSE(result);
    parse_document("1", &value);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_as_bool(&value, &result));
    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_as_bool(NULL, &result));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_as_bool(&value, NULL));
}

void test_unsigned_conversion_preserves_all_bits_and_rejects_noninteger_forms(void) {
    const char *valid[] = {"0", "9007199254740993", "18446744073709551615"};
    const uint64_t expected[] = {0, UINT64_C(9007199254740993), UINT64_MAX};
    esy_json_value_t value;
    uint64_t result;
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        parse_document(valid[i], &value);
        TEST_ASSERT_EQUAL(ESY_OK, esy_json_as_u64(&value, &result));
        TEST_ASSERT_EQUAL_UINT64(expected[i], result);
    }
    const char *invalid[] = {"18446744073709551616", "-1", "-0", "1.0", "1e0"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        result = 42;
        parse_document(invalid[i], &value);
        TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_json_as_u64(&value, &result));
        TEST_ASSERT_EQUAL_UINT64(42, result);
    }
    parse_document("\"1\"", &value);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_as_u64(&value, &result));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_as_u64(NULL, &result));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_as_u64(&value, NULL));
}

void test_signed_conversion_accepts_both_extremes_without_overflow(void) {
    const char *valid[] = {"-9223372036854775808", "9223372036854775807", "-0", "0"};
    const int64_t expected[] = {INT64_MIN, INT64_MAX, 0, 0};
    esy_json_value_t value;
    int64_t result;
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        parse_document(valid[i], &value);
        TEST_ASSERT_EQUAL(ESY_OK, esy_json_as_i64(&value, &result));
        TEST_ASSERT_EQUAL_INT64(expected[i], result);
    }
    const char *invalid[] = {"-9223372036854775809", "9223372036854775808", "1.0", "1e0"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        result = -42;
        parse_document(invalid[i], &value);
        TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_json_as_i64(&value, &result));
        TEST_ASSERT_EQUAL_INT64(-42, result);
    }
    parse_document("null", &value);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_as_i64(&value, &result));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_as_i64(NULL, &result));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_as_i64(&value, NULL));
}

void test_string_copy_decodes_all_escapes_utf8_surrogates_and_embedded_nul(void) {
    esy_json_value_t value;
    parse_document("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\\u0000\\u00e9\\ud83d\\ude00\"", &value);
    const char expected[] = {'"', '\\', '/', '\b', '\f', '\n', '\r', '\t', 0,
        (char)0xc3, (char)0xa9, (char)0xf0, (char)0x9f, (char)0x98, (char)0x80};
    char buffer[sizeof(expected) + 1];
    size_t length = 0;
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_string_copy(&value, NULL, 0, &length));
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), length);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_string_copy(&value, buffer, sizeof(buffer), &length));
    TEST_ASSERT_EQUAL_MEMORY(expected, buffer, sizeof(expected));
    TEST_ASSERT_EQUAL_CHAR(0, buffer[length]);
    parse_document("\"\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80\"", &value);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_string_copy(&value, NULL, 0, &length));
    TEST_ASSERT_EQUAL_size_t(9, length);
}

void test_string_copy_reports_required_size_without_partial_destination(void) {
    esy_json_value_t value;
    parse_document("\"abc\"", &value);
    char buffer[4] = {'x', 'x', 'x', 'x'};
    size_t length = 99;
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_json_string_copy(&value, buffer, 3, &length));
    TEST_ASSERT_EQUAL_size_t(3, length);
    TEST_ASSERT_EQUAL_MEMORY("xxxx", buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_string_copy(&value, buffer, sizeof(buffer), &length));
    TEST_ASSERT_EQUAL_STRING("abc", buffer);
    parse_document("\"\"", &value);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_string_copy(&value, buffer, 1, &length));
    TEST_ASSERT_EQUAL_size_t(0, length);
    TEST_ASSERT_EQUAL_STRING("", buffer);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_string_copy(&value, NULL, 1, &length));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_string_copy(&value, buffer, sizeof(buffer), NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_string_copy(NULL, buffer, sizeof(buffer), &length));
    parse_document("false", &value);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_string_copy(&value, buffer, sizeof(buffer), &length));
}

void test_reader_rejects_unpaired_surrogates_and_invalid_utf8_without_outputs(void) {
    const char *documents[] = {"\"\\ud800\"", "\"\\udc00\"", "\"\\ud800x\"",
        "\"\\ud800\\u0041\"", "\"\xc0\xaf\"", "\"\xed\xa0\x80\"",
        "\"\xf4\x90\x80\x80\"", "\"\xe2\x82\"", "\"\x80\""};
    for (size_t i = 0; i < sizeof(documents) / sizeof(documents[0]); ++i) {
        esy_json_value_t value;
        esy_err_t parsed = esy_json_parse(documents[i], strlen(documents[i]), &value);
        if (parsed != ESY_OK) {
            TEST_ASSERT_EQUAL(ESY_ERR_FAIL, parsed);
            continue;
        }
        char buffer[16];
        memset(buffer, 'x', sizeof(buffer));
        size_t length = 99;
        TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_json_string_copy(&value, buffer, sizeof(buffer), &length));
        TEST_ASSERT_EQUAL_size_t(99, length);
        for (size_t j = 0; j < sizeof(buffer); ++j) TEST_ASSERT_EQUAL_CHAR('x', buffer[j]);
    }
}

void test_writer_builds_nested_all_type_document_with_exact_integer_roundtrip(void) {
    esy_json_writer_t writer;
    char buffer[256];
    uint8_t frames[4];
    init_writer(&writer, buffer, sizeof(buffer), frames, sizeof(frames));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_begin_object(&writer));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_key(&writer, "values", 6));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_begin_array(&writer));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_u64(&writer, UINT64_MAX));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_i64(&writer, INT64_MIN));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_bool(&writer, true));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_bool(&writer, false));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_null(&writer));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_number(&writer, "-1.25e+2", 8));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_begin_object(&writer));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_key(&writer, NULL, 0));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_string(&writer, "x\n\"\\", 4));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_end(&writer));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_end(&writer));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_end(&writer));
    const char *output;
    size_t length;
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_finish(&writer, &output, &length));
    TEST_ASSERT_EQUAL_PTR(buffer, output);
    TEST_ASSERT_EQUAL_size_t(strlen(output), length);
    esy_json_value_t root, array, value;
    parse_document(output, &root);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_object_get(&root, "values", 6, &array));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_array_get(&array, 0, &value));
    uint64_t integer;
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_as_u64(&value, &integer));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, integer);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_array_get(&array, 1, &value));
    int64_t signed_integer;
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_as_i64(&value, &signed_integer));
    TEST_ASSERT_EQUAL_INT64(INT64_MIN, signed_integer);
    bool boolean;
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_array_get(&array, 2, &value));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_as_bool(&value, &boolean));
    TEST_ASSERT_TRUE(boolean);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_array_get(&array, 3, &value));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_as_bool(&value, &boolean));
    TEST_ASSERT_FALSE(boolean);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_array_get(&array, 4, &value));
    TEST_ASSERT_EQUAL(ESY_JSON_NULL, value.type);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_array_get(&array, 5, &value));
    TEST_ASSERT_EQUAL_size_t(8, value.length);
    TEST_ASSERT_EQUAL_MEMORY("-1.25e+2", value.data, value.length);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_array_get(&array, 6, &value));
    esy_json_value_t string;
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_object_get(&value, NULL, 0, &string));
    char decoded[5];
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_string_copy(&string, decoded, sizeof(decoded), &length));
    TEST_ASSERT_EQUAL_size_t(4, length);
    TEST_ASSERT_EQUAL_MEMORY("x\n\"\\", decoded, length);
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_json_array_get(&array, 7, &value));
}

void test_writer_strings_roundtrip_every_control_byte_and_valid_utf8(void) {
    esy_json_writer_t writer;
    char buffer[256];
    uint8_t frames[1];
    char text[40];
    for (size_t i = 0; i < 32; ++i) text[i] = (char)i;
    memcpy(text + 32, "\xc3\xa9\xf0\x9f\x98\x80\"\\", 8);
    init_writer(&writer, buffer, sizeof(buffer), frames, sizeof(frames));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_string(&writer, text, sizeof(text)));
    const char *output;
    size_t length;
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_finish(&writer, &output, &length));
    esy_json_value_t value;
    parse_document(output, &value);
    char decoded[41];
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_string_copy(&value, decoded, sizeof(decoded), &length));
    TEST_ASSERT_EQUAL_size_t(sizeof(text), length);
    TEST_ASSERT_EQUAL_MEMORY(text, decoded, sizeof(text));
}

void test_writer_exact_capacity_includes_terminator_and_short_output_stays_unpublished(void) {
    esy_json_writer_t writer;
    char buffer[5];
    uint8_t frames[1];
    init_writer(&writer, buffer, sizeof(buffer), frames, sizeof(frames));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_null(&writer));
    const char *output = "sentinel";
    size_t length = 99;
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_finish(&writer, &output, &length));
    TEST_ASSERT_EQUAL_STRING("null", output);
    TEST_ASSERT_EQUAL_size_t(4, length);
    init_writer(&writer, buffer, 4, frames, sizeof(frames));
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_json_writer_null(&writer));
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_json_writer_bool(&writer, true));
    output = "sentinel";
    length = 99;
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_json_writer_finish(&writer, &output, &length));
    TEST_ASSERT_EQUAL_STRING("sentinel", output);
    TEST_ASSERT_EQUAL_size_t(99, length);
    init_writer(&writer, buffer, 1, frames, sizeof(frames));
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_json_writer_string(&writer, NULL, 0));
    TEST_ASSERT_EQUAL_CHAR(0, buffer[0]);
}

void test_writer_depth_limit_latches_until_explicit_reinitialization(void) {
    esy_json_writer_t writer;
    char buffer[32];
    uint8_t frames[1];
    init_writer(&writer, buffer, sizeof(buffer), frames, sizeof(frames));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_begin_array(&writer));
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_json_writer_begin_object(&writer));
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_json_writer_end(&writer));
    const char *output;
    size_t length;
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_json_writer_finish(&writer, &output, &length));
    init_writer(&writer, buffer, sizeof(buffer), frames, sizeof(frames));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_begin_object(&writer));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_end(&writer));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_finish(&writer, &output, &length));
    TEST_ASSERT_EQUAL_STRING("{}", output);
}

void test_writer_incomplete_finish_is_recoverable_and_completed_finish_is_stable(void) {
    esy_json_writer_t writer;
    char buffer[32];
    uint8_t frames[1];
    init_writer(&writer, buffer, sizeof(buffer), frames, sizeof(frames));
    const char *output = "sentinel";
    size_t length = 99;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_json_writer_finish(&writer, &output, &length));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_begin_object(&writer));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_key(&writer, "a", 1));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_json_writer_finish(&writer, &output, &length));
    TEST_ASSERT_EQUAL_STRING("sentinel", output);
    TEST_ASSERT_EQUAL_size_t(99, length);
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_i64(&writer, 0));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_end(&writer));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_finish(&writer, &output, &length));
    TEST_ASSERT_EQUAL_STRING("{\"a\":0}", output);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_json_writer_null(&writer));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_finish(&writer, &output, &length));
    TEST_ASSERT_EQUAL_STRING("{\"a\":0}", output);
}

void test_writer_wrong_order_and_missing_object_value_are_errors(void) {
    esy_json_writer_t writer;
    char buffer[64];
    uint8_t frames[2];
    init_writer(&writer, buffer, sizeof(buffer), frames, sizeof(frames));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_json_writer_key(&writer, "a", 1));
    init_writer(&writer, buffer, sizeof(buffer), frames, sizeof(frames));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_json_writer_end(&writer));
    init_writer(&writer, buffer, sizeof(buffer), frames, sizeof(frames));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_begin_object(&writer));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_json_writer_u64(&writer, 1));
    init_writer(&writer, buffer, sizeof(buffer), frames, sizeof(frames));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_begin_object(&writer));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_key(&writer, "a", 1));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_json_writer_end(&writer));
    init_writer(&writer, buffer, sizeof(buffer), frames, sizeof(frames));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_begin_array(&writer));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_json_writer_key(&writer, "a", 1));
}

void test_writer_number_rejects_nonnumber_and_whitespace_and_latches_failure(void) {
    const char *invalid[] = {"", " 1", "1 ", "01", "NaN", "null", "[]", "1 2", "1e", "+1"};
    esy_json_writer_t writer;
    char buffer[32];
    uint8_t frames[1];
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        init_writer(&writer, buffer, sizeof(buffer), frames, sizeof(frames));
        esy_err_t error = esy_json_writer_number(&writer, invalid[i], strlen(invalid[i]));
        TEST_ASSERT_NOT_EQUAL(ESY_OK, error);
        TEST_ASSERT_EQUAL(error, esy_json_writer_u64(&writer, 1));
    }
    init_writer(&writer, buffer, sizeof(buffer), frames, sizeof(frames));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_number(&writer, NULL, 1));
}

void test_writer_rejects_invalid_utf8_input_and_null_nonempty_strings(void) {
    const char *invalid[] = {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\x80"};
    esy_json_writer_t writer;
    char buffer[32];
    uint8_t frames[1];
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        init_writer(&writer, buffer, sizeof(buffer), frames, sizeof(frames));
        TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_string(&writer, invalid[i], strlen(invalid[i])));
        TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_null(&writer));
    }
    init_writer(&writer, buffer, sizeof(buffer), frames, sizeof(frames));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_string(&writer, NULL, 1));
    init_writer(&writer, buffer, sizeof(buffer), frames, sizeof(frames));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_begin_object(&writer));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_key(&writer, NULL, 1));
}

void test_writer_init_overwrites_uninitialized_storage_and_failure_preserves_live_state(void) {
    esy_json_writer_t writer;
    memset(&writer, 0xa5, sizeof(writer));
    char buffer[32];
    uint8_t frames[1];
    init_writer(&writer, buffer, sizeof(buffer), frames, sizeof(frames));
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_u64(&writer, 7));
    esy_json_writer_resources_t resources = {buffer, 0, frames, sizeof(frames)};
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_init(&writer, &resources));
    const char *output;
    size_t length;
    TEST_ASSERT_EQUAL(ESY_OK, esy_json_writer_finish(&writer, &output, &length));
    TEST_ASSERT_EQUAL_STRING("7", output);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_init(NULL, &resources));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_init(&writer, NULL));
    resources.buffer_capacity = sizeof(buffer);
    resources.frame_capacity = 0;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_init(&writer, &resources));
    resources.frame_capacity = sizeof(frames);
    resources.frame_storage = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_init(&writer, &resources));
    resources.frame_storage = frames;
    resources.buffer = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_init(&writer, &resources));
}

void test_writer_checks_null_arguments_and_zero_handles(void) {
    esy_json_writer_t writer = {0};
    const char *output;
    size_t length;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_json_writer_null(&writer));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_json_writer_finish(&writer, &output, &length));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_begin_object(NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_begin_array(NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_end(NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_key(NULL, "a", 1));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_string(NULL, "a", 1));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_u64(NULL, 0));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_i64(NULL, 0));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_bool(NULL, false));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_null(NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_number(NULL, "1", 1));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_finish(NULL, &output, &length));
    char buffer[16];
    uint8_t frames[1];
    init_writer(&writer, buffer, sizeof(buffer), frames, sizeof(frames));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_finish(&writer, NULL, &length));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_json_writer_finish(&writer, &output, NULL));
}
