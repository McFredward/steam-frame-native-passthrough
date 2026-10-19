#!/usr/bin/env python3
"""Real lifecycle module with a fixed-command fake; no device/service access."""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


engine = load(ROOT / 'setup/steam_link_setup.py', 'sfnp_frame_test_engine')
frame = load(Path(os.environ.get('SFNP_LIFECYCLE_TEST_SOURCE', ROOT / 'setup/steam_link_setup_frame.py')), 'sfnp_setup_frame')


class System:
    def __init__(self):
        self.calls, self.actions = [], []
        self.generation, self.now = 2, 0
        self.auto_vr = False
        self.keep_steam_running = False
        self.require_fresh_vr = False
        self.unready_client_starts = 0
        self.live_client_vr_restarts = 0
        self.lost_reply_after = None
        self.fail_vr = self.fail_steam = False
        self.command_error = False
        self.corrupt_status = None
        self.before_action = None
        self.units = {unit:self.value(unit, n + 1) for n, unit in enumerate(frame.UNITS)}

    def value(self, unit, generation, *, active='active', sub='running'):
        return dict(Id=unit, LoadState='loaded', ActiveState=active, SubState=sub,
                    InvocationID=format(generation, '032x'),
                    ActiveEnterTimestampMonotonic=str(generation * 1000),
                    MainPID=str(100 + generation if active == 'active' else 0), Result='success')

    def fresh(self, unit):
        self.generation += 1
        self.units[unit] = self.value(unit, self.generation)

    def __call__(self, argv, **kwargs):
        self.calls.append((list(argv), dict(kwargs)))
        assert argv[:2] == ['/usr/bin/systemctl', '--user']
        assert kwargs == dict(stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE if argv[2] == 'show' else subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, timeout=5, check=False, shell=False)
        unit, action = argv[-1], argv[2]
        assert unit in frame.UNITS
        if self.command_error:
            return subprocess.CompletedProcess(argv, 9, stdout=b'PRIVATE_UNTRUSTED_ERROR')
        if action == 'show':
            assert argv[3:-1] == ['--no-pager', '--all', '--property=' + ','.join(frame.PROPERTIES)]
            raw = self.corrupt_status
            if raw is None:
                raw = ''.join(key + '=' + self.units[unit][key] + '\n' for key in frame.PROPERTIES).encode()
            return subprocess.CompletedProcess(argv, 0, stdout=raw)
        assert action in ('restart', 'start', 'stop', 'reset-failed')
        assert argv[3:-1] == ([] if action == 'reset-failed' else ['--no-block'])
        self.actions.append((action, unit))
        if self.before_action:
            self.before_action(action, unit)
        if action == 'reset-failed':
            self.units[unit]['ActiveState'], self.units[unit]['SubState'] = 'inactive', 'dead'
            self.units[unit]['MainPID'], self.units[unit]['Result'] = '0', 'success'
        elif action == 'stop':
            if not self.keep_steam_running:
                self.units[unit].update(ActiveState='inactive', SubState='dead', MainPID='0')
            if unit == 'steam.service' and self.auto_vr:
                self.fresh('steamvr.service')
        elif unit == 'steam.service' and self.require_fresh_vr and self.units['steamvr.service']['InvocationID'] == format(2, '032x'):
            self.unready_client_starts += 1
            self.units[unit].update(ActiveState='failed', SubState='failed', MainPID='0', Result='exit-code')
        elif self.fail_vr and unit == 'steamvr.service' or self.fail_steam and unit == 'steam.service':
            self.units[unit]['ActiveState'], self.units[unit]['SubState'] = 'failed', 'failed'
            self.units[unit]['MainPID'], self.units[unit]['Result'] = '0', 'start-limit-hit'
        else:
            if unit == 'steamvr.service' and self.units['steam.service']['ActiveState'] == 'active':
                self.live_client_vr_restarts += 1
            # A real systemd start does not restart an active running service.
            if action == 'restart' or self.units[unit]['ActiveState'] != 'active':
                self.fresh(unit)
        if self.lost_reply_after == (action, unit):
            self.lost_reply_after = None
            raise subprocess.TimeoutExpired(argv, 5)
        return subprocess.CompletedProcess(argv, 0, stdout=None)

    def sleep(self, duration):
        self.now += duration


