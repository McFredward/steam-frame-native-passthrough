#!/usr/bin/env python3
"""Validate pinned portable dependencies; return bytes, never install or execute."""
from __future__ import annotations

import hashlib
import io
import json
from pathlib import PurePosixPath
import re
import stat
import struct
import zipfile

MAX_ZIP = 16 * 1024 * 1024
MAX_MEMBER = 8 * 1024 * 1024
MAX_EXPANDED = 32 * 1024 * 1024
OPENSSH_FILES = {'OpenSSH-Win64/ssh.exe', 'OpenSSH-Win64/libcrypto.dll',
                 'OpenSSH-Win64/LICENSE.txt', 'OpenSSH-Win64/NOTICE.txt'}


class DependencyError(ValueError):
    """Fixed dependency validation diagnostics, without archive-supplied text."""


def need(value, message):
    if not value:
        raise DependencyError(message)


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def file_pin(pin, maximum):
    need(type(pin) is dict and set(pin) == {'bytes', 'sha256'} and
         type(pin['bytes']) is int and 0 < pin['bytes'] <= maximum and
         type(pin['sha256']) is str and re.fullmatch('[0-9a-f]{64}', pin['sha256']), 'Invalid dependency pin.')


def safe_name(name):
    need(type(name) is str and 0 < len(name) <= 256 and '\\' not in name and ':' not in name and
         not name.startswith('/') and not any(ord(c) < 32 for c in name), 'Unsafe dependency archive path.')
    parts = name.rstrip('/').split('/')
    need(all(p not in ('', '.', '..') and not p.endswith((' ', '.')) for p in parts) and
         parts[0] == 'OpenSSH-Win64', 'Unexpected dependency archive root.')
    return PurePosixPath(*parts)


def pe_x64(raw):
    need(len(raw) >= 64 and raw[:2] == b'MZ', 'Expected x64 PE dependency.')
    offset = struct.unpack_from('<I', raw, 60)[0]
    need(offset <= len(raw) - 26 and raw[offset:offset + 4] == b'PE\0\0' and
         struct.unpack_from('<H', raw, offset + 4)[0] == 0x8664 and
         struct.unpack_from('<H', raw, offset + 24)[0] == 0x20b, 'Expected x64 PE dependency.')


