#!/usr/bin/env python3
"""Guided PC/Frame inventory and exact-package setup; no implicit runtime restart."""
from __future__ import annotations

import argparse
import base64
import concurrent.futures
from contextlib import nullcontext
import ctypes
import hashlib
import hmac
import ipaddress
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import secrets
import shlex
import shutil
import socket
import stat
import subprocess
import sys
import threading

sys.dont_write_bytecode = True
SCHEMA = 'sfnp-steam-link-setup-v1'
STATE = 'sfnp-steam-link-install-v1'
PAIR = 'sfnp-pairing-v1'
ACTIVE = 'sfnp-active-pairing-v1'
INSPECTION = 'sfnp-setup-inspection-v1'
MAINTENANCE = 'sfnp-setup-maintenance-v1'
MAINTENANCE_HELPER = 'setup/frame_native_uninstall.py'
MAINTENANCE_PIN = {'bytes': 28921, 'sha256': '2a85a9469694e8077ddb31d35231551a2a1e38044adba96dc91e9a13d2caa503'}
MAX_FILE = 32 * 1024 * 1024
MAX_TOTAL = 64 * 1024 * 1024
MAX_OUTPUT = 128 * 1024
DRIVER_NAME = 'steam_link_native_passthrough'
ROOT_NAME = 'steam-link-native-passthrough'


class SetupError(ValueError):
    """Fixed, non-secret diagnostic messages only."""


def require(value, message):
    if not value:
        raise SetupError(message)


def encoded(value):
    return (json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + '\n').encode()


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def parse(raw):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, 'Duplicate JSON key refused.')
            result[key] = value
        return result
    try:
        value = json.loads(raw, object_pairs_hook=pairs,
                           parse_constant=lambda _: (_ for _ in ()).throw(SetupError('Invalid JSON number.')))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise SetupError('Invalid bounded JSON file.') from error
    require(type(value) is dict, 'JSON object required.')
    return value


def relative(value):
    require(type(value) is str and 0 < len(value) < 256 and '\\' not in value and ':' not in value,
            'Relative package path required.')
    path = PurePosixPath(value)
    require(not path.is_absolute() and all(part not in ('', '.', '..') for part in value.split('/')) and
            not any(ord(c) < 32 for c in value), 'Unsafe package path refused.')
    return path


def no_links(path):
    path = Path(path).absolute()
    for item in [path, *path.parents]:
        if not os.path.lexists(item):
            continue
        info = item.lstat()
        require(not stat.S_ISLNK(info.st_mode) and not getattr(info, 'st_file_attributes', 0) & 0x400,
                'Linked or redirected setup path preserved.')
    return path


def file_identity(info, *, cross_api=False):
    # Reading may update atime; content, ownership and replacement must not change.
    fields = ('st_dev', 'st_ino', 'st_mode', 'st_nlink', 'st_uid', 'st_gid', 'st_size', 'st_mtime_ns')
    # CPython Windows stat reports creation time as ctime, while fstat reports
    # change time (cpython#157671). Path stat also invents execute bits from an
    # .exe/.cmd extension; fstat does not. Same-API checks retain every bit/time.
    if not (cross_api and os.name == 'nt'):
        fields += ('st_ctime_ns',)
    return tuple((getattr(info, field) & ~0o111) if cross_api and os.name == 'nt' and field == 'st_mode'
                 else getattr(info, field) for field in fields)


def read(path, limit=MAX_FILE, private=False):
    path = no_links(path)
    before = path.stat()
    require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1 and 0 <= before.st_size <= limit,
            'Bounded ordinary file required.')
    if private and os.name != 'nt':
        require(before.st_uid == os.getuid() and stat.S_IMODE(before.st_mode) == 0o600,
                'Private owned file required.')
    if private and os.name == 'nt':
        verify_private_acl(path)
    descriptor = os.open(path, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
    with os.fdopen(descriptor, 'rb') as stream:
        opened = os.fstat(stream.fileno())
        require(file_identity(opened, cross_api=True) == file_identity(before, cross_api=True), 'File changed before opening.')
        raw = stream.read(limit + 1)
        require(file_identity(os.fstat(stream.fileno())) == file_identity(opened), 'File changed while reading.')
    require(len(raw) == before.st_size and file_identity(path.stat()) == file_identity(before), 'File changed while checking.')
    return raw


class ReferenceReadError(SetupError):
    """Public inventory reasons contain no raw OS errors or environment details."""
    def __init__(self, reason):
        self.reason = reason
        super().__init__(reason)


def read_public_reference(path, limit=MAX_FILE):
    """Read-only public snapshot; hardlinks grant no private/write admission."""
    def check(value, reason):
        if not value:
            raise ReferenceReadError(reason)
    try:
        try:
            path = no_links(path)
        except SetupError:
            raise ReferenceReadError('redirected_path') from None
        before = path.stat()
        check(stat.S_ISREG(before.st_mode), 'not_regular')
        check(before.st_nlink >= 1, 'invalid_metadata')
        check(0 <= before.st_size <= limit, 'size_limit')
        descriptor = os.open(path, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
        with os.fdopen(descriptor, 'rb') as stream:
            opened = os.fstat(stream.fileno())
            check(file_identity(opened, cross_api=True) == file_identity(before, cross_api=True),
                  'metadata_changed')
            payload = stream.read(limit + 1)
            check(file_identity(os.fstat(stream.fileno())) == file_identity(opened), 'metadata_changed')
        check(len(payload) == before.st_size and file_identity(path.stat()) == file_identity(before),
              'metadata_changed')
        return payload, {'bytes':len(payload),'hardlinked':before.st_nlink > 1}
    except ReferenceReadError:
        raise
    except OSError as error:
        # Only bounded categories leave the inspector, never filenames or error text.
        reason = ('sharing_violation' if getattr(error, 'winerror', None) in (32, 33) else
                  'missing' if isinstance(error, FileNotFoundError) else
                  'permission_denied' if isinstance(error, PermissionError) else 'io_error')
        raise ReferenceReadError(reason) from None


def private_acl(path):
    """Apply Windows owner/system-only DACL before secret bytes are written."""
    if os.name != 'nt':
        os.chmod(path, 0o600)
        return
    from ctypes import wintypes
    advapi = ctypes.WinDLL('advapi32', use_last_error=True)
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    convert = advapi.ConvertStringSecurityDescriptorToSecurityDescriptorW
    convert.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p]
    convert.restype = wintypes.BOOL
    apply = advapi.SetFileSecurityW
    apply.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, ctypes.c_void_p]
    apply.restype = wintypes.BOOL
    kernel.LocalFree.argtypes = [ctypes.c_void_p]
    descriptor = ctypes.c_void_p()
    require(convert('D:P(A;;FA;;;SY)(A;;FA;;;OW)', 1, ctypes.byref(descriptor), None), 'Private Windows ACL unavailable.')
    try:
        require(apply(str(path), 0x80000004, descriptor), 'Private Windows ACL could not be applied.')
    finally:
        kernel.LocalFree(descriptor)


def verify_private_acl(path):
    """Refuse a broadened Windows credential/receipt ACL; never repair silently."""
    from ctypes import wintypes
    advapi = ctypes.WinDLL('advapi32', use_last_error=True)
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    query = advapi.GetFileSecurityW
    query.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, ctypes.c_void_p,
                      wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
    query.restype = wintypes.BOOL
    needed = wintypes.DWORD()
    query(str(path), 4, None, 0, ctypes.byref(needed))
    require(0 < needed.value <= 65536, 'Private Windows ACL could not be checked.')
    descriptor = ctypes.create_string_buffer(needed.value)
    require(query(str(path), 4, descriptor, len(descriptor), ctypes.byref(needed)), 'Private Windows ACL unavailable.')
    convert = advapi.ConvertSecurityDescriptorToStringSecurityDescriptorW
    convert.argtypes = [ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
                        ctypes.POINTER(wintypes.LPWSTR), ctypes.c_void_p]
    convert.restype = wintypes.BOOL
    kernel.LocalFree.argtypes = [ctypes.c_void_p]
    value = wintypes.LPWSTR()
    require(convert(descriptor, 1, 4, ctypes.byref(value), None), 'Private Windows ACL could not be read.')
    try:
        require(value.value == 'D:P(A;;FA;;;SY)(A;;FA;;;OW)', 'Changed Windows file permissions preserved; explicit repair required.')
    finally:
        kernel.LocalFree(value)