@unittest.skipUnless(os.name == 'posix', 'The helper runs only on the Linux Frame.')
class LifecycleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='sfnp-frame-lifecycle-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.system = System()
        self.lifecycle = self.instance()
        self.journal = self.root / 'lifecycle.json'

    def instance(self):
        return frame.FrameLifecycle(engine, self.root, runner=self.system,
            sleeper=self.system.sleep, monotonic=lambda:self.system.now,
            poll_attempts=4, poll_interval=.01, phase_timeout=1)

    def record(self):
        return engine.parse(engine.read(self.journal, private=True))

    def test_import_constructor_and_inventory_do_not_write_or_change_services(self):
        self.assertEqual(self.system.calls, [])
        before = list(self.root.iterdir())
        snapshot = self.lifecycle.snapshot()
        self.assertTrue(snapshot['running'])
        self.assertFalse(snapshot['pending'])
        self.assertTrue(snapshot['needs_health_check'])
        self.assertEqual(self.system.actions, [])
        self.assertEqual(list(self.root.iterdir()), before)
        self.assertFalse(hasattr(self.lifecycle, 'stop'), 'No stopped/intervention workflow is offered.')

    def test_one_coordinated_restart_preserves_original_baseline_and_starts_both(self):
        baseline = json.loads(json.dumps(self.system.units))
        self.system.require_fresh_vr = True
        seen = []
        self.system.before_action = lambda action, unit:seen.append(self.record()['phase'])
        result = self.lifecycle.restart()
        self.assertEqual(self.system.actions, [('stop', 'steam.service'), ('restart', 'steamvr.service'), ('start', 'steam.service')])
        self.assertEqual(seen, ['stopping-steam', 'restarting-vr', 'starting-steam'])
        self.assertEqual(self.system.unready_client_starts, 0)
        self.assertEqual(self.system.live_client_vr_restarts, 0)
        self.assertTrue(result['running'])
        self.assertTrue(result['pending'])
        self.assertFalse(result['complete'])
        self.assertTrue(result['needs_health_check'])
        self.assertEqual(self.record()['baseline'], baseline)
        self.assertEqual(self.record()['phase'], 'awaiting-health')
        self.assertEqual(self.journal.stat().st_mode & 0o777, 0o600)

    def test_steam_managed_vr_restart_is_not_repeated(self):
        self.system.auto_vr = True
        result = self.lifecycle.restart()
        self.assertEqual(self.system.actions, [('stop', 'steam.service'), ('start', 'steam.service')])
        self.assertTrue(result['running'])
        self.assertEqual(result['phase'], 'awaiting-health')

    def test_old_active_generation_is_not_mistaken_for_success(self):
        original = self.system.fresh
        self.system.fresh = lambda unit:None if unit == 'steam.service' else original(unit)
        with self.assertRaisesRegex(engine.SetupError, 'Steam did not restart'):
            self.lifecycle.restart()
        self.assertEqual(self.system.actions, [('stop', 'steam.service'), ('restart', 'steamvr.service'), ('start', 'steam.service')])
        self.assertEqual(self.record()['phase'], 'starting-steam')

    def test_failed_vr_start_keeps_recovery_and_retry_restores_runtime(self):
        self.system.fail_vr = True
        baseline = json.loads(json.dumps(self.system.units))
        with self.assertRaisesRegex(engine.SetupError, 'SteamVR did not restart'):
            self.lifecycle.restart()
        self.assertEqual(self.record()['phase'], 'restarting-vr')
        self.assertEqual(self.record()['baseline'], baseline)
        self.assertFalse(self.lifecycle.snapshot()['running'])
        self.system.fail_vr = False  # Caller has atomically restored its files/settings.
        recovered = self.instance().restore()
        self.assertTrue(recovered['running'])
        self.assertFalse(recovered['complete'])
        self.assertEqual(self.system.actions[-3:], [('reset-failed', 'steamvr.service'),
            ('start', 'steamvr.service'), ('start', 'steam.service')])
        self.assertEqual(self.record()['baseline'], baseline, 'Retry cannot replace original recovery baseline.')
        confirmed = self.instance().restored(health_ok=True)
        self.assertTrue(confirmed['healthy'] and confirmed['complete'])
        self.assertFalse(confirmed['pending'])
        self.assertFalse(self.journal.exists())

    def test_failed_steam_start_is_recoverable_after_original_files_return(self):
        self.system.fail_steam = True
        with self.assertRaises(engine.SetupError): self.lifecycle.restart()
        self.system.fail_steam = False
        result = self.instance().restore()
        self.assertTrue(result['running'])
        self.assertIn(('reset-failed', 'steam.service'), self.system.actions)
        self.assertTrue(self.journal.exists(), 'Services alone never complete rollback.')

    def test_health_rejection_keeps_journal_and_requires_both_services_running(self):
        self.lifecycle.restart()
        raw = engine.read(self.journal)
        result = self.lifecycle.restored(health_ok=False)
        self.assertFalse(result['healthy'] or result['complete'])
        self.assertEqual(engine.read(self.journal), raw)
        self.system.units['steamvr.service']['ActiveState'] = 'inactive'
        self.system.units['steamvr.service']['SubState'] = 'dead'
        self.system.units['steamvr.service']['MainPID'] = '0'
        with self.assertRaisesRegex(engine.SetupError, 'not both running'):
            self.lifecycle.restored(health_ok=True)
        self.assertEqual(engine.read(self.journal), raw)

    def test_undo_restarts_services_even_when_original_inventory_was_stopped(self):
        for value in self.system.units.values():
            value.update(ActiveState='inactive', SubState='dead', MainPID='0')
        result = self.lifecycle.restore()
        self.assertTrue(result['running'])
        self.assertEqual(self.system.actions, [('start', 'steamvr.service'), ('start', 'steam.service')])
        self.assertEqual(self.record()['baseline']['steam.service']['ActiveState'], 'inactive')

    def test_unknown_or_missing_units_are_preserved_before_journal_or_action(self):
        for key, value in (('LoadState', 'not-found'), ('Id', 'foreign.service'),
                           ('ActiveState', 'unknown'), ('SubState', 'unexpected'),
                           ('InvocationID', 'PRIVATE_STRING'), ('MainPID', '1;bad')):
            original = self.system.units['steamvr.service'][key]
            self.system.units['steamvr.service'][key] = value
            with self.subTest(key=key), self.assertRaises(engine.SetupError): self.lifecycle.restart()
            self.system.units['steamvr.service'][key] = original
            self.assertEqual(self.system.actions, [])
            self.assertFalse(self.journal.exists())

    def test_steam_stop_timeout_never_refreshes_vr_or_starts_client(self):
        self.system.keep_steam_running = True
        with self.assertRaisesRegex(engine.SetupError, 'Steam did not stop'):
            self.lifecycle.restart()
        self.assertEqual(self.system.actions, [('stop', 'steam.service')])
        self.assertEqual(self.record()['phase'], 'stopping-steam')
        self.assertEqual(self.system.units['steamvr.service']['InvocationID'], format(2, '032x'))

    def test_completed_activation_is_reconciled_without_another_restart(self):
        self.lifecycle.restart()
        actions = list(self.system.actions)
        before = json.loads(json.dumps(self.system.units))
        result = self.instance().restart()
        self.assertTrue(result['running'] and result['pending'])
        self.assertEqual(self.system.actions, actions)
        self.assertEqual(self.system.units, before)

    def test_lost_vr_restart_reply_reconciles_fresh_generation_without_replay(self):
        self.system.lost_reply_after = ('restart', 'steamvr.service')
        with self.assertRaises(engine.SetupError): self.lifecycle.restart()
        self.assertEqual(self.record()['phase'], 'restarting-vr')
        before = self.system.units['steamvr.service']['InvocationID']
        result = self.instance().restart()
        self.assertTrue(result['running'])
        self.assertEqual(self.system.actions, [('stop', 'steam.service'),
            ('restart', 'steamvr.service'), ('start', 'steam.service')])
        self.assertEqual(self.system.units['steamvr.service']['InvocationID'], before)
        self.assertEqual(self.system.live_client_vr_restarts, 0)

    def test_unknown_vr_command_reply_cannot_repeat_old_or_inactive_generation(self):
        for active, sub in [('active', 'running'), ('inactive', 'dead')]:
            with self.subTest(active=active):
                baseline = json.loads(json.dumps(self.system.units))
                value = {'schema':frame.SCHEMA, 'baseline':baseline, 'restart_steam':True,
                    'phase':'restarting-vr', 'activation':{'kind':'prepared', 'baseline':baseline}}
                engine.put(self.journal, engine.encoded(value))
                self.system.units['steam.service'].update(ActiveState='inactive', SubState='dead', MainPID='0')
                self.system.units['steamvr.service'].update(ActiveState=active, SubState=sub,
                    MainPID='102' if active == 'active' else '0')
                with self.assertRaisesRegex(engine.SetupError, 'SteamVR did not restart'):
                    self.instance().restart()
                self.assertEqual(self.system.actions, [], 'An unknown accepted request is observed, not replayed.')
                self.assertEqual(self.record()['phase'], 'restarting-vr')
                self.journal.unlink()
                self.system = System()

    def test_lost_client_start_reply_does_not_repeat_vr_or_client_generation(self):
        self.system.lost_reply_after = ('start', 'steam.service')
        with self.assertRaises(engine.SetupError): self.lifecycle.restart()
        self.assertEqual(self.record()['phase'], 'starting-steam')
        before = json.loads(json.dumps(self.system.units))
        result = self.instance().restart()
        self.assertTrue(result['running'])
        self.assertEqual(self.system.units, before)
        self.assertEqual(self.system.actions, [('stop', 'steam.service'),
            ('restart', 'steamvr.service'), ('start', 'steam.service')])

    def test_restore_is_new_activation_but_repeated_restore_only_reconciles(self):
        self.lifecycle.restart()
        baseline = self.record()['baseline']
        prepared = json.loads(json.dumps(self.system.units))
        result = self.instance().restore()
        self.assertTrue(result['running'])
        self.assertEqual(self.record()['baseline'], baseline)
        self.assertEqual(self.record()['activation'], {'kind':'original', 'baseline':prepared})
        self.assertNotEqual(self.system.units['steamvr.service']['InvocationID'], prepared['steamvr.service']['InvocationID'])
        actions = list(self.system.actions)
        self.instance().restore()
        self.assertEqual(self.system.actions, actions)
        self.assertEqual(self.system.live_client_vr_restarts, 0)

    def test_original_to_prepared_activation_uses_new_staged_baseline(self):
        self.lifecycle.restore()
        original = json.loads(json.dumps(self.system.units))
        baseline = self.record()['baseline']
        result = self.instance().restart()
        self.assertTrue(result['running'])
        self.assertEqual(self.record()['baseline'], baseline)
        self.assertEqual(self.record()['activation'], {'kind':'prepared', 'baseline':original})
        self.assertNotEqual(self.system.units['steamvr.service']['InvocationID'], original['steamvr.service']['InvocationID'])
        self.assertEqual(self.system.live_client_vr_restarts, 0)

    def test_reconciled_pending_vr_stops_unexpected_live_client_without_repeating_vr(self):
        self.system.lost_reply_after = ('restart', 'steamvr.service')
        with self.assertRaises(engine.SetupError): self.lifecycle.restart()
        self.system.fresh('steam.service')
        vr = self.system.units['steamvr.service']['InvocationID']
        result = self.instance().restart()
        self.assertTrue(result['running'])
        self.assertEqual(self.system.units['steamvr.service']['InvocationID'], vr)
        self.assertEqual(self.system.actions.count(('restart', 'steamvr.service')), 1)
        self.assertEqual(self.system.actions[-2:], [('stop', 'steam.service'), ('start', 'steam.service')])
        self.assertEqual(self.system.live_client_vr_restarts, 0)

    def test_legacy_phases_keep_original_baseline_and_reuse_already_fresh_vr(self):
        for phase in ('prepared', 'restarting-steam', 'restarting-vr'):
            with self.subTest(phase=phase):
                baseline = json.loads(json.dumps(self.system.units))
                engine.put(self.journal, engine.encoded({'schema':frame.SCHEMA, 'baseline':baseline,
                    'restart_steam':True, 'phase':phase}))
                self.system.fresh('steamvr.service')
                vr = self.system.units['steamvr.service']['InvocationID']
                result = self.instance().restart()
                self.assertTrue(result['running'])
                self.assertEqual(self.record()['baseline'], baseline)
                self.assertEqual(self.system.units['steamvr.service']['InvocationID'], vr)
                self.assertEqual(self.system.actions, [('stop', 'steam.service'), ('start', 'steam.service')])
                self.journal.unlink()
                self.system = System()

    def test_legacy_completed_activation_waits_for_health_without_restarting(self):
        baseline = json.loads(json.dumps(self.system.units))
        engine.put(self.journal, engine.encoded({'schema':frame.SCHEMA, 'baseline':baseline,
            'restart_steam':True, 'phase':'awaiting-health'}))
        self.assertTrue(self.instance().restart()['running'])
        self.assertEqual(self.system.actions, [])
        self.assertTrue(self.instance().restored(health_ok=True)['complete'])

    def test_client_is_not_started_if_vr_disappears_after_its_fresh_generation(self):
        baseline = json.loads(json.dumps(self.system.units))
        engine.put(self.journal, engine.encoded({'schema':frame.SCHEMA, 'baseline':baseline,
            'restart_steam':True, 'phase':'starting-steam',
            'activation':{'kind':'prepared', 'baseline':baseline}}))
        self.system.units['steam.service'].update(ActiveState='inactive', SubState='dead', MainPID='0')
        self.system.units['steamvr.service'].update(ActiveState='inactive', SubState='dead', MainPID='0')
        with self.assertRaisesRegex(engine.SetupError, 'not ready for its client'): self.instance().restart()
        self.assertEqual(self.system.actions, [])

    def test_fixed_error_messages_never_surface_process_output(self):
        self.system.command_error = True
        with self.assertRaises(engine.SetupError) as failure: self.lifecycle.snapshot()
        self.assertNotIn('PRIVATE', str(failure.exception))
        self.assertEqual(self.system.actions, [])

    def test_malformed_status_refuses_actions_and_never_leaks_payload(self):
        for raw in (b'PRIVATE_STATUS', b'Id=steam.service\nId=steam.service\n', b'X' * 4097, b'\xff'):
            self.system.corrupt_status = raw
            with self.subTest(raw=raw[:5]), self.assertRaises(engine.SetupError) as failure: self.lifecycle.restart()
            self.assertNotIn('PRIVATE', str(failure.exception))
            self.assertEqual(self.system.actions, [])
            self.assertFalse(self.journal.exists())

    def test_foreign_or_changed_journal_is_never_overwritten(self):
        foreign = b'{"schema":"another-program"}\n'
        engine.put(self.journal, foreign)
        with self.assertRaises(engine.SetupError): self.lifecycle.restart()
        self.assertEqual(engine.read(self.journal), foreign)
        self.assertEqual(self.system.actions, [])

    def test_symlink_journal_and_public_directory_refuse_before_service_changes(self):
        target = self.root / 'foreign.json';target.write_text('preserve')
        self.journal.symlink_to(target.name)
        with self.assertRaises(engine.SetupError): self.lifecycle.restart()
        self.assertEqual(target.read_text(), 'preserve')
        self.assertEqual(self.system.actions, [])
        self.journal.unlink();self.root.chmod(0o755)
        with self.assertRaises(engine.SetupError): self.lifecycle.restart()
        self.assertFalse(self.journal.exists())
        self.assertEqual(self.system.actions, [])
        self.root.chmod(0o700)

    def test_final_confirmation_is_idempotent_but_does_not_restart_anything(self):
        self.lifecycle.restart();actions = list(self.system.actions)
        self.lifecycle.restored(health_ok=True)
        again = self.lifecycle.restored(health_ok=True)
        self.assertTrue(again['complete'])
        self.assertEqual(self.system.actions, actions)

    def test_replaced_receipt_during_restart_is_preserved_and_confirmation_refused(self):
        original = self.system.before_action
        def replace(action, unit):
            if unit == 'steam.service': self.journal.write_bytes(b'foreign replacement')
        self.system.before_action = replace
        with self.assertRaises(engine.SetupError): self.lifecycle.restart()
        self.assertEqual(engine.read(self.journal), b'foreign replacement')
        self.assertEqual(self.system.actions, [('stop', 'steam.service')])
        self.system.before_action = original


if __name__ == '__main__':
    unittest.main()
