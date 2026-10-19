#!/usr/bin/env python3
"""Owned native Frame camera-alpha sessions, outside every Valve process.

Only camera/forceSceneAlphaBlending may change. The live mode uses the public
IVRSettings_003 ABI; startup recovery never initializes OpenVR or a service.
"""
from __future__ import annotations

import argparse
import ctypes
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import select
import signal
import stat
import struct
import sys
import time

CONFIG = 'camera-session.json'
LEASE = 'camera-session-lease.json'
MIGRATION = 'camera-session-migration.json'
PAUSE = 'camera-session.pause'
LOCK = 'camera-session.lock'
PAUSE_SCHEMA = 'sfnp-camera-session-pause-v1'
CONFIG_SCHEMA = 'sfnp-camera-session-config-v1'
LEASE_SCHEMA = 'sfnp-camera-session-lease-v1'
MIGRATION_SCHEMA = 'sfnp-camera-session-migration-v1'
SECTION, KEY = 'camera', 'forceSceneAlphaBlending'
MAX_JSON, MAX_SETTINGS, MAX_LIBRARY = 16384, 2 * 1024 * 1024, 2 * 1024 * 1024
_warnings = 0


class SafetyError(RuntimeError):
    pass


class StopRequested(BaseException):
    pass


def require(condition, message):
    if not condition:raise SafetyError(message)


def warn(message):
    global _warnings
    if _warnings < 8:
        _warnings += 1
        print('Steam Link camera: ' + message, file=sys.stderr, flush=True)


def encoded(value):
    return (json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False) + '\n').encode()


def parsed(raw):
    def pairs(items):
        value = {}
        for key, item in items:
            require(key not in value, 'duplicate JSON key preserved')
            value[key] = item
        return value
    return json.loads(raw, object_pairs_hook=pairs,
        parse_constant=lambda _: (_ for _ in ()).throw(SafetyError('invalid JSON number preserved')))


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def absolute(value):
    require(type(value) is str and 1 <= len(value) <= 1024 and value.startswith('/')
            and os.path.normpath(value) == value and '\0' not in value, 'an absolute ordinary path is required')
    return Path(value)


def no_links(path):
    for parent in (path, *path.parents):
        if os.path.lexists(parent):require(not stat.S_ISLNK(parent.lstat().st_mode), 'symlink path preserved')


def private_root(value):
    root = absolute(str(value));no_links(root)
    info = root.stat()
    require(stat.S_ISDIR(info.st_mode) and info.st_uid == os.getuid()
            and stat.S_IMODE(info.st_mode) == 0o700, 'private owned camera directory required')
    return root


def signature(info):
    return [info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns]


def read_file(path, limit, *, private=False):
    no_links(path)
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        before = os.fstat(fd)
        require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1
                and before.st_uid == os.getuid() and 0 <= before.st_size <= limit,
                'ordinary bounded owned data required')
        if private:require(stat.S_IMODE(before.st_mode) == 0o600, 'private camera record required')
        chunks, size = [], 0
        while size <= limit:
            chunk = os.read(fd, min(65536, limit + 1 - size))
            if not chunk:break
            chunks.append(chunk);size += len(chunk)
        require(size == before.st_size and signature(before) == signature(os.fstat(fd)), 'changed data preserved')
        return b''.join(chunks), before
    finally:os.close(fd)