def put(path, raw, expected=None):
    path = no_links(path)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    no_links(path.parent)
    if os.path.lexists(path):
        require(expected is not None and read(path) == expected, 'Changed or foreign setup file preserved.')
    else:
        require(expected is None, 'Expected setup file is missing.')
    temporary = path.with_name('.' + path.name + '.' + secrets.token_hex(8))
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        private_acl(temporary)
        with os.fdopen(descriptor, 'wb', closefd=False) as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(descriptor)
        # Windows denies renaming a file while this CRT descriptor is open.
        # The private file is fully flushed before the checked atomic publish.
        os.close(descriptor)
        descriptor = None
        require((read(path) if os.path.lexists(path) else None) == expected, 'Setup file changed before publication.')
        os.replace(temporary, path)
    finally:
        if descriptor is not None:
            os.close(descriptor)
        if temporary.exists():
            temporary.unlink()


def remove_owned(path, raw):
    if os.path.lexists(path):
        require(read(path) == raw, 'Changed file preserved during rollback.')
        path.unlink()


def protocol(value):
    require(type(value) is dict and set(value) == {'major', 'minor', 'wire_sha256'} and
            type(value['major']) is int and value['major'] == 1 and
            type(value['minor']) is int and value['minor'] == 0 and
            type(value['wire_sha256']) is str and re.fullmatch('[0-9a-f]{64}', value['wire_sha256']),
            'Unknown bridge protocol pin.')
    return value


def records(value):
    require(type(value) is dict and 1 <= len(value) <= 64, 'Bounded exact package inventory required.')
    for name, item in value.items():
        relative(name)
        require(type(item) is dict and set(item) == {'bytes', 'sha256'} and type(item['bytes']) is int and
                0 < item['bytes'] <= MAX_FILE and type(item['sha256']) is str and
                re.fullmatch('[0-9a-f]{64}', item['sha256']), 'Invalid package file pin.')


class Bundle:
    def __init__(self, root):
        self.root = no_links(root)
        raw = read(self.root / 'steam-link-bundle.json', 128 * 1024)
        self.pin = sha(raw)
        self.value = parse(raw)
        require(set(self.value) == {'schema', 'readiness', 'bridge_protocol', 'platforms', 'frame'} and
                self.value['schema'] == SCHEMA and self.value['readiness'] in ('prototype', 'release'),
                'Unsupported setup bundle schema.')
        self.protocol = protocol(self.value['bridge_protocol'])
        self.frame = self.value['frame']
        self.platforms = self.value['platforms']
        require(type(self.platforms) is dict and 1 <= len(self.platforms) <= 2 and
                set(self.platforms) <= {'windows-x86_64', 'linux-x86_64'}, 'Unknown PC platform.')
        self.files = {}
        file_pins = {}
        for target, item in [('frame', self.frame), *self.platforms.items()]:
            require(type(item) is dict and item.get('role') == ('receiver' if target == 'frame' else 'producer') and
                    item.get('architecture') == ('aarch64' if target == 'frame' else 'x86_64') and
                    protocol(item.get('bridge_protocol')) == self.protocol and
                    type(item.get('reviewed')) is bool and type(item.get('native_bridge')) is bool,
                    'Component role, architecture or protocol mismatch.')
            records(item.get('files'))
            for name, pin in item['files'].items():
                payload = read(self.root / relative(name), pin['bytes'])
                require(len(payload) == pin['bytes'] and sha(payload) == pin['sha256'], 'Bundle file hash mismatch.')
                require(name not in self.files or target != 'frame' and name not in self.frame['files'] and
                        file_pins[name] == pin and self.files[name] == payload, 'Overlapping component file ownership.')
                self.files[name] = payload
                file_pins[name] = pin
        require(sum(map(len, self.files.values())) <= MAX_TOTAL, 'Bundle exceeds total size bound.')
        self._frame()
        for target, item in self.platforms.items():
            require(item.get('provider_root') == 'pc-provider' and
                    item.get('provider_manifest') == 'pc-provider/driver.vrdrivermanifest' and
                    item.get('provider_registration') == 'user', 'Unknown local provider layout.')
            require(all(name.startswith('pc-provider/') for name in item['files']), 'PC payload outside owned provider.')
            manifest = parse(self.files[item['provider_manifest']])
            require(manifest.get('name') == DRIVER_NAME, 'Unexpected OpenVR driver name.')
            tool = item.get('vrpathreg')
            require(type(tool) is dict and set(tool) == {'path', 'bytes', 'sha256'} and
                    tool['path'] == ('bin/win64/vrpathreg.exe' if target == 'windows-x86_64' else
                                     'bin/linux64/vrpathreg'), 'Unknown registration tool contract.')
            records({tool['path']: {key: tool[key] for key in ('bytes', 'sha256')}})

    def _frame(self):
        require(self.frame.get('package_root') == 'frame-package' and
                self.frame.get('setup_entry') == 'frame-package/tools/native_passthrough_setup.py' and
                self.frame.get('setup_entry') in self.files and
                all(name.startswith('frame-package/') for name in self.frame['files']), 'Unknown Frame package layout.')
        pins = self.frame.get('runtime_files')
        require(type(pins) is dict and 1 <= len(pins) <= 32, 'Exact Frame runtime pins required.')
        for item in pins.values():
            require(type(item) is dict and set(item) == {'base', 'path', 'bytes', 'sha256'} and
                    item['base'] in ('steamvr', 'home', 'system'), 'Unknown runtime root.')
            relative(item['path'])
            require(item['base'] != 'system' or item['path'].startswith('usr/lib/'), 'Unknown system runtime path.')
            records({item['path']: {key: item[key] for key in ('bytes', 'sha256')}})
        package = parse(self.files['frame-package/package.json'])
        require(package.get('schema') == 'sfnp-native-package-v1', 'Unsupported underlying Frame package.')
        records(package.get('files'))
        require(set(self.frame['files']) == {'frame-package/' + name for name in package['files']} |
                {'frame-package/package.json', self.frame['setup_entry']}, 'Frame package inventory differs.')
        for name, pin in package['files'].items():
            require(self.frame['files']['frame-package/' + name] == pin, 'Nested Frame package pin mismatch.')
        profile = parse(self.files['frame-package/compatibility.json'])
        require(profile.get('complete') is True and profile.get('architecture') == 'aarch64' and
                profile.get('profile') == package.get('profile'), 'Unsupported Frame profile.')
        expected = {role: {'base': {'absolute': 'system'}.get(item['location'], item['location']),
                           'path': item['path'].lstrip('/'), 'bytes': item['bytes'], 'sha256': item['sha256']}
                    for role, item in profile['runtime_files'].items()}
        require(expected == pins, 'Frame runtime pins differ from package profile.')

    def admit(self, pc):
        raise SetupError('Retired setup bundle: use the paired stream-bridge release. No legacy transport can be installed.')

    def installed_frame_files(self):
        package = parse(self.files['frame-package/package.json'])
        return {'package/' + name: pin for name, pin in package['files'].items()} | {
            'package/package.json': self.frame['files']['frame-package/package.json'],
            'tools/native_passthrough_setup.py': self.frame['files'][self.frame['setup_entry']]}


