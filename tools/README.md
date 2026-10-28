<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# tools

<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

This directory contains EsyMcuPlatform-owned quality and maintenance scripts.
They validate the submodule itself and do not govern consuming firmware
repositories.

## Entry Point

Install the host prerequisites first: Python 3, Git, a C/C++ toolchain, CMake,
Ninja, Ruby with Ceedling 1.1.8, gcovr, ripgrep and OpenSSL. MQTT broker smoke
tests also use Mosquitto and OpenSSL development headers. The provider tool
prepares source dependencies; it does not install system packages.

From the EsyMcuPlatform root, prepare the locked providers and run the full gate:

```sh
./setup.sh
tools/quality/ci_quality_gate.sh test:all
tools/quality/ci_quality_gate.sh gcov:all
tools/checks/check_gcov_thresholds.sh
```

`test:all` is the default when no argument is provided. The gate runs source
contract checks and then executes each module-owned Ceedling test project under
`esy_common/**/tests`, `esy_hal/**/tests`, `esy_middleware/**/tests` and reusable
fake/fixture tests under `esy_test/**/tests`. Generated `build/` trees are
excluded from discovery. Coverage floors include every production module test
project, including common data structures and utilities; reusable fake tests are
run without treating their coverage as production evidence.

### Software Provider Setup

This setup is for contributors extending or fixing EsyMcuPlatform. It works
from an independent library checkout and needs no consuming firmware project.
Run `./setup.sh` once from the library root to prepare its seven host-test
providers: `corejson`, `coremqtt`, `llhttp`, `mbedtls`, `littlefs`, `fatfs` and
`lvgl`. [The tracked test-provider lock](providers/providers.lock.json) records
their official locations, exact Git revisions and archive checksums. Local
verification and CI use the same baseline.

The default cache is `$XDG_CACHE_HOME/esymcuplatform/providers`, or
`~/.cache/esymcuplatform/providers` when `XDG_CACHE_HOME` is unset. Managed
directory names contain the version and pin prefix so different library
revisions can keep their test dependencies side by side. Moving the checkout
does not move the shared cache. For a custom root, supply the same absolute
`ESY_PROVIDER_CACHE_DIR` to setup and subsequent test commands.

Library tests, `run_module_tests.rb` and the quality gate discover the cache
automatically in later shells. No `source` command, generated environment file
or provider path configuration is needed. Tests and quality tools only read
prepared sources; they do not download missing dependencies and report the setup
command when required inputs are absent. Source-path variables remain available
for testing an explicitly selected checkout. An explicit empty or invalid value
fails instead of silently selecting the cache. Setup populates the managed
cache even when those variables are set, without modifying the supplied sources.

This test lock does not select firmware dependencies. Firmware CMake continues
to use its consumer-owned provider targets and paths, including LVGL and
FreeRTOS submodules. Setup does not read or change a parent project's
configuration, initialize its submodules or replace its dependencies. FreeRTOS
is not downloaded: host suites use their test ports and fixtures. Hardware SDKs,
RTOS kernels and target toolchains remain inputs to real firmware builds.

Managed Git inputs must match their locked revisions with clean worktrees and
initialized submodules. Archive downloads are SHA256-verified before extraction;
reused archive trees are checked against their recorded file hashes. Setup
leaves invalid or modified caches untouched. Repair the reported input or
select a different cache before retrying. Recheck the prepared inputs without
network access with:

```sh
./setup.sh --offline
```

Repeat `--provider NAME` to prepare a smaller set instead of all defaults.
For example, prepare coreMQTT and run its focused suite:

```sh
./setup.sh --provider coremqtt
ruby tools/quality/run_module_tests.rb esy_middleware/mqtt/tests /tmp/esy-mqtt-tests test:all
```

With Mosquitto and OpenSSL installed, broker and native LVGL consumers also
discover the sources prepared by setup:

```sh
python3 -B esy_middleware/mqtt/tests/test_broker_smoke.py
python3 -B esy_middleware/lvgl/tests/test_cmake_consumer.py
```

