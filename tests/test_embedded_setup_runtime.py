#!/usr/bin/env python3
"""Execute with the actual bundled Windows interpreter; never contact a Frame."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--package', type=Path, required=True)
args = parser.parse_args()
engine = args.package.resolve() / 'setup/steam_link_setup.py'
result = subprocess.run([sys.executable, '-I', '-B', str(engine), '--inspect', '--check',
                         '--bundle', str(args.package.resolve())], stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, timeout=30, check=False)
assert result.returncode == 0, result.stderr.decode(errors='replace')
report = json.loads(result.stdout)
# The CLI is an actual no-frame inspection, not an installed-bridge claim.
assert 'pc' in report and report.get('frame') is None
spec = importlib.util.spec_from_file_location('sfnp_actual_embedded_engine', engine)
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
ui = module.load_presentation()
assert ui.language('de') == 'de' and ui.language('en') == 'en'
with tempfile.TemporaryDirectory(prefix='sfnp-embedded-acl-') as temporary:
    path = Path(temporary) / 'owned-dummy.json'
    module.put(path, b'{"dummy":"not-a-credential"}\n')
    assert module.read(path, private=True) == b'{"dummy":"not-a-credential"}\n'
    # A later modification makes Windows creation/change timestamps differ.
    # Exercise the actual read after replacement and actual DACL application.
    module.put(path, b'{"dummy":"updated"}\n', b'{"dummy":"not-a-credential"}\n')
    assert module.read(path, private=True) == b'{"dummy":"updated"}\n'
    # Actual NTFS hardlinks are valid public references, never private admission.
    public = Path(temporary) / 'public-vrpathreg.exe'
    public.write_bytes(b'non-executable public reference fixture')
    alias = Path(temporary) / 'public-link.exe'
    import os
    os.link(public, alias)
    snapshot, metadata = module.read_public_reference(public)
    assert snapshot == b'non-executable public reference fixture'
    assert metadata == {'bytes': len(snapshot), 'hardlinked': True}
    try:
        module.read(public, private=True)
    except module.SetupError:
        pass
    else:
        raise AssertionError('public hardlink must not relax private admission')
    public.write_bytes(b'updated public reference fixture')
    assert module.read_public_reference(alias)[0] == b'updated public reference fixture'
    if sys.platform == 'win32':
        # Force the fallback selection without changing Windows Optional Features.
        # -V is local: no key generation, Frame, network or service is contacted.
        ssh = module.ssh_program(system='Windows', system_directory=Path(temporary) / 'absent-system',
                                 bundle_root=args.package.resolve())
        version = subprocess.run([ssh, '-V'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 timeout=15, check=False)
        assert version.returncode == 0 and b'OpenSSH_for_Windows_10.0' in version.stdout, version.stdout.decode(errors='replace')
print(json.dumps({'actual_bundled_python': sys.version, 'inspection_cli': 'passed',
                  'actual_windows_private_acl': sys.platform == 'win32', 'dummy_file_read': 'passed',
                  'actual_windows_public_hardlink': sys.platform == 'win32', 'public_hardlink_snapshot': 'passed',
                  'included_ui_import': 'passed', 'portable_windows_ssh_execution': sys.platform == 'win32',
                  'frame_contact': False, 'native_patch_install': False}))