def sync_directory(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:os.fsync(fd)
    finally:os.close(fd)


def write_new(path, raw):
    no_links(path.parent)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(raw);stream.flush();os.fsync(stream.fileno())
        sync_directory(path.parent)
    except BaseException:
        # Keep uncertain durable ownership; never remove an unverified record.
        raise


def replace(path, raw, before, *, private=True):
    previous, info = read_file(path, MAX_JSON if private else MAX_SETTINGS, private=private)
    require(previous == before, 'changed camera data preserved')
    temp = path.with_name('.' + path.name + '.' + os.urandom(8).hex())
    try:
        write_new(temp, raw)
        os.chmod(temp, stat.S_IMODE(info.st_mode))
        current, after = read_file(path, MAX_JSON if private else MAX_SETTINGS, private=private)
        require(current == before and signature(info) == signature(after), 'changed camera data preserved')
        os.replace(temp, path);sync_directory(path.parent)
    finally:
        if temp.exists():temp.unlink()


def remove_owned(path, raw):
    current, _ = read_file(path, MAX_JSON, private=True)
    require(current == raw, 'changed camera ownership record preserved')
    path.unlink();sync_directory(path.parent)


def load_config(root):
    raw, _ = read_file(root / CONFIG, MAX_JSON, private=True);value = parsed(raw)
    require(type(value) is dict and set(value) in (
        {'schema', 'settings', 'library'}, {'schema', 'settings', 'library', 'legacy_migration'})
        and value['schema'] == CONFIG_SCHEMA, 'unknown camera configuration preserved')
    absolute(value['settings'])
    lib = value['library']
    require(type(lib) is dict and set(lib) == {'path', 'bytes', 'sha256'}
        and type(lib['bytes']) is int and 64 <= lib['bytes'] <= MAX_LIBRARY
        and type(lib['sha256']) is str and re.fullmatch('[0-9a-f]{64}', lib['sha256']), 'unknown native provider pin')
    absolute(lib['path'])
    if 'legacy_migration' in value:
        old = value['legacy_migration']
        require(type(old) is dict and set(old) == {'id', 'section_present', 'present', 'value', 'installed'}
            and type(old['id']) is str and re.fullmatch('[0-9a-f]{64}', old['id'])
            and type(old['section_present']) is bool and type(old['present']) is bool
            and old['installed'] is True and (not old['present'] or old['section_present'])
            and (type(old['value']) is bool if old['present'] else old['value'] is None),
            'unknown legacy camera authority preserved')
    return value, digest(raw)


def settings(path):
    raw, info = read_file(path, MAX_SETTINGS);value = parsed(raw)
    require(type(value) is dict and type(value.get(SECTION, {})) is dict, 'invalid camera settings preserved')
    section = value.get(SECTION, {})
    require(KEY not in section or type(section[KEY]) is bool, 'nonboolean camera setting preserved')
    return value, section.get(KEY), raw, info


def own_start(pid):
    try:
        raw = Path('/proc/' + str(pid) + '/stat').read_text()
        require(len(raw) <= 4096, 'process identity unavailable')
        tail = raw[raw.rfind(')') + 2:].split()
        return int(tail[19])
    except FileNotFoundError:return None


def owner_alive(lease):
    try:
        start = own_start(lease['helper_pid'])
        return start is not None and start == lease['helper_start_ticks']
    except (OSError, ValueError, IndexError, SafetyError):return None


def runtime_generation():
    entries = list(Path('/proc').iterdir())
    require(len(entries) <= 8192, 'runtime state unavailable')
    result = []
    for path in entries:
        if not path.name.isdecimal():continue
        try:
            if path.stat().st_uid != os.getuid():continue
            name = (path / 'comm').read_text()[:64].strip()
            if name in ('vrserver', 'vrcompositor'):
                result.append((int(path.name), own_start(int(path.name)), name))
        except FileNotFoundError:continue
        except OSError:raise SafetyError('runtime state unavailable') from None
    return tuple(sorted(result))


def runtime_running():return bool(runtime_generation())


@contextmanager
def locked(root, timeout=3.0):
    path = root / LOCK;no_links(path)
    fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        info = os.fstat(fd)
        require(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid() and info.st_nlink == 1
            and stat.S_IMODE(info.st_mode) == 0o600, 'private camera lock required')
        deadline = time.monotonic() + timeout
        while True:
            try:fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB);break
            except BlockingIOError:
                require(time.monotonic() < deadline, 'camera owner busy')
                time.sleep(0.01)
        yield
    finally:os.close(fd)


def pause(root, sha):
    # Config bytes change on updates; the owned target path remains stable.
    config, _ = load_config(root)
    path = root / PAUSE
    if path.exists():
        raw, _ = read_file(path, MAX_JSON, private=True);v = parsed(raw)
        require(v == {'schema':PAUSE_SCHEMA, 'settings':config['settings']}, 'unknown camera pause preserved')
    else:write_new(path, encoded({'schema':PAUSE_SCHEMA, 'settings':config['settings']}))


def unpaused(root):
    require(not os.path.lexists(root / PAUSE), 'camera session paused for installer')