For advanced use, `tools/providers/prepare_providers.py` retains `--directory`
and `--exec COMMAND ...`. A custom `--directory` is not persisted; later test
commands need the same directory through `ESY_PROVIDER_CACHE_DIR`. Commands
after `--exec` retain the caller's working directory. Setup only prepares
software sources; it does not install system packages or select firmware
features and boards.

Crypto and TLS tests use the cached complete Mbed TLS 3.6.x release or an
explicit `ESY_MBEDTLS_PATH`. The module runner replaces `@ESY_MBEDTLS_PATH@` in
their Ceedling configurations. Public adapter-header checks use that resolved include
path, while generic headers are checked without native include paths. The
quality gate does not download dependencies. Real OpenSSL loopback tests cover
TLS authentication, client identity, certificate-date/hostname rejection and
record retry behavior. For target entropy and resource-budget measurement
procedures, see [TLS](../esy_middleware/tls/README.md) and
[cryptographic randomness](../esy_middleware/crypto/README.md).

Filesystem adapter-header checks and backend tests use cached official littlefs
2.11.2 and FatFs R0.16, with `ESY_LITTLEFS_PATH` and `ESY_FATFS_PATH` available as
explicit overrides. Setup provisions the pinned littlefs revision and verifies
the official FatFs archive checksum in both local and CI runs.
The module runner stages FatFs beside the platform-owned configuration without
changing upstream inputs, runs both actual libraries, and isolates sanitizer
artifacts. Core-only filesystem tests need neither provider; the full quality
gate requires both and includes backend production sources in its coverage.
See [Filesystem](../esy_middleware/fs/README.md) for focused commands and target
test procedures.

The Credentials suite also covers its optional persistence target through real
File and Dual Flash Store adapters. Tests exercise complete confirmed/candidate
updates, interrupted writes, readback failures after a committed effect,
reopening and borrowed-RAM lifetime. Its CMake consumer checks keep the memory
target independent of File and native crypto, and reject incomplete persistence
feature selections. Consumer board procedures cover Flash layout, secure
storage properties and recovery after power loss. See the
[Credentials contract](../esy_middleware/credentials/README.md).

MQTT tests compile the real coreMQTT v2.3.1 protocol provider from the cache or
an explicit `ESY_COREMQTT_PATH` checkout. The quality gate does not download this
dependency; setup prepares the locked revision for local runs and both CI jobs.
The core suite runs through `run_module_tests.rb`, which replaces its literal
`@ESY_COREMQTT_PATH@` path marker from the resolved path without enabling
inline Ruby evaluation. Use the same entry point across Ceedling versions:

```sh
ruby tools/quality/run_module_tests.rb esy_middleware/mqtt/tests esy_middleware/mqtt/tests/build test:all
```

MQTT CMake consumers verify caller-driven C11/C++17 integration without HAL/OS,
provider/configuration ownership, feature isolation and invalid source/version
diagnostics. Module tests use an in-memory byte stream and a controlled clock;
The host CI job also runs `esy_middleware/mqtt/tests/test_broker_smoke.py` against
a local Mosquitto broker over plain TCP and an OpenSSL TLS stream, including
hostname verification failure. Install Mosquitto, OpenSSL and its development
headers to run it locally; `MOSQUITTO_BIN` can select a broker executable. In
consumer tests, measure device network-stack timing and exercise deployed TLS
providers.

HTTP client tests use the cached pre-generated llhttp v9.4.3 release or an
explicit `ESY_LLHTTP_PATH`; the runner replaces its literal `@ESY_LLHTTP_PATH@` marker.
Both CI jobs use the revision selected by the shared provider lock. No Node.js
generator or dependency download runs during library configuration. C11/C++17
consumers verify private parser dependencies and feature isolation, while the
module suite exercises streaming responses, bounded storage and transaction
lifecycle behavior. The HTTP loopback smoke test runs separately through
`esy_middleware/http/tests/test_http_smoke.py`. See the
[HTTP client contract](../esy_middleware/http/README.md) for prerequisites and
target-network and OTA test guidance.