class Inspection:
    """Pinned file inventory only: no payloads, executable paths or admission grant."""
    PC_FILES = {
        'windows-x86_64': {'drivers/vrlink/bin/win64/driver_vrlink.dll', 'bin/win64/vrpathreg.exe'},
        'linux-x86_64': {'drivers/vrlink/bin/linux64/driver_vrlink.so', 'bin/linux64/vrpathreg'}}

    def __init__(self, root):
        value = parse(read(no_links(root) / 'setup-inspection.json', 128 * 1024))
        require(set(value) == {'schema','expected_pc','runtime_files'} and value['schema'] == INSPECTION,
                'A valid setup inspection profile is required. No setup action is available.')
        self.value = value
        self.expected_pc = value['expected_pc']
        require(type(self.expected_pc) is dict and 1 <= len(self.expected_pc) <= 2 and
                set(self.expected_pc) <= set(self.PC_FILES), 'Unknown inspection platform.')
        for target, pins in self.expected_pc.items():
            records(pins)
            require(set(pins) == self.PC_FILES[target], 'Unknown inspection file paths.')
        self.runtime_files = value['runtime_files']
        require(type(self.runtime_files) is dict and 1 <= len(self.runtime_files) <= 32,
                'Bounded Frame inspection pins required.')
        for role, pin in self.runtime_files.items():
            require(type(role) is str and re.fullmatch('[a-z_]{1,32}', role) and type(pin) is dict and
                    set(pin) == {'base','path','bytes','sha256'} and pin['base'] in ('steamvr','home','system'),
                    'Unknown Frame inspection root.')
            relative(pin['path'])
            require(pin['base'] != 'system' or pin['path'].startswith('usr/lib/'), 'Unknown system inspection path.')
            records({pin['path']: {key:pin[key] for key in ('bytes','sha256')}})

    def request(self):
        return {'command':'inventory-inspect','runtime_files':self.runtime_files,'installed_files':{}}

    def pc_files(self, info):
        expected = self.expected_pc.get(info['platform'], {})
        result = {}
        for name, pin in expected.items():
            status, actual, reason = 'missing', None, 'steamvr_not_found'
            metadata = {'bytes':None,'hardlinked':None}
            if info.get('steamvr'):
                try:
                    payload, metadata = read_public_reference(Path(info['steamvr']) / relative(name))
                    actual = sha(payload)
                    status = 'match' if len(payload) == pin['bytes'] and actual == pin['sha256'] else 'different'
                    reason = 'reference_match' if status == 'match' else 'hash_or_size_mismatch'
                except ReferenceReadError as error:
                    reason = error.reason
                    status = 'missing' if reason == 'missing' else 'unreadable'
            result[name] = {'status':status,'reason':reason,'sha256':actual,**metadata}
        return result


def inspection_report(local, pc, frame, profile):
    observed = None if frame is None else {key:frame.get(key) for key in
               ('architecture','installed','legacy_installed','enabled','runtime_compatible')}
    if observed is not None and 'host' in frame:
        observed['host'] = host(frame['host'])
    return {'schema':INSPECTION,'inspection_only':True,'bridge_available':False,'automatic_restart':False,
            'pc':{'platform':pc['platform'],'steamvr':pc.get('steamvr'),
                  'installed':pc.get('installed'),'registered':pc.get('registered'),'files':profile.pc_files(pc)},
            'frame':observed}


def save_inspection(local, report):
    path = local.root / 'setup-inspection.json'
    before = read(path, 128 * 1024, private=True) if path.exists() else None
    if before is not None:
        previous = parse(before)
        require(set(previous) == set(report) and previous.get('schema') == INSPECTION and
                previous.get('inspection_only') is True and previous.get('bridge_available') is False and
                previous.get('automatic_restart') is False, 'Unknown inspection report preserved.')
    raw = encoded(report)
    if before != raw:
        put(path, raw, before)
    return path


class Maintenance:
    """Only the checked legacy uninstaller, never a runtime or install grant."""
    def __init__(self, root):
        root = no_links(root)
        value = parse(read(root / 'setup-maintenance.json', 4096))
        require(value == {'schema':MAINTENANCE,'helpers':{MAINTENANCE_HELPER:MAINTENANCE_PIN}},
                'The checked Frame removal helper is unavailable. Existing files are preserved.')
        self.helper = read(root / MAINTENANCE_HELPER, MAINTENANCE_PIN['bytes'])
        require(len(self.helper) == MAINTENANCE_PIN['bytes'] and sha(self.helper) == MAINTENANCE_PIN['sha256'],
                'The Frame removal helper differs from the reviewed source. Existing files are preserved.')

    def remove(self, remote, plan):
        require(plan.get('schema') == 'sfnp-maintenance-plan-v1' and plan.get('can_remove') is True and
                type(plan.get('receipt_sha256')) is str and re.fullmatch('[0-9a-f]{64}',plan['receipt_sha256']),
                'Frame ownership or unchanged installed files could not be confirmed; nothing was staged.')
        result = remote.call({'command':'maintenance-remove','receipt_sha256':plan['receipt_sha256'],
                              'helper':{**MAINTENANCE_PIN,'data':base64.b64encode(self.helper).decode()}})
        require(result.get('schema') == 'sfnp-maintenance-result-v1' and
                result.get('reason') in ('uninstalled','uninstall_pending','not_installed') and
                result.get('automatic_restart') is False and result.get('scope') == 'frame_legacy_patch',
                'Frame removal was not confirmed. It may be disabled or partially removed; no complete removal is claimed.')
        return {key:result.get(key) for key in ('schema','reason','scope','automatic_restart','enabled',
                                               'camera_restored','current_process_may_remain_loaded')}


def inventory_request(bundle, challenge):
    return {'command':'inventory','challenge':challenge,
            'runtime_files':bundle.frame['runtime_files'] if bundle else {},
            'installed_files':bundle.installed_frame_files() if bundle else {}}


def host(value):
    require(type(value) is str and 0 < len(value) <= 253 and
            re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9.-]*', value) and '..' not in value and not value.endswith('.'),
            'Enter a plain IP address or DNS name; SSH options, ports and commands are not accepted.')
    return value


def user(value):
    require(type(value) is str and re.fullmatch('[a-z_][a-z0-9_-]{0,31}', value), 'Invalid SSH user name.')
    return value


class Runner:
    def runtime_running(self):
        if os.name != 'nt':
            return False
        import csv
        from ctypes import wintypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        query = kernel.GetSystemDirectoryW
        query.argtypes = [wintypes.LPWSTR, wintypes.UINT]
        query.restype = wintypes.UINT
        buffer = ctypes.create_unicode_buffer(32768)
        size = query(buffer, len(buffer))
        require(0 < size < len(buffer), 'Windows system directory unavailable.')
        code, raw = self.run([str(Path(buffer.value) / 'tasklist.exe'), '/FO', 'CSV', '/NH'], timeout=10)
        require(code == 0, 'Running SteamVR state could not be checked.')
        return any(row and row[0].lower() in ('vrserver.exe','vrcompositor.exe')
                   for row in csv.reader(raw.decode('utf-8', errors='replace').splitlines()))

    def run(self, args, data=None, *, interactive=False, timeout=20):
        """Bound stdout and lifetime; SSH alone owns password/fingerprint prompts."""
        process = subprocess.Popen(args, stdin=subprocess.PIPE if data is not None else subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=None if interactive else subprocess.STDOUT)
        captured = bytearray()
        overflow = []
        def drain():
            while True:
                chunk = process.stdout.read(4096)
                if not chunk:
                    break
                if len(captured) + len(chunk) > MAX_OUTPUT:
                    overflow.append(True)
                    process.kill()
                    break
                captured.extend(chunk)
        def feed():
            try:
                if data is not None:
                    process.stdin.write(data)
                    process.stdin.close()
            except (BrokenPipeError, OSError):
                pass
            finally:
                if process.stdin is not None and not process.stdin.closed:
                    try:
                        process.stdin.close()
                    except OSError:
                        pass
        reader = threading.Thread(target=drain)
        writer = threading.Thread(target=feed)
        reader.start(); writer.start()
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait()
            raise SetupError('System command timed out; no automatic retry or restart.')
        finally:
            reader.join(); writer.join()
            process.stdout.close()
        require(not overflow, 'System command exceeded output bound.')
        return code, bytes(captured)


