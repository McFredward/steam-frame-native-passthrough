#!/usr/bin/env python3
"""Assemble an admitted PC/Frame release and pinned portable Python; no downloads."""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import stat
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def require(value, reason):
    if not value:
        raise ValueError(reason)


def pin(raw):
    return {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}


def windows_python(raw, expected_sha256, source_url):
    require(type(expected_sha256) is str and re.fullmatch('[0-9a-f]{64}', expected_sha256) and
            len(raw) <= 32 * 1024 * 1024 and pin(raw)['sha256'] == expected_sha256,
            'Portable Python archive differs from its release-engineering pin.')
    require(re.fullmatch(r'https://www\.python\.org/ftp/python/(3\.(?:[9]|[1-9][0-9])\.[0-9]+)/python-\1-embed-amd64\.zip', source_url),
            'Record the exact official stable amd64 embeddable Python URL.')
    files = {}; total = 0
    with zipfile.ZipFile(io.BytesIO(raw)) as package:
        require(0 < len(package.infolist()) <= 128, 'Unexpected portable Python inventory.')
        for item in package.infolist():
            require(re.fullmatch('[A-Za-z0-9_.-]+', item.filename) and not item.is_dir() and
                    item.filename.lower() not in {name.lower() for name in files} and not item.flag_bits & 1 and
                    stat.S_IFMT(item.external_attr >> 16) in (0, stat.S_IFREG) and
                    0 < item.file_size <= 32 * 1024 * 1024, 'Unsupported Python archive entry.')
            total += item.file_size
            require(total <= 64 * 1024 * 1024, 'Portable Python exceeds package bound.')
            contents = package.read(item)
            require(len(contents) == item.file_size, 'Python archive entry size differs.')
            files[item.filename] = contents
    require({'python.exe', 'LICENSE.txt'} <= set(files) and files['python.exe'].startswith(b'MZ') and
            any(re.fullmatch(r'python[0-9]+\.zip', name) for name in files), 'Complete licensed embeddable Python required.')
    prefix = 'setup/runtime/python/'
    result = {prefix + name: value for name, value in files.items()}
    result[prefix + 'SFNP-PROVENANCE.json'] = (json.dumps({
        'schema': 'sfnp-portable-python-source-v1', 'source_url': source_url,
        'archive': pin(raw), 'note': 'Explicit release-engineering source pin; no runtime download.'}, indent=2) + '\n').encode()
    return result


def assemble(files):
    require('bootstrap-package.json' not in files and
            {'steam-link-bundle.json', 'setup/steam_link_setup.py', 'setup/runtime/python/python.exe',
             'patch-steam-link.cmd', 'patch-steam-link.sh'} <= set(files), 'Incomplete online setup inventory.')
    require(len(files) < 512 and sum(map(len, files.values())) <= 192 * 1024 * 1024, 'Setup package bound exceeded.')
    manifest = {'schema': 'sfnp-bootstrap-package-v1', 'readiness': 'release',
                'windows_python': 'setup/runtime/python/python.exe',
                'files': {name: pin(raw) for name, raw in sorted(files.items())}}
    result = io.BytesIO()
    with zipfile.ZipFile(result, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as target:
        for name, raw in sorted(files.items()):
            require(not name.startswith('/') and '\\' not in name and ':' not in name and
                    all(part not in ('', '.', '..') for part in name.split('/')) and
                    0 < len(raw) <= 32 * 1024 * 1024, 'Unsafe setup file inventory.')
            target.writestr(name, raw)
        target.writestr('bootstrap-package.json', json.dumps(manifest, sort_keys=True, indent=2) + '\n')
    raw = result.getvalue()
    require(len(raw) <= 128 * 1024 * 1024, 'Setup download bound exceeded.')
    return raw


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', required=True, type=Path)
    parser.add_argument('--python-zip', required=True, type=Path)
    parser.add_argument('--python-sha256', required=True)
    parser.add_argument('--python-source-url', required=True)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    # Import our checked-in validator, never executable code from the bundle.
    spec = importlib.util.spec_from_file_location('sfnp_release_setup', ROOT / 'setup/steam_link_setup.py')
    engine = importlib.util.module_from_spec(spec); spec.loader.exec_module(engine)
    bundle = engine.Bundle(args.bundle)
    bundle.admit('windows-x86_64')
    # Keep the real existing native-package verifier independent of the outer
    # readiness label. Its current allowlist deliberately rejects a made-up
    # bridge variant until the actual receiver release has been reviewed.
    spec = importlib.util.spec_from_file_location('sfnp_release_verify', ROOT / 'tools/prepare_native_release.py')
    verifier = importlib.util.module_from_spec(spec); spec.loader.exec_module(verifier)
    verifier.verify_package(args.bundle.resolve() / 'frame-package')
    files = dict(bundle.files)
    files['steam-link-bundle.json'] = engine.read(args.bundle / 'steam-link-bundle.json', 128 * 1024)
    for name in ('patch-steam-link.cmd', 'patch-steam-link.sh', 'steam_link_setup.py'):
        destination = name if name != 'steam_link_setup.py' else 'setup/' + name
        files[destination] = (ROOT / 'setup' / name).read_bytes()
    files.update(windows_python(engine.read(args.python_zip), args.python_sha256, args.python_source_url))
    raw = assemble(files)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, 'wb', closefd=False) as output:
            output.write(raw); output.flush(); os.fsync(descriptor)
    finally:
        os.close(descriptor)
    print(json.dumps({'artifact': str(args.output), **pin(raw), 'no_device_changes': True}, sort_keys=True))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile):
        print('Setup packaging refused: a complete admitted native bridge and exact licensed Python pin are required.', file=sys.stderr)
        raise SystemExit(2)
