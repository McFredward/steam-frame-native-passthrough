#!/usr/bin/env python3
"""Owned native Frame camera-alpha sessions, outside every Valve process.

Only camera/forceSceneAlphaBlending may change. The live mode uses the public
IVRSettings_003 ABI; startup recovery never initializes OpenVR or a service.
"""
from __future__ import annotations

import argparse
import ctypes
import errno
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


# Exact authored reasons only. Never print arbitrary exception/config/path text.
_FAILURE_REASONS = frozenset({
    'runtime state unavailable', 'process identity unavailable', 'native runtime unavailable',
    'native runtime generation changed', 'native camera readiness lost',
    'active native camera readiness lost', 'native camera read refused',
    'native camera write refused', 'native camera removal refused',
    'camera enable not acknowledged', 'camera restore not acknowledged',
    'camera session paused for installer', 'camera owner busy',
    'camera lease replaced by another owner', 'live or uncertain camera owner preserved',
    'file/runtime camera baseline disagrees', 'changed camera override preserved',
    'changed runtime camera preserved', 'changed runtime camera baseline preserved',
    'unknown camera lease preserved', 'unknown camera configuration preserved',
    'unknown native provider pin', 'native provider ownership differs',
    'native provider identity differs', 'native Frame AArch64 provider required',
    'background native initialization refused', 'native settings interface unavailable',
    'native settings interface incomplete', 'private owned camera directory required',
    'ordinary bounded owned data required', 'private camera record required',
    'private camera lock required', 'changed data preserved', 'changed camera data preserved',
    'changed camera ownership record preserved', 'invalid camera settings preserved',
    'nonboolean camera setting preserved', 'unknown camera pause preserved',
    'invalid camera command', 'stale camera command', 'incomplete camera command',
    'camera command buffer exceeded', 'camera command exceeded',
    'running native runtime preserved during startup recovery',
    'native camera persistence not confirmed', 'offline camera restoration not confirmed',
})
_FAILURE_PHASES = frozenset({'setup', 'native-open', 'lease-adopt', 'health', 'enable',
    'disable-restore', 'restore', 'startup', 'serve', 'ipc-read', 'ipc-write', 'ipc-command'})


@contextmanager
def failure_phase(phase):
    try:yield
    except (OSError, ValueError, KeyError, TypeError, SafetyError, AttributeError) as error:
        if not hasattr(error, '_camera_phase'):error._camera_phase = phase
        raise


def warn_failure(phase, error):
    if getattr(error, '_camera_reported', False):return
    error._camera_reported = True
    phase = getattr(error, '_camera_phase', phase)
    if phase not in _FAILURE_PHASES:phase = 'serve'
    reason = str(error) if isinstance(error, SafetyError) and str(error) in _FAILURE_REASONS else (
        'os-error' if isinstance(error, OSError) else 'safety-check-refused' if isinstance(error, SafetyError) else 'invalid-data')
    os_error = error if isinstance(error, OSError) else error.__cause__
    number = os_error.errno if isinstance(os_error, OSError) and type(os_error.errno) is int else None
    category = {errno.ENOENT:'missing', errno.ESRCH:'process-gone', errno.EACCES:'permission-denied',
        errno.EPERM:'permission-denied', errno.EIO:'io-error', errno.EPIPE:'pipe-closed',
        errno.EBADF:'bad-descriptor', errno.EINTR:'interrupted', errno.ETIMEDOUT:'timeout',
        errno.EAGAIN:'busy', errno.EBUSY:'busy'}.get(number, 'other-os-error' if number is not None else 'none')
    warn('phase=' + phase + '; reason=' + reason + '; errno=' + category
        + ('(' + str(number) + ')' if number is not None and 0 < number < 4096 else '')
        + '; existing settings and camera recovery records preserved')


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


def resolve_settings(home=None):
    """Resolve the active OpenVR config; later registry entries are history.

    Match CVRPathRegistry_Public::GetPaths/GetConfigPath: VR_CONFIG_PATH has
    priority, then the selected registry's first config path. Never guess a
    Steam-directory fallback when the current registry cannot be verified.
    """
    home = absolute(str(Path.home() if home is None else home))
    selected = os.environ.get('VR_CONFIG_PATH', '')
    if not selected:
        override = os.environ.get('VR_PATHREG_OVERRIDE', '')
        xdg = os.environ.get('XDG_CONFIG_HOME', '')
        registry = (absolute(override) if override else
            (absolute(xdg) if xdg else home / '.config') / 'openvr/openvrpaths.vrpath')
        raw, _ = read_file(registry, MAX_JSON);value = parsed(raw)
        require(type(value) is dict, 'invalid OpenVR registry preserved')
        paths = value.get('config')
        require(type(paths) is list and 1 <= len(paths) <= 32
            and all(type(item) is str and 1 <= len(item) <= 1024 for item in paths),
            'bounded nonempty OpenVR config history required')
        selected = paths[0]
    directory = absolute(selected);no_links(directory)
    info = directory.stat()
    require(stat.S_ISDIR(info.st_mode) and info.st_uid == os.getuid()
        and not info.st_mode & 0o022, 'ordinary owned OpenVR config directory required')
    return directory / 'steamvr.vrsettings'


