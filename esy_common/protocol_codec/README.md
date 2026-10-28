<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Protocol codecs

## Purpose

`esy_common_protocol_codec` provides protocol wire-format codecs that are
independent of OS, HAL tick, hardware I/O, transport state, and business
semantics.

The optional `esy_common_protocol_json` target provides generic JSON reading
and generation separately from the binary/TLV/Modbus target. It adapts
the unmodified third-party coreJSON parser and provides a bounded writer.

## Public Surface

Public headers live under `esy_common/protocol_codec/include/esy_common/protocol/`.

The physical module directory is `protocol_codec` to distinguish CPU-only
codecs from protocol sessions/channels in `esy_middleware/protocol`. Public
headers use the `esy_common/protocol/...` include namespace. Source, test and
include-root paths refer to the `protocol_codec` directory.

Components:

- `esy_proto_bin`: build and parse ProtoBin frames with CRC32.
- `esy_proto_tlv`: append and iterate simple TLV payload items.
- `esy_proto_modbus`: build, verify, and parse complete Modbus RTU/TCP frames.
- `esy_proto_json`: borrow validated JSON views, iterate collections, find
  decoded object keys, decode strings, convert exact integers and generate JSON.

## Integration

Link the codec target:

```cmake
target_link_libraries(<your_target> PRIVATE esy_common_protocol_codec)
```

Codec users should include narrow headers such as:

```c
#include "esy_common/protocol/esy_proto_bin.h"
```

Transport and manager APIs remain in `esy_middleware/protocol`.

### Optional JSON

Select JSON in the consumer's authoritative feature file:

```cmake
set(ENABLE_ESY_COMMON_PROTOCOL ON)
set(ENABLE_ESY_COMMON_PROTOCOL_JSON ON)
```