The optional raw lwIP stream adapter owns a separate suite in
`esy_middleware/network/lwip/tests`. Automatic discovery includes its normal and
coverage tests; sanitizer runs use an isolated build directory. The core suite
and adapter suite report their own production sources separately.

Protocol consumer tests select ProtoBin and Modbus independently, verify
explicit UART/USB/TCP channel gates and reject missing providers. Session tests
cover caller-owned lifecycle, borrowed sender reservations and transaction
completion; OTA integration tests cover direct senders and owner-queue bridges
without a generic protocol backend container.

The optional JSON codec has a separate real-provider suite under
`esy_common/protocol_codec/tests/json`, using cached coreJSON v3.3.1 or an
explicit `ESY_COREJSON_PATH`. Existing ProtoBin/TLV/Modbus tests require no JSON provider.
The quality gate discovers both suites, runs JSON separately under sanitizers,
and requires every protocol source to meet its per-file coverage floor across
the two disjoint reports. Setup prepares the locked checkout;
CMake configuration and module tests never download it. Run the focused suite with:

```sh
ruby tools/quality/run_module_tests.rb esy_common/protocol_codec/tests/json /tmp/esy-json-tests test:all
```

The JSON TCP channel has its own real-codec suite under
`esy_middleware/protocol/tests/json`. Its injected connection callbacks exercise
fragmented/coalesced length-prefixed messages, bounded prefix sends, EOF/ACK
drain, session changes and callback reply/reentry behavior without a native
network stack. It uses the same explicit coreJSON checkout and isolated
sanitizer runner. Its coverage report joins the existing middleware protocol
report for the complete per-file protocol population.

The same suite also tests JSON over real CDC and its TinyUSB backend, using the
shared TinyUSB fake. It checks caller-owned stack pumping, DTR/physical session
changes, stale FIFO isolation, preservation of new-peer RX, bounded partial
transfers and callback replies. The USB channel never pumps the global stack;
target USB procedures cover native timing and host interoperability.

The gate also compiles and runs the task fixture against the shared OS fakes,
including failed creation, retry without losing backing capacity, preservation
of existing tasks and reset. Modbus limit consumers compile with both valid
alternate limits and rejected invalid macro values in
`esy_common/protocol_codec/tests/test_modbus_limits_compile.py` on every gate run.

The hardware-boundary check and its negative fixtures reject middleware access
to board/backend headers, calls or targets, board/backend header leaks through
public HAL headers, and backend dependence on HAL. Bootloader source contracts,
module tests and real HAL C11/C++17 consumers also run in the gate. The bootloader
consumer checks include linking and execution without an OS.

`python3 -B tools/checks/test_cmake_integration.py` checks the optional library
build helpers with temporary consumers, SDK/provider fixtures and paths containing
spaces. It verifies builds without a consumer override file, conventional path
discovery, explicit empty/CLI overrides, provider ownership, target usage
requirements, SDK/component ordering and firmware slot/output wiring.
Selection regressions cover explicit platform/device validation and reconfiguration.
They verify that conflicting or invalid `.project.conf` contents do not select
hardware and that build overrides cannot select or replace the target. Private
environment files are ignored unless the consumer loads its own settings before
early import. Script-mode dependency discovery uses the same explicit inputs
without importing an SDK.
The same check compiles the default application-version header as C11/C++17,
including incomplete project versions, and verifies template precedence,
explicit output paths and disabling generation. Set
`ESY_TEST_PICO_SDK_PATH` and, if needed, `ESY_TEST_PICO_TOOLCHAIN_PATH` to add a real
Pico SDK firmware build. `ESY_TEST_ARM_TOOLCHAIN_ROOT` independently enables the
STM32 compiler capability probe before any target options exist; the Pico
toolchain path can supply the same GNU Arm compiler. CI host fixtures do not
require an installed vendor SDK.

