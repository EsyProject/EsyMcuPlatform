<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# esy_common_ds

## Purpose

`esy_common_ds` provides deterministic, platform-independent data structures
for embedded firmware.

The module is designed for reusable middleware and application data paths:

- no dynamic allocation;
- caller-owned storage;
- bounded memory usage;
- explicit concurrency contracts;
- defensive public API boundaries where they do not change the cost model.

## Public Surface

Public headers live under `esy_common/ds/include/esy_common/ds/`.

## Integration

Link the `esy_common_ds` target, or link the aggregate `esy_common` target when
the DS feature is enabled:

```cmake
target_link_libraries(<your_target> PRIVATE esy_common_ds)
```

Public components:

- `esy_ring_buffer`: byte-oriented SPSC ring buffer.
- `esy_block_ring`: fixed-size block SPSC ring buffer.
- `esy_block_pool`: fixed-size block pool.
- `esy_list`: non-owning linked-list container storing user data pointers.
- `esy_prio_slot_queue`: priority-based fixed-slot queue.

## Contracts

All data structures use caller-owned storage. The caller must keep backing
buffers valid for the lifetime of the data structure instance.

SPSC structures are safe only for the documented single-producer /
single-consumer access pattern. Init/reset/deinit calls require endpoints to be
stopped.

`esy_ring_buffer` requires a nonzero power-of-two byte capacity, including one.
Initialization rejects zero and non-power-of-two capacities with
`ESY_ERR_INVALID_ARG`, leaving the object and backing buffer unchanged. Callers
choose a power-of-two capacity explicitly; capacity is never rounded. Index masking
preserves FIFO order across unsigned 32-bit counter wraparound.

`esy_prio_slot_queue` accepts 1 through
`ESY_PRIO_SLOT_QUEUE_MAX_SLOT_COUNT` (32768) slots. Its signed 16-bit allocation,
selection and lookup results cover indices 0 through 32767, with -1 reserved for
failure or no match. Init rejects larger counts before modifying either the
queue or supplied slots. Callers using larger arrays must reduce the configured
slot count. Queued slots use intrusive FIFO links, so an older request retains
its order across any number of operations on other slots. Enqueue/requeue and
removal take O(1); selection scans queued slots. The owner must serialize all
operations. All translation units must use the same public bookkeeping layouts.

`esy_list_clear(list)` only detaches nodes and resets their links. It preserves
data pointers and performs no resource cleanup. Caller-owned nodes may be
reinserted. Resource teardown belongs to the owner outside the container; there
is no cleanup callback parameter.

## Verification

Run module tests from:

```sh
cd external/EsyMcuPlatform/esy_common/ds/tests
ceedling test:all
```

Ring-buffer tests cover invalid-capacity rejection without modifying an existing
ring, single-byte capacity, full/empty transitions, buffer wraparound, and FIFO
order across unsigned 32-bit counter wraparound.
Priority-slot tests cover the largest valid capacity and last usable index,
plus rejection of 32769/65535 slots without changing an existing queue or either
the existing or proposed backing storage.

List-clear tests verify cleared links, preserved data (including NULL), repeated
clear and node reuse. Priority-slot tests also cover middle/head/tail removal,
requeue ordering and storage reuse. The optimized public-API regression
`python3 -B tests/test_prio_fifo_turnover.py` keeps a request queued while another
slot completes 2^31 transactions, then verifies FIFO ordering with a new peer.