def ssh_program(*, system=None, system_directory=None, bundle_root=None, prefer_bundled=False):
    if (system or platform.system()) != 'Windows':
        return shutil.which('ssh') or 'ssh'
    if prefer_bundled:
        root=Path(bundle_root) if bundle_root is not None else Path(__file__).resolve().parents[1]
        candidate=no_links(root/'setup/runtime/openssh/ssh.exe')
        require(candidate.is_file(),'The setup download is missing its included SSH client. Run the setup command again.')
        return str(candidate)
    if system_directory is None:
        from ctypes import wintypes
        query = ctypes.WinDLL('kernel32', use_last_error=True).GetSystemDirectoryW
        query.argtypes = [wintypes.LPWSTR, wintypes.UINT]
        query.restype = wintypes.UINT
        buffer = ctypes.create_unicode_buffer(32768)
        size = query(buffer, len(buffer))
        require(0 < size < len(buffer), 'Windows system directory unavailable.')
        system_directory = Path(buffer.value)
    candidate = Path(system_directory) / 'OpenSSH/ssh.exe'
    if candidate.is_file():
        return str(no_links(candidate))
    root = Path(bundle_root) if bundle_root is not None else Path(__file__).resolve().parents[1]
    candidate = no_links(root / 'setup/runtime/openssh/ssh.exe')
    require(candidate.is_file(), 'The setup download is missing its included SSH client. Run the setup command again.')
    return str(candidate)


def ssh_args(address, login, *, interactive=False, inline_auth=False):
    return [ssh_program(prefer_bundled=True) if inline_auth else ssh_program(), '-o', 'ConnectTimeout=4', '-o', 'ConnectionAttempts=1', '-o', 'ServerAliveInterval=5',
            '-o', 'ServerAliveCountMax=2', '-o', 'StrictHostKeyChecking=' + ('ask' if interactive else 'yes'),
            '-o', 'UpdateHostKeys=no', '-o', 'ControlMaster=no', '-o', 'ControlPath=none',
            '-o', 'ForwardAgent=no', '-o', 'ClearAllForwardings=yes',
            '-o', 'BatchMode=' + ('no' if interactive else 'yes'), user(login) + '@' + host(address)]


def discover(cidr, connector=None):
    network = ipaddress.ip_network(cidr, strict=True)
    allowed = [ipaddress.ip_network(value) for value in ('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16')]
    require(network.version == 4 and network.prefixlen >= 24 and
            any(network.subnet_of(item) for item in allowed), 'Discovery accepts one private IPv4 /24 or smaller only.')
    def attempt(address):
        try:
            if connector:
                return str(address) if connector(str(address), 22, .25) else None
            with socket.create_connection((str(address), 22), timeout=.25):
                return str(address)
        except OSError:
            return None
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        return [value for value in pool.map(attempt, network.hosts()) if value]


