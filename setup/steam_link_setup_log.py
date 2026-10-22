"""Private, bounded setup diagnostics; never a console/SSH transcript.

Only fixed event codes, numeric OS codes and known recovery-receipt paths enter
this log. Importing this module does not create files. Inspection/dry-run callers
must not open a session. A logging failure never raises over an operation error.
"""
import ctypes
import datetime
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import platform
import re
import secrets
import stat
import subprocess
import threading

ROOT_NAME = 'steam-link-native-passthrough'
SCHEMA = 'sfnp-setup-session-log-v1'
MAX_BYTES = 256 * 1024
MAX_EVENT_BYTES = 4096
_LIMIT_RESERVE = 512
STAGES = frozenset(('session', 'bundle', 'preflight', 'pc_inventory',
                    'frame_discovery', 'frame_inventory', 'pairing',
                    'frame_prepare', 'pc_install', 'frame_commit',
                    'startup_baseline', 'startup_health', 'picture_check',
                    'undo', 'cleanup', 'complete','runtime_stop','runtime_start','rollback'))
KINDS = frozenset(('start', 'stage', 'result', 'failure', 'recovery', 'complete'))
ACTIONS = frozenset(('install','update','repair','uninstall','check','exit',
                     'retry','back','rollback','continue'))
STATUSES = frozenset(('started', 'ok', 'partial', 'failed', 'refused', 'skipped',
                      'pending', 'cancelled', 'unavailable', 'unchanged'))
COMMANDS = frozenset(('inspect', 'check', 'install', 'update', 'repair',
                      'uninstall', 'undo', 'bridge-inventory',
                      'bridge-health-baseline', 'bridge-health', 'bridge-prepare',
                      'bridge-commit', 'bridge-undo', 'maintenance-plan',
                      'maintenance-remove', 'inventory', 'inventory-inspect',
                      'bridge-runtime-snapshot','bridge-runtime-restart','bridge-runtime-restored',
                      'bridge-restore','bridge-checkpoint-finish'))
REASONS = frozenset(('unknown_error', 'invalid_input', 'operation_refused',
                     'remote_refused', 'remote_unavailable', 'timeout',
                     'permission_denied', 'sharing_violation', 'missing',
                     'already_exists', 'io_error', 'interrupted', 'cancelled',
                     'unsupported', 'reference_match', 'hash_or_size_mismatch',
                     'steamvr_not_found', 'redirected_path', 'not_regular',
                     'size_limit', 'invalid_metadata', 'metadata_changed',
                     'log_limit', 'log_unavailable', 'recovery_retained',
                     'committed', 'removed', 'pending_stop', 'health_failed',
                     'picture_unconfirmed', 'complete'))
ERROR_TYPES = frozenset(('os_error', 'permission_error', 'missing_error',
                         'exists_error', 'timeout_error', 'process_error',
                         'validation_error', 'interrupted_error',
                         'cancelled_error', 'unknown_error'))
_CHECK = re.compile(r'check_[0-9a-f]{16}\Z')


class _Refused(Exception):
    def __init__(self, code):
        self.code = code


def _require(value, code):
    if not value:
        raise _Refused(code)


def default_root(system=None):
    """Match the setup engine's persistent PC data root, never bootstrap temp."""
    if (system or platform.system()) == 'Windows':
        base = Path(os.environ.get('LOCALAPPDATA') or Path.home() / 'AppData/Local')
    else:
        base = Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local/share')
    _require(base.is_absolute(), 'invalid_path')
    return base / ROOT_NAME


def _plain_absolute(value):
    _require(isinstance(value, (str, os.PathLike)), 'invalid_path')
    value = os.fspath(value)
    _require(type(value) is str and 0 < len(value) <= 1024 and
             not any(ord(c) < 32 or ord(c) == 127 for c in value), 'invalid_path')
    # No traversal, network shares, device names or drive-relative paths.
    win = PureWindowsPath(value)
    posix = PurePosixPath(value)
    _require('..' not in win.parts and '..' not in posix.parts and
             not value.startswith(('\\\\', '//')) and
             (posix.is_absolute() or (win.is_absolute() and len(win.drive) == 2)),
             'invalid_path')
    return value


def _no_links(path):
    for item in (path, *path.parents):
        try:
            info = item.lstat()
        except FileNotFoundError:
            continue
        _require(not stat.S_ISLNK(info.st_mode) and
                 not getattr(info, 'st_file_attributes', 0) & 0x400,
                 'redirected_path')