Common-only regressions also compile, link and execute C11/C++17 programs with
DS and protocol enabled, both with and without an existing optimization target.
`esy_common/ds/tests/test_prio_fifo_turnover.py` runs an optimized public-API
regression through 2^31 transactions while retaining an older queued request.
`esy_hal/tests/test_xio_storage_contract.py` checks independent/global XIO and
enclosing UART/SPI storage overrides and matching C/C++ layouts.
`esy_hal/tests/test_hal_storage_aliasing.py` exercises all eight private HAL
runtime overlays and the independent UART DMA subobject pointers with strict
aliasing at `-O2`, `-O3` and `-O3 -flto`. It uses `CC` or the host `cc` and adds
non-LTO Clang checks when available. Aggregate assignments in this probe
test representation access only; they do not authorize resetting live public
handles or bypassing resource cleanup.

`esy_hal/tests/test_stm32_handoff_native.py` compiles the real STM32F4 backend
against native CMSIS/HAL headers at three optimization settings and checks the
stack-free MSP switch. Set both `ESY_TEST_ARM_TOOLCHAIN_ROOT` and
`ESY_TEST_STM32_SUPPORT_ROOT` to enable it in the gate; otherwise it explicitly
skips. A configured but missing provider fails. The handoff HIL plan describes
application-entry, register and stack observations on the target.

Network-provider fixtures compile SDK interface sources using consumer lwipopts
and native configuration headers. They cover independent Wi-Fi/TCP gates,
disabled parent modules, bare-metal/FreeRTOS dependency selection, explicit
provider precedence, project-only imports and missing SDK/configuration targets.
Assembly-boundary checks preserve provider definitions after project import and
keep unrelated direct consumers independent of another directory's helper use.
Automatic setup regressions cover the recommended early import,
consumer `project()` and library `add_subdirectory()` flow, resolved consumer
settings, application version ownership and explicit project-helper ordering.

Input target-consumer checks compile independent C11/C++17 public headers and
link/execute the caller-driven core without HAL or OS targets, including with
`ESY_RTOS=FreeRTOS` when the module is assembled directly. They cover disabled-module
assembly, optional borrowed GPIO integration and missing HAL dependencies. The
discovered Ceedling suite participates in the default coverage floor and runs
separately under sanitizers.

Sensor target-consumer checks verify independent C11/C++17 public headers,
feature-off assembly and the core without HAL or OS targets. Optional I2C
drivers link through the public HAL target; missing provider requirements fail
during configuration. The discovered Ceedling suite participates in the default
coverage floor and runs separately under sanitizers.

Touch target-consumer checks compile and link C11/C++17 facade clients using
`esy_touch`, with the controller enabled and disabled. Device-integration cases
explicitly link `esy_touch_device`; GPIO-free facade cases omit that target.
The checks also cover disabled-module assembly. Header usage requirements
propagate through the selected public target.

BLE target-consumer checks compile C11/C++17 facade headers and build the core
without HAL or OS targets. They verify disabled-module assembly, optional BTstack
parent/provider diagnostics, adapter target contracts, private build-option
isolation, rejected native CCC persistence settings and unsupported EATT. Set
`ESY_BTSTACK_PATH` or pass `--btstack-path` to the module's consumer
test to compile its adapter source against real BTstack headers; absent an
explicit path, that case reports skipped. The fixture's host configuration does
not prove native-stack linking or radio behavior.
`esy_middleware/ble/tests/test_btstack_runtime.py` separately links and runs the
production core/adapter against real headers and module-local host API fakes;
its native ATT mode also links real upstream `att_db.c` to verify request/error
decoding, bounded read snapshots and notification payload limits.
It uses the same explicit path inputs and supports `--sanitize`. It reports
skipped without a stack path. Consumer target procedures cover controller,
native-runtime and bond-persistence behavior.
The discovered core Ceedling suite participates in the default coverage floor
and runs separately under sanitizers with isolated artifacts.