# This fixed read/apply agent has no shell commands supplied by a manifest. It
# never stops/restarts services. Only the separately hash-verified native setup
# owns Frame activation. stdin carries secrets; stdout never includes them.
REMOTE_AGENT = r'''
import base64,hashlib,hmac,importlib.util,json,os,pathlib,platform,re,secrets,stat,sys
P=pathlib.Path
def need(v,m):
 if not v: raise ValueError(m)
def digest(b):return hashlib.sha256(b).hexdigest()
def identity(s):return tuple(getattr(s,k) for k in ('st_dev','st_ino','st_mode','st_nlink','st_uid','st_gid','st_size','st_mtime_ns','st_ctime_ns'))
def raw(p,limit=33554432):
 p=P(p);s=p.lstat();need(stat.S_ISREG(s.st_mode) and s.st_nlink==1 and 0<=s.st_size<=limit,'ordinary file required')
 fd=os.open(p,os.O_RDONLY|os.O_NOFOLLOW)
 with os.fdopen(fd,'rb') as f:need(identity(os.fstat(f.fileno()))==identity(s),'changed open');b=f.read(limit+1)
 need(len(b)==s.st_size and identity(p.stat())==identity(s),'changed file');return b
def load(p):return json.loads(raw(p,131072))
def safe(p,home,create=False):
 need(p.is_relative_to(home),'owned path required')
 for q in reversed((p,*p.parents)):
  if q==home or home in q.parents:
   if os.path.lexists(q):
    s=q.lstat();need(not stat.S_ISLNK(s.st_mode) and s.st_uid==os.getuid() and not(s.st_mode&0o022),'unsafe owned path')
   elif create:q.mkdir(mode=0o700)
def put(p,b,old=None):
 safe(p.parent,HOME,True)
 need((raw(p) if os.path.lexists(p) else None)==old,'foreign file preserved')
 t=p.with_name('.'+p.name+'.'+secrets.token_hex(8));fd=os.open(t,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
 try:
  with os.fdopen(fd,'wb',closefd=False) as f:f.write(b);f.flush();os.fsync(fd)
  need((raw(p) if os.path.lexists(p) else None)==old,'changed file');os.replace(t,p)
 finally:
  os.close(fd)
  if t.exists():t.unlink()
def data(v):return (json.dumps(v,sort_keys=True)+'\n').encode()
def rel(n):
 need(type(n)is str and 0<len(n)<256 and '\\' not in n and ':' not in n and not n.startswith('/') and all(x not in ('','.','..')for x in n.split('/')),'bad relative path');return n
def runtime(pins):
 for spec in pins.values():
  n=rel(spec['path']);base={'steamvr':P('/opt/steamvr'),'home':HOME,'system':P('/')}[spec['base']]
  b=raw(base/n);need(len(b)==spec['bytes'] and digest(b)==spec['sha256'],'runtime mismatch')
def owned_receipt():
 p=HOME/'.config/SteamFrameNativePassthrough/install.json'
 if not p.exists():return None
 safe(p.parent,HOME);s=p.lstat();need(s.st_uid==os.getuid() and stat.S_IMODE(s.st_mode)==0o600,'private receipt required')
 v=load(p);data_root=HOME/'.local/share/SteamFrameNativePassthrough'
 target=P('/run/user')/str(os.getuid())/'steam-frame-native-passthrough/receiver.json'
 need(set(v)=={'schema','home','data','manifest_link','package_sha256','files'} and v['schema']=='sfnp-user-install-v1' and v['home']==str(HOME) and v['data']==str(data_root) and v['manifest_link']==str(target),'unknown owned receipt')
 need(re.fullmatch('[0-9a-f]{64}',v['package_sha256']) and type(v['files'])is dict and 1<=len(v['files'])<=32,'receipt bound')
 for name,pin in v['files'].items():
  q=P(name);need(q.is_absolute() and '..' not in q.parts and (q.is_relative_to(data_root) or q==HOME/'.config/systemd/user/steamvr.service.d/80-steam-frame-native-passthrough.conf') and re.fullmatch('[0-9a-f]{64}',pin),'receipt ownership')
 return v
def installed_files_current(request,receipt):
 if receipt is None:return False
 pins=request.get('installed_files',{});need(type(pins)is dict and len(pins)<=64,'installed artifact bound')
 if not pins:return False
 base=HOME/'.local/share/SteamFrameNativePassthrough';total=0
 try:
  safe(base,HOME)
  for name,pin in pins.items():
   n=rel(name);need(n.startswith('package/') or n=='tools/native_passthrough_setup.py','installed artifact path')
   need(set(pin)=={'bytes','sha256'} and type(pin['bytes'])is int and 0<pin['bytes']<=33554432 and re.fullmatch('[0-9a-f]{64}',pin['sha256']),'installed artifact pin')
   total+=pin['bytes'];need(total<=67108864,'installed artifact total bound')
   path=base/n;safe(path.parent,HOME);b=raw(path,pin['bytes'])
   need(receipt['files'].get(str(path))==pin['sha256'] and len(b)==pin['bytes'] and digest(b)==pin['sha256'],'installed artifact changed')
 except (OSError,ValueError,KeyError,TypeError):return False
 return True
def inventory(request):
 pairs={};pairdir=HOME/'.config/steam-link-native-passthrough/pairings';active=None
 if request['command']!='inventory-inspect' and pairdir.exists():
  safe(pairdir,HOME)
  paths=list(pairdir.iterdir());need(len(paths)<=64,'pairing inventory bound')
  for path in paths:
   if re.fullmatch('[0-9a-f]{32}[.]json',path.name):
    s=path.lstat();need(stat.S_IMODE(s.st_mode)==0o600,'private key mode')
    v=load(path);need(v.get('schema')=='sfnp-pairing-v1' and v.get('role')=='receiver' and v.get('pair_id')==path.stem and v.get('protocol_major')==1 and re.fullmatch('[0-9a-f]{64}',v.get('key_hex','')),'invalid pairing')
    pairs[path.stem]=hmac.new(bytes.fromhex(v['key_hex']),bytes.fromhex(request['challenge']),hashlib.sha256).hexdigest()
  if (pairdir/'active.json').exists():
   v=load(pairdir/'active.json');need(set(v)=={'schema','pair_id'} and v['schema']=='sfnp-active-pairing-v1' and v['pair_id'] in pairs,'unknown active pair');active=v['pair_id']
 ok=False
 try:runtime(request.get('runtime_files',{}));ok=bool(request.get('runtime_files'))
 except (OSError,ValueError,KeyError):pass
 receipt=HOME/'.config/SteamFrameNativePassthrough/install.json'
 control=HOME/'.config/SteamFrameNativePassthrough/control.json'
 component=HOME/'.local/share/SteamFrameNativePassthrough/package/bridge-component.json'
 try:bridge=load(component) if component.exists() else None
 except (OSError,ValueError,TypeError):bridge=None
 legacy=receipt.exists() and not component.exists()
 try:checked_receipt=owned_receipt()
 except (OSError,ValueError,KeyError,TypeError):checked_receipt=None
 package_sha=checked_receipt.get('package_sha256') if checked_receipt else None
 current=installed_files_current(request,checked_receipt)
 return {'schema':'sfnp-wizard-frame-v1','architecture':platform.machine(),'home':str(HOME),'installed':receipt.exists(),'owned_receipt_valid':checked_receipt is not None,'legacy_installed':legacy,'bridge_component':bridge,'package_sha256':package_sha,'files_current':current,'enabled':load(control).get('enabled') if control.exists() else None,'runtime_compatible':ok,'active_pair':active,'pair_proofs':pairs,'automatic_restart':False}
def module(path,name):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);sys.modules[name]=m;spec.loader.exec_module(m);return m
def apply(request):
 need(False,'retired setup RPC: use the paired stream-bridge release')
def maintenance_plan(request):
 receipt=owned_receipt()
 if receipt is None:return {'schema':'sfnp-maintenance-plan-v1','installed':False,'can_remove':False,'receipt_sha256':None}
 component=HOME/'.local/share/SteamFrameNativePassthrough/package/bridge-component.json'
 config=HOME/'.config/SteamFrameNativePassthrough'
 for filename,schema in (('control.json','sfnp-user-control-v1'),('status.json','sfnp-user-status-v1')):
  path=config/filename
  if os.path.lexists(path):
   value=load(path);need(value.get('schema')==schema,'unknown mutable setup file preserved')
   if filename=='control.json':need(set(value)=={'schema','enabled'} and type(value['enabled'])is bool,'unknown control preserved')
   else:need(type(value.get('reason'))is str and len(value['reason'])<=128 and value.get('automatic_restart')is False,'unknown status preserved')
 total=0;missing=0
 for name,pin in receipt['files'].items():
  path=P(name);safe(path.parent,HOME)
  if not os.path.lexists(path):missing+=1;continue
  b=raw(path,16777216);total+=len(b);need(total<=134217728 and digest(b)==pin,'changed owned artifact preserved')
 receipt_path=HOME/'.config/SteamFrameNativePassthrough/install.json'
 return {'schema':'sfnp-maintenance-plan-v1','installed':True,'can_remove':not component.exists(),
         'receipt_sha256':digest(raw(receipt_path)),'missing_files':missing,'scope':'frame_legacy_patch'}
def maintenance_remove(request):
 plan=maintenance_plan(request)
 need(plan.get('can_remove') is True and plan['receipt_sha256']==request['receipt_sha256'],'changed removal plan')
 item=request['helper'];need(set(item)=={'bytes','sha256','data'} and item['bytes']==28921 and item['sha256']=='2a85a9469694e8077ddb31d35231551a2a1e38044adba96dc91e9a13d2caa503','unknown removal helper')
 payload=base64.b64decode(item['data'],validate=True)
 need(len(payload)==item['bytes'] and digest(payload)==item['sha256'],'removal helper pin')
 base=HOME/'.local/share/steam-link-native-passthrough/maintenance';safe(base,HOME,True)
 stage=base/secrets.token_hex(16);stage.mkdir(mode=0o700);path=stage/'frame_native_uninstall.py'
 try:
  put(path,payload);setup=module(path,'sfnp_checked_legacy_removal')
  def offline_only(*args):raise setup.SafetyError('Stop SteamVR manually before restoring camera settings; no native runtime was admitted.')
  unit=setup.Setup(HOME,verifier=offline_only,runtime_factory=lambda:{})
  with unit.locked():
   current=maintenance_plan(request)
   need(current.get('can_remove') is True and current['receipt_sha256']==request['receipt_sha256'],'changed locked removal plan')
   result=unit.uninstall()
  reason=result.get('reason');need(reason in ('uninstalled','uninstall_pending','not_installed'),'unknown removal result')
  return {'schema':'sfnp-maintenance-result-v1','reason':reason,'scope':'frame_legacy_patch','enabled':False,
          'camera_restored':result.get('camera',{}).get('restored',reason in ('uninstalled','not_installed')),
          'current_process_may_remain_loaded':reason=='uninstall_pending','automatic_restart':False}
 finally:
  if path.exists() and raw(path)==payload:path.unlink()
  try:stage.rmdir()
  except OSError:pass
def pairing_finish(request):
 d=HOME/'.config/steam-link-native-passthrough/pairings';safe(d,HOME)
 journal=d/'transaction.json'
 if not journal.exists():return {'reason':'no-pending-pairing','automatic_restart':False}
 v=load(journal);need(v['token']==request['transaction'],'unknown pairing transaction')
 active=d/'active.json';expected=data({'schema':'sfnp-active-pairing-v1','pair_id':v['pair_id']})
 if request['command']=='rollback-pair':
  previous=base64.b64decode(v['previous']) if v['previous'] else None
  current=raw(active) if active.exists() else None
  need(current in (expected,previous),'changed active pairing preserved')
  if current!=previous:
   if previous is None:active.unlink()
   else:put(active,previous,expected)
  key=d/(v['pair_id']+'.json')
  if v['created'] and key.exists():need(digest(raw(key))==v['key_sha256'],'changed key preserved');key.unlink()
 journal.unlink();return {'reason':'pairing-committed' if request['command']=='commit-pair' else 'pairing-rolled-back','automatic_restart':False}
def run(request,home=None):
 global HOME
 HOME=home if home is not None else P.home()
 if request['command'] in ('inventory','inventory-inspect'):return inventory(request)
 if request['command']=='apply':return apply(request)
 if request['command']=='maintenance-plan':return maintenance_plan(request)
 if request['command']=='maintenance-remove':return maintenance_remove(request)
 if request['command'] in ('commit-pair','rollback-pair'):return pairing_finish(request)
 raise ValueError('unknown operation')
if __name__=='__main__':
 try:
  b=sys.stdin.buffer.read(94371841);need(len(b)<=94371840,'input bound');r=run(json.loads(b));print(json.dumps(r,sort_keys=True))
 except Exception:print(json.dumps({'error':'Frame setup refused; existing files preserved; inspect the original setup status locally.'}));sys.exit(2)
'''