def own_start(pid):
    try:
        raw = Path('/proc/' + str(pid) + '/stat').read_text()
        require(len(raw) <= 4096, 'process identity unavailable')
        tail = raw[raw.rfind(')') + 2:].split()
        return int(tail[19])
    except OSError as error:
        if error.errno in (errno.ENOENT, errno.ESRCH):return None
        raise
    except (ValueError, IndexError):raise SafetyError('process identity unavailable') from None


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
                start = own_start(int(path.name))
                if start is not None:result.append((int(path.name), start, name))
        except OSError as error:
            if error.errno in (errno.ENOENT, errno.ESRCH):continue
            raise SafetyError('runtime state unavailable') from error
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
    require(type(v) is dict and set(v) in ({'schema', 'config_sha256', 'helper_pid', 'helper_start_ticks',
        'override', 'effective', 'section_present', 'write_intent', 'settings_signature'},
        {'schema', 'config_sha256', 'helper_pid', 'helper_start_ticks', 'override', 'effective',
        'section_present', 'write_intent', 'settings_signature', 'restore_signature'})
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
        and all(type(n) is int and n >= 0 for n in v['settings_signature'])
        and (v.get('restore_signature') is None or type(v['restore_signature']) is list
            and len(v['restore_signature']) == 5 and all(type(n) is int and n >= 0 for n in v['restore_signature'])),
        'unknown camera lease preserved')
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
            start = own_start(os.getpid());require(start is not None, 'process identity unavailable')
            value = dict(self.lease);value.update(helper_pid=os.getpid(), helper_start_ticks=start)
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
            start = own_start(os.getpid());require(start is not None, 'process identity unavailable')
            self.save({'schema':LEASE_SCHEMA, 'config_sha256':self.sha, 'helper_pid':os.getpid(),
                'helper_start_ticks':start, 'override':before, 'effective':effective,
                'section_present':SECTION in value, 'write_intent':effective is not True,
                'settings_signature':signature(info), 'restore_signature':None})
        else:
            require(before is self.lease['override'] or self.lease['write_intent'] and before is True,
                    'changed camera override preserved')
        if self.lease.get('restore_signature') is not None:
            renewed = dict(self.lease);renewed['restore_signature'] = None;self.save(renewed)
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
        if self.lease['write_intent'] and self.lease.get('restore_signature') is None:
            # Distinguish a save after D from unrelated saves that preceded D.
            restoring = dict(self.lease);restoring['restore_signature'] = signature(info);self.save(restoring)
        if current is not wanted:
            if self.lease['override'] is None:self.camera.remove_override()
            else:self.camera.write(self.lease['override'])
        require(self.camera.read() is wanted, 'camera restore not acknowledged')
        self.enabled = False
        _, observed, _, info = settings(self.path)
        persisted = observed is self.lease['override'] and (
            not self.lease['write_intent'] or signature(info) != self.lease['restore_signature'])
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


def migration_record(root):
    path = root / MIGRATION
    if not path.exists():return None, None
    raw, _ = read_file(path, MAX_JSON, private=True);v = parsed(raw)
    require(type(v) is dict and set(v) == {'schema', 'id', 'phase', 'settings_signature'}
        and v['schema'] == MIGRATION_SCHEMA and type(v['id']) is str
        and re.fullmatch('[0-9a-f]{64}', v['id']) and v['phase'] in ('pending', 'consumed')
        and type(v['settings_signature']) is list and len(v['settings_signature']) == 5
        and all(type(n) is int and n >= 0 for n in v['settings_signature']),
        'unknown camera migration record preserved')
    return v, raw


def migration_state(root, old):
    value, raw = migration_record(root)
    return (value if value is not None and value['id'] == old['id'] else None), raw


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


def inactive_target(active_settings, home):
    home = absolute(str(Path.home() if home is None else home))
    active = absolute(str(active_settings))
    require(active == resolve_settings(home), 'active OpenVR config changed or was not verified')
    target = home / '.local/share/Steam/config/steamvr.vrsettings'
    require(target != active, 'only the known inactive historical camera target is permitted')
    settings(active)
    return target


