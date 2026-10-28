#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Enforce public HAL access to board resources and hardware backends."""

import argparse
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
NON_CODE = re.compile(
    r'/\*.*?(?:\*/|\Z)|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"\n]+)[>"]', re.M)
LOWER_CALL = re.compile(r'\b(?:esy_hb_|esy_board_)[A-Za-z0-9_]*\s*\(')
HAL_CALL = re.compile(r'\besy_hal_(?!backend)[A-Za-z0-9_]*\s*\(')
NATIVE_RTOS_HEADER = re.compile(
    r'^(?:freertos/|(?:FreeRTOS|task|queue|semphr|timers|event_groups|'
    r'stream_buffer|message_buffer|portmacro|portable)\.h$)')
NATIVE_RTOS_CALL = re.compile(r'\b(?:[xv]Task|[xv]Queue|[xv]Semaphore|[xv]Timer)\w*\s*\(')
NATIVE_HARDWARE_HEADER = re.compile(
    r'^(?:hardware/|pico/|driver/|soc/|hal/|rom/|esp_[^/]*\.h$|'
    r'stm32[^/]*\.h$|core_cm\w*\.h$|cmsis_\w*\.h$|sdkconfig\.h$)')
NATIVE_HARDWARE_CALL = re.compile(
    r'\b(?:HAL_[A-Za-z0-9_]+|__HAL_[A-Za-z0-9_]+|NVIC_[A-Za-z0-9_]+|'
    r'(?:gpio|pio|dma|hardware_alarm|uart|spi|i2c|adc|watchdog|clock|esp_timer|cyw43)_'
    r'[A-Za-z0-9_]+)\s*\(')
UPPER_COMMON_HEADER = re.compile(r'^(?:esy_hal/|esy_hal_backend/|esy_middleware/|esy_board/)')
UPPER_COMMON_CALL = re.compile(r'\besy_(?:hal|hb|board|os)_[A-Za-z0-9_]*\s*\(')
LOCAL_FUNCTION = re.compile(r'^\s*static\s+[^;{}=]*?\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{', re.M)
# Version-pinned private SDK integration; this is not a platform-module seam.
VENDOR_INTERNAL_HEADERS = {
    'esy_hal_backend/src/esp32/esy_hb_fault.c': {'esp_private/panic_internal.h'},
}
# Pure JSON parsing has no HAL/OS dependency. Permit only this codec's private
# software provider; similarly named hardware, network and RTOS targets stay banned.
COMMON_PRIVATE_SOFTWARE_PROVIDERS = {
    ('esy_common/protocol_codec/CMakeLists.txt', 'esy_common_protocol_json'):
        {'esy_json_parser_provider'},
}


def module_owner(relative):
    """Return the library module owning implementation-private headers."""
    parts = relative.parts
    return parts[:2] if parts[0] in ('esy_common', 'esy_middleware') else parts[:1]


def source_views(source):
    """Keep include literals but hide literals in call checks after C splicing."""
    source = re.sub(r'\\\r?\n', '', source)

    def mask(match, literals):
        token = match.group()
        if not literals and token[0] in ('"', "'"):
            return token
        return ''.join('\n' if character == '\n' else ' ' for character in token)

    return (NON_CODE.sub(lambda match: mask(match, False), source),
            NON_CODE.sub(lambda match: mask(match, True), source))


def has_native_hardware_call(code):
    """Avoid classifying local helpers and injected function pointers by name."""
    local = set(LOCAL_FUNCTION.findall(code))
    for match in NATIVE_HARDWARE_CALL.finditer(code):
        name = match.group().split('(')[0].strip()
        before = code[:match.start()].rstrip()
        if name not in local and not before.endswith(('.', '->')):
            return True
    return False


def common_linkage_allowed(relative, linkage):
    """Keep software-provider exceptions private and local to their codec target."""
    owner = re.match(r'\s*(\S+)', linkage)
    allowed_providers = COMMON_PRIVATE_SOFTWARE_PROVIDERS.get(
        (relative.as_posix(), owner.group(1) if owner else ''), set())
    scope = None
    for token in re.findall(
            r'\b(?:PUBLIC|PRIVATE|INTERFACE|LINK_PUBLIC|LINK_PRIVATE|'
            r'esy_\w+|hardware_\w+|pico_\w+|idf::\w+)\b', linkage):
        if token in ('PUBLIC', 'PRIVATE', 'INTERFACE', 'LINK_PUBLIC', 'LINK_PRIVATE'):
            scope = token
        elif token.startswith('esy_common') or token == 'esy_opt_default':
            continue
        elif scope != 'PRIVATE' or token not in allowed_providers:
            return False
    return True