class Remote:
    def __init__(self, address, login='steamos', runner=None, interactive=False):
        self.address, self.login = host(address), user(login)
        self.runner, self.interactive = runner or Runner(), interactive

    def call(self, request):
        args = ssh_args(self.address, self.login, interactive=self.interactive)
        args += ['python3 -B -c ' + shlex.quote(REMOTE_AGENT)]
        code, raw = self.runner.run(args, encoded(request), interactive=self.interactive,
                                    timeout=20 if request['command'] in ('inventory','inventory-inspect') else 120)
        removing = request.get('action') == 'uninstall' or request['command']=='maintenance-remove'
        recovery = 'Frame removal refused. Use the checked Frame setup emergency-off, stop SteamVR manually, then retry Uninstall; no complete removal is claimed.'
        require(code == 0, recovery if removing else 'Frame operation refused or SSH unavailable. No automatic restart was attempted.')
        result = parse(raw)
        require(not result.get('error'), recovery if removing else 'Frame operation refused; use its retained setup status for repair.')
        return result


def data_root(home=None, system=None):
    home = Path.home() if home is None else Path(home)
    system = platform.system() if system is None else system
    if system == 'Windows':
        base = Path(os.environ.get('LOCALAPPDATA') or home / 'AppData/Local')
    else:
        base = Path(os.environ.get('XDG_DATA_HOME') or home / '.local/share')
    require(base.is_absolute(), 'Absolute user data directory required.')
    return no_links(base / ROOT_NAME)


def registry_path(home=None, system=None):
    home = Path.home() if home is None else Path(home)
    if (system or platform.system()) == 'Windows':
        return Path(os.environ.get('LOCALAPPDATA') or home / 'AppData/Local') / 'openvr/openvrpaths.vrpath'
    return Path(os.environ.get('XDG_CONFIG_HOME') or home / '.config') / 'openvr/openvrpaths.vrpath'


class Local:
    def __init__(self, home=None, system=None, runner=None, steamvr=None):
        self.system = system or platform.system()
        self.home = Path.home() if home is None else Path(home)
        self.root = data_root(self.home, self.system)
        self.registry = registry_path(self.home, self.system)
        self.runner = runner or Runner()
        self.explicit_steamvr = Path(steamvr) if steamvr else None

    def state(self):
        path = self.root / 'install.json'
        if not os.path.lexists(path):
            return None
        value = parse(read(path, 128 * 1024, private=True))
        require(set(value) == {'schema', 'driver', 'manifest_sha256', 'files', 'vrpathreg', 'bridge_protocol'} and
                value['schema'] == STATE and value['driver'] == str(self.root / 'pc-provider'), 'Unknown installation receipt preserved.')
        records(value['files']); protocol(value['bridge_protocol'])
        require(all(name.startswith('pc-provider/') for name in value['files']), 'Foreign receipt file ownership.')
        return value

    def inventory(self, inspection=False):
        paths = parse(read(self.registry, 128 * 1024)) if self.registry.exists() else {}
        runtimes = paths.get('runtime', [])
        require(type(runtimes) is list and len(runtimes) <= 8, 'OpenVR runtime inventory exceeded bound.')
        roots = [self.explicit_steamvr] if self.explicit_steamvr else [Path(p) for p in runtimes if type(p) is str]
        state = self.state()
        return {'platform': self.system.lower() + '-x86_64' if platform.machine().lower() in ('amd64', 'x86_64') else 'unsupported',
                'steamvr': str(roots[0]) if roots else None, 'registry': str(self.registry),
                'registered': str(self.root / 'pc-provider') in paths.get('external_drivers', []),
                'installed': state is not None, 'state': state, 'active_pair': None if inspection else self.active_pair(),
                'automatic_restart': False}

    def active_pair(self):
        path = self.root / 'pairings/active.json'
        if not path.exists():
            return None
        value = parse(read(path, 4096, private=True))
        require(set(value) == {'schema', 'pair_id'} and value['schema'] == ACTIVE and
                re.fullmatch('[0-9a-f]{32}', value['pair_id']), 'Unknown active pairing preserved.')
        return value['pair_id']

    def pairing(self, pair_id):
        if pair_id is None:
            return None
        require(re.fullmatch('[0-9a-f]{32}', pair_id), 'Invalid pairing ID.')
        value = parse(read(self.root / 'pairings' / (pair_id + '.json'), 4096, private=True))
        require(set(value) == {'schema','pair_id','key_hex','role','protocol_major'} and value['schema'] == PAIR and
                value['pair_id'] == pair_id and value['role'] == 'producer' and value['protocol_major'] == 1 and
                re.fullmatch('[0-9a-f]{64}', value['key_hex']), 'Unknown local credential preserved.')
        return value

    def verify_tool(self, info, spec):
        require(info['steamvr'] is not None, 'SteamVR not found. Install SteamVR or specify --steamvr.')
        tool = no_links(Path(info['steamvr']) / relative(spec['path']))
        raw = read(tool, spec['bytes'])
        require(len(raw) == spec['bytes'] and sha(raw) == spec['sha256'], 'SteamVR registration tool differs from reviewed bundle.')
        return tool

    def register(self, tool, command):
        code, _ = self.runner.run([str(tool), command, str(self.root / 'pc-provider')], timeout=10)
        require(code == 0, 'OpenVR registration failed. Owned payload/receipt retained for repair.')

    def install(self, bundle, info):
        raise SetupError('Retired setup bundle: use the paired stream-bridge release.')

    def uninstall(self, info):
        state = self.state()
        if state is None:
            return
        tool = self.verify_tool(info, state['vrpathreg'])
        for name, pin in state['files'].items():
            raw = read(self.root / relative(name))
            require(sha(raw) == pin['sha256'], 'Changed installed payload preserved; repair manually before removal.')
        self.register(tool, 'removedriver')
        require(not self.inventory()['registered'], 'Owned driver is still registered.')
        for name, pin in state['files'].items():
            path = self.root / relative(name); remove_owned(path, read(path))
        remove_owned(self.root / 'install.json', encoded(state))


def pair_matches(local, remote_info, challenge):
    pair_id = local.active_pair()
    if pair_id is None or remote_info.get('active_pair') != pair_id:
        return False
    value = local.pairing(pair_id)
    proof = hmac.new(bytes.fromhex(value['key_hex']), bytes.fromhex(challenge), hashlib.sha256).hexdigest()
    return hmac.compare_digest(proof, remote_info.get('pair_proofs', {}).get(pair_id, ''))


def public_report(pc, frame, bundle):
    # Never serialize pairing proofs, credentials, arbitrary SSH output or commands.
    return {'pc': {key: pc.get(key) for key in ('platform','steamvr','installed','registered','active_pair')},
            'frame': None if frame is None else {key: frame.get(key) for key in
                ('architecture','installed','legacy_installed','enabled','runtime_compatible','files_current','active_pair')},
            'bundle': None if bundle is None else {'readiness': bundle.value['readiness'], 'sha256': bundle.pin},
            'automatic_restart': False}


