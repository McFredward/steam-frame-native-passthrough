"""Enable the private, bootstrap-verified Textual runtime without installing it."""
from __future__ import annotations

import json
from pathlib import Path
import sys


def enable_vendor(setup_directory):
    """Prefer this exact bundle's dependencies; never use pip or write files.

    Source checkouts may use a developer environment when no runtime is bundled.
    A present but incomplete runtime is an error, rather than a silent fallback.
    The online bootstrap already verifies every vendor member against its manifest.
    """
    setup = Path(setup_directory).absolute()
    runtime, vendor = setup / 'runtime', setup / 'runtime' / 'vendor'
    for directory in (setup, runtime, vendor):
        if directory.is_symlink():
            raise ValueError('The private setup runtime cannot use linked directories.')
    if not vendor.exists():
        if (runtime / 'python').exists():
            raise ValueError('The private terminal runtime is incomplete. Download the setup again.')
        return None
    marker = vendor / 'SFNP-TEXTUAL-PROVENANCE.json'
    if not vendor.is_dir() or marker.is_symlink() or not marker.is_file() or marker.stat().st_size > 32768:
        raise ValueError('The private terminal runtime provenance is missing.')
    record = json.loads(marker.read_text(encoding='utf-8'))
    if (type(record) is not dict or record.get('schema') != 'sfnp-textual-vendor-v1'
            or type(record.get('packages')) is not dict
            or type(record['packages'].get('textual')) is not dict
            or record['packages']['textual'].get('version') != '8.2.8'):
        raise ValueError('The private terminal runtime differs from the supported version.')
    for name in ('textual', 'rich'):
        package = vendor / name
        if package.is_symlink() or not package.is_dir() or (package / '__init__.py').is_symlink() or not (package / '__init__.py').is_file():
            raise ValueError('The private terminal runtime is incomplete.')
    value = str(vendor)
    if value in sys.path:
        sys.path.remove(value)
    sys.path.insert(0, value)
    return vendor
