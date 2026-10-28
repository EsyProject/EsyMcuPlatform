# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Validate gcovr rows against canonical sources in the local checkout."""

from pathlib import Path
import re


def resolve_coverage_source(raw, allowed, anchors):
    """Resolve absolute paths or paths relative to the explicitly supplied roots.

    Callers supply canonical local files. A matching suffix or basename is not
    evidence of source identity; every accepted path must resolve to one file
    in that set, independently of the current working directory.
    """
    raw = Path(raw.replace("\\", "/"))
    candidates = {raw.resolve()} if raw.is_absolute() else {
        (anchor / raw).resolve() for anchor in anchors}
    paths = candidates & allowed
    if len(paths) != 1:
        raise ValueError(f"Foreign or ambiguous coverage source: {raw}")
    return paths.pop()


def coverage_entries(entries, expected, anchors, optional=()):
    """Require unique JSON rows for every expected local source.

    Optional local files, such as internal headers emitted by gcovr, may appear
    once but cannot substitute for a required source. All identities, including
    aliases containing '..' or symlinks, are canonicalized before duplicates
    are checked. Coverage thresholds remain the responsibility of each gate.
    """
    expected = {Path(path).resolve() for path in expected}
    allowed = expected | {Path(path).resolve() for path in optional}
    anchors = tuple(Path(anchor).resolve() for anchor in anchors)
    if not expected:
        raise ValueError("No production sources in coverage population")
    rows = {}
    for entry in entries:
        path = resolve_coverage_source(entry["file"], allowed, anchors)
        if path in rows:
            raise ValueError(f"Duplicate coverage population source: {path}")
        rows[path] = entry
    missing = expected - rows.keys()
    if missing:
        raise ValueError(f"Missing coverage population sources: {sorted(map(str, missing))}")
    return rows


def check_module_lines(root, module, report, minimum=85, require_total=False,
                       extra_anchors=()):
    """Require each production C file; optional local header rows are validated.

    Report may be one path or a sequence of disjoint source-population reports.
    Paths may be absolute or relative to the platform, module or test directory;
    explicitly supplied extra anchors support independently owned nested suites.
    Basenames alone cannot identify a production file. Thresholds use the line
    counts rather than gcovr's rounded display percentage. This helper checks
    coverage evidence only; it does not establish branch or hardware coverage.
    """
    root = Path(root).resolve()
    module_dir = root / module
    source_dir = module_dir / "src"
    expected = {path.resolve() for path in source_dir.rglob("*.c")}
    allowed = expected | {path.resolve() for path in source_dir.rglob("*.h")}
    if not expected:
        raise ValueError(f"No production C sources for {module}")
    anchors = (root, module_dir, module_dir / "tests", *extra_anchors)
    reports = (report,) if isinstance(report, (str, Path)) else tuple(report)
    if not reports:
        raise ValueError(f"No coverage reports for {module}")
    rows = {}
    for source_report in reports:
        text = Path(source_report).read_text().replace("\\", "/")
        # Gcovr puts long filenames on a separate line, including internal headers.
        text = re.sub(r"(\.[A-Za-z0-9_]+)\n\s+(?=\d)", r"\1 ", text)
        total = None
        for line in text.splitlines():
            match = re.match(r"\s*(.+?\.[A-Za-z0-9_]+)\s+(\d+)\s+(\d+)\s+\d+(?:\.\d+)?%", line)
            if match:
                path = resolve_coverage_source(match[1].strip(), allowed, anchors)
                if path in rows:
                    raise ValueError(f"Duplicate {module} coverage source: {path}")
                rows[path] = (int(match[2]), int(match[3]))
                if rows[path][1] > rows[path][0]:
                    raise ValueError(f"Invalid coverage counts: {path}")
            else:
                match = re.match(r"\s*TOTAL\s+(\d+)\s+(\d+)\s+\d+(?:\.\d+)?%", line)
                if match:
                    if total is not None:
                        raise ValueError(f"Duplicate {module} coverage total")
                    total = (int(match[1]), int(match[2]))
        if require_total and (total is None or total[0] == 0 or total[1] > total[0]
                              or 100 * total[1] < minimum * total[0]):
            raise ValueError(f"{module}: invalid aggregate or coverage below {minimum}%")
    missing = expected - rows.keys()
    if missing:
        raise ValueError(f"Missing {module} coverage: {sorted(str(path) for path in missing)}")
    for path in sorted(expected):
        lines, executed = rows[path]
        if lines == 0 or 100 * executed < minimum * lines:
            raise ValueError(f"{path}: {executed}/{lines} covered lines; requires {minimum}%")
    return len(expected)