def production_files(root):
    """Prune generated/test directories before descending into the source tree."""
    import os
    for module in ('esy_common', 'esy_middleware', 'esy_hal', 'esy_hal_backend'):
        directory = root / module
        if not directory.is_dir():
            continue
        for parent, directories, files in os.walk(directory, followlinks=False):
            directories[:] = sorted(name for name in directories
                                     if name not in ('tests', 'build', '.git')
                                     and not (Path(parent) / name).is_symlink())
            for name in sorted(files):
                path = Path(parent) / name
                if not path.is_symlink() and (path.suffix in ('.c', '.h', '.cmake')
                                              or name == 'CMakeLists.txt'):
                    yield path


def check(root):
    errors = []
    files = list(production_files(root))
    internal_headers = {}
    for path in files:
        if path.name.endswith('_internal.h'):
            internal_headers.setdefault(path.name, []).append(path)
    for path in files:
        relative = path.relative_to(root)
        module = relative.parts[0]
        source = path.read_text(encoding='utf-8')
        public_hal = module == 'esy_hal' and 'include' in relative.parts
        if path.suffix in ('.c', '.h'):
            includes, code = source_views(source)
            os_port = tuple(relative.parts[:4]) == ('esy_middleware', 'os', 'src', 'freertos')
            for include in INCLUDE.findall(includes):
                if module == 'esy_common' and UPPER_COMMON_HEADER.search(include):
                    errors.append(f'{relative}: common must not include upper-layer header {include}')
                hardware_visible = module in ('esy_common', 'esy_middleware') or public_hal
                if hardware_visible and NATIVE_HARDWARE_HEADER.search(include):
                    errors.append(f'{relative}: native hardware header {include} belongs to private HAL/backend code')
                if not os_port and NATIVE_RTOS_HEADER.search(include):
                    errors.append(f'{relative}: native RTOS header {include} belongs to the OS port')
                lower_header = include.startswith(('esy_hal_backend/', 'esy_board/')) or include.endswith('esy_board_defs.h')
                if (module == 'esy_middleware' or public_hal) and lower_header:
                    errors.append(f'{relative}: lower-layer header {include} bypasses the public HAL boundary')
                if module == 'esy_hal_backend' and include.startswith('esy_hal/'):
                    errors.append(f'{relative}: a hardware backend must not include its HAL caller')
                if (include.endswith('_internal.h') and include not in
                        VENDOR_INTERNAL_HEADERS.get(relative.as_posix(), set())):
                    candidates = internal_headers.get(Path(include).name, [])
                    explicit = {(path.parent / include).resolve(), (root / include).resolve()}
                    exact = [candidate for candidate in candidates if candidate.resolve() in explicit]
                    if exact:
                        candidates = exact
                    elif '/' in include:
                        candidates = []
                    if ('include' in relative.parts or not candidates or
                            not any(module_owner(candidate.relative_to(root)) == module_owner(relative)
                                    for candidate in candidates)):
                        errors.append(f'{relative}: internal header {include} is not public or cross-module API')
            if module == 'esy_middleware' and LOWER_CALL.search(code):
                errors.append(f'{relative}: middleware must access hardware through public HAL APIs')
            if module == 'esy_hal_backend' and HAL_CALL.search(code):
                errors.append(f'{relative}: a hardware backend must not call its HAL caller')
            if module == 'esy_common' and path.suffix == '.c' and UPPER_COMMON_CALL.search(code):
                errors.append(f'{relative}: common must not call upper-layer APIs')
            if module in ('esy_common', 'esy_middleware') and has_native_hardware_call(code):
                errors.append(f'{relative}: native hardware operations belong to private HAL/backend code')
            if not os_port and NATIVE_RTOS_CALL.search(code):
                errors.append(f'{relative}: native RTOS calls belong to the OS port')
        elif module in ('esy_common', 'esy_middleware'):
            code = re.sub(r'#[^\n]*', '', source)
            for linkage in re.findall(r'\btarget_link_libraries\s*\((.*?)\)', code, re.S):
                if module == 'esy_middleware' and re.search(r'\b(?:esy_hal_backend|esy_board)\b', linkage):
                    errors.append(f'{relative}: middleware must not link a hardware backend or board target directly')
                if module == 'esy_common':
                    if not common_linkage_allowed(relative, linkage):
                        errors.append(f'{relative}: common must not link upper-layer or native provider targets')
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    args = parser.parse_args()
    errors = check(args.root.resolve())
    for error in errors:
        print(f'error: {error}')
    if errors:
        return 1
    print('Hardware dependency boundaries passed.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
