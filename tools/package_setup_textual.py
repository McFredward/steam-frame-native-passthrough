#!/usr/bin/env python3
"""Build an offline Textual runtime from exact, licensed pure-Python wheels.

Only release engineering may download these reviewed inputs. End-user packages
contain the expanded private runtime and never call pip or change system Python.
"""
from __future__ import annotations

import argparse
import email
import hashlib
import io
import json
import os
from pathlib import Path
import re
import stat
import urllib.parse
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = 'setup/runtime/vendor/'
PACKAGES = frozenset(('textual', 'rich', 'pygments', 'markdown-it-py', 'mdit-py-plugins',
                      'platformdirs', 'typing-extensions', 'linkify-it-py', 'mdurl', 'uc-micro-py'))
MAX_WHEEL = 4 * 1024 * 1024
MAX_MEMBER = 2 * 1024 * 1024
MAX_EXPANDED = 24 * 1024 * 1024
MAX_ENTRIES = 2048


class VendorError(ValueError):
    """Bounded release-engineering dependency diagnostics."""


def need(value, message):
    if not value:
        raise VendorError(message)


def normalize(value):
    return re.sub('[-_.]+', '-', value).lower()


def pin(raw):
    return {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}


def checked_pins(value):
    need(type(value) is dict and set(value) == {'schema', 'textual_version', 'packages'} and
         value['schema'] == 'sfnp-textual-wheel-pins-v1' and value['textual_version'] == '8.2.8' and
         type(value['packages']) is dict and set(value['packages']) == PACKAGES,
         'Unknown Textual dependency set.')
    for name, item in value['packages'].items():
        need(type(item) is dict and set(item) == {'version', 'filename', 'source_url', 'bytes', 'sha256', 'licenses'} and
             type(item['version']) is str and re.fullmatch('[0-9]+(?:[.][0-9]+){1,3}', item['version']) and
             type(item['filename']) is str and item['filename'] == name.replace('-', '_') + '-' + item['version'] + '-py3-none-any.whl' and
             type(item['bytes']) is int and 0 < item['bytes'] <= MAX_WHEEL and
             type(item['sha256']) is str and re.fullmatch('[0-9a-f]{64}', item['sha256']),
             'Invalid pure-Python dependency pin.')
        need(type(item['source_url']) is str and re.fullmatch(
             'https://files[.]pythonhosted[.]org/packages/[0-9a-f]{2}/[0-9a-f]{2}/[0-9a-f]{60}/' +
             re.escape(item['filename']), item['source_url']), 'Unapproved PyPI wheel source.')
        need(type(item['licenses']) is list and 1 <= len(item['licenses']) <= 8 and
             all(type(path) is str and '.dist-info/' in path and
                 Path(path).name.startswith(('LICENSE', 'COPYING')) for path in item['licenses']) and
             len(set(item['licenses'])) == len(item['licenses']), 'Full wheel license paths required.')
    need(value['packages']['textual']['version'] == value['textual_version'], 'Textual version differs.')
    return value


def path_name(name):
    need(type(name) is str and 0 < len(PREFIX + name) <= 240 and
         not any(c in name for c in '\\:*?"<>|') and not name.startswith('/'), 'Unsafe wheel member path.')
    parts = name.rstrip('/').split('/')
    for part in parts:
        need(part not in ('', '.', '..') and not part.endswith((' ', '.')) and
             not any(ord(c) < 32 for c in part) and not re.fullmatch(
                 r'(?i)(con|prn|aux|nul|conin\$|conout\$|com[1-9¹²³]|lpt[1-9¹²³])(?:\..*)?', part),
             'Unsafe wheel member component.')
    need(not any(part.endswith('.data') for part in parts) and
         not name.lower().endswith(('.pth', '.pyd', '.so', '.dll', '.exe', '.pyc')),
         'Executable wheel hooks and native extensions are not supported.')
    return '/'.join(parts)


def read_wheel(path, expected):
    before = path.lstat()
    need(stat.S_ISREG(before.st_mode) and not path.is_symlink() and
         before.st_size == expected['bytes'], 'Pinned wheel is missing, linked or has a different size.')
    with path.open('rb') as stream:
        opened = os.fstat(stream.fileno())
        need(before.st_dev == opened.st_dev and before.st_ino == opened.st_ino and
             before.st_size == opened.st_size, 'Pinned wheel changed while opening.')
        raw = stream.read(MAX_WHEEL + 1)
    need(pin(raw) == {key: expected[key] for key in ('bytes', 'sha256')}, 'Pinned wheel hash mismatch.')
    return raw