def load_lease(root, config_sha):
    raw, _ = read_file(root / LEASE, MAX_JSON, private=True);v = parsed(raw)
    require(type(v) is dict and set(v) == {'schema', 'config_sha256', 'helper_pid', 'helper_start_ticks',
        'override', 'effective', 'section_present', 'write_intent', 'settings_signature'}
        and v['schema'] == LEASE_SCHEMA and v['config_sha256'] == config_sha
        and type(v['helper_pid']) is int and 0 < v['helper_pid'] < 2**31
        and type(v['helper_start_ticks']) is int and v['helper_start_ticks'] >= 0
        and (v['override'] is None or type(v['override']) is bool)
        and (v['effective'] is None or type(v['effective']) is bool)
        and type(v['section_present']) is bool and type(v['write_intent']) is bool
        and (v['override'] is None or v['override'] is v['effective'])
        and (v['override'] is None or v['section_present'])
        and v['write_intent'] is (v['effective'] is not True)
        and type(v['settings_signature']) is list and len(v['settings_signature']) == 5
        and all(type(n) is int and n >= 0 for n in v['settings_signature']), 'unknown camera lease preserved')
    return v, raw


_ERR = ctypes.POINTER(ctypes.c_int)
_FN = ctypes.CFUNCTYPE


class SettingsTable(ctypes.Structure):
    _fields_ = [('GetSettingsErrorNameFromEnum', _FN(ctypes.c_char_p, ctypes.c_int)),
        ('SetBool', _FN(None, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_bool, _ERR)),
        ('SetInt32', ctypes.c_void_p), ('SetFloat', ctypes.c_void_p), ('SetString', ctypes.c_void_p),
        ('GetBool', _FN(ctypes.c_bool, ctypes.c_char_p, ctypes.c_char_p, _ERR)),
        ('GetInt32', ctypes.c_void_p), ('GetFloat', ctypes.c_void_p), ('GetString', ctypes.c_void_p),
        ('RemoveSection', ctypes.c_void_p), ('RemoveKeyInSection', _FN(None, ctypes.c_char_p, ctypes.c_char_p, _ERR))]


class NativeCamera:
    def __init__(self, pin):self.pin, self.fd, self.started = pin, -1, False

    def __enter__(self):
        require(sys.platform == 'linux' and os.uname().machine in ('aarch64', 'arm64'), 'native Frame AArch64 provider required')
        self.fd = os.open(self.pin['path'], os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK)
        try:
            info = os.fstat(self.fd)
            require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and info.st_uid in (0, os.getuid())
                and not info.st_mode & 0o022 and info.st_size == self.pin['bytes'], 'native provider ownership differs')
            raw = os.read(self.fd, MAX_LIBRARY + 1)
            require(len(raw) == self.pin['bytes'] and digest(raw) == self.pin['sha256']
                and raw[:7] == b'\x7fELF\x02\x01\x01' and struct.unpack_from('<H', raw, 18)[0] == 183,
                'native provider identity differs')
            self.api = ctypes.CDLL('/proc/self/fd/' + str(self.fd))
            self.api.VR_InitInternal2.restype = ctypes.c_uint32
            self.api.VR_InitInternal2.argtypes = [_ERR, ctypes.c_int, ctypes.c_char_p]
            self.api.VR_GetGenericInterface.restype = ctypes.c_void_p
            self.api.VR_GetGenericInterface.argtypes = [ctypes.c_char_p, _ERR]
            self.api.VR_ShutdownInternal.restype = None;self.api.VR_ShutdownInternal.argtypes = []
            error = ctypes.c_int(0)
            self.api.VR_InitInternal2(ctypes.byref(error), 3, None)
            require(error.value == 0, 'background native initialization refused')
            self.started = True
            self.generation = runtime_generation()
            require(bool(self.generation), 'native runtime unavailable')
            address = self.api.VR_GetGenericInterface(b'FnTable:IVRSettings_003', ctypes.byref(error))
            require(bool(address) and error.value == 0, 'native settings interface unavailable')
            self.fn = ctypes.cast(address, ctypes.POINTER(SettingsTable)).contents
            require(all(bool(getattr(self.fn, n)) for n in ('SetBool', 'GetBool', 'RemoveKeyInSection')), 'native settings interface incomplete')
            return self
        except BaseException:
            self.__exit__(None, None, None);raise

    def __exit__(self, *unused):
        if self.started:self.api.VR_ShutdownInternal();self.started = False
        if self.fd >= 0:os.close(self.fd);self.fd = -1

    def health(self):
        require(runtime_generation() == self.generation, 'native runtime generation changed')

    def read(self):
        self.health()
        error = ctypes.c_int(0);value = self.fn.GetBool(SECTION.encode(), KEY.encode(), ctypes.byref(error))
        if error.value == 5:return None
        require(error.value == 0, 'native camera read refused')
        return bool(value)

    def write(self, value):
        require(type(value) is bool, 'exact camera boolean required')
        error = ctypes.c_int(0);self.fn.SetBool(SECTION.encode(), KEY.encode(), value, ctypes.byref(error))
        require(error.value == 0, 'native camera write refused')

    def remove_override(self):
        error = ctypes.c_int(0);self.fn.RemoveKeyInSection(SECTION.encode(), KEY.encode(), ctypes.byref(error))
        require(error.value == 0, 'native camera removal refused')