def _windows_private(path):
    """Owner/system-only DACL, applied before the first diagnostic byte."""
    from ctypes import wintypes
    advapi = ctypes.WinDLL('advapi32', use_last_error=True)
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    convert = advapi.ConvertStringSecurityDescriptorToSecurityDescriptorW
    convert.argtypes = (wintypes.LPCWSTR, wintypes.DWORD,
                        ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p)
    convert.restype = wintypes.BOOL
    apply = advapi.SetFileSecurityW
    apply.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, ctypes.c_void_p)
    apply.restype = wintypes.BOOL
    kernel.LocalFree.argtypes = (ctypes.c_void_p,)
    descriptor = ctypes.c_void_p()
    _require(convert('D:P(A;;FA;;;SY)(A;;FA;;;OW)', 1,
                     ctypes.byref(descriptor), None), 'permission_denied')
    try:
        _require(apply(str(path), 0x80000004, descriptor), 'permission_denied')
    finally:
        kernel.LocalFree(descriptor)


def _open_private(path):
    """Walk POSIX directory handles so a link swap cannot redirect creation."""
    _no_links(path)
    if os.name == 'nt':
        # Do not change an existing user's directory permissions.
        for directory in reversed((path.parent, *path.parent.parents)):
            if not directory.exists():
                directory.mkdir(mode=0o700)
                _windows_private(directory)
        _no_links(path)
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                     getattr(os, 'O_BINARY', 0), 0o600)
        try:
            _windows_private(path)
            _no_links(path)
            _require(_same_file(os.fstat(fd), path.lstat()), 'metadata_changed')
        except BaseException:
            os.close(fd)
            raise
        return fd
    directory = os.open(path.anchor, os.O_RDONLY | os.O_DIRECTORY)
    try:
        components = path.parent.parts[1:]
        for index, component in enumerate(components):
            try:
                os.mkdir(component, 0o700, dir_fd=directory)
            except FileExistsError:
                pass
            child = os.open(component, os.O_RDONLY | os.O_DIRECTORY |
                            os.O_NOFOLLOW, dir_fd=directory)
            os.close(directory)
            directory = child
            info = os.fstat(directory)
            final = index >= len(components) - 2  # PC root and logs.
            _require(info.st_uid in (0, os.getuid()), 'permission_denied')
            if final:
                _require(info.st_uid == os.getuid() and
                         stat.S_IMODE(info.st_mode) & 0o077 == 0,
                         'permission_denied')
            else:
                _require(not info.st_mode & 0o022 or
                         bool(info.st_mode & stat.S_ISVTX), 'permission_denied')
        return os.open(path.name, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                       os.O_NOFOLLOW, 0o600, dir_fd=directory)
    finally:
        os.close(directory)


def _same_file(left, right):
    # Windows path stat and fstat have different ctime/execute-bit conventions.
    return (left.st_dev, left.st_ino, left.st_nlink) == (right.st_dev, right.st_ino, 1)


def _os_code(value):
    return value if type(value) is int and -(2**31) <= value < 2**32 else None


def classify_failure(error):
    """Never call str/repr(error) or inspect arbitrary exception attributes."""
    try:
        number = _os_code(getattr(error, 'errno', None)) if isinstance(error, OSError) else None
        win = _os_code(getattr(error, 'winerror', None)) if isinstance(error, OSError) else None
    except Exception:
        # A hostile/custom exception attribute must not replace the real failure.
        number = win = None
    if isinstance(error, (TimeoutError, subprocess.TimeoutExpired)):
        kind, reason = 'timeout_error', 'timeout'
    elif isinstance(error, PermissionError):
        kind, reason = 'permission_error', 'permission_denied'
    elif isinstance(error, FileNotFoundError):
        kind, reason = 'missing_error', 'missing'
    elif isinstance(error, FileExistsError):
        kind, reason = 'exists_error', 'already_exists'
    elif isinstance(error, InterruptedError):
        kind, reason = 'interrupted_error', 'interrupted'
    elif isinstance(error, KeyboardInterrupt):
        kind, reason = 'cancelled_error', 'cancelled'
    elif isinstance(error, OSError):
        kind, reason = 'os_error', 'sharing_violation' if win in (32, 33) else 'io_error'
    elif isinstance(error, subprocess.SubprocessError):
        kind, reason = 'process_error', 'remote_unavailable'
    elif isinstance(error, (ValueError, TypeError)):
        kind, reason = 'validation_error', 'invalid_input'
    else:
        kind, reason = 'unknown_error', 'unknown_error'
    return {'error_type': kind, 'reason': reason, 'errno': number, 'winerror': win}


def _reason(value):
    return type(value) is str and (value in REASONS or bool(_CHECK.fullmatch(value)))


