#!/usr/bin/env bash
# Online entry point: no sudo, service restart, or unverified source execution.
set -euo pipefail
sfnp_python=$(command -v python3 || true)
sfnp_runtime_stage=''
trap 'if [[ -n "$sfnp_runtime_stage" ]]; then rm -rf -- "$sfnp_runtime_stage"; fi' EXIT
if [[ -z "$sfnp_python" ]] || ! "$sfnp_python" -I -B -c 'import sys; raise SystemExit(sys.version_info < (3, 9))' >/dev/null 2>&1; then
    # A private, pinned interpreter replaces a missing/old system Python. This
    # does not run a package manager, require sudo, or change the user's PATH.
    case "$(uname -m)" in
        x86_64)
            sfnp_python_archive='cpython-3.13.16%2B20261013-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz'
            sfnp_python_sha='d0080766b6b74aeb81e1949ef0d9a20e6c17f7b160fb295a69b936ccb17660be'
            sfnp_python_size=35099487 ;;
        aarch64|arm64)
            sfnp_python_archive='cpython-3.13.16%2B20261013-aarch64-unknown-linux-gnu-install_only_stripped.tar.gz'
            sfnp_python_sha='69a30ed26198178732c37b8de0b6b5191479fbacfe526954da776befe38f18e7'
            sfnp_python_size=29331623 ;;
        *) echo 'This setup does not yet support your PC architecture.' >&2; exit 2 ;;
    esac
    umask 077
    sfnp_runtime_stage=$(mktemp -d)
    sfnp_archive="$sfnp_runtime_stage/python.tar.gz"
    echo 'Preparing the setup assistant: downloading a private Python runtime...'
    curl --fail --location --show-error --proto '=https' --proto-redir '=https' --max-time 180 \
        --max-filesize "$sfnp_python_size" \
        "https://github.com/astral-sh/python-build-standalone/releases/download/20261013/$sfnp_python_archive" \
        --output "$sfnp_archive"
    [[ "$(wc -c < "$sfnp_archive")" -eq "$sfnp_python_size" ]] || { echo 'Runtime download is incomplete.' >&2; exit 2; }
    printf '%s  %s\n' "$sfnp_python_sha" "$sfnp_archive" | sha256sum --check --status
    tar -tzf "$sfnp_archive" | while IFS= read -r sfnp_entry; do
        case "$sfnp_entry" in python|python/*) ;; *) echo 'Unexpected runtime path.' >&2; exit 2 ;; esac
        case "/$sfnp_entry/" in *'/../'*|*'/./'*) echo 'Unsafe runtime path.' >&2; exit 2 ;; esac
    done
    tar --extract --gzip --file "$sfnp_archive" --directory "$sfnp_runtime_stage" --no-same-owner --no-same-permissions
    sfnp_python="$sfnp_runtime_stage/python/bin/python3.13"
fi
"$sfnp_python" -I -B - "$@" <<'SFNP_BOOTSTRAP_PY'
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
REPOSITORY_COMMIT = 'e93dc3912bceb81845876d6e72e698d5d705ec35'
REPOSITORY_ASSET = 'distribution/steam-link-native-passthrough.zip'
REPOSITORY_BYTES = 18336655
REPOSITORY_SHA256 = '8b6aeb98ff3e5c16f419e18299dafc1e1900aa3f7a1bbfa09013f03033b1b66c'
INSPECTION_TAG = 'setup-inspection-v2'
INSPECTION_ASSET = 'steam-link-native-passthrough-inspection.zip'
MANAGER_TAG = 'setup-manager-v1'
MANAGER_ASSET = 'steam-link-native-passthrough-manager.zip'
BRIDGE_TAG = 'stream-bridge-preview-v1'
BRIDGE_ASSET = 'steam-link-native-passthrough-bridge-preview.zip'
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
                ('github.com', 'raw.githubusercontent.com', 'release-assets.githubusercontent.com', 'objects.githubusercontent.com') and
                not parsed.username and not parsed.password, 'Unexpected download redirect.')
        return super().redirect_request(request, fp, code, msg, headers, url)


def download(url, limit):
    request = urllib.request.Request(url, headers={'User-Agent': 'Steam-Link-Native-Passthrough-Setup',
                                                  'Accept': 'application/vnd.github+json'})
    with urllib.request.build_opener(Redirect()).open(request, timeout=30) as response:
        result = response.read(limit + 1)
    require(len(result) <= limit, 'Download exceeds size bound.')
    return result


def repository_asset():
    # The ordinary wizard is pinned to a reviewed repository archive, not a release channel.
    return {'browser_download_url': 'https://raw.githubusercontent.com/' + REPO + '/' +
            REPOSITORY_COMMIT + '/' + REPOSITORY_ASSET,
            'size': REPOSITORY_BYTES, 'digest': 'sha256:' + REPOSITORY_SHA256}


def select_asset(release, inspection=False, maintenance=False, bridge_preview=False):
    require(sum(map(bool, (inspection, maintenance, bridge_preview))) <= 1, 'Choose one setup mode.')
    # Ordinary and explicit preview invocations install the same paired route.
    bridge_preview = not inspection and not maintenance
    preview = True
    require(type(release) is dict and release.get('draft') is False and
            release.get('prerelease') is preview,
            'No matching bridge preview release is available.' if bridge_preview else
            'No matching public inspection release is available.' if inspection else
            'No matching check/removal release is available.')
    asset_name = BRIDGE_ASSET if bridge_preview else MANAGER_ASSET if maintenance else INSPECTION_ASSET
    tag = release.get('tag_name')
    require(type(tag) is str and re.fullmatch('[A-Za-z0-9][A-Za-z0-9._-]{0,63}', tag), 'Unknown release tag.')
    require(not inspection or tag == INSPECTION_TAG, 'Unexpected inspection release tag.')
    require(not maintenance or tag == MANAGER_TAG, 'Unexpected manager release tag.')
    require(not bridge_preview or tag == BRIDGE_TAG, 'Unexpected bridge preview release tag.')
    candidates = [item for item in release.get('assets', []) if item.get('name') == asset_name]
    require(len(candidates) == 1, 'This release has no complete PC/Frame setup package. Nothing was installed.')
    asset = candidates[0]
    require(asset.get('state') == 'uploaded' and type(asset.get('size')) is int and
            0 < asset['size'] <= MAX_ZIP and
            type(asset.get('digest')) is str and re.fullmatch('sha256:[0-9a-f]{64}', asset['digest']) and
            asset.get('browser_download_url') == 'https://github.com/' + REPO + '/releases/download/' + tag + '/' + asset_name,
            'Release asset lacks its exact GitHub URL, size or SHA-256 digest.')
    return asset


def inspection_file(name):
    return (name in {'setup-inspection.json', 'setup/steam_link_setup.py', 'setup/steam_link_setup_ui.py',
                     'setup/steam_link_setup_textual.py', 'setup/steam_link_setup_vendor.py',
                     'README.md', 'README.de.md', 'README.txt', 'LICENSE',
                     'patch-steam-link.cmd', 'patch-steam-link.sh'} or
            name.startswith('setup/runtime/python/') or name.startswith('setup/runtime/openssh/') or
            name.startswith('setup/runtime/vendor/'))


def arguments(args):
    require(args in ([], ['--inspect'], ['--manage'], ['--bridge-preview']),
            'Choose only --inspect, --manage or --bridge-preview.')
    return True if args == ['--inspect'] else 'manage' if args == ['--manage'] else 'bridge-preview'


def manager_file(name):
    return inspection_file(name) or name in {'setup-maintenance.json', 'setup/frame_native_uninstall.py'}


def extract(archive, target, inspection=False, maintenance=False, bridge_preview=False):
    require(sum(map(bool, (inspection, maintenance, bridge_preview))) <= 1, 'Choose one setup mode.')
    bridge_preview = not inspection and not maintenance
    with zipfile.ZipFile(archive) as package:
        items = package.infolist()
        require(0 < len(items) <= 4096, 'Archive entry bound exceeded.')
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
        require('bootstrap-package.json' in files and files['bootstrap-package.json'].file_size <= 1024 * 1024,
                'Bootstrap manifest missing.')
        manifest = parse(package.read(files['bootstrap-package.json']))
        require(type(manifest) is dict and set(manifest) == {'schema', 'readiness', 'windows_python', 'files'} and
                manifest['schema'] == ('sfnp-bootstrap-stream-bridge-v1' if bridge_preview else 'sfnp-bootstrap-manager-v1' if maintenance else 'sfnp-bootstrap-inspection-v1') and
                manifest['readiness'] == ('maintenance' if maintenance else 'inspection-only' if inspection else 'release') and
                type(manifest['files']) is dict and set(manifest['files']) == set(files) - {'bootstrap-package.json'} and
                ({'stream-bridge-bundle.json', 'setup/stream_bridge_install.py', 'setup/steam_link_setup.py'} if bridge_preview else
                 {'setup-inspection.json', 'setup-maintenance.json', 'setup/steam_link_setup.py', 'setup/frame_native_uninstall.py'} if maintenance else
                 {'setup-inspection.json', 'setup/steam_link_setup.py'}) <= set(manifest['files']),
                'Incomplete or mismatched setup package; nothing was installed.')
        require(not inspection or all(inspection_file(name) for name in manifest['files']),
                'Inspection package contains an installation payload.')
        require(not maintenance or all(manager_file(name) for name in manifest['files']),
                'Manager package contains an installation payload.')
        require(not bridge_preview or 'steam-link-bundle.json' not in files,
                'Retired Frame-only setup bundles cannot be installed.')
        if bridge_preview:
            bundle = parse(package.read(files['stream-bridge-bundle.json']))
            require(type(bundle) is dict and bundle.get('schema') == 'sfnp-stream-bridge-bundle-v1' and
                    bundle.get('readiness') == 'release', 'Only complete release bridge bundles may start setup.')
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
            path.chmod(0o700 if name == 'setup/steam-link-askpass.sh' else 0o600)
    if inspection or maintenance:
        profile = parse((target / 'setup-inspection.json').read_bytes())
        require(type(profile) is dict and set(profile) == {'schema', 'expected_pc', 'runtime_files'} and
                profile['schema'] == 'sfnp-setup-inspection-v1', 'Invalid inspection profile.')
    else:
        bundle = parse((target / 'stream-bridge-bundle.json').read_bytes())
        require(type(bundle) is dict and
                bundle.get('schema') == 'sfnp-stream-bridge-bundle-v1' and bundle.get('readiness') == 'release',
                'Only complete release bundles may start interactive setup.')


def main():
    require(sys.version_info >= (3, 9), 'Python 3.9 or newer is required.')
    mode = arguments(sys.argv[1:])
    inspection, maintenance, bridge_preview = mode is True, mode == 'manage', mode == 'bridge-preview'
    # curl | bash owns stdin. The wizard must receive the real terminal so its
    # review, SSH fingerprint/password prompts and Apply confirmation work.
    try:
        terminal = open('/dev/tty', 'r+b', buffering=0)
    except OSError as error:
        raise ValueError('Run this command in an interactive terminal.') from error
    stage = None
    try:
        print('Steam Link Native Passthrough — downloading ' +
              ('the guided PC and Frame setup...' if bridge_preview else 'the check/removal assistant...' if maintenance else 'read-only inspection (no installation or restart)...' if inspection else 'the guided setup...'), flush=True)
        if bridge_preview:
            asset = repository_asset()
        else:
            endpoint = '/releases/tags/' + MANAGER_TAG if maintenance else '/releases/tags/' + INSPECTION_TAG
            release = parse(download('https://api.github.com/repos/' + REPO + endpoint, 1024 * 1024))
            asset = select_asset(release, inspection, maintenance)
        stage = Path(tempfile.mkdtemp(prefix='sfnp-setup-'))
        stage.chmod(0o700)
        raw = download(asset['browser_download_url'], asset['size'])
        require(len(raw) == asset['size'] and hashlib.sha256(raw).hexdigest() == asset['digest'][7:],
                'Setup download failed its SHA-256 check.')
        archive = stage / 'download.zip'; archive.write_bytes(raw)
        target = stage / 'package'; target.mkdir(mode=0o700)
        extract(archive, target, inspection, maintenance, bridge_preview)
        print('Manager verified. Removal requires your confirmation; the new bridge is not yet installable.' if maintenance else
              'Inspection package verified. This mode only reads and reports; it cannot apply changes.' if inspection else
              'Package verified. The assistant will check both devices before offering changes.', flush=True)
        argv = [sys.executable, '-I', '-B', str(target / 'setup/steam_link_setup.py'), '--bundle', str(target)]
        if inspection:
            argv.append('--inspect')
        elif maintenance:
            argv.append('--manage')
        return subprocess.call(argv, stdin=terminal)
    finally:
        terminal.close()
        if stage is not None:
            shutil.rmtree(stage)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile) as error:
        reason = str(error) if type(error) is ValueError else 'Check your connection and the complete PC/Frame setup package.'
        print('Could not start the verified setup: ' + reason + ' No restart was performed.', file=sys.stderr)
        sys.exit(2)
SFNP_BOOTSTRAP_PY
