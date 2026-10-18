#!/usr/bin/env bash
# Online entry point: no sudo, service restart, or unverified source execution.
set -euo pipefail
command -v python3 >/dev/null 2>&1 || { echo 'Python 3.9 or newer is required.' >&2; exit 2; }
python3 -I -B - <<'SFNP_BOOTSTRAP_PY'
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
import zipfile

REPO = 'McFredward/steam-link-native-passthrough'
ASSET = 'steam-link-native-passthrough-setup.zip'
MAX_ZIP = 128 * 1024 * 1024
MAX_FILE = 32 * 1024 * 1024
MAX_TOTAL = 192 * 1024 * 1024


def require(value, message):
    if not value:
        raise ValueError(message)


def parse(raw):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, 'Duplicate JSON key refused.')
            result[key] = value
        return result
    return json.loads(raw, object_pairs_hook=pairs)


def safe_name(name):
    require(type(name) is str and 0 < len(name) <= 240 and not any(c in name for c in '\\:*?"<>|'),
            'Unsafe archive path.')
    parts = name.rstrip('/').split('/')
    for part in parts:
        require(part not in ('', '.', '..') and not part.endswith((' ', '.')) and
                not any(ord(c) < 32 for c in part) and
                not re.fullmatch(r'(?i)(con|prn|aux|nul|conin\$|conout\$|com[1-9\u00b9\u00b2\u00b3]|lpt[1-9\u00b9\u00b2\u00b3])(?:\..*)?', part),
                'Unsafe archive component.')
    return '/'.join(parts)


class Redirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, url):
        parsed = urllib.parse.urlsplit(url)
        require(parsed.scheme == 'https' and parsed.hostname in
                ('github.com', 'release-assets.githubusercontent.com', 'objects.githubusercontent.com') and
                not parsed.username and not parsed.password, 'Unexpected download redirect.')
        return super().redirect_request(request, fp, code, msg, headers, url)


def download(url, limit):
    request = urllib.request.Request(url, headers={'User-Agent': 'Steam-Link-Native-Passthrough-Setup',
                                                  'Accept': 'application/vnd.github+json'})
    with urllib.request.build_opener(Redirect()).open(request, timeout=30) as response:
        result = response.read(limit + 1)
    require(len(result) <= limit, 'Download exceeds size bound.')
    return result


def select_asset(release):
    require(type(release) is dict and release.get('draft') is False and release.get('prerelease') is False,
            'No public stable setup release is available.')
    tag = release.get('tag_name')
    require(type(tag) is str and re.fullmatch('[A-Za-z0-9][A-Za-z0-9._-]{0,63}', tag), 'Unknown release tag.')
    candidates = [item for item in release.get('assets', []) if item.get('name') == ASSET]
    require(len(candidates) == 1, 'This release has no complete PC/Frame setup package. Nothing was installed.')
    asset = candidates[0]
    require(asset.get('state') == 'uploaded' and type(asset.get('size')) is int and
            0 < asset['size'] <= MAX_ZIP and
            type(asset.get('digest')) is str and re.fullmatch('sha256:[0-9a-f]{64}', asset['digest']) and
            asset.get('browser_download_url') == 'https://github.com/' + REPO + '/releases/download/' + tag + '/' + ASSET,
            'Release asset lacks its exact GitHub URL, size or SHA-256 digest.')
    return asset


def extract(archive, target):
    with zipfile.ZipFile(archive) as package:
        items = package.infolist()
        require(0 < len(items) <= 512, 'Archive entry bound exceeded.')
        files = {}; names = set(); total = 0
        for item in items:
            name = safe_name(item.filename)
            require(name.casefold() not in names and not item.flag_bits & 1, 'Duplicate or encrypted archive entry.')
            names.add(name.casefold())
            mode = stat.S_IFMT(item.external_attr >> 16)
            require(mode in (0, stat.S_IFDIR if item.is_dir() else stat.S_IFREG), 'Archive links/devices refused.')
            if item.is_dir():
                continue
            require(0 <= item.file_size <= MAX_FILE, 'Archive file exceeds bound.')
            total += item.file_size
            require(total <= MAX_TOTAL, 'Expanded archive exceeds bound.')
            files[name] = item
        require('bootstrap-package.json' in files and files['bootstrap-package.json'].file_size <= 256 * 1024,
                'Bootstrap manifest missing.')
        manifest = parse(package.read(files['bootstrap-package.json']))
        require(type(manifest) is dict and set(manifest) == {'schema', 'readiness', 'windows_python', 'files'} and
                manifest['schema'] == 'sfnp-bootstrap-package-v1' and manifest['readiness'] == 'release' and
                type(manifest['files']) is dict and set(manifest['files']) == set(files) - {'bootstrap-package.json'} and
                {'steam-link-bundle.json', 'setup/steam_link_setup.py'} <= set(manifest['files']),
                'Incomplete or prototype setup package; nothing was installed.')
        for name, pin in manifest['files'].items():
            require(type(pin) is dict and set(pin) == {'bytes', 'sha256'} and
                    type(pin['bytes']) is int and pin['bytes'] == files[name].file_size and
                    type(pin['sha256']) is str and re.fullmatch('[0-9a-f]{64}', pin['sha256']),
                    'Invalid inner file pin.')
            raw = package.read(files[name])
            require(len(raw) == pin['bytes'] and hashlib.sha256(raw).hexdigest() == pin['sha256'], 'Inner file hash mismatch.')
            path = target / name
            path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            with path.open('xb') as output:
                output.write(raw)
            path.chmod(0o600)
    bundle = parse((target / 'steam-link-bundle.json').read_bytes())
    require(bundle.get('schema') == 'sfnp-steam-link-setup-v1' and bundle.get('readiness') == 'release',
            'Only complete release bundles may start interactive setup.')


def main():
    require(sys.version_info >= (3, 9), 'Python 3.9 or newer is required.')
    # curl | bash owns stdin. The wizard must receive the real terminal so its
    # review, SSH fingerprint/password prompts and Apply confirmation work.
    try:
        terminal = open('/dev/tty', 'r+b', buffering=0)
    except OSError as error:
        raise ValueError('Run this command in an interactive terminal.') from error
    stage = None
    try:
        print('Steam Link Native Passthrough — downloading the guided setup...', flush=True)
        release = parse(download('https://api.github.com/repos/' + REPO + '/releases/latest', 1024 * 1024))
        asset = select_asset(release)
        stage = Path(tempfile.mkdtemp(prefix='sfnp-setup-'))
        stage.chmod(0o700)
        raw = download(asset['browser_download_url'], asset['size'])
        require(len(raw) == asset['size'] and hashlib.sha256(raw).hexdigest() == asset['digest'][7:],
                'Setup download failed its SHA-256 check.')
        archive = stage / 'download.zip'; archive.write_bytes(raw)
        target = stage / 'package'; target.mkdir(mode=0o700)
        extract(archive, target)
        print('Package verified. The assistant will check both devices before offering changes.', flush=True)
        return subprocess.call([sys.executable, '-I', '-B', str(target / 'setup/steam_link_setup.py'),
                                '--bundle', str(target)], stdin=terminal)
    finally:
        terminal.close()
        if stage is not None:
            shutil.rmtree(stage)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile):
        print('Could not start the verified setup. Check your connection and whether a complete PC/Frame setup release is available. No restart was performed.', file=sys.stderr)
        sys.exit(2)
SFNP_BOOTSTRAP_PY