def inactive_config(root, active_settings, home):
    """Prove the historical wrong target is not the current native config."""
    root = private_root(root);config, sha = load_config(root)
    historical = inactive_target(active_settings, home)
    target = absolute(config['settings'])
    require(target == historical, 'only the known inactive historical camera target is permitted')
    # Both files must exist independently; read_file rejects symlinks/hardlinks.
    settings(target)
    return root, config, sha


def inactive_lease(root, sha):
    if not (root / LEASE).exists():return None
    lease, raw = load_lease(root, sha)
    # A write through the native API may have affected the real active file.
    # Its original runtime baseline cannot be inferred from an unused file.
    require(lease['write_intent'] is False and lease['effective'] is True,
        'native camera write authority requires live restoration')
    return raw


def completed_inactive(root, config=None):
    require(not os.path.lexists(root / LEASE), 'camera target transfer requires no session lease')
    marker, _ = migration_record(root)
    old = config.get('legacy_migration') if config is not None else None
    # With no grant and no lease, no settings restore was ever authorized.
    # Only the still-present pause is transferred; this grants no write rights.
    require((marker is None and old is None) or marker is not None and
        marker['phase'] == 'consumed' and (old is None or marker['id'] == old['id']),
        'completed inactive migration authority required')


def pause_inactive(root, config, sha, active_settings):
    """Recover a pre-publication target transfer while staying paused."""
    path = root / PAUSE
    if path.exists():
        raw, _ = read_file(path, MAX_JSON, private=True)
        if parsed(raw) == {'schema':PAUSE_SCHEMA, 'settings':str(active_settings)}:
            completed_inactive(root, config)
            replace(path, encoded({'schema':PAUSE_SCHEMA, 'settings':config['settings']}), raw)
    pause(root, sha)


def prepare_target(root, new_settings, *, home=None):
    """Transfer a completed recovery's pause before publishing the new config.

    The pause is never absent: the old helper remains blocked throughout the
    target change, and only startup with the newly published config can clear
    the transferred pause. A rollback before publication can normalize it back.
    """
    root = private_root(root);inactive_target(new_settings, home)
    with locked(root):
        historical = inactive_target(new_settings, home)
        config = None
        if os.path.lexists(root / CONFIG):
            _, config, _ = inactive_config(root, new_settings, home)
        completed_inactive(root, config)
        path = root / PAUSE;raw, _ = read_file(path, MAX_JSON, private=True)
        before = parsed(raw)
        desired = {'schema':PAUSE_SCHEMA, 'settings':str(new_settings)}
        require(before == {'schema':PAUSE_SCHEMA, 'settings':str(historical)} or before == desired,
            'unknown camera pause preserved during target transfer')
        if before != desired:replace(path, encoded(desired), raw)
    return {'status':'unchanged' if before == desired else 'restored', 'persisted':True}


def restore_inactive(root, active_settings, *, home=None):
    """Quiesce a proven unused legacy config without touching the native API."""
    root, config, sha = inactive_config(root, active_settings, home)
    changed = False
    with locked(root):
        # Recheck the native registry after waiting for the session owner.
        inactive_config(root, active_settings, home)
        lease_raw = inactive_lease(root, sha)
        pause_inactive(root, config, sha, active_settings)
        old = config.get('legacy_migration')
        if old:
            marker, _ = migration_state(root, old)
            if not marker or marker['phase'] != 'consumed':
                _, current, _, _ = settings(absolute(config['settings']))
                require(current is old['value'] or current is True,
                    'changed inactive camera override preserved')
                marker = migration_begin(root, config)
                if marker:
                    restore_offline(config, old['value'], old['section_present'], owned_true=True)
                    migration_finish(root, old, marker);changed = True
        if lease_raw is not None:
            remove_owned(root / LEASE, lease_raw);changed = True
    return {'status':'restored' if changed else 'unchanged', 'persisted':True}