The gate also runs `esy_middleware/core_lockout/tests/test_cmake_consumer.py`
to verify standalone C11/C++17 target consumers, disabled-module assembly, and
Flash-independent execution and rejection of missing system/OS dependencies.

Dual Flash store checks compile independent C11/C++17 public-header consumers and
link/execute the real module with the reusable Flash fake, without an OS. They
cover disabled-module assembly and rejection of missing Flash support. The
module's discovered Ceedling suite participates in the default coverage floor
and runs separately under sanitizers with isolated artifacts.

Settings checks compile independent C11/C++17 public-header consumers and
link/execute the real settings and dual Flash store modules with the shared Flash
fake, without an OS. They cover disabled-module assembly and rejection of missing
dual Flash store or Flash features. The discovered Ceedling suite participates in
the default coverage floor and runs separately under sanitizers.

The gate also checks freq_synth source contracts and runs
`esy_middleware/freq_synth/tests/test_cmake_consumer.py`, including C11/C++17
compilation, linking and execution for all six module/backend combinations.
Coverage checks enumerate expected module projects and reject missing and empty
reports, so a partial coverage run cannot silently satisfy the repository gate.
For isolated validation, `ESY_GCOV_REPORT_ROOT` selects a report tree containing
`<module>/artifacts/gcov/gcovr/coverage.txt`; it does not change coverage policy.

HAL, numerics, OTA and OS JSON inventories identify canonical sources in the current
checkout. They accept absolute local paths or paths relative to the platform
root, module root or module tests directory, independently of the shell's
working directory. Foreign same-name files and duplicate aliases, including
symlinks, are rejected. OS permits local internal-header rows without counting
them as required C sources. Coverage must be regenerated for this checkout;
matching names alone cannot make another checkout's report current evidence.
These identity checks preserve each module's existing line/branch policy.

Power-manager checks also validate source contracts and C11/C++17 consumers of
all exported targets. Its module-owned JSON gate checks production source
completeness, 85% per-file line coverage and 70% aggregate branch coverage;
`test_coverage_gate.py` verifies rejection of incomplete or insufficient reports.

`gcov:all` refreshes module coverage reports. The repository coverage floor is
stored in `tools/policy/gcov_thresholds.env` and enforced by
`tools/checks/check_gcov_thresholds.sh`.

`tools/quality/ci_quality_gate.sh` is the entry point. `tools/checks/`
contains single-purpose checks that can also be run directly. CI and module
test project metadata use Ceedling 1.1.8.

The I/O scheduler CMake fixtures and per-file coverage checks run in the gate.
Sanitizer runs for I/O schedulers, common/protocol_codec, middleware/protocol and Touch
use `quality/run_module_tests.rb`, which
writes a complete temporary Ceedling configuration with an explicit output root.
Ceedling 1.0.1 mixins can leave `project.build_root` unchanged; using only
`--mixin=sanitizer.yml` does not establish artifact isolation. Example:

```sh
ruby tools/quality/run_module_tests.rb esy_middleware/io_scheduler/tests /tmp/esy-io-scheduler-sanitizer test:all --sanitizer
```

The same runner accepts `--mixin=NAME.yml` for module-local variants. Hashes are
merged recursively and arrays are appended without duplicates; the explicit
output directory always overrides a mixin's `project.build_root`. The OS gate
uses this path for snapshot, critical-section and 16/64-bit or SMP tickless
regressions, keeping their artifacts separate from default coverage:

```bash
ruby tools/quality/run_module_tests.rb esy_middleware/os/tests /tmp/esy-os-smp test:test_esy_os --mixin=tickless_smp.yml
```

The helper also supports `gcov:all` and `test:test_<name>` without `--sanitizer`.
It runs from the original suite directory and preserves Ceedling path modifiers,
report roots and source filters. Coverage discovery uses only the selected
output directory. An explicit gcovr config is copied temporarily beside its
original so relative options keep their meaning; the original is unchanged and
the temporary file is removed on completion. That config directory must be
writable. Without a config file, the helper uses Ceedling's tool modification
shortcut to append only the isolated object search path. Ceedling's report
options and native tool argument template remain intact, including the different
argument contracts of Ceedling 1.0.1 and 1.1.8.
Firmware builds check BSP linkage; target smoke/HIL procedures exercise
peripheral IRQ/DMA behavior.

