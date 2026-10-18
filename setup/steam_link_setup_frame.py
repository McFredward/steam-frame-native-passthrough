"""Explicit user-service lifecycle for a reviewed Frame setup transaction.

Import and snapshot are read-only. The caller owns Apply authorization, file/
settings rollback and the independent native startup-health check. This module
never claims that service ActiveState alone proves a usable headset picture.
"""
from __future__ import annotations

import os
from pathlib import Path
import re
import stat
import subprocess
import time

SCHEMA = 'sfnp-frame-lifecycle-v1'
SNAPSHOT = 'sfnp-frame-runtime-v1'
UNITS = ('steam.service', 'steamvr.service')
PROPERTIES = ('Id', 'LoadState', 'ActiveState', 'SubState', 'InvocationID',
              'ActiveEnterTimestampMonotonic', 'MainPID', 'Result')
PHASES = frozenset(('prepared', 'restarting-steam', 'restarting-vr', 'awaiting-health'))
ACTIVE = frozenset(('active', 'inactive', 'failed', 'activating', 'deactivating',
                    'reloading', 'maintenance'))
SUBSTATES = frozenset(('dead', 'condition', 'start-pre', 'start', 'start-post',
    'running', 'exited', 'reload', 'reload-signal', 'reload-notify', 'stop',
    'stop-watchdog', 'stop-sigterm', 'stop-sigkill', 'stop-post', 'final-watchdog',
    'final-sigterm', 'final-sigkill', 'failed', 'dead-before-auto-restart',
    'failed-before-auto-restart', 'dead-resources-pinned', 'auto-restart',
    'auto-restart-queued', 'cleaning', 'maintenance'))
RESULTS = frozenset(('success', 'resources', 'protocol', 'timeout', 'exit-code',
    'signal', 'core-dump', 'watchdog', 'start-limit-hit', 'dependency', 'oom-kill',
    'exec-condition', 'assert', 'condition', 'skipped'))


