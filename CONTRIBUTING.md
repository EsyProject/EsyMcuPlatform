<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Contributing to EsyMcuPlatform

EsyMcuPlatform is maintained as a reusable MCU platform library. Contributions must preserve submodule-friendly integration: downstream firmware projects should be able to clone this repository independently and link only the features they need.

## Licensing

EsyMcuPlatform is an EsyProject library: **Embedded software for you.** Original
contributions are provided under Apache-2.0; see [LICENSE](LICENSE). Retain
existing copyright years, independent author notices, and third-party terms.
Do not replace an upstream license with the project's default license.

Original C sources and headers start with this notice, using the actual year
or year range for that file:

```c
/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
```

Use the file format's native comment syntax for other source and build files.
Keep script interpreter directives on the first line. Document imported or
adapted code in [NOTICE](NOTICE), preserve its attribution in the source, and
include its license text when required. The SPI and UART RX PIO adapters
demonstrate combined `Apache-2.0 AND BSD-3-Clause` notices.

## Before Changing Code

- Apply the repository-local engineering rules.
- Identify whether the change affects public API, build integration, platform ports, middleware behavior, or shared test support.
- Keep application policy in the consuming project. Do not hard-code product-specific paths, startup order, board resources, or application directory layouts into EsyMcuPlatform.
- Check existing middleware and data-structure modules before adding a new abstraction.

## Development Setup

Install the [host prerequisites](tools/README.md#entry-point), then prepare an
independent library checkout:

```sh
git clone https://github.com/EsyProject/EsyMcuPlatform.git
cd EsyMcuPlatform
./setup.sh
```

Setup downloads this library's seven pinned host-test providers to the shared
user cache. Library test tools reuse that cache automatically in later shells;
no `source` command, provider path configuration or consuming firmware project
is required. Run `./setup.sh --offline` to verify already prepared dependencies.
Setup leaves consuming projects and their Git submodules untouched. Host tools,
hardware SDKs and toolchains are installed separately. See
[software provider setup](tools/README.md#software-provider-setup) for cache
locations and focused preparation.

## Build Rules

- Keep CMake target dependencies explicit.
- Prefer target-level `target_include_directories`, `target_link_libraries`, `target_compile_definitions`, and `target_compile_options`.
- Avoid global `include_directories`, `add_definitions`, `add_compile_options`, and `add_link_options` in new code.
- Do not add developer-machine absolute paths.
- Feature variables such as `ENABLE_ESY_*` must be defined only by the consuming project's `esy_feature_import.cmake`.
- EsyMcuPlatform CMake files may consume feature variables, but must not define a second feature source.

For a new device or hardware family, follow the
[porting guide](docs/integration/porting.md). Each `cmake/ports/<port>/` keeps a
device map, SDK/toolchain entry, build-hook definitions and firmware image entry;
backend and OS implementations remain with their owning modules. Shared entry
points control include/hook timing and validate contracts without maintaining
platform-name lists. Loading a build-hook file must not perform SDK or target
setup.

## Test Layout

Root `esy_test/` contains cross-project test support only:

- fakes
- fixtures
- mock adapters
- host-test utilities

EsyMcuPlatform's own module tests must live inside the module:

- `esy_hal/tests/`
- `esy_common/ds/tests/`
- `esy_common/protocol_codec/tests/`
- `esy_middleware/logger/tests/`
- `esy_middleware/protocol/tests/`

New public behavior should include module-level tests when the behavior can be tested on host.

## Code Style

- Use C11-compatible C.
- Use English comments and English Doxygen text.
- Keep file-local functions `static`.
- Validate pointer and range arguments for public APIs.
- Return documented error codes for recoverable failures.
- Outside the OS module, do not use heap allocators directly, including aliases
  and function pointers. Caller-owned buffers and contexts remain the default.
  A hardware/software adapter may create provider-owned native resources only
  through its declared lifecycle or control API, with documented ownership,
  allocation/blocking costs and failure cleanup. Do not create/destroy them in
  ISR, ordinary asynchronous data callbacks or steady-state data paths. Declared
  owner lifecycle hooks follow their documented execution context and retirement
  contract. A direct-allocation source check does
  not establish that the provider is heap-free or meets a timing budget.
- Do not leave dead code or speculative extension points.

## Public API Changes

When changing a public header:

- Update Doxygen in the same change.
- Document ownership, concurrency, ISR safety, memory lifetime, and error behavior where relevant.
- Consider downstream compatibility.
- Add or update tests for the changed behavior.

## Verification

Run the narrowest useful checks first, then broaden based on risk:

From a configured consuming firmware project, run its embedded build:

```sh
cmake --build build
```

For a new consumer, follow the [quick start](README.md#quick-start)
and configure that consumer before building. The library root requires
consumer-owned board, optimization targets and SDK providers; a bare
`cmake -S external/EsyMcuPlatform` command cannot supply them.

From an independent EsyMcuPlatform checkout, run host checks without configuring
a firmware target after the development setup above:

```sh
tools/checks/check_esy_common_contracts.sh
cd esy_common/ds/tests
ceedling test:all
ceedling gcov:all
```

Run Ceedling from the selected module's `tests/` directory. Its coverage output
is written to `build/artifacts/gcov/gcovr/` beneath that directory. From the
repository root, run all module projects and enforce the repository coverage
floor using the prepared dependencies:

```sh
tools/quality/ci_quality_gate.sh test:all
tools/quality/ci_quality_gate.sh gcov:all
tools/checks/check_gcov_thresholds.sh
```

The [tools guide](tools/README.md) describes source checks, test discovery,
isolated sanitizer runs and CI/release entry points. The direct allocation
check is available separately as `tools/checks/check_no_dynamic_alloc.sh`; its
documented exclusions and provider boundary limit what a passing scan proves.

Host checks cover portable contracts and build assembly. Consumer firmware
builds cover target integration; board procedures cover peripheral timing and
electrical behavior.

If a check cannot be run in the current environment, state the reason in the delivery notes.

## Commit Messages

Commit titles must be English and use `<type>: <summary>` or
`<type>(<scope>): <summary>`. Allowed types are `feat`, `fix`, `docs`, `style`,
`refactor`, `test`, `chore`, and `ci`. The exact title `Initial commit` is also
accepted.

Scope is optional. Prefer it for changes belonging to a clear module or build
area; changes spanning modules may omit it. Keep scopes short and stable, with
no fixed allowlist. A scope must match `[a-z0-9][a-z0-9-]*`: start with a
lowercase letter or digit, followed by lowercase letters, digits, or hyphens.

Validate the intended title before committing, then check the latest commit:

```sh
tools/checks/check_commit_message.sh --message "fix(uart): handle RX restart failures"
tools/checks/check_commit_message.sh --last
```

## Release Notes

Before creating a release tag:

- Update `CHANGELOG.md` with the release section.
- Run `tools/quality/ci_quality_gate.sh test:all`.
- Run `tools/quality/ci_quality_gate.sh gcov:all`.
- Run `tools/checks/check_gcov_thresholds.sh`.
- Generate and review a draft with `tools/quality/generate_changelog.sh HEAD`.

After committing the release entry and creating the local tag, run
`tools/checks/check_release_readiness.sh <tag>` and regenerate the final draft
with `tools/quality/generate_changelog.sh <tag>`. Both commands require an
existing reference. Review their results before pushing the tag.

The full checklist is maintained in `docs/release/release_tag_checklist.md`.