Supply [coreJSON v3.3.1](https://github.com/FreeRTOS/coreJSON/tree/v3.3.1) through
an existing `esy_json_parser_provider` target or an explicit `ESY_COREJSON_PATH`
CMake or environment input. Project import assembles the provider only when JSON
is enabled.

For library host tests, `./setup.sh` prepares seven pinned official providers;
the Python/Ruby test helpers discover their cache automatically. Firmware CMake
uses the consumer's explicit providers, independently of this test cache and
lock. See [software provider setup](../../tools/README.md#software-provider-setup).
The provider is MIT licensed; retain its original license and notices. No CMake
step downloads a dependency. Direct common-only consumers must call the helper
below or supply the provider target before adding the common subtree:

```cmake
include("${PLATFORM_ROOT}/cmake/esy_corejson_provider.cmake")
esy_configure_corejson_provider()
add_subdirectory("${PLATFORM_ROOT}/esy_common" common)
target_link_libraries(my_target PRIVATE Esy::protocol_json)
```

`esy_common_protocol_codec` stays independent of JSON and its provider.
The common aggregate includes JSON only when selected. Disabling the JSON child
feature excludes its two source files and parser provider. Existing protocol
consumers require no changes.

The public header has no coreJSON types:

```c
#include "esy_common/protocol/esy_proto_json.h"

int json_example(void)
{
    char output[64];
    uint8_t frames[4];
    esy_json_writer_t writer;
    esy_json_writer_resources_t resources = {
        output, sizeof(output), frames, sizeof(frames)
    };
    const char *document;
    size_t length;
    esy_json_value_t root;
    esy_json_value_t count;
    uint64_t result;

    if (esy_json_writer_init(&writer, &resources) != ESY_OK ||
        esy_json_writer_begin_object(&writer) != ESY_OK ||
        esy_json_writer_key(&writer, "count", 5) != ESY_OK ||
        esy_json_writer_u64(&writer, UINT64_MAX) != ESY_OK ||
        esy_json_writer_end(&writer) != ESY_OK ||
        esy_json_writer_finish(&writer, &document, &length) != ESY_OK ||
        esy_json_parse(document, length, &root) != ESY_OK ||
        esy_json_object_get(&root, "count", 5, &count) != ESY_OK ||
        esy_json_as_u64(&count, &result) != ESY_OK) {
        return 1;
    }
    return result == UINT64_MAX ? 0 : 1;
}
```

## Contracts

- No OS dependency.
- No HAL or HAL backend dependency.
- No dynamic allocation.
- Caller-owned buffers only.
- Codec APIs do not own transport, timeout, retry, or scheduling state.

ProtoBin parsing accepts only the fixed command enum (1 through 5). Command
zero and values at or above the sentinel are rejected even with a valid CRC;
extensions require a shared codec change.

Modbus RTU and TCP build, verify, and parse accept PDU lengths from 1 through
`ESY_PROTO_MODBUS_MAX_PDU_LEN`, independently of frame-buffer capacity. Invalid
build lengths return `ESY_ERR_INVALID_ARG` without changing the frame descriptor
or destination bytes. Verify returns false and parse returns `ESY_ERR_FAIL` for
an over-limit complete frame. Every parse failure leaves the output view
unchanged.

### JSON

- `esy_json_parse` validates the complete length-delimited document, including
  trailing input, before returning one borrowed root. A terminating NUL is not
  part of the document. Malformed/truncated JSON returns `ESY_ERR_FAIL`; the
  parser nesting limit returns `ESY_ERR_NO_MEM` (default 32 containers).
- Views always describe complete JSON lexemes; strings retain their quotes and
  escapes until `esy_json_string_copy` decodes them. Input must remain immutable
  and alive while views/iterators borrow it. Do not forge or modify views.
- Iteration visits direct children in source order and preserves duplicate
  members. Array iteration returns a null/empty key view. `ESY_ERR_NOT_SUPPORTED`
  means end of iteration or a missing lookup, with unchanged output views.
- Object lookup compares decoded UTF-8 bytes exactly, supports empty keys and
  literal dots/brackets, and performs no Unicode normalization. Duplicate
  matching decoded keys, such as `"a"` and `"\u0061"`, return `ESY_ERR_FAIL`
  rather than silently selecting a value. Whole-object duplicate rejection and
  schema/unknown-field policy belong to the consuming adapter.
- Signed/unsigned integer accessors do not use floating point. Fractions and
  exponents are preserved in number views but rejected by integer accessors.
  Values outside the requested 64-bit range fail without changing output.
  Unsigned conversion rejects negative values, including `-0`.
- String decoding validates UTF-8 and surrogate pairs, supports embedded decoded
  NUL and appends a separate NUL terminator. Use its explicit output length.
  A NULL destination with zero capacity measures the decoded byte count.
  Insufficient space reports that count and leaves the destination unchanged.
- The writer accepts decoded UTF-8 strings, escapes quotes/backslashes/control
  bytes, manages object/array separators and uses one workspace byte per open
  container. Integer output is exact; `writer_number` accepts a validated decimal
  lexeme for fractional/exponent values without converting through a C float.
  Output capacity always includes one trailing NUL.
- A failed write latches its error until explicit init. Output may be partial
  after failure and must never be published. Only successful `writer_finish`
  publishes a complete document. Finish of an incomplete document is recoverable;
  finish of a complete document is repeatable. Writes after successful finish
  return STATE while preserving that completed result.
- Calls are synchronous and non-ISR. One owner serializes each iterator/writer;
  independent objects may run concurrently. Parsing/validation is O(input
  bytes); object lookup scans the complete object to detect duplicate matches,
  and indexed array lookup is O(index) plus collection validation. Bound input
  size and nesting in the consumer; these are not hard-real-time operations.

## Resource Model

All codec buffers belong to the caller; no hidden state or dynamic allocation is
used. A frame descriptor needs no initialization before its init function.

JSON has no singleton, task, queue, callback, watchdog or timebase. Iterator
and writer init overwrite handle state before any read and permit uninitialized
storage. Resetting them explicitly releases prior borrows and discards progress;
there is no asynchronous work and no deinit requirement. Input/handle/output and
backing-resource objects must be disjoint, except the documented borrowed views.
Do not copy live handles. Writer buffers and nesting workspace are aggregated in
`esy_json_writer_resources_t` and borrowed through reinit or the end of use.
Backing storage need not be zeroed. JSON has no OTA task policy or timing/rollover
state; its caller is responsible for OTA scheduling and Flash safety.

## Known Limits

ProtoBin command values are the fixed enum; extending them requires updating the
shared codec contract. Reset parser and frame after every complete frame or error
before feeding the next frame. Modbus PDU capacity defaults to 252 bytes (a library
limit, one byte below the protocol maximum). Override
`ESY_PROTO_MODBUS_MAX_PDU_LEN` consistently for the codec and every consumer; the
accepted configuration range is 1..253, enforced at compile time. The manager's
optional request-storage limit must not exceed the codec limit. Larger buffers
do not enable larger PDUs. Over-limit and malformed frames are rejected before
dispatch. Failed parse preserves the output view; always check the return
status before using the view.

JSON does not provide a DOM, reflection, automatic structure mapping, schema
registry, JSONPath, transport framing, incremental fragmented-document parsing
or a Remote Manager wire adapter. Collect one bounded complete document before
parsing. Binary data encoding and JavaScript-safe representation of integers
above 2^53 are application wire contracts; exact MCU-side integer support does
not prevent a JavaScript client from rounding numeric JSON values. There are no
C floating-point conversion APIs; use validated decimal lexemes when needed.
Writer workspace depth and parser depth are independent resource bounds: choose
a compatible workspace limit if generated documents must be read by this parser.

For framed transport borrowers of this codec, select the independent
[JSON TCP channel](../../esy_middleware/protocol/README.md#json-tcp-channel) or
[JSON USB channel](../../esy_middleware/protocol/README.md#json-usb-channel).
Its transport/session state stays outside the common codec target.

## Verification

From the platform repository root, run the provider-free codec suite:

```sh
cd esy_common/protocol_codec/tests
ceedling test:all
ceedling --mixin=sanitizer.yml test:all
python3 -B test_modbus_limits_compile.py
```

The suite covers Modbus RTU/TCP frame encoding, validation and parsing, configured
PDU limits, failed-output preservation, and ProtoBin arithmetic overflow. The
compile probe checks accepted and rejected limit overrides, manager/codec limit
agreement, and boundary behavior against the real codec implementation. Run with
ASan/UBSan when changing lengths or storage layout. ASCII and RAW Modbus buffer
codecs are not provided.

JSON has a separate suite so existing codec tests need no third-party checkout.
From the platform repository root, run `./setup.sh` once to prepare the cached
software providers, then run:

```sh
ruby tools/quality/run_module_tests.rb esy_common/protocol_codec/tests/json \
  esy_common/protocol_codec/tests/json/build test:all
ruby tools/quality/run_module_tests.rb esy_common/protocol_codec/tests/json \
  esy_common/protocol_codec/tests/json/build/sanitizer test:all --sanitizer
python3 -B esy_common/protocol_codec/tests/test_json_cmake_consumer.py
tools/checks/check_no_dynamic_alloc.sh esy_common/protocol_codec
```

Optionally set the `ESY_COREJSON_PATH` environment variable to test another
compatible coreJSON checkout.

JSON tests use the real parser, including malformed/truncated input, escaped and
duplicate keys, Unicode/NUL strings, exact 64-bit boundaries, nested generation,
capacity/depth exhaustion and writer reset/completion/error paths. The CMake
probe builds and runs C11/C++17 consumers, checks private provider isolation and
feature/provider diagnostics, and reconfigures JSON from enabled to disabled.
The quality gate discovers the JSON suite separately and merges its coverage
with the existing protocol reports.