def remember_frame(local, address, login):
    path = local.root / 'frame-address.json'
    before = read(path, 4096, private=True) if path.exists() else None
    if before is not None:
        prior = parse(before)
        require(set(prior) == {'schema','host','user'} and prior['schema'] == 'sfnp-frame-address-v1',
                'Unknown cached-address file preserved.')
        host(prior['host']); user(prior['user'])
    raw = encoded({'schema':'sfnp-frame-address-v1','host':host(address),'user':user(login)})
    if before != raw:
        put(path, raw, before)


def write_report(local, action, inventory, result=None, failed=False):
    path = local.root / 'setup-last.log'
    before = read(path, 128 * 1024, private=True) if path.exists() else None
    if before is not None:
        prior = parse(before)
        fields = {'schema','action','inventory','result','failed','automatic_restart'}
        require(prior.get('schema') == 'sfnp-wizard-log-v1' and set(prior) in (fields, fields | {'recovery'}),
                'Unknown setup-log file preserved.')
    value = {'schema':'sfnp-wizard-log-v1','action':action,'inventory':inventory,
             'result':result,'failed':failed,'automatic_restart':False}
    if failed:
        value['recovery'] = ('Removal may be partial. Use the checked Frame setup emergency-off, stop SteamVR manually and retry with a matching bundle; no complete removal is claimed.'
                             if action == 'uninstall' else 'Pairing rollback was attempted. A component may already be installed; run Check, then Repair. No automatic restart occurred.')
    raw = encoded(value)
    if before != raw:
        put(path, raw, before)
    return path


def plan(pc, frame, bundle):
    if bundle is None:
        return 'A complete reviewed release bundle is missing. Inventory is available; installation and pairing are blocked.'
    if bundle.value['readiness'] != 'release':
        return 'This is a prototype bundle. Checks and dry-run only; ordinary Steam Link is retained.'
    if pc['platform'] != 'windows-x86_64':
        return 'The native Linux sender is not reviewed. Nothing will be installed on this PC.'
    if frame is None:
        return 'Connect the Frame by SSH before choosing Install, Update or Repair.'
    if not frame.get('runtime_compatible'):
        return 'The Frame runtime is unsupported by this bundle. Installation is blocked; Uninstall can still remove a valid owned installation.'
    if not pc['installed'] or not frame['installed']:
        return 'Install the missing PC/Frame components and pair them securely.'
    if pc['state']['manifest_sha256'] != bundle.pin:
        return 'Update both checked components; preserve previous pairing credentials for rollback.'
    if not frame.get('files_current'):
        return 'Repair the installed Frame files: an expected artifact is missing or changed. Unchanged metadata alone is not a current installation.'
    if not pc['registered'] or pc['active_pair'] != frame['active_pair'] or not pc['active_pair']:
        return 'Repair registration or explicitly replace the mismatched active pairing.'
    return 'The checked components are current. No update is needed.'


def apply(action, bundle, local, remote, pc, frame, challenge):
    raise SetupError('Retired setup bundle: use the paired stream-bridge release. For old installations use checked maintenance removal.')


