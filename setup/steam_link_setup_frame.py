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
PHASES = frozenset(('prepared', 'stopping-steam', 'refreshing-vr', 'starting-steam',
                    'restarting-steam', 'restarting-vr', 'awaiting-health'))
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
                 poll_attempts=120, poll_interval=.25, phase_timeout=30):
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
        raw = self.e.read(self.path, 128 * 1024, private=True)
        value = self.e.parse(raw)
        self.e.require(set(value) in ({'schema', 'baseline', 'restart_steam', 'phase'},
                                   {'schema', 'baseline', 'restart_steam', 'phase', 'activation'}) and
                       value['schema'] == SCHEMA and value['restart_steam'] is True and
                       type(value['phase']) is str and value['phase'] in PHASES and type(value['baseline']) is dict and
                       set(value['baseline']) == set(UNITS),
                       'Unknown Frame lifecycle recovery record preserved.')
        for unit in UNITS:
            self._validate_unit(value['baseline'][unit], unit)
        if 'activation' in value:
            activation = value['activation']
            self.e.require(type(activation) is dict and set(activation) in ({'kind', 'baseline'}, {'kind', 'baseline', 'health'}) and
                           activation['kind'] in ('prepared', 'original') and
                           type(activation['baseline']) is dict and set(activation['baseline']) == set(UNITS),
                           'Unknown Frame activation recovery record preserved.')
            for unit in UNITS:
                self._validate_unit(activation['baseline'][unit], unit)
            if 'health' in activation:
                self._health_receipt(activation['health'])
        self._raw, self._value = raw, value
        return value

    def _health_receipt(self, value):
        self.e.require(type(value) is dict and set(value) == {'baseline', 'checkpoint'} and
                       type(value['baseline']) is dict and type(value['checkpoint']) is dict,
                       'A bounded startup health receipt is required.')
        raw = self.e.encoded(value)
        self.e.require(len(raw) <= 64 * 1024, 'The startup health receipt exceeds its bound.')
        # Preserve a private immutable JSON snapshot, not the caller's mutable
        # dict. Detailed native evidence is validated by the existing observer.
        return self.e.parse(raw)

    @staticmethod
    def _activation(kind, baseline, health):
        value = {'kind':kind, 'baseline':baseline}
        if health is not None:value['health'] = health
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

    def _wait(self, unit, predicate, deadline, *, stopping=False):
        for attempt in range(self.attempts):
            value = self._show(unit)
            if predicate(value):
                return value
            if attempt + 1 == self.attempts or self.clock() >= deadline:
                break
            self.sleeper(min(self.interval, max(0, deadline - self.clock())))
        if unit == 'steam.service':
            if stopping:
                raise self.e.SetupError('Frame Steam did not stop. Recovery details were kept.')
            raise self.e.SetupError('Frame Steam did not restart. Recovery details were kept.')
        raise self.e.SetupError('Frame SteamVR did not restart. Recovery details were kept.')

    def _result(self, units, *, complete=False, healthy=False):
        result = {'schema':SNAPSHOT, 'units':units, 'running':all(self._running(v) for v in units.values()),
                'pending':self._value is not None, 'phase':self._value['phase'] if self._value else None,
                'complete':complete, 'healthy':healthy, 'needs_health_check':not healthy}
        activation = self._value.get('activation', {}) if self._value else {}
        if 'health' in activation:result['health_receipt'] = self._health_receipt(activation['health'])
        return result

    def snapshot(self):
        self._load()
        return self._result(self._units())

    @classmethod
    def _fresh_running(cls, value, previous):
        return cls._running(value) and bool(value['InvocationID']) and value['InvocationID'] != previous['InvocationID']

    def _stop_client(self):
        steam = self._show('steam.service')
        if steam['ActiveState'] == 'failed':
            self._run(['reset-failed', 'steam.service'])
            steam = self._show('steam.service')
        if not self._stopped(steam):
            # Repeating an uncertain stop is idempotent; unlike restart it
            # cannot manufacture another loaded VR/client generation.
            self._run(['stop', '--no-block', 'steam.service'])
            self._wait('steam.service', self._stopped, self.clock() + self.timeout, stopping=True)

    def restart(self, *, restoring=False, health_receipt=None):
        """Activate staged files without restarting VR under a running client.

        Steam explicitly depends on SteamVR. Stop the client, make the already
        staged VR generation ready, then start the client. The durable activation
        identity distinguishes an interrupted RPC from activating restored files;
        neither a fresh VR invocation nor a completed activation is replayed.
        """
        self.e.require(type(restoring) is bool, 'An exact Frame activation intent is required.')
        health = self._health_receipt(health_receipt) if health_receipt is not None else None
        self._load()
        before = self._units()
        kind = 'original' if restoring else 'prepared'
        prior = self._value
        if prior is None:
            self._save({'schema':SCHEMA, 'baseline':before, 'restart_steam':True, 'phase':'prepared',
                        'activation':self._activation(kind, before, health)})
        elif 'activation' not in prior:
            self.e.require(health is None or restoring or prior['phase'] == 'prepared',
                           'Fresh startup health authority requires a new Frame activation intent; recovery details were kept.')
            # Old journal phases remain recoverable. A finished old activation
            # only awaits its independent health check; never replay it merely
            # because its RPC response was lost. An explicit Undo is a new
            # activation after original files have already been restored.
            if prior['phase'] == 'awaiting-health' and not restoring:
                self.e.require(all(self._running(v) for v in before.values()),
                               'Frame Steam and SteamVR are not both running. Recovery details were kept.')
                return self._result(before)
            baseline = dict(before)
            if not restoring and health is None:
                baseline['steamvr.service'] = prior['baseline']['steamvr.service']
            value = dict(prior)
            value.update(phase='prepared', activation=self._activation(kind, baseline, health))
            self._save(value)
        elif prior['activation']['kind'] != kind:
            value = dict(prior)
            value.update(phase='prepared', activation=self._activation(kind, before, health))
            self._save(value)
        elif health is not None and 'health' not in prior['activation']:
            self.e.require(prior['phase'] == 'prepared',
                           'Fresh startup health authority requires a new Frame activation intent; recovery details were kept.')
            value = dict(prior)
            value['activation'] = self._activation(kind, before, health)
            self._save(value)
        # The same activation always retains its original health baseline and
        # log checkpoint, even after a lost reply or a new setup/SSH process.
        # A newly observed current generation cannot replace that authority.

        phase = self._value['phase']
        baseline = self._value['activation']['baseline']
        if phase == 'awaiting-health':
            self.e.require(all(self._running(v) for v in before.values()),
                           'Frame Steam and SteamVR are not both running. Recovery details were kept.')
            return self._result(before)

        if phase in ('prepared', 'stopping-steam', 'restarting-steam'):
            self._phase('stopping-steam')
            self._stop_client()
            self._phase('refreshing-vr')
            phase = 'refreshing-vr'

        if phase in ('refreshing-vr', 'restarting-vr'):
            # A caller/user may have restarted Steam while SSH was away. Keep
            # any already-issued VR request phase, but never refresh under it.
            self._stop_client()
            vr_deadline = self.clock() + self.timeout
            vr = self._show('steamvr.service')
            if vr['ActiveState'] in ('activating', 'deactivating', 'reloading'):
                vr = self._wait('steamvr.service',
                    lambda item:self._running(item) or item['ActiveState'] in ('inactive', 'failed'),
                    vr_deadline)
            if not self._fresh_running(vr, baseline['steamvr.service']):
                # restarting-vr means the command may already have reached
                # systemd before SSH lost its reply. Observe that request; an
                # unchanged old active invocation is not permission to replay it.
                if phase == 'restarting-vr' and vr['ActiveState'] != 'failed':
                    vr = self._wait('steamvr.service',
                        lambda item:self._fresh_running(item, baseline['steamvr.service']),
                        vr_deadline)
                else:
                    self._phase('restarting-vr')
                    if vr['ActiveState'] == 'failed':
                        self._run(['reset-failed', 'steamvr.service'])
                    action = 'restart' if vr['ActiveState'] in ('active', 'reloading') else 'start'
                    self._run([action, '--no-block', 'steamvr.service'])
                    vr = self._wait('steamvr.service',
                        lambda item:self._fresh_running(item, baseline['steamvr.service']),
                        vr_deadline)
            self._phase('starting-steam')

        self.e.require(self._fresh_running(self._show('steamvr.service'), baseline['steamvr.service']),
                       'Frame SteamVR is not ready for its client. Recovery details were kept.')
        steam = self._show('steam.service')
        if not self._fresh_running(steam, baseline['steam.service']):
            if steam['ActiveState'] == 'failed':
                self._run(['reset-failed', 'steam.service'])
            # start is deliberately idempotent on an already queued/running
            # client; never issue a second restart after an uncertain reply.
            self._run(['start', '--no-block', 'steam.service'])
            self._wait('steam.service',
                lambda item:self._fresh_running(item, baseline['steam.service']),
                self.clock() + self.timeout)
        self._phase('awaiting-health')
        units = self._units()
        self.e.require(all(self._running(v) for v in units.values()),
                       'Frame Steam and SteamVR are not both running. Recovery details were kept.')
        return self._result(units)

    def restore(self, *, health_receipt=None):
        # The caller has restored the exact original files/settings FIRST.
        # A distinct activation intent must not reuse the prepared generation.
        return self.restart(restoring=True, health_receipt=health_receipt)

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