Numerics checks run source contracts, C11/C++17 target consumers with all feature
combinations, shared numerical-limit configuration and rejection of local ABI
macro overrides. Numerics also runs ASan/UBSan/float-cast-overflow tests. Its JSON
coverage gate requires every production source and at least 85% line and branch
coverage per source; malformed populations and low coverage have regression tests.
The source-language check uses the Han script property, allowing mathematical
punctuation without accepting Han text; this distinction has a fixture test.

Copyright checks require C and header files to begin with the EsyProject
contributors SPDX notice and the `Apache-2.0` license identifier. Existing
four-digit copyright years and increasing year ranges are accepted. The checker
also preserves independent authors listed in additional nonempty
`SPDX-FileCopyrightText` lines after the project notice. Each author line must
include a four-digit year or increasing year range and an author name. It
rejects missing notices, proprietary headers, and incorrect owners or license
identifiers. Its regression fixtures run in the quality gate:

```sh
tools/checks/check_copyright_header.sh
python3 -B tools/checks/test_copyright_header.py
```

## CI and Release Entry Points

GitHub Actions runs `.github/workflows/quality.yml` for pull requests, pushes to
`main`, and `v*` tags. Tag builds also run the release readiness check and
upload an automated release-notes draft.
Host and coverage jobs run `./setup.sh` with the same provider lock as local
verification, then invoke the ordinary quality gate commands. The host job also
runs the broker smoke and native LVGL consumer against the prepared cache.

Release tags use `vMAJOR.MINOR.PATCH` or `vMAJOR.MINOR.PATCH-PRERELEASE`.

Release helpers:

```sh
tools/quality/generate_changelog.sh HEAD > release-notes.md
```

Before pushing an existing local release tag, validate it with
`tools/checks/check_release_readiness.sh <tag>` and regenerate the final draft
with `tools/quality/generate_changelog.sh <tag>`. These commands require an
existing tag or commit reference; the readiness check specifically requires a
tag. Creating or pushing tags remains an explicit release action.

The release tag checklist is maintained in
`docs/release/release_tag_checklist.md`.

## Layout

`tools/` top level keeps directory documentation. Scripts are grouped by
responsibility:

- `tools/quality/`: repository-level quality gate entry points.
- `tools/providers/`: locked software-provider preparation and cache discovery shared by local runs and CI.
- `tools/checks/`: single-purpose source, contract, and policy checks.
- `tools/coredump/`: retained-record decoding and exact-firmware symbol lookup.

## Checks