def _recovery(value):
    _require(type(value) is dict and 0 < len(value) <= 3 and
             set(value) <= {'local', 'pc', 'frame'}, 'invalid_event')
    result = {}
    for role, path in value.items():
        text = _plain_absolute(path)
        normalized = text.replace('\\', '/')
        parts = tuple(part for part in normalized.split('/') if part)
        if role in ('local', 'pc'):
            _require(len(parts) >= 2 and parts[-2] == ROOT_NAME and
                     parts[-1] in ('bridge-install.json', 'setup-last.log',
                                   'setup-inspection.json','setup-restore.json','lifecycle.json'), 'invalid_event')
        else:
            _require(parts[-3:] in (('SteamLinkNativePassthrough', 'stream-bridge', 'install.json'),
                                   ('SteamLinkNativePassthrough', 'stream-bridge', 'setup-restore.json'),
                                   ('SteamLinkNativePassthrough', 'stream-bridge', 'lifecycle.json')) or
                     parts[-2:] == ('SteamFrameNativePassthrough', 'install.json'), 'invalid_event')
        result[role] = text
    return result


def _versions(value):
    _require(type(value)is dict and set(value)=={'pc','frame'},'invalid_event')
    result={}
    for role,record in value.items():
        _require(type(record)is dict and set(record)=={'installed','installed_version','target_version'} and
                 (record['installed'] is None or type(record['installed'])is bool),'invalid_event')
        for key in ('installed_version','target_version'):
            version=record[key]
            _require(version is None or type(version)is str and len(version)<=80 and
                     bool(re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?',version)),
                     'invalid_event')
        result[role]=dict(record)
    return result


def _runtime_checks(value):
    """Retain only fixed component match flags, never runtime observations."""
    roles = {'pc': ('vrlink', 'vrpathreg', '_runtime_versions'),
             'frame': ('vrlink', 'compositor', 'fragment', 'openvr', '_runtime_versions')}
    _require(type(value) is dict and len(value) == len(roles) and
             set(value) == set(roles), 'invalid_event')
    result = {}
    for role, keys in roles.items():
        record = value[role]
        _require(type(record) is dict and len(record) == len(keys) and
                 set(record) == set(keys) and
                 all(type(record[key]) is bool for key in keys), 'invalid_event')
        result[role] = {key: record[key] for key in keys}
    return result


class SessionLog:
    """One exclusive JSONL file, capped at 256 KiB; no log rotation/deletion.

    event() returns False for invalid/unavailable/limited writes. Inspect
    last_event_error (fixed code) or failure_code; no caller exception is replaced.
    Recovery accepts only known receipt-path shapes, never arbitrary file names.
    """
    def __init__(self):
        self.level = 'Debug'
        self.path = None
        self.available = False
        self.failure_code = None
        self.last_event_error = None
        self._fd = None
        self._identity = None
        self._bytes = 0
        self._sequence = 0
        self._limited = False
        self._closed = False
        self._lock = threading.Lock()

    @classmethod
    def open(cls, local_root=None, system=None, *, level=None):
        result = cls()
        if level is None:
            try:
                package=Path(__file__).resolve().parents[1]/'stream-bridge-bundle.json'
                with package.open('rb') as stream:raw=stream.read(128*1024+1)
                if len(raw)<=128*1024:level=json.loads(raw).get('logging',{}).get('level')
            except (OSError,ValueError,TypeError,AttributeError):pass
        candidate=os.environ.get('SFNP_LOG_LEVEL') or level or 'Debug'
        result.level=next((n for n in ('Debug','Important','Warning','Error') if n.lower()==str(candidate).lower()),'Important')
        try:
            root = Path(_plain_absolute(local_root if local_root is not None else default_root(system)))
            _require(root.is_absolute(), 'invalid_path')
            stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
            result.path = root / 'logs' / ('setup-' + stamp + '-' + secrets.token_hex(8) + '.jsonl')
            result._fd = _open_private(result.path)
            result._identity = os.fstat(result._fd)
            _require(stat.S_ISREG(result._identity.st_mode) and
                     result._identity.st_nlink == 1, 'not_regular')
            if os.name != 'nt':
                _require(result._identity.st_uid == os.getuid() and
                         stat.S_IMODE(result._identity.st_mode) == 0o600, 'permission_denied')
            result.available = True
            result.event('session', 'start', status='started')
        except Exception as error:
            result._disable(error.code if isinstance(error, _Refused) else
                            classify_failure(error)['reason'])
        return result

    def _disable(self, code):
        self.available = False
        self.failure_code = code
        if self._fd is not None:
            descriptor, self._fd = self._fd, None
            try:
                os.close(descriptor)
            except OSError:
                pass

    def _write(self, value):
        raw = (json.dumps(value, ensure_ascii=True, separators=(',', ':'),
                          allow_nan=False) + '\n').encode('ascii')
        _require(len(raw) <= MAX_EVENT_BYTES and self._bytes + len(raw) <= MAX_BYTES,
                 'log_limit')
        _no_links(self.path)
        current = os.fstat(self._fd)
        _require(_same_file(self._identity, current) and
                 _same_file(current, self.path.lstat()) and
                 current.st_size == self._bytes and
                 stat.S_ISREG(current.st_mode), 'metadata_changed')
        if os.name != 'nt':
            _require(current.st_uid == os.getuid() and
                     stat.S_IMODE(current.st_mode) == 0o600, 'permission_denied')
        cursor = 0
        while cursor < len(raw):
            wrote = os.write(self._fd, raw[cursor:])
            _require(wrote > 0, 'io_error')
            cursor += wrote
        self._bytes += len(raw)
        os.fsync(self._fd)

    def event(self, stage, kind, *, status=None, command=None, reason=None,
              error_type=None, errno=None, winerror=None, recovery=None, action=None,versions=None,
              runtime_checks=None):
        with self._lock:
            self.last_event_error = None
            if not self.available or self._closed or self._limited:
                self.last_event_error = self.failure_code or ('closed' if self._closed else 'log_limit')
                return False
            try:
                _require(type(stage) is str and stage in STAGES and
                         type(kind) is str and kind in KINDS, 'invalid_event')
                value = {'schema': SCHEMA, 'sequence': self._sequence,
                         'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='milliseconds'),
                         'stage': stage, 'kind': kind}
                for key, candidate, allowed in (('status', status, STATUSES),
                                                 ('command', command, COMMANDS),
                                                 ('action', action, ACTIONS),
                                                 ('error_type', error_type, ERROR_TYPES)):
                    if candidate is not None:
                        _require(type(candidate) is str and candidate in allowed, 'invalid_event')
                        value[key] = candidate
                if reason is not None:
                    _require(_reason(reason), 'invalid_event')
                    value['reason'] = reason
                for key, number in (('errno', errno), ('winerror', winerror)):
                    if number is not None:
                        _require(_os_code(number) is not None, 'invalid_event')
                        value[key] = number
                if recovery is not None:
                    value['recovery'] = _recovery(recovery)
                if versions is not None:value['versions']=_versions(versions)
                if runtime_checks is not None:
                    value['runtime_checks'] = _runtime_checks(runtime_checks)
                # Validate/redact before filtering. Quiet logs retain failures,
                # cancellation and recovery; routine polling/stages are Debug.
                severity=('Error' if kind=='failure' else 'Warning' if kind=='recovery' or
                          status in ('failed','refused','partial','cancelled','unavailable') else
                          'Important' if kind in ('start','complete') or status=='pending' or versions is not None else 'Debug')
                if ('Error','Warning','Important','Debug').index(severity) > ('Error','Warning','Important','Debug').index(self.level):
                    return True
                # Reserve one final, guaranteed limit marker instead of silently
                # growing or overwriting old evidence. No feature timer is used.
                if self._bytes + MAX_EVENT_BYTES > MAX_BYTES - _LIMIT_RESERVE:
                    self._write({'schema': SCHEMA, 'sequence': self._sequence,
                                 'stage': 'session', 'kind': 'result',
                                 'status': 'partial', 'reason': 'log_limit'})
                    self._sequence += 1
                    self._limited = True
                    self.last_event_error = 'log_limit'
                    return False
                self._write(value)
                self._sequence += 1
                return True
            except _Refused as error:
                self.last_event_error = error.code
                if error.code != 'invalid_event' and error.code != 'invalid_path':
                    self._disable(error.code)
                return False
            except Exception as error:
                self.last_event_error = classify_failure(error)['reason']
                self._disable(self.last_event_error)
                return False

    def record_failure(self, stage, error, *, reason=None, command=None, recovery=None):
        fields = classify_failure(error)
        if reason is not None:
            fields['reason'] = reason
        return self.event(stage, 'failure', status='failed', command=command,
                          recovery=recovery, **fields)

    def close(self):
        with self._lock:
            if self._fd is not None:
                descriptor, self._fd = self._fd, None
                try:
                    os.close(descriptor)
                except OSError:
                    self.failure_code = self.failure_code or 'io_error'
            self._closed = True
            self.available = False

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
