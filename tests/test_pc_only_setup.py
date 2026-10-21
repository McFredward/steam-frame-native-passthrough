"""Production setup on owned temp files; Frame calls must stay read-only."""
import contextlib
import copy
import importlib.util
import os
from pathlib import Path
import sys
import types
import unittest
from unittest import mock

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('pc_only_fixtures',ROOT/'tests/test_stream_bridge_install.py')
fixtures=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixtures)
s,b=fixtures.s,fixtures.b


class Tests(fixtures.Tests):
    def frame_snapshot(self):
        return {str(p.relative_to(self.fhome)):(p.read_bytes(),p.stat().st_mode,p.stat().st_mtime_ns,p.stat().st_ino)
                for p in self.fhome.rglob('*') if p.is_file()}

    def pc_candidate(self):
        self.install_component_fixture(pc='0.2.12',frame='0.2.12')
        name='pc-provider/bin/win64/driver_steam_link_native_passthrough.dll';path=self.bundle_root/name
        raw=path.read_bytes()+b' PC-only candidate';s.put(path,raw,path.read_bytes())
        self.manifest['files'][name]={'bytes':len(raw),'sha256':s.sha(raw)}
        self.version_bundle('0.2.13','0.2.12')
        self.active=True

    def checked_wizard(self,action='update',*,confirm=True,choices=(),install_fault=None,remove_fault=None,snapshot_fault=None,runtime_factory=None,frame_available=True):
        owner=self;commands=[];events=[];questions=[];messages=[];options=[];selections=iter(choices)
        class Screen:
            def banner(inner,**kw):pass
            def status(inner,*a):pass
            def path(inner,*a,**kw):pass
            def say(inner,key):messages.append(key)
            def inventory(inner,**kw):pass
            def version_plan(inner,**kw):pass
            def suspend(inner):return contextlib.nullcontext()
            def choose_action(inner,actions,**kw):options.append((list(actions),kw));return next(selections,'exit')
            def confirm(inner,key):questions.append(key);return confirm
        class Runtime:
            def __init__(inner,*a):pass
            def snapshot(inner):
                events.append('pc-snapshot')
                if snapshot_fault:snapshot_fault()
                path=owner.local.root/'lifecycle.json'
                if not path.exists():s.put(path,b'{"fixture":"owned-PC-lifecycle"}\n')
            def stop(inner):events.append('pc-stop');owner.system.live=False
            def restore(inner):events.append('pc-restore')
            def finish(inner):
                events.append('pc-finish');path=owner.local.root/'lifecycle.json'
                if path.exists():s.remove_owned(path,path.read_bytes())
        class Peer:
            def call(inner,request):
                command=request['command'];commands.append(command)
                if command=='bridge-health-baseline':return {'baseline':'synthetic','checkpoint':'synthetic'}
                if command=='bridge-health':return {'software_ready':True,'status':'ready'}
                if command.startswith('bridge-runtime-'):return {'running':True}
                return b.remote_request(s,request,unit=owner.unit)
            def close(inner):pass
        real=b.load_helper;real_install=b.LocalInstall.install;real_remove=s.remove_owned
        def helper(engine,name):return types.SimpleNamespace(Lifecycle=runtime_factory or Runtime) if name=='steam_link_setup_pc' else real(engine,name)
        def install(installed,*a,**kw):
            result=real_install(installed,*a,**kw)
            if install_fault:install_fault(installed)
            return result
        def remove(path,*a,**kw):
            if remove_fault:remove_fault(path)
            return real_remove(path,*a,**kw)
        screen=Screen();args=types.SimpleNamespace(bundle=self.bundle_root,check=False,dry_run=False,lang='en',frame='192.168.1.25' if frame_available else None,user='steamos',action=action)
        with mock.patch.object(s.sys.stdin,'isatty',return_value=True),mock.patch.object(s,'load_presentation',return_value=types.SimpleNamespace(Presenter=lambda **kw:screen)),mock.patch.object(b,'local_address',return_value='192.168.1.4'),mock.patch.object(b,'load_helper',side_effect=helper),mock.patch.object(b.LocalInstall,'install',install),mock.patch.object(s,'remove_owned',side_effect=remove):
            with self.fake_generation():result=b.run(args,s,self.local,remote_factory=lambda *a,**kw:Peer(),discover_frame=lambda *a,**kw:None)
        return types.SimpleNamespace(result=result,commands=commands,events=events,questions=questions,messages=messages,options=options)

    def assert_frame_read_only(self,before,run):
        self.assertLessEqual(set(run.commands),{'bridge-inventory'},run.commands)
        self.assertEqual(self.frame_snapshot(),before)
        self.assertNotIn('bridge_picture',run.questions);self.assertNotIn('bridge_original_picture',run.questions)
        self.assertNotIn('bridge_starting',run.messages)

    def test_pc_update_changes_only_pc_without_frame_writes_or_restart(self):
        self.pc_candidate();before=self.frame_snapshot();original_pair=s.read(self.local.root/'pairings/active.json')
        run=self.checked_wizard()
        self.assertEqual(run.result,0)
        self.assert_frame_read_only(before,run)
        self.assertEqual(b.LocalInstall(s,self.local).state()['version'],'0.2.13')
        self.assertEqual(s.read(self.local.root/'pairings/active.json'),original_pair)
        self.assertEqual(self.unit.tx.state()['version'],'0.2.12')
        self.assertIn('bridge_pc_only_plan',run.messages);self.assertIn('bridge_pc_only_installed',run.messages)
        self.assertIn('pc-stop',run.events);self.assertNotIn('pc-restore',run.events)

    def test_unchanged_frame_payload_is_current_despite_new_global_bundle(self):
        self.pc_candidate();inventory=self.unit.inventory(self.bundle)
        self.assertFalse(inventory['files_current'])
        self.assertTrue(inventory['component_files_current']);self.assertTrue(b.frame_component_current(self.bundle,inventory))

    def test_failed_pc_update_restores_pc_only(self):
        self.pc_candidate();before=self.frame_snapshot();installed=b.LocalInstall(s,self.local)
        old=s.read(installed.receipt);provider=self.local.root/'pc-provider/bin/win64/driver_steam_link_native_passthrough.dll';raw=provider.read_bytes()
        def fail_after_write(unused):raise s.SetupError('fixture failure after actual PC update')
        run=self.checked_wizard(install_fault=fail_after_write)
        self.assertEqual(run.result,2);self.assert_frame_read_only(before,run)
        self.assertEqual(installed.receipt.read_bytes(),old);self.assertEqual(provider.read_bytes(),raw)
        self.assertIn('pc-restore',run.events);self.assertIn('bridge_pc_rolled_back',run.messages)
        self.assertFalse((self.local.root/'setup-restore.json').exists());self.assertFalse((self.local.root/b.PC_ONLY_RECORD).exists())

    def test_ctrl_c_retains_pc_scope_and_new_run_recovers_only_pc(self):
        self.pc_candidate();before=self.frame_snapshot();installed=b.LocalInstall(s,self.local);old=installed.receipt.read_bytes()
        def cancel(unused):raise KeyboardInterrupt()
        cancelled=self.checked_wizard(install_fault=cancel)
        self.assertEqual(cancelled.result,130);self.assert_frame_read_only(before,cancelled)
        self.assertTrue((self.local.root/b.PC_ONLY_RECORD).exists());self.assertTrue((self.local.root/'setup-restore.json').exists())
        self.assertNotIn('pc-restore',cancelled.events)
        restored=self.checked_wizard(action='rollback')
        self.assertEqual(restored.result,0);self.assert_frame_read_only(before,restored)
        self.assertEqual(installed.receipt.read_bytes(),old)
        self.assertIn('bridge_pc_recover_confirm',restored.questions)
        self.assertFalse((self.local.root/b.PC_ONLY_RECORD).exists());self.assertFalse((self.local.root/'lifecycle.json').exists())

    def test_interrupted_pc_update_can_recover_without_a_frame_connection(self):
        self.pc_candidate();before=self.frame_snapshot();old=b.LocalInstall(s,self.local).receipt.read_bytes()
        def cancel(unused):raise KeyboardInterrupt()
        self.assertEqual(self.checked_wizard(install_fault=cancel).result,130)
        recovered=self.checked_wizard(action='rollback',frame_available=False)
        self.assertEqual(recovered.result,0);self.assert_frame_read_only(before,recovered)
        self.assertEqual(recovered.commands,[]);self.assertEqual(b.LocalInstall(s,self.local).receipt.read_bytes(),old)
        self.assertFalse((self.local.root/b.PC_ONLY_RECORD).exists())

    def test_interrupted_pc_update_resumes_without_frame_recovery(self):
        self.pc_candidate();before=self.frame_snapshot()
        def cancel(unused):raise KeyboardInterrupt()
        self.assertEqual(self.checked_wizard(install_fault=cancel).result,130)
        resumed=self.checked_wizard()
        self.assertEqual(resumed.result,0);self.assert_frame_read_only(before,resumed)
        self.assertIn('bridge_pc_pending_apply',resumed.messages)
        self.assertNotIn('pc-restore',resumed.events)
        self.assertEqual(b.LocalInstall(s,self.local).state()['version'],'0.2.13')
        self.assertFalse((self.local.root/b.PC_ONLY_RECORD).exists())

    def test_partial_pc_completion_reentry_only_finishes_pc_records(self):
        self.pc_candidate();before=self.frame_snapshot()
        def refuse_marker(path):
            if Path(path).name==b.PC_ONLY_RECORD:raise OSError(13,'fixture completion cleanup refusal')
        first=self.checked_wizard(remove_fault=refuse_marker)
        self.assertEqual(first.result,2);self.assert_frame_read_only(before,first)
        scope=b.pc_only_scope(s,self.local);self.assertIs(scope['completion']['restored'],False)
        self.assertFalse((self.local.root/'setup-restore.json').exists());self.assertFalse((self.local.root/'lifecycle.json').exists())
        resumed=self.checked_wizard(action='retry')
        self.assertEqual(resumed.result,0);self.assert_frame_read_only(before,resumed)
        self.assertEqual(resumed.questions,[])
        self.assertNotIn('pc-stop',resumed.events);self.assertNotIn('pc-snapshot',resumed.events)
        self.assertNotIn('pc-restore',resumed.events)
        self.assertFalse((self.local.root/b.PC_ONLY_RECORD).exists())

    def test_remaining_pc_completion_can_finish_without_a_frame_connection(self):
        self.pc_candidate();before=self.frame_snapshot()
        def refuse_marker(path):
            if Path(path).name==b.PC_ONLY_RECORD:raise OSError(13,'fixture cleanup refusal')
        self.assertEqual(self.checked_wizard(remove_fault=refuse_marker).result,2)
        completed=self.checked_wizard(action='retry',frame_available=False)
        self.assertEqual(completed.result,0);self.assert_frame_read_only(before,completed)
        self.assertEqual(completed.commands,[]);self.assertNotIn('pc-stop',completed.events)
        self.assertFalse((self.local.root/b.PC_ONLY_RECORD).exists())

    def test_lost_reply_after_last_pc_record_removal_retries_only_finalization(self):
        self.pc_candidate();before=self.frame_snapshot();lost=[]
        real=s.remove_owned
        def lost_reply(path):
            if Path(path).name==b.PC_ONLY_RECORD and not lost:
                lost.append(True);real(path,Path(path).read_bytes());raise OSError(5,'fixture lost completion reply')
        run=self.checked_wizard(choices=['retry'],remove_fault=lost_reply)
        self.assertEqual(run.result,0);self.assert_frame_read_only(before,run)
        self.assertEqual(run.events.count('pc-stop'),1);self.assertNotIn('pc-restore',run.events)
        self.assertIn('bridge_pc_finalize_incomplete',run.messages)

    def test_finalization_reentry_preserves_changed_pc_receipt(self):
        self.pc_candidate();before=self.frame_snapshot()
        def refuse_marker(path):
            if Path(path).name==b.PC_ONLY_RECORD:raise OSError(13,'fixture cleanup refusal')
        self.assertEqual(self.checked_wizard(remove_fault=refuse_marker).result,2)
        installed=b.LocalInstall(s,self.local);raw=installed.receipt.read_bytes();value=s.parse(raw)
        value['registered_before']=not value['registered_before'];s.put(installed.receipt,s.encoded(value),raw)
        run=self.checked_wizard(action='retry')
        self.assertEqual(run.result,2);self.assert_frame_read_only(before,run)
        self.assertTrue((self.local.root/b.PC_ONLY_RECORD).exists());self.assertNotIn('pc-stop',run.events)

    def test_untested_unchanged_inactive_frame_stays_untouched(self):
        self.change_runtime('frame','vrlink')
        self.version_bundle('0.2.12','0.2.12');self.assertEqual(self.checked_wizard(action='install').result,0)
        self.version_bundle('0.2.13','0.2.12');before=self.frame_snapshot()
        self.assertEqual(self.unit.tx.state()['activation_mode'],'inactive')
        run=self.checked_wizard()
        self.assertEqual(run.result,0);self.assert_frame_read_only(before,run)
        self.assertIn('bridge_untested_confirm',run.questions)

    def test_actual_frame_version_change_keeps_paired_prepare_and_restart(self):
        self.pc_candidate();name='frame/'+b.LIBRARY;path=self.bundle_root/name;raw=path.read_bytes()+b' actual Frame candidate'
        s.put(path,raw,path.read_bytes());self.manifest['files'][name]={'bytes':len(raw),'sha256':s.sha(raw)}
        self.version_bundle('0.2.13','0.2.13')
        run=self.checked_wizard()
        self.assertEqual(run.result,0);self.assertIn('bridge-prepare',run.commands)
        self.assertEqual(run.commands.count('bridge-runtime-restart'),1)
        self.assertIn('bridge_picture',run.questions);self.assertIn('bridge_install_plan',run.messages)
        self.assertNotIn('bridge_pc_only_plan',run.messages)

    def test_target_frame_helper_change_requires_frame_preparation(self):
        self.pc_candidate();name='frame/tools/compositor_startup_health.py';path=self.bundle_root/name
        raw=path.read_bytes()+b'# actual helper change\n';s.put(path,raw,path.read_bytes())
        self.manifest['files'][name]={'bytes':len(raw),'sha256':s.sha(raw)};self.version_bundle('0.2.13','0.2.12')
        self.assertFalse(self.unit.inventory(self.bundle)['component_files_current'])
        run=self.checked_wizard()
        self.assertEqual(run.result,0);self.assertIn('bridge-prepare',run.commands)
        self.assertEqual(run.commands.count('bridge-runtime-restart'),1)

    def test_frame_damage_or_recovery_or_different_pair_cannot_be_called_pc_only(self):
        self.pc_candidate();inventory=self.unit.inventory(self.bundle);state=b.LocalInstall(s,self.local).state()
        for key,value in [('installed_version','0.2.11'),('component_files_current',False),('patch_payload_current',False),
                          ('owned_files_current',False),('runtime_configuration_current',False),('recovery_pending',True),
                          ('legacy_installed',True),('runtime_admitted',False),('active_pair','34'*16)]:
            other={**inventory,key:value}
            with self.subTest(key=key),mock.patch.object(b,'local_address',return_value='192.168.1.4'):
                self.assertFalse(b.pc_only_candidate(self.bundle,state,other,'192.168.1.25'))
        with mock.patch.object(b,'local_address',return_value='192.168.2.4'):
            self.assertFalse(b.pc_only_candidate(self.bundle,state,inventory,'192.168.1.25'))

    def test_failed_pc_snapshot_before_stop_never_changes_frame(self):
        self.pc_candidate();before=self.frame_snapshot();old=b.LocalInstall(s,self.local).receipt.read_bytes()
        def refuse():raise OSError(13,'fixture snapshot failure before PC stop')
        run=self.checked_wizard(snapshot_fault=refuse)
        self.assertEqual(run.result,2);self.assert_frame_read_only(before,run)
        self.assertNotIn('pc-stop',run.events);self.assertNotIn('pc-restore',run.events)
        self.assertEqual(b.LocalInstall(s,self.local).receipt.read_bytes(),old)
        self.assertFalse((self.local.root/b.PC_ONLY_RECORD).exists())

    def test_real_pc_lifecycle_journal_survives_cancel_and_recovers_without_frame_calls(self):
        self.pc_candidate();before=self.frame_snapshot();old=b.LocalInstall(s,self.local).receipt.read_bytes()
        spec=importlib.util.spec_from_file_location('pc_only_real_lifecycle_fixture',ROOT/'tests/test_setup_pc.py')
        fixture=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixture)
        adapter=fixture.ChildAdapter(self.runtime);self.addCleanup(adapter.close)
        # Real lifecycle persistence/stop/restore/finish; canonical process
        # authority is supplied by the fixture over its own child processes.
        factory=lambda engine,local,runtime:fixture.pc.Lifecycle(engine,local,runtime,adapter=adapter)
        def cancel(unused):raise KeyboardInterrupt()
        first=self.checked_wizard(install_fault=cancel,runtime_factory=factory)
        self.assertEqual(first.result,130);self.assert_frame_read_only(before,first)
        self.assertEqual(s.parse((self.local.root/'lifecycle.json').read_bytes())['schema'],fixture.pc.SCHEMA)
        second=self.checked_wizard(action='rollback',runtime_factory=factory)
        self.assertEqual(second.result,0);self.assert_frame_read_only(before,second)
        self.assertEqual(b.LocalInstall(s,self.local).receipt.read_bytes(),old)
        self.assertFalse((self.local.root/'lifecycle.json').exists());self.assertFalse((self.local.root/b.PC_ONLY_RECORD).exists())
        self.assertEqual(adapter.launches,0)

    def test_pc_only_ports_refuse_every_frame_mutation_before_sending(self):
        self.pc_candidate();before=self.frame_snapshot();sent=[]
        class Peer:
            def call(inner,request):sent.append(request['command']);raise AssertionError('forbidden remote call')
        real=b.load_helper
        with mock.patch.object(b,'load_helper',side_effect=lambda engine,name:types.SimpleNamespace(Lifecycle=lambda *a:None) if name=='steam_link_setup_pc' else real(engine,name)):
            ports=b.PcOnlyAutomation(s,self.bundle,self.local,b.LocalInstall(s,self.local),Peer(),None,{'log':None,'stage':'preflight'},None,'192.168.1.25','steamos')
        for command in ('bridge-prepare','bridge-commit','bridge-runtime-snapshot','bridge-runtime-restart',
                        'bridge-restore','bridge-undo','bridge-finalize-inactive','bridge-checkpoint-finish','bridge-runtime-restored'):
            with self.subTest(command=command),self.assertRaisesRegex(s.SetupError,'cannot change or restart'):
                ports.call(command)
        self.assertEqual(sent,[]);self.assertEqual(self.frame_snapshot(),before)

    def test_declining_pc_update_changes_neither_device(self):
        self.pc_candidate();before=self.frame_snapshot();receipt=b.LocalInstall(s,self.local).receipt.read_bytes()
        run=self.checked_wizard(confirm=False)
        self.assertEqual(run.result,0);self.assert_frame_read_only(before,run)
        self.assertNotIn('pc-stop',run.events);self.assertFalse((self.local.root/b.PC_ONLY_RECORD).exists())
        self.assertEqual(b.LocalInstall(s,self.local).receipt.read_bytes(),receipt)

    def test_unknown_pc_scope_cannot_start_a_paired_recovery(self):
        self.pc_candidate();before=self.frame_snapshot();s.put(self.local.root/b.PC_ONLY_RECORD,b'{"schema":"foreign"}\n')
        run=self.checked_wizard()
        self.assertEqual(run.result,2);self.assert_frame_read_only(before,run)
        self.assertNotIn('pc-stop',run.events)

    def test_second_check_does_not_offer_another_update_due_to_old_frame_bundle(self):
        self.pc_candidate();before=self.frame_snapshot();first=self.checked_wizard()
        self.assertEqual(first.result,0)
        second=self.checked_wizard(action=None)
        self.assertEqual(second.result,0);self.assert_frame_read_only(before,second)
        self.assertEqual(second.options[0][1]['recommended'],'check')


if __name__=='__main__':
    suite=unittest.TestSuite(Tests(n) for n in Tests.__dict__ if n.startswith('test_'))
    raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