def vendor(wheel_directory, pins):
    """Return a complete private runtime, including upstream licenses/metadata."""
    checked_pins(pins)
    need(wheel_directory is not None, 'Supply --textual-wheels with the pinned offline wheel cache.')
    directory = Path(wheel_directory)
    need(directory.is_dir() and not directory.is_symlink(), 'Private wheel cache must be a physical directory.')
    result, paths, file_paths, total, count = {}, set(), set(), 0, 0
    for project, expected in sorted(pins['packages'].items()):
        raw = read_wheel(directory / expected['filename'], expected)
        try:
            with zipfile.ZipFile(io.BytesIO(raw)) as archive:
                items = archive.infolist()
                need(0 < len(items) <= MAX_ENTRIES, 'Wheel entry bound exceeded.')
                wheel_files = {}
                for item in items:
                    name = path_name(item.filename)
                    key = name.casefold()
                    need(key not in paths and not item.flag_bits & 1, 'Colliding or encrypted wheel member.')
                    paths.add(key)
                    mode = stat.S_IFMT(item.external_attr >> 16)
                    need(mode in (0, stat.S_IFDIR if item.is_dir() else stat.S_IFREG), 'Linked wheel member refused.')
                    if item.is_dir():
                        continue
                    need(0 <= item.file_size <= MAX_MEMBER, 'Wheel member exceeds size bound.')
                    total += item.file_size
                    count += 1
                    need(total <= MAX_EXPANDED and count <= MAX_ENTRIES, 'Expanded Textual runtime exceeds bound.')
                    payload = archive.read(item)
                    need(len(payload) == item.file_size, 'Truncated wheel member.')
                    wheel_files[name] = payload
                    file_paths.add(key)
                metadata = [name for name in wheel_files if name.endswith('.dist-info/METADATA')]
                wheel = [name for name in wheel_files if name.endswith('.dist-info/WHEEL')]
                need(len(metadata) == 1 and len(wheel) == 1, 'A complete pure-Python wheel is required.')
                facts = email.message_from_bytes(wheel_files[metadata[0]])
                tags = email.message_from_bytes(wheel_files[wheel[0]])
                need(len(facts.get_all('Name', [])) == 1 and len(facts.get_all('Version', [])) == 1 and
                     len(tags.get_all('Root-Is-Purelib', [])) == 1 and
                     normalize(facts.get('Name', '')) == project and facts.get('Version') == expected['version'] and
                     tags.get('Root-Is-Purelib') == 'true' and tags.get_all('Tag') == ['py3-none-any'],
                     'Wheel identity or pure-Python tag differs.')
                need(all(name in wheel_files and len(wheel_files[name]) > 0 for name in expected['licenses']),
                     'An original complete wheel license is missing.')
                result.update({PREFIX + name: payload for name, payload in wheel_files.items()})
        except (zipfile.BadZipFile, RuntimeError, OSError, NotImplementedError) as error:
            raise VendorError('Invalid pinned pure-Python wheel.') from error
    for name in file_paths:
        parts = name.split('/')
        need(not any('/'.join(parts[:index]) in file_paths for index in range(1, len(parts))),
             'Wheel file/directory collision refused.')
    provenance = {'schema': 'sfnp-textual-vendor-v1', 'packages': {
        name: {key: item[key] for key in ('version', 'filename', 'source_url', 'bytes', 'sha256')}
        for name, item in sorted(pins['packages'].items())}}
    result[PREFIX + 'SFNP-TEXTUAL-PROVENANCE.json'] = (json.dumps(provenance, sort_keys=True, indent=2) + '\n').encode()
    return result


class PyPIOnly(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, url):
        parsed = urllib.parse.urlsplit(url)
        need(parsed.scheme == 'https' and parsed.hostname == 'files.pythonhosted.org' and
             parsed.port in (None, 443) and not parsed.username and not parsed.password,
             'Unexpected wheel download redirect.')
        return super().redirect_request(request, fp, code, message, headers, url)


def fetch(directory, pins):
    """Populate a release-engineering cache; exact existing files are reused."""
    checked_pins(pins)
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    need(not directory.is_symlink(), 'Wheel cache cannot be a symlink.')
    for item in pins['packages'].values():
        target = directory / item['filename']
        if target.exists() or target.is_symlink():
            read_wheel(target, item)
            continue
        request = urllib.request.Request(item['source_url'], headers={'User-Agent': 'Steam-Link-Native-Passthrough-Release'})
        with urllib.request.build_opener(PyPIOnly()).open(request, timeout=30) as response:
            raw = response.read(MAX_WHEEL + 1)
        need(pin(raw) == {key: item[key] for key in ('bytes', 'sha256')}, 'Downloaded wheel differs from its exact pin.')
        descriptor = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, 'wb') as stream:
            stream.write(raw)
    return vendor(directory, pins)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--download', action='store_true', help='Release-engineering download of only exact pinned PyPI wheels.')
    args = parser.parse_args()
    pins = json.loads((ROOT / 'packaging/setup-textual.json').read_bytes())
    files = fetch(args.cache, pins) if args.download else vendor(args.cache, pins)
    print(json.dumps({'result': 'PASS', 'members': len(files), 'expanded_bytes': sum(map(len, files.values())),
                      'textual_version': pins['textual_version'], 'runtime_pip': False, 'device_actions': False}))


if __name__ == '__main__':
    main()
