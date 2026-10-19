#!/usr/bin/env python3
"""Build the explicit inspection-only setup preview; contains no native patch."""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
README = b'''Steam Link Native Passthrough - setup inspection preview

This preview checks your PC and, optionally, your Frame over SSH. It does not
install or activate a patch, create pairing credentials, change Steam/SteamVR
settings, or restart either device. Only a redacted inspection report is saved
on the PC. OpenSSH may ask you to confirm the Frame's host fingerprint.

The new two-sided video/coverage bridge is not ready for installation. This is
a setup test, not a passthrough image-quality test. Existing installations remain
unchanged. Python is bundled privately for this run; it is not installed system-wide.
An official portable OpenSSH client is included for Windows PCs without one;
no SSH server, Windows feature or service is installed.

Deutsch: Dieser Probelauf prueft PC und Frame, ohne den Patch oder Einstellungen
zu aendern und ohne Neustart. Auf dem PC entsteht ein Pruefbericht. Der neue
Bilduebertragungsweg wird damit noch nicht installiert oder aktiviert.
'''


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build(python_zip: Path, openssh_zip: Path, textual_wheels: Path):
    setup = load_module('sfnp_inspection_engine', ROOT / 'setup/steam_link_setup.py')
    shared = load_module('sfnp_release_packaging', ROOT / 'tools/package_stream_link_setup.py')
    dependencies = load_module('sfnp_dependency_packaging', ROOT / 'tools/package_setup_dependencies.py')
    source_pin = setup.parse(setup.read(ROOT / 'packaging/setup-python.json', 4096))
    setup.require(set(source_pin) == {'schema', 'source_url', 'bytes', 'sha256', 'release_notes'} and
                  source_pin['schema'] == 'sfnp-portable-python-pin-v1', 'Unknown portable Python source pin.')
    runtime = setup.read(python_zip)
    setup.require(len(runtime) == source_pin['bytes'], 'Portable Python download size differs.')
    # Validate this profile with the actual runtime parser before distribution.
    setup.Inspection(ROOT / 'packaging')
    files = {
        'setup-inspection.json': setup.read(ROOT / 'packaging/setup-inspection.json', 128 * 1024),
        'setup/steam_link_setup.py': setup.read(ROOT / 'setup/steam_link_setup.py'),
        'setup/steam_link_setup_ui.py': setup.read(ROOT / 'setup/steam_link_setup_ui.py'),
        'setup/steam_link_setup_textual.py': setup.read(ROOT / 'setup/steam_link_setup_textual.py'),
        'setup/steam_link_setup_vendor.py': setup.read(ROOT / 'setup/steam_link_setup_vendor.py'),
        'README.txt': README,
        'LICENSE': setup.read(ROOT / 'LICENSE', 65536),
    }
    files.update(shared.windows_python(runtime, source_pin['sha256'], source_pin['source_url']))
    ssh_pin = setup.parse(setup.read(ROOT / 'packaging/setup-openssh.json', 16384))
    files.update(dependencies.windows_openssh(setup.read(openssh_zip), ssh_pin))
    textual = load_module('sfnp_textual_packaging', ROOT / 'tools/package_setup_textual.py')
    files.update(textual.vendor(textual_wheels, json.loads((ROOT / 'packaging/setup-textual.json').read_bytes())))
    setup.require(len(files) < 4096 and sum(map(len, files.values())) <= 192 * 1024 * 1024,
                  'Inspection package exceeds size bound.')
    manifest = {'schema': 'sfnp-bootstrap-inspection-v1', 'readiness': 'inspection-only',
                'windows_python': 'setup/runtime/python/python.exe',
                'files': {name: shared.pin(raw) for name, raw in sorted(files.items())}}
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as target:
        for name, raw in sorted(files.items()):
            setup.relative(name)
            # Fixed timestamp makes local/CI assembly reproducible, independent
            # of ZIP extraction timestamps. No executable POSIX permission needed.
            info = zipfile.ZipInfo(name, date_time=(2026, 10, 18, 0, 0, 0))
            info.external_attr = 0o100600 << 16
            target.writestr(info, raw, compress_type=zipfile.ZIP_DEFLATED, compresslevel=6)
        info = zipfile.ZipInfo('bootstrap-package.json', date_time=(2026, 10, 18, 0, 0, 0))
        info.external_attr = 0o100600 << 16
        target.writestr(info, setup.encoded(manifest), compress_type=zipfile.ZIP_DEFLATED, compresslevel=6)
    return stream.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--python-zip', type=Path, required=True)
    parser.add_argument('--openssh-zip', type=Path, required=True)
    parser.add_argument('--textual-wheels', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    raw = build(args.python_zip, args.openssh_zip, args.textual_wheels)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, 'wb', closefd=False) as output:
            output.write(raw); output.flush(); os.fsync(descriptor)
    finally:
        os.close(descriptor)
    print(json.dumps({'artifact': str(args.output), 'bytes': len(raw),
                      'sha256': hashlib.sha256(raw).hexdigest(), 'inspection_only': True}, sort_keys=True))


if __name__ == '__main__':
    # Release engineering runs on known non-secret inputs. Preserve the precise
    # validation failure here; hiding it would make cross-platform failures opaque.
    main()