def windows_openssh(raw, pin):
    """Return only ssh.exe, its reviewed DLL, full notices and provenance."""
    need(type(pin) is dict and set(pin) == {'schema', 'architecture', 'source_url', 'release_notes',
         'upstream_status', 'bytes', 'sha256', 'files'} and pin['schema'] == 'sfnp-portable-openssh-pin-v1' and
         pin['architecture'] == 'x86_64' and pin['upstream_status'] == 'preview', 'Unknown OpenSSH source contract.')
    match = re.fullmatch(r'https://github[.]com/PowerShell/Win32-OpenSSH/releases/download/([0-9.]+p[0-9]+-Preview)/OpenSSH-Win64[.]zip',
                         pin['source_url']) if type(pin['source_url']) is str else None
    need(match and pin['release_notes'] == 'https://github.com/PowerShell/Win32-OpenSSH/releases/tag/' + match[1],
         'Unapproved OpenSSH source.')
    file_pin({key: pin[key] for key in ('bytes', 'sha256')}, MAX_ZIP)
    need(type(raw) is bytes and len(raw) == pin['bytes'] and digest(raw) == pin['sha256'], 'OpenSSH archive pin mismatch.')
    need(type(pin['files']) is dict and set(pin['files']) == OPENSSH_FILES, 'Exact client and license pins required.')
    for value in pin['files'].values():
        file_pin(value, MAX_MEMBER)
    result = {}
    try:
        with zipfile.ZipFile(io.BytesIO(raw)) as archive:
            entries = archive.infolist()
            need(0 < len(entries) <= 128, 'Dependency archive entry bound exceeded.')
            names, total = set(), 0
            for item in entries:
                safe_name(item.filename)
                folded = item.filename.rstrip('/').casefold()
                need(folded not in names, 'Duplicate dependency archive path.')
                names.add(folded)
                kind = stat.S_IFMT(item.external_attr >> 16)
                need(not item.flag_bits & 1 and kind in (0, stat.S_IFREG, stat.S_IFDIR), 'Linked or encrypted dependency entry refused.')
                need(0 <= item.file_size <= MAX_MEMBER, 'Dependency entry size bound exceeded.')
                total += item.file_size
                need(total <= MAX_EXPANDED, 'Expanded dependency size bound exceeded.')
                if item.filename not in OPENSSH_FILES:
                    continue  # Server/services/tools stay inside the verified source ZIP.
                expected = pin['files'][item.filename]
                need(not item.is_dir() and item.file_size == expected['bytes'], 'Dependency file size mismatch.')
                payload = archive.read(item)
                need(digest(payload) == expected['sha256'], 'Dependency file hash mismatch.')
                if item.filename.endswith(('.exe', '.dll')):
                    pe_x64(payload)
                result['setup/runtime/openssh/' + PurePosixPath(item.filename).name] = payload
    except (zipfile.BadZipFile, RuntimeError, OSError, NotImplementedError) as error:
        raise DependencyError('Invalid pinned OpenSSH archive.') from error
    need(len(result) == 4, 'Required client or license file missing.')
    provenance = {'schema': 'sfnp-portable-dependency-source-v1', 'project': 'Microsoft Win32-OpenSSH',
                  **{key: pin[key] for key in ('source_url', 'release_notes', 'upstream_status', 'bytes', 'sha256', 'files')}}
    result['setup/runtime/openssh/source.json'] = (json.dumps(provenance, sort_keys=True, indent=2) + '\n').encode()
    return result


def validate_linux_python(pin):
    """Validate metadata only; the bootstrap retains the whole original tarball."""
    need(type(pin) is dict and set(pin) == {'schema', 'release', 'python_version', 'license_info', 'architectures'} and
         pin['schema'] == 'sfnp-linux-python-pins-v1' and type(pin['release']) is str and
         re.fullmatch('[0-9]{8}', pin['release']) and type(pin['python_version']) is str and
         re.fullmatch(r'3[.](?:1[1-5])[.][0-9]+', pin['python_version']), 'Unknown portable Python metadata.')
    need(pin['license_info'] == 'https://github.com/astral-sh/python-build-standalone/blob/' + pin['release'] + '/docs/running.rst#licensing',
         'Unapproved portable Python license source.')
    targets = {'x86_64': 'x86_64-unknown-linux-gnu', 'aarch64': 'aarch64-unknown-linux-gnu'}
    need(type(pin['architectures']) is dict and 1 <= len(pin['architectures']) <= 2 and
         set(pin['architectures']) <= set(targets), 'Unsupported portable Python architecture.')
    for architecture, item in pin['architectures'].items():
        need(type(item) is dict and set(item) == {'target', 'glibc_min', 'source_url', 'bytes', 'sha256',
             'archive_root', 'executable', 'license_path'} and item['target'] == targets[architecture] and
             item['glibc_min'] == '2.17' and item['archive_root'] == 'python', 'Unknown portable Python target.')
        version = pin['python_version']
        need(item['source_url'] == 'https://github.com/astral-sh/python-build-standalone/releases/download/' +
             pin['release'] + '/cpython-' + version + '%2B' + pin['release'] + '-' + targets[architecture] +
             '-install_only_stripped.tar.gz', 'Unapproved portable Python source.')
        need(item['executable'] == 'python/bin/python' + '.'.join(version.split('.')[:2]) and
             item['license_path'] == 'python/lib/python' + '.'.join(version.split('.')[:2]) + '/LICENSE.txt',
             'Unknown portable Python layout.')
        file_pin({key: item[key] for key in ('bytes', 'sha256')}, 96 * 1024 * 1024)
    return pin
