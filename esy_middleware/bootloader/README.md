# Bootloader

## Purpose

Provide the reusable two-slot image format, authenticated boot selection,
trial-state transitions and running-image confirmation. This module creates no
OS resources and can execute before a scheduler starts.

## Public Surface

`esy_middleware/bootloader/esy_bootloader.h` exposes `esy_bootloader_*`, the
256-byte firmware metadata format and the product-owned signature verification
callback. Metadata reads/writes and running/inactive-slot queries serve both
boot executables and application-side OTA. `esy_bootloader_mark_success()` records
confirmation; the consumer decides when startup health checks are sufficient.
`esy_bootloader_activate_image()` submits an inactive TRY_BOOT image for the next
consumer-owned reboot, including native boot-target persistence when required.

## Integration

Enable `ENABLE_ESY_BOOTLOADER`, `ENABLE_ESY_SYSTEM` and `ENABLE_ESY_FLASH` in the
consumer feature import. Link `esy_bootloader` directly for a boot executable;
there is no need to link the middleware aggregate or an OS target. OTA links the
same target. HAL and common utilities are the only production dependencies.

Initialize physical Flash and establish the required safe execution policy before
metadata mutation. HAL supplies the board-owned physical layout through
`esy_hal_boot_get_layout()`, Flash operations,
`esy_hal_boot_get_runtime()` and `esy_hal_boot_jump_to_app()`. System control is
not an image-handoff API. The middleware never includes or calls a board or hardware
backend interface. Public layout data lives in `esy_common/defs/image/esy_boot_defs.h`
and exposes no board APIs.

### Usage Example

On a direct-boot target, call this once from the startup owner after initializing
Flash and satisfying the HAL's IRQ, peer-core and asynchronous-user handoff
requirements. The supplied configuration contains the product's real signature
verifier and trust-key context. Init borrows them only for its synchronous call;
there is no deinit operation. Managed-mode application startup uses its existing
native boot path instead of this function.

```c
#include "esy_middleware/bootloader/esy_bootloader.h"

esy_err_t boot_direct_image(const esy_bootloader_config_t *config) {
    esy_err_t result = esy_bootloader_init(config);
    if (result != ESY_OK) {
        return result;
    }
    if (esy_bootloader_get_selected_slot() < 0) {
        return ESY_ERR_STATE;
    }
    /* Successful handoff never returns. */
    (void)esy_bootloader_jump_to_app();
    return ESY_ERR_IO;
}
```

The running application calls `esy_bootloader_mark_success()` only after its
startup health checks pass, and checks that result before permitting another OTA.

## Contracts

Calls are synchronous and require a single startup/task execution owner with
externally serialized Flash, boot metadata and OTA writers. No API is ISR-safe.
Each init clears the previous selected slot. In direct mode it validates both
images, invokes the supplied signature verifier and chooses a slot; a trial
selection updates TRY_BOOT to UNCONFIRMED before handoff. In managed mode it
validates only the native bootloader's currently running image and selects that
slot without promoting or selecting another trial. Native boot state owns
selection and rollback; Esy metadata never overrides its decision. Init failure
leaves no selected
slot; I/O failures may leave partially changed Flash and do not authorize blindly
replaying a transaction. Configuration and verification context are borrowed only
for the synchronous init call; signature code must not reenter this module.

Direct selection tries authenticated TRY_BOOT candidates in descending version
order. Each candidate must persist its own UNCONFIRMED state before it can be
selected. A failed promotion excludes only that candidate for the current init;
the next trial is subject to the same promotion requirement. If every trial
promotion fails, only existing authenticated UNCONFIRMED/CONFIRMED candidates
remain eligible; with no eligible candidate init fails and jump is disabled.

Signatures cover
`version_le32 || firmware_size_le32 || image_sha256`; the product owns the public
key and ECDSA P-256 implementation. Confirmation changes only state and metadata
CRC, preserving authenticated image fields. The module neither chooses when to
confirm nor performs product startup checks. HAL supplies the running image in
Flash coordinates, so memory-mapped execution addresses need not equal physical
Flash offsets. When native state reserves the inactive image for rollback or
already has another image scheduled, inactive-slot queries fail before OTA can
erase or write that region.

After writing and hashing the complete image, OTA writes TRY_BOOT metadata and
calls `esy_bootloader_activate_image()`. This checks inactive-slot identity,
update permission and metadata format/CRC/length/state; it does not reuse the
init verifier or authenticate a candidate. Managed mode additionally submits the
exact image address and size through HAL, whose provider validates the native
partition and image format. Direct mode needs no extra persistent selection.
The consumer owns signature admission and the eventual reboot.

Confirmation first persists Esy CONFIRMED metadata, then confirms the native
image in managed mode. Native confirmation is attempted even if Esy metadata is
already CONFIRMED, allowing a retry after native confirmation failed. Metadata
and native boot records are separate persistent writes, not an atomic
transaction: an error can leave one record changed. A native confirmation error
still permits native rollback, regardless of the Esy state. A submission error
does not undo TRY_BOOT metadata and may require inspecting native boot state.
Consumers must report these errors and reconcile state before another update.

The hardware jump delegates to `esy_hal_boot_jump_to_app()` after selection. Consumers must satisfy the
backend's IRQ, multicore, peripheral and asynchronous-user handoff preconditions.
Any return from the jump means handoff failed. Managed application startup
already runs the selected image and must not repeat the direct init/jump flow;
activation followed by a consumer-owned reboot installs a new image.
Slot layout comes from the board;
this module does not choose product addresses or partition sizes. HAL geometry
is the only source of physical capacity and programming granularity. Every
image and metadata region must be disjoint, contained in that geometry,
program-aligned and own complete physical sectors. Layout validation rejects
overflow, overlap and partial sectors before any Flash I/O. Metadata erase
uses its exact validated region size, including on nonuniform sector layouts;
no global erase size or rounded-up erase can reach an adjacent region.
`meta_size[slot]` describes each metadata partition's erase capacity separately
from its record length. Boards must initialize both entries; unlike the equally
sized image regions, metadata regions may have different capacities. Metadata
writes erase only the selected slot's capacity and still program only the
256-byte record. The scalar-to-array API change requires rebuilding board,
HAL and middleware consumers together, without changing the persistent format.

## Resource Model

One static context retains only the selected slot until reset or the next init.
Image hashing uses a bounded stack buffer and metadata operations use fixed-size
stack objects. No task, queue, timer, dynamic allocation or private hardware
resource is created. Flash and safe-execution resources remain consumer-owned.

## Verification

From `tests/`, run `ceedling test:all`, `ceedling gcov:all` and
`python3 -B test_cmake_consumer.py`. Unit tests exercise real boot logic through
shared HAL fakes: metadata validation, signatures, trial promotion, slot selection,
confirmation and I/O failures, managed selection/rollback ownership, physical
offset image identity, prohibited updates, activation and confirmation retry.
Trial regressions cover two authenticated TRY_BOOT slots, failed preferred
promotion followed by successful fallback promotion, repeated failure of both
promotions, and preserved nontrial fallbacks.
The CMake regression links real boot and HAL sources
with host board/backend fixtures, verifies C11/C++17 headers, rejects board-header
leaks, and checks feature-off isolation without an OS target.

## Known Limits

The format supports two equally sized image regions with separate metadata
regions. It does not provide rollback protection against an authenticated older
image. Host tests do not establish hardware handoff,
Flash/XIP safety, power-loss durability, key provisioning or target timing.