def apply_owned_inactive(root, authority, active_settings, *, home=None):
    """Restore an exact checkpoint delta in the unused historical file only."""
    root, config, sha = inactive_config(root, active_settings, home)
    require(type(authority) is dict and set(authority) == {'schema', 'settings', 'before', 'desired', 'section_present'}
        and authority['schema'] == 'sfnp-camera-override-authority-v1'
        and authority['settings'] == config['settings']
        and all(authority[k] is None or type(authority[k]) is bool for k in ('before', 'desired'))
        and type(authority['section_present']) is bool, 'unknown installer camera authority')
    with locked(root):
        inactive_config(root, active_settings, home)
        lease_raw = inactive_lease(root, sha)
        pause_inactive(root, config, sha, active_settings)
        path = absolute(config['settings']);value, current, raw, _ = settings(path)
        require(current is authority['before'], 'changed inactive camera checkpoint preserved')
        desired = authority['desired']
        if current is not desired:
            if desired is None:
                value[SECTION].pop(KEY)
                if not value[SECTION] and not authority['section_present']:value.pop(SECTION)
            else:value.setdefault(SECTION, {})[KEY] = desired
            replace(path, encoded(value), raw, private=False)
            require(settings(path)[1] is desired, 'inactive camera checkpoint restoration not confirmed')
        if lease_raw is not None:remove_owned(root / LEASE, lease_raw)
    return {'status':'unchanged' if current is desired else 'restored', 'persisted':True}


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
    except (OSError, ValueError, KeyError, TypeError, SafetyError) as error:
        warn_failure('startup', error)
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
        if not cleanup_pending(root, config, sha):
            return {'status':'unchanged', 'persisted':True}
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


def cleanup_pending(root, config, sha):
    """Called under the ownership lock; missing native ABI is not permission."""
    if (root / LEASE).exists():
        load_lease(root, sha)
        return True
    old = config.get('legacy_migration')
    if old:
        marker, _ = migration_state(root, old)
        return not marker or marker['phase'] != 'consumed'
    return False


def prepare_offline_restore(root):
    """Quiesce renewals; existing startup restores exact ownership before VR starts."""
    root = private_root(root);config, sha = load_config(root)
    with locked(root):
        cleanup_pending(root, config, sha)
        pause(root, sha)
    return {'status':'deferred', 'persisted':False}


def offline_cleanup_ready(root):
    root = private_root(root);config, sha = load_config(root)
    with locked(root):
        return not cleanup_pending(root, config, sha) and not (root / PAUSE).exists()


def serve(root, *, input_stream=None, output_stream=None, api_factory=NativeCamera):
    with failure_phase('setup'):
        root = private_root(root);config, sha = load_config(root)
    # A retained installer pause must not repeatedly initialize OpenVR merely
    # to refuse the session afterwards. Recheck after API setup for races.
    with failure_phase('setup'), locked(root):
        unpaused(root);settings(absolute(config['settings']))
    input_stream = sys.stdin.buffer if input_stream is None else input_stream
    output_stream = sys.stdout if output_stream is None else output_stream
    def reply(value):
        with failure_phase('ipc-write'):output_stream.write(value + '\n');output_stream.flush()
    with failure_phase('native-open'), api_factory(config['library']) as camera:
        session = Session(root, config, sha, camera)
        wanted, previous = False, 0
        try:
            with failure_phase('lease-adopt'), locked(root):
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
                        with failure_phase('ipc-read'):ready = select.select([fd], [], [], 0.5 if wanted else None)[0]
                        if not ready:
                            try:
                                with failure_phase('health'), locked(root):
                                    unpaused(root);session.refresh()
                                    require(camera.read() is True, 'native camera readiness lost')
                            except (OSError, ValueError, KeyError, TypeError, SafetyError) as error:
                                warn_failure('health', error)
                                reply('N ' + str(previous));raise
                            continue
                        with failure_phase('ipc-read'):chunk = os.read(fd, 128)
                        if not chunk:
                            with failure_phase('ipc-command'):require(not pending, 'incomplete camera command')
                            break
                        pending.extend(chunk)
                    with failure_phase('ipc-command'):require(len(pending) <= 256, 'camera command buffer exceeded')
                    if b'\n' not in pending:
                        with failure_phase('ipc-command'):require(len(pending) <= 64, 'camera command exceeded')
                        continue
                    end = pending.index(10) + 1
                    raw = bytes(pending[:end]);del pending[:end]
                else:
                    with failure_phase('ipc-read'):raw = input_stream.readline(65)
                    if not raw:break
                with failure_phase('ipc-command'):
                    match = re.fullmatch(rb'([ED]) ([1-9][0-9]{0,19})\n', raw)
                    require(match is not None, 'invalid camera command')
                    sequence = int(match[2]);require(previous < sequence < 2**64, 'stale camera command')
                previous = sequence
                with failure_phase('enable' if match[1] == b'E' else 'disable-restore'), locked(root):
                    if match[1] == b'E':session.enable();wanted = True;reply('A ' + str(sequence))
                    else:session.restore();wanted = False;reply('D ' + str(sequence))
        finally:
            try:
                with locked(root):session.restore()
            except (OSError, ValueError, KeyError, TypeError, SafetyError) as error:warn_failure('restore', error)
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
    except (OSError, ValueError, KeyError, TypeError, SafetyError, AttributeError) as error:
        warn_failure(args.mode, error);return 2


if __name__ == '__main__':raise SystemExit(main())