def load_presentation():
    # The online bootstrap verifies this exact sibling in the package manifest.
    # Explicit loading also works in the isolated portable Windows interpreter.
    path = no_links(Path(__file__).with_name('steam_link_setup_ui.py'))
    spec = importlib.util.spec_from_file_location('_sfnp_setup_ui', path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def remembered_frame_hosts(local):
    candidates = []
    for filename, schema in (('frame-address.json','sfnp-frame-address-v1'),
                             ('setup-inspection.json',INSPECTION)):
        try:
            value = parse(read(local.root / filename, 128 * 1024, private=True))
            if value.get('schema') != schema:
                continue
            candidate = (value.get('frame') or {}).get('host') if schema == INSPECTION else value.get('host')
            if candidate:
                candidates.append(host(candidate))
        except (OSError, SetupError, TypeError, AttributeError):
            # Unknown/corrupt address hints are never changed, nor a reason to
            # prevent a new read-only discovery attempt.
            continue
    return tuple(dict.fromkeys(candidates))


def load_textual():
    """Load the local-only UI after the bootstrap verified its private runtime."""
    name = '_sfnp_setup_textual'
    if name in sys.modules:
        return sys.modules[name]
    path = no_links(Path(__file__).with_name('steam_link_setup_vendor.py'))
    spec = importlib.util.spec_from_file_location('_sfnp_setup_vendor', path)
    vendor = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(vendor)
    vendor.enable_vendor(path.parent)
    path = no_links(path.with_name('steam_link_setup_textual.py'))
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    try:
        spec.loader.exec_module(module)
    except BaseException:
        sys.modules.pop(name, None)
        raise
    return module


def main(argv=None, *, local=None, remote_factory=Remote, input_fn=input, discover_frame=None,
         _textual_session=False):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manage', action='store_true', help='Check setup or explicitly remove an owned existing Frame patch; no new bridge installation.')
    parser.add_argument('--inspect', action='store_true', help='Check PC/Frame setup only; no installation, pairing or restart.')
    parser.add_argument('--check', action='store_true', help='Read-only JSON inventory; no credential or log writes.')
    parser.add_argument('--dry-run', action='store_true', help='Display the plan without staging, pairing or installing.')
    parser.add_argument('--action', choices=('install','update','repair','uninstall'))
    parser.add_argument('--bundle', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--frame', help='Frame IP or DNS name. Credentials are handled only by OpenSSH.')
    parser.add_argument('--user', default='steamos')
    parser.add_argument('--steamvr', type=Path)
    parser.add_argument('--lang', choices=('en','de'), help='Override the automatically selected setup language.')
    args = parser.parse_args(argv)
    if (not _textual_session and not args.check and not args.dry_run
            and input_fn is input and sys.stdin.isatty() and sys.stdout.isatty()):
        try:
            terminal = load_textual()
        except (ImportError, OSError, ValueError) as error:
            print('The terminal interface could not load. Download the complete setup again. '
                  'No installation was started. (' + type(error).__name__ + ')', file=sys.stderr)
            return 2
        return terminal.run_setup(
            lambda: main(argv, local=local, remote_factory=remote_factory, input_fn=input_fn,
                         discover_frame=discover_frame, _textual_session=True), lang=args.lang)
    local = local or Local(steamvr=args.steamvr)
    local.apply_started = False
    ui = None
    if not args.inspect and (args.bundle / 'stream-bridge-bundle.json').is_file():
        # The bootstrap verifies this sibling alongside the entry point. A
        # prototype bundle may check, but never gains installation permission.
        try:
            helper = no_links(Path(__file__).with_name('stream_bridge_install.py'))
            spec = importlib.util.spec_from_file_location('sfnp_full_resolution_setup', helper)
            bridge = importlib.util.module_from_spec(spec)
            sys.modules[spec.name] = bridge
            spec.loader.exec_module(bridge)
            owner = sys.modules.get(__name__)
            if owner is None:
                owner = type(sys)(__name__)
                owner.__dict__.update(globals())
            return bridge.run(args, owner, local, input_fn=input_fn, discover_frame=discover_frame,
                              remote_factory=None if remote_factory is Remote else remote_factory)
        except KeyboardInterrupt:
            print('Setup stopped. Check the displayed setup log and retained recovery status.', file=sys.stderr)
            return 130
        except (OSError, ValueError, KeyError, TypeError) as error:
            detail = str(error)[:500] if isinstance(error, SetupError) else 'Check the package and retained recovery records.'
            print('Setup could not finish: ' + detail + ' Check the displayed setup log and recovery status.', file=sys.stderr)
            return 2
    try:
        require(not (args.inspect and args.action), '--inspect cannot be combined with an installation action.')
        require(not (args.inspect and args.manage), 'Choose either --inspect or --manage.')
        require(not (args.manage and args.action not in (None,'uninstall')), 'Management currently offers Check or Uninstall only; the new bridge is unavailable.')
        interactive = not args.check and sys.stdin.isatty()
        presentation = load_presentation() if interactive else None
        if presentation is not None:
            ui = presentation.Presenter(input_fn=input_fn, lang=args.lang, input_enabled=True)
            ui.banner(inspection=args.inspect)
        with ui.progress('pc_check') if ui else nullcontext():
            pc = local.inventory(inspection=True) if args.inspect or args.manage else local.inventory()
        inspection = Inspection(args.bundle) if args.inspect or args.manage else None
        bundle = None
        if not (args.inspect or args.manage):
            try:
                bundle = Bundle(args.bundle)
            except (OSError, SetupError, KeyError, TypeError):
                pass
        challenge = secrets.token_hex(32)
        frame = remote = None
        address = args.frame
        request = inspection.request() if inspection else inventory_request(bundle, challenge)
        if args.inspect and not args.check and ui is None:
            print('Setup inspection only; the new bridge is unavailable. Nothing will be installed, paired or restarted.')
            print('Only a PC inspection report may be saved. OpenSSH may ask to trust the Frame fingerprint or enter its password.')
        if interactive and not address:
            def validate(candidate, *, interactive=False):
                try:
                    probe = remote_factory(host(candidate), user(args.user), interactive=interactive)
                    observed = probe.call(request)
                    # A name, SSH port or ARM CPU alone is not Frame identity.
                    if observed.get('architecture') in ('aarch64','arm64') and any(
                            observed.get(key) is True for key in ('runtime_compatible','installed','legacy_installed')):
                        return probe, observed
                except (OSError, SetupError):
                    return None
                return None
            finder = discover_frame or presentation.find_frame
            found = finder(validate, cached_hosts=remembered_frame_hosts(local), presenter=ui)
            if found is not None:
                address = found.host
                remote, frame = found.value
        if address and frame is None:
            remote = remote_factory(host(address), user(args.user), interactive=interactive)
            with ui.suspend() if ui else nullcontext():
                frame = remote.call(request)
        if frame is not None and address:
            frame = {**frame, 'host':host(address)}
        if args.manage:
            report = inspection_report(local, pc, frame, inspection)
            if args.check or args.dry_run:
                print(json.dumps(report, sort_keys=True)); return 0
            if ui:
                files = report['pc']['files']
                matching = bool(files) and all(item['status']=='match' for item in files.values())
                ui.status('pc_ready' if matching else 'pc_partial','ok' if matching else 'warning')
                if frame is not None:
                    ready = frame.get('runtime_compatible') is True
                    ui.status('receiver_ready' if ready else 'receiver_changed','ok' if ready else 'warning')
                ui.status('bridge_unavailable','info')
            maintenance = None
            try:
                maintenance = Maintenance(args.bundle)
            except (OSError, SetupError, KeyError, TypeError):
                pass
            actions = ['check','exit']
            if maintenance is not None and remote is not None and frame.get('owned_receipt_valid') and frame.get('legacy_installed'):
                actions.insert(1,'uninstall')
            action = args.action
            if action is None and interactive:
                action = ui.choose_action(actions,recommended='check')
            if action == 'uninstall':
                require('uninstall' in actions, 'No checked, receipt-owned legacy Frame installation is available for removal.')
                require(interactive, 'Uninstall requires an interactive review and explicit confirmation.')
                with ui.suspend():
                    removal_plan = remote.call({'command':'maintenance-plan'})
                require(removal_plan.get('can_remove') is True, 'Frame removal ownership could not be confirmed; no changes were made.')
                ui.say('remove_plan')
                if not ui.confirm_uninstall():
                    print('No changes made.'); return 0
                local.apply_started = True
                with ui.suspend():
                    result = maintenance.remove(remote,removal_plan)
                log = write_report(local,'uninstall',report,result)
                ui.say('remove_pending' if result['reason']=='uninstall_pending' else 'remove_done')
                ui.path(log)
                ui.ask('done_prompt')
                return 0
            if action == 'check':
                report_path = save_inspection(local,report)
                if ui:ui.path(report_path);ui.say('check_finish');ui.ask('done_prompt')
                else:print('Inspection report: ' + str(report_path))
            return 0
        if args.inspect:
            report = inspection_report(local, pc, frame, inspection)
            if args.check:
                print(json.dumps(report, sort_keys=True))
                return 0
            matches = sum(item['status'] == 'match' for item in report['pc']['files'].values())
            if ui:
                ui.say('pc_ready' if matches and matches == len(report['pc']['files']) else 'pc_partial')
                if frame is not None:
                    ui.say('receiver_ready' if frame.get('runtime_compatible') else 'receiver_changed')
                ui.say('check_finish')
            else:
                print('PC SteamVR reference files: ' + str(matches) + '/' + str(len(report['pc']['files'])) + ' match.')
                print('Frame: ' + ('not connected' if frame is None else
                      'connected; reference runtime ' + ('matches' if frame.get('runtime_compatible') else 'differs')) + '.')
                print('This check does not install or prove the new sender/receiver bridge. Ordinary Steam Link is unchanged.')
            if not args.dry_run:
                print((ui.text('report') if ui else 'Inspection report: ') + str(save_inspection(local, report)))
            if interactive and not args.dry_run:
                ui.ask('done_prompt')
            return 0
        report = public_report(pc, frame, bundle)
        suggestion = plan(pc, frame, bundle)
        if args.check:
            print(json.dumps({**report,'suggestion':suggestion}, sort_keys=True))
            return 0
        print(suggestion)
        if args.dry_run:
            print(json.dumps(report, sort_keys=True)); return 0
        action = args.action
        if not action and interactive:
            answer = input_fn('Choose Install / Update / Repair / Uninstall / Quit [Quit]: ').strip().lower()
            action = answer if answer in ('install','update','repair','uninstall') else None
        if not action:
            print('No changes made.'); return 0
        require(interactive, 'Applying changes requires an interactive review. Use --check or --dry-run for automation.')
        print('Selected: ' + action + '. No Steam, SteamVR or Frame restart will be performed.')
        if input_fn('Apply this exact checked action? [y/N] ').strip().lower() != 'y':
            print('No changes made.'); return 0
        result = apply(action, bundle, local, remote, pc, frame, challenge)
        # This explicit action may now persist its non-secret address and log.
        remember_frame(local, address, args.user)
        log = write_report(local, action, report, result)
        print('Finished. Setup log: ' + str(log))
        print('Rollback: run this assistant again and choose Uninstall. Existing older pairing keys are retained for explicit repair.')
        input_fn('All writes are finished. Close and start SteamVR yourself when convenient; press Enter to exit. ')
        return 0
    except KeyboardInterrupt:
        if ui:
            ui.say('cancelled')
        else:
            print('Setup stopped. No automatic restart was performed.', file=sys.stderr)
        return 130
    except (OSError, SetupError, KeyError, TypeError, ValueError) as error:
        if getattr(local, 'apply_started', False):
            try:
                path = write_report(local, action, report, failed=True)
                print('Setup report: ' + str(path), file=sys.stderr)
            except (OSError, SetupError, KeyError, TypeError, ValueError):
                print('Existing log could not be replaced safely; it was preserved.', file=sys.stderr)
        detail = str(error)[:500] if isinstance(error, SetupError) else 'Check the bundle, SSH access and retained install receipts.'
        print('Setup refused the operation: ' + detail + ' No automatic restart was attempted; unknown files are preserved.', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
