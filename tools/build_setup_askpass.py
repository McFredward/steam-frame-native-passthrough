#!/usr/bin/env python3
"""Build the small owned Windows OpenSSH askpass child; never install or run it."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MAX_EXECUTABLE = 1024 * 1024
SYSTEM_IMPORTS = {'kernel32.dll', 'msvcrt.dll', 'shell32.dll', 'ws2_32.dll',
                  'ucrtbase.dll'}


def inspect(raw):
    """Validate the architecture, GUI subsystem and bounded system-only imports."""
    if not 128 <= len(raw) <= MAX_EXECUTABLE or raw[:2] != b'MZ':
        raise ValueError('Expected a bounded x64 PE executable.')
    offset = struct.unpack_from('<I', raw, 60)[0]
    if offset > len(raw) - 26 or raw[offset:offset + 4] != b'PE\0\0':
        raise ValueError('Invalid PE signature.')
    machine, sections = struct.unpack_from('<HH', raw, offset + 4)
    optional_size, flags = struct.unpack_from('<HH', raw, offset + 20)
    optional = offset + 24
    if (machine != 0x8664 or not 1 <= sections <= 32 or optional_size < 240
            or optional + optional_size + sections * 40 > len(raw) or flags & 0x2000
            or struct.unpack_from('<H', raw, optional)[0] != 0x20b
            or struct.unpack_from('<H', raw, optional + 68)[0] != 2):
        raise ValueError('Askpass must be a standalone x64 Windows GUI executable.')
    if struct.unpack_from('<II', raw, optional + 112) != (0, 0):
        raise ValueError('Askpass unexpectedly exports an interface.')

    def file_offset(rva, size):
        for index in range(sections):
            section = optional + optional_size + index * 40
            virtual_size, virtual_address, disk_size, disk_offset = struct.unpack_from('<IIII', raw, section + 8)
            if virtual_address <= rva and rva - virtual_address + size <= disk_size:
                result = disk_offset + rva - virtual_address
                if result + size <= len(raw):
                    return result
        raise ValueError('Unmapped PE import bytes.')

    imports_rva, imports_size = struct.unpack_from('<II', raw, optional + 120)
    if not imports_rva or not 20 <= imports_size <= 65536:
        raise ValueError('Expected bounded Windows system imports.')
    imports = []
    for index in range(32):
        if (index + 1) * 20 > imports_size:
            raise ValueError('Unterminated PE import directory.')
        entry = file_offset(imports_rva + index * 20, 20)
        descriptor = struct.unpack_from('<IIIII', raw, entry)
        if descriptor == (0, 0, 0, 0, 0):
            break
        name_at = file_offset(descriptor[3], 1)
        name_end = raw.find(b'\0', name_at, min(name_at + 128, len(raw)))
        if name_end < 0:
            raise ValueError('Unterminated PE import name.')
        try:
            name = raw[name_at:name_end].decode('ascii').lower()
        except UnicodeError as error:
            raise ValueError('Invalid PE import name.') from error
        if name not in SYSTEM_IMPORTS and not re.fullmatch(r'api-ms-win-crt-[a-z0-9-]+-l1-1-0[.]dll', name):
            raise ValueError('Askpass must not require separately installed libraries.')
        if name in imports:
            raise ValueError('Duplicate system import descriptor.')
        imports.append(name)
    else:
        raise ValueError('PE import descriptor bound exceeded.')
    if not {'kernel32.dll', 'shell32.dll', 'ws2_32.dll'} <= set(imports):
        raise ValueError('Required Windows interfaces are missing.')
    return {'architecture': 'windows-x86_64', 'subsystem': 'windows-gui',
            'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest(), 'imports': imports}


def build(output, compiler=None, source=None):
    source = ROOT / 'setup/steam_link_askpass.c' if source is None else Path(source).resolve()
    selected = compiler or os.environ.get('CC')
    candidates = [selected] if selected else ['x86_64-w64-mingw32-gcc', *(['gcc', 'cl.exe', 'cl'] if os.name == 'nt' else [])]
    executable = next((shutil.which(candidate) for candidate in candidates if candidate and shutil.which(candidate)), None)
    if executable is None:
        raise RuntimeError('An x64 MinGW-w64 or MSVC C compiler is required to build the distribution helper.')
    output = Path(output).resolve();output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='sfnp-askpass-build-', dir=output.parent) as directory:
        work = Path(directory);binary = work / 'steam-link-askpass.exe'
        if Path(executable).name.lower() in ('cl', 'cl.exe'):
            command = [executable, '/nologo', '/O2', '/W4', '/WX', '/MT', '/DUNICODE', '/D_UNICODE',
                       '/D_WIN32_WINNT=0x0600', str(source), '/Fe:' + str(binary), '/Fo:' + str(work / 'askpass.obj'),
                       '/link', '/SUBSYSTEM:WINDOWS', '/MACHINE:X64', 'ws2_32.lib', 'shell32.lib']
        else:
            command = [executable, '-std=c11', '-Os', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                       '-municode', '-mwindows', '-static-libgcc', '-Wl,--no-insert-timestamp',
                       '-s', str(source), '-lws2_32', '-lshell32', '-o', str(binary)]
        completed = subprocess.run(command, cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, timeout=120)
        if completed.returncode:
            raise RuntimeError('Askpass C compilation failed:\n' + completed.stdout[-8192:])
        raw = binary.read_bytes();record = inspect(raw)
        record.update(schema='sfnp-setup-askpass-build-v1', source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                      compiler=Path(executable).name, compiler_output=completed.stdout)
        os.replace(binary, output)
    return record


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler')
    parser.add_argument('--record', type=Path)
    args = parser.parse_args()
    record = build(args.output, args.compiler)
    if args.record:
        args.record.parent.mkdir(parents=True, exist_ok=True)
        args.record.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, sort_keys=True))


if __name__ == '__main__':
    main()
