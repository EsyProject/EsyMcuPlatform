<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Release Tag Checklist

This checklist defines the required release evidence for EsyMcuPlatform tags.
The tag workflow enforces the machine-checkable items and publishes a generated
release-notes draft as a CI artifact.

## Required Tag Format

Release tags must use:

```text
vMAJOR.MINOR.PATCH
vMAJOR.MINOR.PATCH-PRERELEASE
```

Examples:

```text
v1.4.0
v1.4.0-rc.1
```

## Pre-Tag Checklist

- Confirm that the working tree is clean.
- Run `tools/quality/ci_quality_gate.sh test:all`.
- Run `tools/quality/ci_quality_gate.sh gcov:all`.
- Run `tools/checks/check_gcov_thresholds.sh`.
- Update `CHANGELOG.md` with a section for the release tag or version.
- Generate a release-notes draft with `tools/quality/generate_changelog.sh HEAD`.
- Review public API contracts, supported configurations and known integration risks.
- Record affected consumer builds, enabled features and SDK/toolchain versions
  alongside the release evidence.

## Local Tag Validation

After committing the release entry and creating the local tag, run:

```sh
tools/checks/check_release_readiness.sh <tag>
tools/quality/generate_changelog.sh <tag> > release-notes.md
```

The readiness check requires an existing local tag. The generator accepts an
existing commit or tag, so `HEAD` is suitable before tagging and the release tag
identifies the final draft. Review these results before pushing the tag. If the
candidate needs changes, select and validate the corrected release commit before
publishing its tag.

## CI Enforcement

The `.github/workflows/quality.yml` workflow runs on pull requests, pushes to
`main`, and `v*` tags. Tag builds must pass:

- Contract checks and all module-owned Ceedling tests.
- Gcov runs for all module-owned Ceedling projects.
- Repository-level gcov threshold checks.
- Release readiness checks for tag format, changelog presence, and release
  checklist ownership.

## Changelog Automation

Use the changelog generator to produce a draft from commits since the previous
tag:

```sh
tools/quality/generate_changelog.sh v1.4.0 > release-notes.md
```

The generated file is a review aid. The curated release entry still belongs in
`CHANGELOG.md` so downstream consumers have a stable, human-readable change
history in the repository.