class FrameLifecycle:
    """Fixed systemctl adapter and a private, compare-before-write journal.

    ``engine`` supplies SetupError, require, no_links, read(private=True), parse,
    encoded, put(expected=...) and remove_owned. ``runner`` has subprocess.run's
    signature; no arbitrary command, environment or shell is accepted. Methods
    are serialized by the setup's existing Apply/RPC transaction ownership.
    ``restored(health_ok=True)`` must only follow the caller's verified intended
    file/settings state AND a fresh independent native health check.
    """
    def __init__(self, engine, root, *, runner=subprocess.run,
                 sleeper=time.sleep, monotonic=time.monotonic,
                 poll_attempts=60, poll_interval=.25, phase_timeout=30):
        self.e = engine
        self.root = engine.no_links(Path(root))
        engine.require(self.root.is_absolute(), 'A private Frame recovery directory is required.')
        engine.require(type(poll_attempts) is int and 1 <= poll_attempts <= 120 and
                       type(poll_interval) in (int, float) and 0 < poll_interval <= 1 and
                       type(phase_timeout) in (int, float) and 0 < phase_timeout <= 60,
                       'Frame lifecycle polling bounds are invalid.')
        self.runner, self.sleeper, self.clock = runner, sleeper, monotonic
        self.attempts, self.interval, self.timeout = poll_attempts, poll_interval, phase_timeout
        self.path = self.root / 'lifecycle.json'
        self._raw = self._value = None

    def _run(self, args, *, output=False):
        try:
            result = self.runner(['/usr/bin/systemctl', '--user', *args],
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE if output else subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, timeout=5, check=False, shell=False)
        except (OSError, subprocess.SubprocessError):
            raise self.e.SetupError('Frame service command failed. Recovery details were kept.') from None
        self.e.require(type(result.returncode) is int and result.returncode == 0,
                       'Frame service command failed. Recovery details were kept.')
        if not output:
            return b''
        raw = result.stdout
        self.e.require(type(raw) in (str, bytes), 'Frame service status could not be checked safely.')
        try:
            raw = raw.decode('ascii') if type(raw) is bytes else raw
            self.e.require(len(raw) <= 4096 and raw.isascii(),
                           'Frame service status could not be checked safely.')
        except UnicodeError:
            raise self.e.SetupError('Frame service status could not be checked safely.') from None
        return raw

    def _validate_unit(self, value, unit):
        e = self.e
        e.require(type(value) is dict and set(value) == set(PROPERTIES) and
                  all(type(item) is str for item in value.values()),
                  'The Frame service status format is unsupported.')
        e.require(value['Id'] == unit and value['LoadState'] == 'loaded',
                  'The expected Frame user services are unavailable. Existing files were preserved.')
        e.require(value['ActiveState'] in ACTIVE and value['SubState'] in SUBSTATES and
                  value['Result'] in RESULTS,
                  'A Frame service has an unsupported state. Existing files were preserved.')
        e.require(bool(re.fullmatch('[0-9a-f]{32}|', value['InvocationID'])) and
                  bool(re.fullmatch('[0-9]{1,20}', value['ActiveEnterTimestampMonotonic'])) and
                  int(value['ActiveEnterTimestampMonotonic']) <= 2**64 - 1 and
                  bool(re.fullmatch('[0-9]{1,10}', value['MainPID'])) and int(value['MainPID']) <= 2**31 - 1,
                  'The Frame service generation could not be checked safely.')
        return value

    def _show(self, unit):
        raw = self._run(['show', '--no-pager', '--all', '--property=' + ','.join(PROPERTIES), unit], output=True)
        value = {}
        for line in raw.splitlines():
            key, separator, item = line.partition('=')
            self.e.require(separator == '=' and key in PROPERTIES and key not in value,
                           'The Frame service status format is unsupported.')
            value[key] = item
        return self._validate_unit(value, unit)

    def _units(self):
        return {unit:self._show(unit) for unit in UNITS}

    @staticmethod
    def _running(value):
        return value['ActiveState'] == 'active' and value['SubState'] == 'running' and int(value['MainPID']) > 0

    @staticmethod
    def _stopped(value):
        return value['ActiveState'] == 'inactive' and value['SubState'] == 'dead'

    def _load(self):
        self.e.no_links(self.path)
        self._raw = self._value = None
        if not os.path.lexists(self.path):
            return None
        raw = self.e.read(self.path, 16384, private=True)
        value = self.e.parse(raw)
        self.e.require(set(value) == {'schema', 'baseline', 'restart_steam', 'phase'} and
                       value['schema'] == SCHEMA and value['restart_steam'] is True and
                       type(value['phase']) is str and value['phase'] in PHASES and type(value['baseline']) is dict and
                       set(value['baseline']) == set(UNITS),
                       'Unknown Frame lifecycle recovery record preserved.')
        for unit in UNITS:
            self._validate_unit(value['baseline'][unit], unit)
        self._raw, self._value = raw, value
        return value

    def _private_root(self):
        self.e.no_links(self.root)
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        info = self.root.lstat()
        self.e.require(stat.S_ISDIR(info.st_mode) and info.st_uid == os.getuid() and
                       stat.S_IMODE(info.st_mode) == 0o700,
                       'A private owned Frame recovery directory is required.')

    def _sync_directory(self):
        try:
            descriptor = os.open(self.root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
            try:
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
        except OSError:
            raise self.e.SetupError('Frame recovery details could not be made durable; no further service changes were made.') from None

    def _save(self, value):
        self._private_root()
        raw = self.e.encoded(value)
        self.e.put(self.path, raw, expected=self._raw)
        self._sync_directory()
        self._raw, self._value = raw, value

    def _phase(self, phase):
        value = dict(self._value)
        value['phase'] = phase
        self._save(value)

    def _wait(self, unit, predicate, deadline):
        for attempt in range(self.attempts):
            value = self._show(unit)
            if predicate(value):
                return value
            if attempt + 1 == self.attempts or self.clock() >= deadline:
                break
            self.sleeper(min(self.interval, max(0, deadline - self.clock())))
        if unit == 'steam.service':
            raise self.e.SetupError('Frame Steam did not restart. Recovery details were kept.')
        raise self.e.SetupError('Frame SteamVR did not restart. Recovery details were kept.')

    def _result(self, units, *, complete=False, healthy=False):
        return {'schema':SNAPSHOT, 'units':units, 'running':all(self._running(v) for v in units.values()),
                'pending':self._value is not None, 'phase':self._value['phase'] if self._value else None,
                'complete':complete, 'healthy':healthy, 'needs_health_check':not healthy}

    def snapshot(self):
        self._load()
        return self._result(self._units())

    @classmethod
    def _fresh_running(cls, value, previous):
        return cls._running(value) and bool(value['InvocationID']) and value['InvocationID'] != previous['InvocationID']

    def restart(self):
        """Activate already staged files in one immediate coordinated restart.

        No stopped/intervention phase exists. Steam is restarted once; SteamVR
        is not restarted again if that operation already gave it a fresh running
        generation. The caller stages or rolls back files atomically FIRST.
        """
        self._load()
        before = self._units()
        if self._value is None:
            self._save({'schema':SCHEMA, 'baseline':before, 'restart_steam':True, 'phase':'prepared'})
        deadline = self.clock() + self.timeout
        self._phase('restarting-steam')
        if before['steam.service']['ActiveState'] == 'failed':
            self._run(['reset-failed', 'steam.service'])
        self._run(['restart', '--no-block', 'steam.service'])
        self._wait('steam.service', lambda value:self._fresh_running(value, before['steam.service']), deadline)
        value = self._show('steamvr.service')
        if value['ActiveState'] in ('activating', 'deactivating', 'reloading'):
            value = self._wait('steamvr.service',
                lambda item:self._running(item) or item['ActiveState'] in ('inactive', 'failed'), deadline)
        if not self._fresh_running(value, before['steamvr.service']):
            self._phase('restarting-vr')
            if value['ActiveState'] == 'failed':
                self._run(['reset-failed', 'steamvr.service'])
            action = 'restart' if value['ActiveState'] in ('active', 'reloading') else 'start'
            self._run([action, '--no-block', 'steamvr.service'])
            self._wait('steamvr.service', lambda item:self._fresh_running(item, before['steamvr.service']), deadline)
        self._phase('awaiting-health')
        return self._result(self._units())

    def restore(self):
        # Undo restores a usable Frame, even if a service happened to be stopped
        # at inventory. The caller first restores exact original files/settings.
        return self.restart()

    def restored(self, *, health_ok=False):
        self.e.require(type(health_ok) is bool, 'A fresh Frame startup health result is required.')
        self._load()
        units = self._units()
        self.e.require(all(self._running(v) for v in units.values()),
                       'Frame Steam and SteamVR are not both running. Recovery details were kept.')
        if not health_ok:
            return self._result(units)
        if self._value is not None:
            self.e.require(self._value['phase'] == 'awaiting-health',
                           'Frame recovery is not ready for its final health confirmation.')
            self.e.remove_owned(self.path, self._raw)
            self._sync_directory()
            self._raw = self._value = None
        return self._result(units, complete=True, healthy=True)
