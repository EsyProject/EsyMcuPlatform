#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Reject direct C/POSIX allocator references outside the OS implementation.

This lexical check covers calls, macro aliases and function-pointer references.
It does not preprocess sources or inspect allocation inside SDKs/libraries.
"""

import argparse
import os
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_PATHS = ('esy_common', 'esy_hal', 'esy_hal_backend', 'esy_middleware')
EXCLUDED_DIRECTORIES = {'tests', 'build', 'esy_test', '.git'}
ALLOCATOR = re.compile(
    r'\b(?:malloc|calloc|realloc|free|aligned_alloc|strdup|strndup|reallocarray|'
    r'posix_memalign|memalign|valloc|pvalloc|asprintf|vasprintf)\b')
ALLOCATION_HEADER = re.compile(
    r'^[ \t]*#[ \t]*include[ \t]*[<"][ \t]*(?:stdlib|malloc)\.h[ \t]*[>"]', re.M)
NON_CODE = re.compile(
    r'/\*.*?(?:\*/|\Z)|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)


def excluded(path, root):
    """Exempt the existing OS module, without exempting other modules named os."""
    if any(part in EXCLUDED_DIRECTORIES for part in path.parts):
        return True
    try:
        return path.relative_to(root).parts[:2] == ('esy_middleware', 'os')
    except ValueError:
        return False


def production_files(paths, root):
    """Resolve CLI paths as the original shell entry point did, then prune early."""
    files = set()

    def walk_error(error):
        raise error

    for name in paths:
        path = Path(name)
        if not path.is_absolute() and not path.exists():
            path = root / path
        path = path.absolute()
        if path.is_symlink():
            raise ValueError(f'Scan path must not be a symbolic link: {path}')
        path = path.resolve()
        if not path.exists():
            raise ValueError(f'Scan path does not exist: {path}')
        if excluded(path, root):
            continue
        if path.is_file():
            if path.suffix in ('.c', '.h'):
                files.add(path)
            continue
        for parent, directories, names in os.walk(path, followlinks=False, onerror=walk_error):
            parent = Path(parent)
            directories[:] = sorted(name for name in directories
                                     if not (parent / name).is_symlink()
                                     and not excluded(parent / name, root))
            for name in names:
                candidate = parent / name
                if candidate.suffix in ('.c', '.h') and not candidate.is_symlink():
                    files.add(candidate)
    return sorted(files)


def source_references(source):
    """Apply C line splicing before masking comments/literals; keep source lines."""
    characters = []
    source_lines = []
    line = 1
    position = 0
    while position < len(source):
        splice = re.match(r'\\\r?\n', source[position:position + 3])
        if splice:
            position += len(splice.group())
            line += 1
            continue
        characters.append(source[position])
        source_lines.append(line)
        line += source[position] == '\n'
        position += 1
    spliced = ''.join(characters)

    def mask(match, literals):
        token = match.group()
        if not literals and token[0] in ('"', "'"):
            return token
        return ''.join('\n' if character == '\n' else ' ' for character in token)

    comments_masked = NON_CODE.sub(lambda match: mask(match, False), spliced)
    code = NON_CODE.sub(lambda match: mask(match, True), spliced)
    references = [(source_lines[match.start()], f'allocator reference {match.group()}')
                  for match in ALLOCATOR.finditer(code)]
    references.extend((source_lines[match.start()], 'allocation header')
                      for match in ALLOCATION_HEADER.finditer(comments_masked))
    return sorted(references)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('paths', nargs='*', help='Files or directories; defaults to all production roots')
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        files = production_files(args.paths or DEFAULT_PATHS, root)
        errors = []
        for path in files:
            try:
                label = path.relative_to(root)
            except ValueError:
                label = path
            errors.extend(f'{label}:{line}: {message}'
                          for line, message in source_references(path.read_text(encoding='utf-8')))
    except (OSError, UnicodeError, ValueError) as error:
        print(f'error: {error}', file=sys.stderr)
        return 2
    for error in errors:
        print(error)
    if errors:
        print('Direct dynamic allocation references found in production sources.', file=sys.stderr)
        return 1
    print(f'No direct allocator references found in {len(files)} production C/header files. '
          'SDK/library internal allocation is not checked.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