class Session:
    def __init__(self, root, config, config_sha, camera):
        self.root, self.config, self.sha, self.camera = root, config, config_sha, camera
        self.path = absolute(config['settings']);self.lease = self.raw = None;self.enabled = False

    def save(self, value):
        raw = encoded(value)
        if self.raw is None:write_new(self.root / LEASE, raw)
        else:replace(self.root / LEASE, raw, self.raw)
        self.lease, self.raw = value, raw

    def adopt(self):
        if not (self.root / LEASE).exists():return
        lease, raw = load_lease(self.root, self.sha)
        require(owner_alive(lease) is False, 'live or uncertain camera owner preserved')
        self.lease, self.raw = lease, raw
        self.restore()
        if self.lease:
            value = dict(self.lease);value.update(helper_pid=os.getpid(), helper_start_ticks=own_start(os.getpid()))
            self.save(value)

    def refresh(self):
        if self.lease is not None:
            lease, raw = load_lease(self.root, self.sha)
            require(raw == self.raw, 'camera lease replaced by another owner')

    def enable(self):
        unpaused(self.root);self.refresh()
        value, before, _, info = settings(self.path);effective = self.camera.read()
        if self.enabled:require(effective is True, 'active native camera readiness lost')
        if self.lease is None:
            require(before is None or before is effective, 'file/runtime camera baseline disagrees')
            self.save({'schema':LEASE_SCHEMA, 'config_sha256':self.sha, 'helper_pid':os.getpid(),
                'helper_start_ticks':own_start(os.getpid()), 'override':before, 'effective':effective,
                'section_present':SECTION in value, 'write_intent':effective is not True,
                'settings_signature':signature(info)})
        else:
            require(before is self.lease['override'] or self.lease['write_intent'] and before is True,
                    'changed camera override preserved')
        if effective is not True:
            require(effective is self.lease['effective'], 'changed runtime camera baseline preserved')
            self.camera.write(True)
        require(self.camera.read() is True, 'camera enable not acknowledged')
        self.enabled = True

    def restore(self):
        if self.lease is None:return
        self.refresh()
        _, current_override, _, info = settings(self.path)
        wanted = self.lease['effective'];current = self.camera.read()
        require(current_override is self.lease['override'] or self.lease['write_intent'] and current_override is True,
                'changed camera override preserved')
        require(current is wanted or self.lease['write_intent'] and current is True, 'changed runtime camera preserved')
        if current is not wanted:
            if self.lease['override'] is None:self.camera.remove_override()
            else:self.camera.write(self.lease['override'])
        require(self.camera.read() is wanted, 'camera restore not acknowledged')
        self.enabled = False
        _, observed, _, info = settings(self.path)
        persisted = observed is self.lease['override'] and (
            not self.lease['write_intent'] or signature(info) != self.lease['settings_signature'])
        if persisted:
            remove_owned(self.root / LEASE, self.raw);self.lease = self.raw = None


def restore_offline(config, override, section_present, *, owned_true):
    path = absolute(config['settings']);value, current, raw, _ = settings(path)
    if current is override:return 'already-restored'
    require(owned_true and current is True and override is not True, 'changed camera override preserved')
    if override is None:
        value[SECTION].pop(KEY)
        if not value[SECTION] and not section_present:value.pop(SECTION)
    else:value.setdefault(SECTION, {})[KEY] = override
    replace(path, encoded(value), raw, private=False)
    require(settings(path)[1] is override, 'offline camera restoration not confirmed')
    return 'restored'


def migration_state(root, old):
    path = root / MIGRATION
    if not path.exists():return None, None
    raw, _ = read_file(path, MAX_JSON, private=True);v = parsed(raw)
    require(type(v) is dict and set(v) == {'schema', 'id', 'phase', 'settings_signature'}
        and v['schema'] == MIGRATION_SCHEMA and type(v['id']) is str
        and re.fullmatch('[0-9a-f]{64}', v['id']) and v['phase'] in ('pending', 'consumed')
        and type(v['settings_signature']) is list and len(v['settings_signature']) == 5
        and all(type(n) is int and n >= 0 for n in v['settings_signature']),
        'unknown camera migration record preserved')
    return (v if v['id'] == old['id'] else None), raw