| Script | Purpose |
|---|---|
| `../setup.sh` | Prepares the library's pinned host-test providers in the shared user cache; supports provider selection and offline verification. |
| `providers/prepare_providers.py` | Implements setup and retains advanced cache-directory and child-command options. |
| `providers/provider_cache.py` | Shared read-only provider cache selection and lookup for Python tools. |
| `providers/test_prepare_providers.py` | Exercises portable cache selection, provider verification, overrides and child-command environment handling. |
| `quality/ci_quality_gate.sh` | Full submodule quality entry point: contract checks plus module tests. |
| `quality/generate_changelog.sh` | Generates release-note drafts from commits since the previous tag. |
| `checks/check_source_contracts.sh` | Aggregates source-level checks for selected module paths. |
| `checks/check_no_dynamic_alloc.sh` | Scans all four production roots for direct allocator calls/references and aliases, excluding the OS module and test/build support. Comments and literals are ignored; SDK/library internal allocation is not checked. |
| `checks/check_copyright_header.sh` | Verifies first-line EsyProject contributors SPDX notices and Apache-2.0 licensing in C and header files. |
| `checks/test_copyright_header.py` | Checks valid historical years and rejects missing, malformed, proprietary, or mismatched notices. |
| `checks/check_source_language.sh` | Enforces English-only source comments and build metadata. |
| `checks/check_c_empty_parameter_definitions.sh` | Rejects C definitions that use an empty `()` parameter list. |
| `checks/check_public_header_compatibility.sh` | Checks public headers for C self-containment and C++ compatibility. |
| `checks/check_esy_common_contracts.sh` | Verifies `esy_common` public header contracts. |
| `checks/check_esy_hal_backend_contracts.sh` | Verifies `esy_hal_backend` platform source/header API closure and placeholder stubs. |
| `checks/check_hardware_boundaries.py` | Enforces middleware/HAL/backend/board dependency direction. |
| `checks/test_hardware_boundaries.py` | Exercises valid boundaries and rejection of direct calls, includes and links. |
| `checks/check_gcov_thresholds.sh` | Enforces repository-level gcov line coverage floors. |
| `checks/check_release_readiness.sh` | Validates release tag format, changelog entry, and release automation ownership. |
| `checks/check_commit_message.sh` | Validates English commit titles with an optional scope; see [commit conventions](../CONTRIBUTING.md#commit-messages). |
| `checks/test_maintenance_checks.py` | Exercises commit title acceptance and rejection, allocation checks, isolated test runners, and release headings. |
| `checks/test_port_stages.py` | Builds an independent temporary port through the three-file device/pre-project/post-project contract; verifies hook timing, firmware outputs, direct integration, scope promotion and vector ownership across reconfiguration. |
| `checks/test_pico_host_tools.py` | Exercises SDK host-tool package discovery, explicit and standard search precedence, optional installer packages, SDK changes and caller-owned SDK/tool targets. |
| `coredump/esy_coredump_decode.py` | Reassembles CD1 Logger chunks or validates a raw record, writes binary/JSON artifacts, and optionally verifies an ELF build ID before symbolizing saved PC/LR. |
| `coredump/test_esy_coredump_decode.py` | Exercises log loss, reordering, duplicate conflicts, record validation and exact-ELF matching. |

## Scope

These scripts own only EsyMcuPlatform quality concerns:

- `esy_common/**`
- `esy_hal_backend/**`
- `esy_hal/**`
- `esy_middleware/**`
- EsyMcuPlatform module tests

They must not assume a consuming application's startup policy, board resource
policy, feature-gate source of truth, linker script, SDK path, or toolchain
cache.

Consumer-project firmware builds must be verified in the consuming project CI.
HAL backend compilation uses consumer-owned SDK, toolchain and build settings.

## Maintenance

- Keep scripts executable and runnable from the EsyMcuPlatform repository root.
- Prefer small single-purpose `check_*.sh` scripts and let
  `quality/ci_quality_gate.sh` compose them.
- If a check has the same meaning in a consuming project, keep the script name
  aligned while allowing each repository to own its own paths and exemptions.
- Do not move consumer-project quality policy into this directory.

Watchdog monitor source contracts and its module-owned CMake consumer regression
are included in the quality gate. Its Ceedling tests cover compound slot access,
callback ownership, cooperative shutdown and certified deadline failure paths.
The consumer regression requires a host compiler with ThreadSanitizer and pthread
support. Coverage mode checks the exact production source population and enforces
85% line and branch floors; empty or fixture-only reports fail.
Consumer builds check target integration. Target smoke/HIL procedures cover
hardware enable/reset and cross-core reclamation.

Wi-Fi source contracts, C/C++ target consumers, provider/feature isolation and
concurrent request/query/process/PM regressions are part of the quality gate.
The host toolchain must support pthreads, ThreadSanitizer and AddressSanitizer.
The Wi-Fi module JSON coverage gate rejects missing/foreign/duplicate production
sources and enforces 85% line and branch coverage per file. The module's Wi-Fi
HIL plan describes native networking, PM transitions and IRQ behavior.
