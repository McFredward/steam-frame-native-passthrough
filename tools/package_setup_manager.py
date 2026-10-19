#!/usr/bin/env python3
"""Package inspection and owned removal; never admit an unfinished video bridge."""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
README = b'''Steam Link Native Passthrough - setup manager

Check your PC and Frame, or remove an existing owned patch. The assistant asks
before removal and never restarts Steam, SteamVR or the headset. If SteamVR is
running, removal may need a second pass after you stop it yourself. Original
camera settings and recovery receipts are retained until restoration finishes.
Unknown or changed files are preserved.

The new full-resolution sender/receiver bridge is NOT included or installable
in this package. Installing the old Frame patch would not fix its image-quality
limitations. No pairing credentials are created by this maintenance package.
Private Python and a Windows SSH-client fallback are included for this run.
'''


def build(python_zip: Path, openssh_zip: Path, textual_wheels: Path):
    spec = importlib.util.spec_from_file_location('sfnp_inspection_packager', ROOT / 'tools/package_setup_inspection.py')
    inspection = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(inspection)
    raw = inspection.build(python_zip, openssh_zip, textual_wheels)
    with zipfile.ZipFile(io.BytesIO(raw)) as source:
        files = {name: source.read(name) for name in source.namelist() if name != 'bootstrap-package.json'}
    helper = (ROOT / 'tools/native_passthrough_setup.py').read_bytes()
    engine = inspection.load_module('sfnp_manager_engine', ROOT / 'setup/steam_link_setup.py')
    pin = lambda value: {'bytes': len(value), 'sha256': hashlib.sha256(value).hexdigest()}
    engine.require(pin(helper) == engine.MAINTENANCE_PIN, 'Removal helper differs from the reviewed engine pin.')
    files[engine.MAINTENANCE_HELPER] = helper
    encode = lambda value: (json.dumps(value, sort_keys=True, indent=2) + '\n').encode()
    files['setup-maintenance.json'] = encode({'schema': 'sfnp-setup-maintenance-v1',
                                              'helpers': {engine.MAINTENANCE_HELPER: pin(helper)}})
    files['README.txt'] = README
    manifest = {'schema': 'sfnp-bootstrap-manager-v1', 'readiness': 'maintenance',
                'windows_python': 'setup/runtime/python/python.exe',
                'files': {name: pin(value) for name, value in sorted(files.items())}}
    files['bootstrap-package.json'] = encode(manifest)
    output = io.BytesIO()
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name, value in sorted(files.items()):
            info = zipfile.ZipInfo(name, date_time=(2026, 10, 18, 0, 0, 0))
            info.external_attr = 0o100600 << 16
            archive.writestr(info, value, compress_type=zipfile.ZIP_DEFLATED, compresslevel=6)
    return output.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--python-zip', type=Path, required=True)
    parser.add_argument('--openssh-zip', type=Path, required=True)
    parser.add_argument('--textual-wheels', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    raw = build(args.python_zip, args.openssh_zip, args.textual_wheels)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, 'wb', closefd=False) as output:
            output.write(raw); output.flush(); os.fsync(fd)
    finally:
        os.close(fd)
    print(json.dumps({'artifact': str(args.output), 'bytes': len(raw),
                      'sha256': hashlib.sha256(raw).hexdigest(), 'new_bridge_installable': False}))


if __name__ == '__main__':
    main()