def migration_begin(root, config):
    old = config.get('legacy_migration')
    if not old:return None
    marker, raw = migration_state(root, old)
    if marker and marker['phase'] == 'consumed':return None
    _, current, _, info = settings(absolute(config['settings']))
    if marker:
        require(current is old['value'] or current is True and signature(info) == marker['settings_signature'],
            'changed pending migration preserved')
    else:
        marker = {'schema':MIGRATION_SCHEMA, 'id':old['id'], 'phase':'pending',
            'settings_signature':signature(info)}
        # A third-party change spends this one-time grant without changing it.
        if current is not old['value'] and current is not True:marker['phase'] = 'consumed'
        if raw is None:write_new(root / MIGRATION, encoded(marker))
        else:replace(root / MIGRATION, encoded(marker), raw)
    return marker if marker['phase'] == 'pending' else None


def migration_finish(root, old, marker):
    current, raw = migration_state(root, old)
    require(current == marker, 'changed migration authority preserved')
    done = dict(marker);done['phase'] = 'consumed'
    replace(root / MIGRATION, encoded(done), raw)


def startup(root, *, running=runtime_running, alive=owner_alive):
    """Offline only. Unknown data produces a warning, never a blocked UI boot."""
    try:
        root = private_root(root);config, sha = load_config(root)
        with locked(root):
            require(not running(), 'running native runtime preserved during startup recovery')
            if (root / LEASE).exists():
                lease, raw = load_lease(root, sha)
                require(alive(lease) is False, 'live or uncertain camera owner preserved')
                restore_offline(config, lease['override'], lease['section_present'], owned_true=lease['write_intent'])
                remove_owned(root / LEASE, raw)
            marker = migration_begin(root, config)
            if marker:
                old = config['legacy_migration']
                restore_offline(config, old['value'], old['section_present'], owned_true=True)
                migration_finish(root, old, marker)
            if (root / PAUSE).exists():
                pause(root, sha)
                raw, _ = read_file(root / PAUSE, MAX_JSON, private=True)
                remove_owned(root / PAUSE, raw)
    except (OSError, ValueError, KeyError, TypeError, SafetyError):
        warn('startup recovery incomplete; existing settings and recovery records retained')
    return 0


def wait_persisted(session, timeout):
    deadline = time.monotonic() + timeout
    while True:
        session.restore()
        if session.lease is None:return
        require(time.monotonic() < deadline, 'native camera persistence not confirmed')
        time.sleep(0.05)


def runtime_matches_override(runtime, override):
    # Removing an absent key may expose the native default False instead of
    # ReadFailed/None. Never accept a live True as an unknown absent baseline.
    return runtime is override or override is None and runtime is False


def apply_owned_override(root, authority, *, api_factory=NativeCamera, persist_timeout=3.0):
    """Installer-only exact checkpoint delta; never a general settings writer.

    The caller must already have verified its immutable operation/checkpoint.
    Both disk and native current values must confirm the provided before value.
    """
    root = private_root(root);config, sha = load_config(root)
    require(type(authority) is dict and set(authority) == {'schema', 'settings', 'before', 'desired', 'section_present'}
        and authority['schema'] == 'sfnp-camera-override-authority-v1'
        and authority['settings'] == config['settings']
        and all(authority[k] is None or type(authority[k]) is bool for k in ('before', 'desired'))
        and type(authority['section_present']) is bool, 'unknown installer camera authority')
    require(type(persist_timeout) in (int, float) and 0 <= persist_timeout <= 10, 'invalid persistence bound')
    with locked(root):
        pause(root, sha)
        _, current, _, _ = settings(absolute(config['settings']))
        require(current is authority['before'], 'changed installer camera checkpoint preserved')
        with api_factory(config['library']) as camera:
            require(runtime_matches_override(camera.read(), current), 'runtime installer camera checkpoint disagrees')
            desired = authority['desired']
            if current is not desired:
                if desired is None:camera.remove_override()
                else:camera.write(desired)
            require(runtime_matches_override(camera.read(), desired), 'installer runtime restore not confirmed')
            deadline = time.monotonic() + persist_timeout
            while settings(absolute(config['settings']))[1] is not desired:
                require(time.monotonic() < deadline, 'installer native persistence not confirmed')
                time.sleep(0.05)
    return {'status':'unchanged' if current is desired else 'restored', 'persisted':True}


def restore_owned(root, *, api_factory=NativeCamera, persist_timeout=3.0):
    """Pause renewals and restore only durable owned session/migration changes."""
    root = private_root(root);config, sha = load_config(root)
    require(type(persist_timeout) in (int, float) and 0 <= persist_timeout <= 10, 'invalid persistence bound')
    changed = False
    with locked(root):
        pause(root, sha)
        with api_factory(config['library']) as camera:
            session = Session(root, config, sha, camera)
            if (root / LEASE).exists():
                # The lock plus pause permits takeover even if the owner is live.
                session.lease, session.raw = load_lease(root, sha)
                wait_persisted(session, persist_timeout);changed = True
            marker = migration_begin(root, config)
            if marker:
                old = config['legacy_migration']
                _, current, _, _ = settings(session.path)
                runtime = camera.read()
                require(current in (old['value'], True) and (runtime_matches_override(runtime, old['value']) or runtime is True),
                    'changed legacy runtime camera preserved')
                if runtime is not old['value']:
                    if old['value'] is None:camera.remove_override()
                    else:camera.write(old['value'])
                require(runtime_matches_override(camera.read(), old['value']), 'legacy runtime restore not confirmed')
                deadline = time.monotonic() + persist_timeout
                while settings(session.path)[1] is not old['value']:
                    require(time.monotonic() < deadline, 'legacy native persistence not confirmed')
                    time.sleep(0.05)
                migration_finish(root, old, marker);changed = True
    return {'status':'restored' if changed else 'unchanged', 'persisted':True}


def serve(root, *, input_stream=None, output_stream=None, api_factory=NativeCamera):
    root = private_root(root);config, sha = load_config(root)
    input_stream = sys.stdin.buffer if input_stream is None else input_stream
    output_stream = sys.stdout if output_stream is None else output_stream
    def reply(value):output_stream.write(value + '\n');output_stream.flush()
    with api_factory(config['library']) as camera:
        session = Session(root, config, sha, camera)
        wanted, previous = False, 0
        try:
            with locked(root):
                unpaused(root);settings(session.path);session.adopt();reply('R 0')
            try:fd = input_stream.fileno()
            except (OSError, ValueError, AttributeError):fd = None
            pending = bytearray()
            while True:
                # Health polling is confined to this helper while MR is wanted.
                # Read the pipe directly: BufferedReader prefetch must not hide a
                # queued D command from select or starve session revalidation.
                if fd is not None:
                    if b'\n' not in pending:
                        ready = select.select([fd], [], [], 0.5 if wanted else None)[0]
                        if not ready:
                            try:
                                with locked(root):
                                    unpaused(root);session.refresh()
                                    require(camera.read() is True, 'native camera readiness lost')
                            except (OSError, ValueError, KeyError, TypeError, SafetyError):
                                reply('N ' + str(previous));raise
                            continue
                        chunk = os.read(fd, 128)
                        if not chunk:
                            require(not pending, 'incomplete camera command');break
                        pending.extend(chunk)
                    require(len(pending) <= 256, 'camera command buffer exceeded')
                    if b'\n' not in pending:
                        require(len(pending) <= 64, 'camera command exceeded');continue
                    end = pending.index(10) + 1
                    raw = bytes(pending[:end]);del pending[:end]
                else:
                    raw = input_stream.readline(65)
                    if not raw:break
                match = re.fullmatch(rb'([ED]) ([1-9][0-9]{0,19})\n', raw)
                require(match is not None, 'invalid camera command')
                sequence = int(match[2]);require(previous < sequence < 2**64, 'stale camera command')
                previous = sequence
                with locked(root):
                    if match[1] == b'E':session.enable();wanted = True;reply('A ' + str(sequence))
                    else:session.restore();wanted = False;reply('D ' + str(sequence))
        finally:
            try:
                with locked(root):session.restore()
            except (OSError, ValueError, KeyError, TypeError, SafetyError):warn('runtime restore incomplete; camera lease retained')
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('serve', 'startup', 'restore'))
    parser.add_argument('--root', required=True)
    args = parser.parse_args(argv)
    if args.mode == 'startup':return startup(args.root)
    def stop(*_):raise StopRequested()
    signal.signal(signal.SIGTERM, stop)
    try:
        if args.mode == 'restore':restore_owned(args.root);return 0
        return serve(args.root)
    except (StopRequested, KeyboardInterrupt):return 0
    except (OSError, ValueError, KeyError, TypeError, SafetyError, AttributeError):
        warn('camera session refused; ordinary native output retained');return 2


if __name__ == '__main__':raise SystemExit(main())
