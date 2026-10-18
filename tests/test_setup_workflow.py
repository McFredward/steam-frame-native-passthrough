"""Exercise actual rollback files and confirmed workflow on owned temp inputs."""
import importlib.util
from pathlib import Path
import unittest
from unittest import mock
import contextlib
import types
import sys

ROOT=Path(__file__).resolve().parents[1]
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
fixtures=load('sfnp_checkpoint_fixture',ROOT/'tests/test_stream_bridge_install.py')
flow=load('sfnp_actual_workflow',ROOT/'setup/steam_link_setup_workflow.py')
s,b=fixtures.s,fixtures.b


class Files(fixtures.Tests):
    # Inherit only the fixture setup; unittest filters inherited cases below.
    def newer_bundle(self):
        for name in ('pc-provider/bin/win64/driver_steam_link_native_passthrough.dll','frame/'+b.LIBRARY):
            path=self.bundle_root/name;raw=s.read(path)+b' owned update fixture'
            s.put(path,raw,s.read(path));self.manifest['files'][name]={'bytes':len(raw),'sha256':s.sha(raw)}
        path=self.bundle_root/'stream-bridge-bundle.json';s.put(path,s.encoded(self.manifest),s.read(path))
        return b.Bundle(self.bundle_root,s)

    def test_confirmed_update_survives_recovery_and_installs_the_new_package(self):
        installer=b.LocalInstall(s,self.local)
        installer.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631,backup=True)
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
        self.unit.tx.commit();updated=self.newer_bundle();choices=[];confirmations=[];commands=[];inventories=[]
        class Screen:
            def banner(inner,**kw):pass
            def status(inner,*a):pass
            def path(inner,*a,**kw):pass
            def say(inner,key):pass
            def suspend(inner):return contextlib.nullcontext()
            def inventory(inner,**kw):inventories.append(kw)
            def choose_action(inner,actions,**kw):
                choices.append(actions)
                if len(choices)>1:raise AssertionError('Confirmed Update must continue after recovery without asking again')
                return 'update'
            def confirm(inner,key):confirmations.append(key);return True
        class Runtime:
            def __init__(inner,*a):pass
            def snapshot(inner):pass
            def stop(inner):self.system.live=False
            def restore(inner):pass
            def finish(inner):pass
        class Peer:
            def call(inner,request):
                command=request['command'];commands.append(command)
                if command=='bridge-health-baseline':return {}
                if command=='bridge-health':return {'software_ready':True}
                if command.startswith('bridge-runtime-'):return {'running':True}
                return b.remote_request(s,request,unit=self.unit)
            def close(inner):pass
        real=b.load_helper
        def helper(engine,name):return types.SimpleNamespace(Lifecycle=Runtime) if name=='steam_link_setup_pc' else real(engine,name)
        args=types.SimpleNamespace(bundle=self.bundle_root,check=False,dry_run=False,lang='en',frame='192.168.1.25',user='steamos',action=None)
        with mock.patch.object(s.sys.stdin,'isatty',return_value=True),mock.patch.object(s,'load_presentation',return_value=types.SimpleNamespace(Presenter=lambda **kw:Screen())),mock.patch.object(b,'local_address',return_value='192.168.1.4'),mock.patch.object(b,'load_helper',side_effect=helper),self.fake_generation():
            self.assertEqual(b.run(args,s,self.local,remote_factory=lambda *a,**kw:Peer()),0)
        self.assertEqual(len(choices),1)
        self.assertEqual(confirmations,['bridge_confirm','bridge_picture'])
        self.assertLess(commands.index('bridge-restore'),commands.index('bridge-prepare'))
        self.assertEqual(installer.state()['bundle'],updated.pin)
        self.assertEqual(self.unit.tx.state()['bundle'],updated.pin)
        self.assertTrue(inventories[-1]['pc']['installed']);self.assertTrue(inventories[-1]['frame']['installed'])
        self.assertFalse(installer.checkpoint().path.exists());self.assertFalse(self.unit.checkpoint().path.exists())

    def test_pc_update_retains_initial_uninstall_baseline_and_pairing(self):
        installer=b.LocalInstall(s,self.local);initial=s.parse(s.read(self.settings))
        installer.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631)
        key=self.local.root/'pairings'/(self.pair['pair_id']+'.json');credential=s.read(key,4096,private=True)
        next_bundle=self.newer_bundle()
        result=installer.install(next_bundle,self.local.inventory(),self.pair,'192.168.1.5',47631,backup=True)
        self.assertEqual(result['reason'],'updated');self.assertEqual(installer.state()['bundle'],next_bundle.pin)
        self.assertEqual(s.read(key,4096,private=True),credential)
        self.assertEqual(s.parse(s.read(self.settings))[b.SETTINGS_SECTION]['controlListenAddress'],'192.168.1.5')
        installer.checkpoint().finish()
        installer.uninstall(self.local.inventory())
        self.assertEqual(s.parse(s.read(self.settings)),initial);self.assertFalse(self.local.inventory()['registered'])

    def test_pc_update_rollback_restores_previous_version_and_owned_preferences(self):
        installer=b.LocalInstall(s,self.local)
        installer.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631)
        receipt=s.read(installer.receipt);preferences=s.parse(s.read(self.settings))
        image=self.local.root/'pc-provider/bin/win64/driver_steam_link_native_passthrough.dll';original=s.read(image)
        installer.install(self.newer_bundle(),self.local.inventory(),self.pair,'192.168.1.5',47631,backup=True)
        installer.checkpoint().restore();installer.checkpoint().verify_restored()
        self.assertEqual(s.read(installer.receipt),receipt);self.assertEqual(s.read(image),original)
        self.assertEqual(s.parse(s.read(self.settings)),preferences);self.assertTrue(self.local.inventory()['registered'])
        installer.checkpoint().finish(restored=True)

    def test_frame_update_runs_live_and_retains_initial_uninstall_baseline(self):
        initial=s.parse(s.read(self.fsettings))
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.unit.tx.commit();self.active=True;next_bundle=self.newer_bundle()
        with self.fake_generation(),mock.patch.object(self.unit,'cold',side_effect=AssertionError('Do not stop the Frame UI')):
            result=self.unit.install(next_bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
        self.assertEqual(result['reason'],'prepared-update');self.unit.tx.commit()
        self.assertTrue(self.unit.inventory(next_bundle)['files_current']);self.assertTrue(self.active)
        self.unit.checkpoint().finish();self.unit.undo(live=True)
        self.assertEqual(s.parse(s.read(self.fsettings)),initial);self.assertFalse((self.froot/b.LIBRARY).exists())

    def test_failed_frame_update_restores_previous_version_and_transaction(self):
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.unit.tx.commit();receipt=s.read(self.unit.tx.path);image=s.read(self.froot/b.LIBRARY)
        with self.fake_generation(),mock.patch.object(self.unit,'system',side_effect=s.SetupError('fixture reload failure')):
            with self.assertRaises(s.SetupError):
                self.unit.install(self.newer_bundle(),{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
        self.unit.checkpoint().restore();self.unit.checkpoint().verify_restored()
        self.assertEqual(s.read(self.unit.tx.path),receipt);self.assertEqual(s.read(self.froot/b.LIBRARY),image)
        self.assertTrue(self.unit.inventory(self.bundle)['files_current'])
        self.unit.checkpoint().finish(restored=True)

    def test_frame_update_repairs_missing_owned_activation_and_rollback_retains_its_absence(self):
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.unit.tx.commit();marker=self.froot/'receiver.enable';marker.unlink();receipt=s.read(self.unit.tx.path)
        with self.fake_generation():self.unit.install(self.newer_bundle(),{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
        self.unit.tx.commit();self.assertTrue(marker.exists())
        self.unit.checkpoint().restore();self.unit.checkpoint().verify_restored()
        self.assertFalse(marker.exists());self.assertEqual(s.read(self.unit.tx.path),receipt)
        self.unit.checkpoint().finish(restored=True)

    def test_changed_bundle_offers_update_without_uninstall(self):
        installer=b.LocalInstall(s,self.local)
        installer.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631)
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.unit.tx.commit();self.newer_bundle();seen=[]
        class Screen:
            def banner(inner,**kw):pass
            def status(inner,*a):pass
            def path(inner,*a,**kw):pass
            def say(inner,key):pass
            def suspend(inner):return contextlib.nullcontext()
            def choose_action(inner,actions,**kw):seen.append((actions,kw));return 'update'
            def confirm(inner,key):return False
        class Peer:
            def call(inner,request):return b.remote_request(s,request,unit=self.unit)
            def close(inner):pass
        args=types.SimpleNamespace(bundle=self.bundle_root,check=False,dry_run=False,lang='en',frame='192.168.1.25',user='steamos',action=None)
        with mock.patch.object(s.sys.stdin,'isatty',return_value=True),mock.patch.object(s,'load_presentation',return_value=types.SimpleNamespace(Presenter=lambda **kw:Screen())):
            self.assertEqual(b.run(args,s,self.local,remote_factory=lambda *a,**kw:Peer()),0)
        self.assertIn('update',seen[0][0]);self.assertEqual(seen[0][1]['recommended'],'update')

    def test_update_refuses_foreign_changed_payload_before_any_mutation(self):
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.unit.tx.commit();next_bundle=self.newer_bundle();image=self.froot/b.LIBRARY
        s.put(image,b'foreign replacement',s.read(image));receipt=s.read(self.unit.tx.path)
        with self.fake_generation(),self.assertRaisesRegex(s.SetupError,'Changed Frame files'):
            self.unit.install(next_bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
        self.assertEqual(s.read(image),b'foreign replacement');self.assertEqual(s.read(self.unit.tx.path),receipt)
        self.assertFalse(self.unit.checkpoint().path.exists())

    def test_live_frame_install_and_restore_keep_ui_up_and_original_settings(self):
        self.active=True;before=s.read(self.fsettings)
        with self.fake_generation(),mock.patch.object(self.unit,'cold',side_effect=AssertionError('UI must remain running')):
            self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
        self.unit.tx.commit();self.assertTrue(self.unit.checkpoint().path.exists())
        preferences=s.parse(s.read(self.fsettings));preferences['user']['later']=8
        s.put(self.fsettings,s.encoded(preferences),s.read(self.fsettings))
        self.unit.checkpoint().restore();self.assertIsNone(self.unit.tx.state())
        self.unit.checkpoint().verify_restored()
        expected=s.parse(before);expected['user']['later']=8
        self.assertEqual(s.parse(s.read(self.fsettings)),expected)
        self.assertEqual(self.fsettings.stat().st_mode&0o777,0o666)
        self.assertFalse((self.froot/b.LIBRARY).exists());self.assertTrue(self.active)
        self.unit.checkpoint().finish(restored=True)
        self.assertFalse(self.unit.checkpoint().path.exists())
        self.assertFalse(any('stop' in cmd for cmd in self.commands))

    def test_failed_frame_repair_restores_existing_installation_not_uninstall(self):
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.unit.tx.commit();receipt=s.read(self.unit.tx.path)
        missing=self.froot/'coverage.spv';missing.unlink();self.active=True
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
        self.assertTrue(missing.exists());self.unit.checkpoint().restore()
        self.assertFalse(missing.exists());self.assertEqual(s.read(self.unit.tx.path),receipt)
        self.assertTrue((self.froot/b.LIBRARY).exists())
        self.assertTrue(s.parse(s.read(self.fsettings))['camera']['forceSceneAlphaBlending'])
        self.unit.checkpoint().finish(restored=True)

    def test_frame_uninstall_can_restore_exact_pre_uninstall_state(self):
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.unit.tx.commit();receipt=s.read(self.unit.tx.path)
        self.active=True;self.unit.undo(backup=True,live=True)
        self.assertFalse(self.unit.tx.path.exists());self.unit.checkpoint().restore()
        self.assertEqual(s.read(self.unit.tx.path),receipt);self.assertTrue(self.unit.inventory(self.bundle)['files_current'])
        self.unit.checkpoint().finish(restored=True)

    def test_pc_install_rollback_removes_only_its_new_credentials(self):
        installer=b.LocalInstall(s,self.local);before=s.read(self.settings)
        installer.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631,backup=True)
        checkpoint=installer.checkpoint();result=checkpoint.restore()
        self.assertFalse(result['registered'])
        self.assertFalse(installer.receipt.exists());self.assertEqual(s.parse(s.read(self.settings)),s.parse(before))
        self.assertFalse((self.local.root/'pairings'/(self.pair['pair_id']+'.json')).exists())
        self.assertFalse((self.local.root/'pairings/active.json').exists())
        checkpoint.finish(restored=True)

    def test_pc_repair_rollback_preserves_original_receipt_and_missing_file(self):
        installer=b.LocalInstall(s,self.local)
        installer.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631)
        receipt=s.read(installer.receipt);missing=self.local.root/'pc-provider/bin/win64/driver_steam_link_native_passthrough.dll';missing.unlink()
        installer.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631,backup=True)
        installer.checkpoint().restore();self.assertFalse(missing.exists());self.assertEqual(s.read(installer.receipt),receipt)
        installer.checkpoint().finish(restored=True)

    def test_unknown_file_change_blocks_recovery_without_destroying_evidence(self):
        installer=b.LocalInstall(s,self.local)
        installer.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631,backup=True)
        target=self.local.root/'pc-provider/bin/win64/driver_steam_link_native_passthrough.dll'
        s.put(target,b'foreign',s.read(target))
        with self.assertRaisesRegex(s.SetupError,'changed file'):installer.checkpoint().restore()
        self.assertEqual(s.read(target),b'foreign');self.assertTrue(installer.checkpoint().path.exists())

    def test_discovery_retry_and_success_keep_same_authenticated_connection(self):
        class Screen:
            def __init__(inner):inner.choices=iter(['retry','check']);inner.messages=[]
            def banner(inner,**kw):pass
            def status(inner,*a):pass
            def path(inner,*a,**kw):pass
            def say(inner,key):inner.messages.append(key)
            def suspend(inner):return contextlib.nullcontext()
            def choose_action(inner,*a,**kw):return next(inner.choices)
        class Peer:
            def __init__(inner):inner.calls=0;inner.closed=False
            def call(inner,request):inner.calls+=1;return b.remote_request(s,request,unit=self.unit)
            def close(inner):inner.closed=True
        peer=Peer();screen=Screen();searches=[]
        def finder(*a,**kw):
            searches.append(1)
            return types.SimpleNamespace(host='192.168.1.25',value=(peer,peer.call(b.request(self.bundle,'bridge-inventory'))))
        args=types.SimpleNamespace(bundle=self.bundle_root,check=False,dry_run=False,lang='en',frame=None,user='steamos',action=None)
        with mock.patch.object(s.sys.stdin,'isatty',return_value=True),mock.patch.object(s,'load_presentation',return_value=types.SimpleNamespace(Presenter=lambda **kw:screen)):
            self.assertEqual(b.run(args,s,self.local,discover_frame=finder),0)
        self.assertEqual(len(searches),1);self.assertEqual(peer.calls,2);self.assertTrue(peer.closed)

    def test_network_failure_offers_retry_before_any_mutation(self):
        class Screen:
            def __init__(inner):inner.choices=iter(['retry','check'])
            def banner(inner,**kw):pass
            def status(inner,*a):pass
            def path(inner,*a,**kw):pass
            def say(inner,key):pass
            def suspend(inner):return contextlib.nullcontext()
            def choose_action(inner,*a,**kw):return next(inner.choices)
        attempts=[]
        class Peer:
            def call(inner,request):
                attempts.append(1)
                if len(attempts)==1:raise OSError(13,'secret must not be printed')
                return b.remote_request(s,request,unit=self.unit)
            def close(inner):pass
        args=types.SimpleNamespace(bundle=self.bundle_root,check=False,dry_run=False,lang='en',frame='192.168.1.25',user='steamos',action=None)
        screen=Screen()
        with mock.patch.object(s.sys.stdin,'isatty',return_value=True),mock.patch.object(s,'load_presentation',return_value=types.SimpleNamespace(Presenter=lambda **kw:screen)):
            self.assertEqual(b.run(args,s,self.local,remote_factory=lambda *a,**kw:Peer()),0)
        self.assertEqual(len(attempts),2);self.assertFalse(self.unit.tx.path.exists())
        self.assertFalse(b.LocalInstall(s,self.local).receipt.exists())

    def test_pc_recovery_failure_does_not_block_original_frame_restart(self):
        screen=UI();screen.path=lambda *a,**kw:None;screen.suspend=lambda:contextlib.nullcontext()
        class Runtime:
            def __init__(inner,*a):pass
            def snapshot(inner):pass
            def stop(inner):pass
            def restore(inner):raise AssertionError('Do not start unrecovered PC code')
        commands=[]
        class Peer:
            def call(inner,request):
                commands.append(request['command'])
                if request['command']=='bridge-health':return {'software_ready':True}
                return {'restored':True}
        real=b.load_helper
        with mock.patch.object(b,'load_helper',side_effect=lambda e,name:types.SimpleNamespace(Lifecycle=Runtime) if name=='steam_link_setup_pc' else real(e,name)):
            installed=b.LocalInstall(s,self.local)
            ports=b.Automation(s,self.bundle,self.local,installed,Peer(),screen,{'log':None,'peers':[],'stage':'rollback'},None,'192.168.1.25','steamos')
        with mock.patch.object(installed,'checkpoint',return_value=types.SimpleNamespace(restore=mock.Mock(side_effect=OSError(13,'private')))):
            self.assertFalse(flow.Workflow(ports,screen).recover())
        self.assertIn('bridge-runtime-restart',commands);self.assertIn('bridge-health',commands)
        self.assertNotIn('bridge_rolled_back',screen.messages);self.assertIn('bridge_recovery_incomplete',screen.messages)

    def test_prepared_restart_leaves_pc_steamvr_closed(self):
        commands=[];screen=UI();screen.suspend=lambda:contextlib.nullcontext()
        class Runtime:
            def __init__(inner,*a):pass
            def start(inner):raise AssertionError('Installing must not start PC SteamVR')
        class Peer:
            def call(inner,request):commands.append(request['command']);return {'running':True}
        real=b.load_helper
        with mock.patch.object(b,'load_helper',side_effect=lambda e,name:types.SimpleNamespace(Lifecycle=Runtime) if name=='steam_link_setup_pc' else real(e,name)):
            ports=b.Automation(s,self.bundle,self.local,b.LocalInstall(s,self.local),Peer(),screen,
                {'log':None,'peers':[],'stage':'runtime_start'},None,'192.168.1.25','steamos')
        ports.restart_prepared()
        self.assertEqual(commands,['bridge-runtime-restart'])


class UI:
    def __init__(self,choices=(),picture=True):self.choices=iter(choices);self.messages=[];self.picture=picture
    def say(self,key):self.messages.append(key)
    def confirm(self,key):return self.picture
    def choose_action(self,actions,recommended=None):return next(self.choices,'exit')


class Ports:
    error=ValueError
    def __init__(self,prepared=True,original=True):
        self.events=[];self.prepared=prepared;self.original=original;self.fail_restore=0;self.frame_running=True
    def mark(self,key):self.events.append(key)
    def begin(self):self.mark('begin')
    def stop_pc(self):self.mark('stop_pc')
    def prepare(self,action):
        assert self.frame_running,'Frame UI must stay up while files are prepared'
        self.mark('prepare_'+action)
    def restart_prepared(self):self.mark('restart_prepared')
    def prepared_health(self):self.mark('prepared_health');return self.prepared
    def restore_files(self):
        assert self.frame_running,'Frame UI must stay up while files are restored'
        self.mark('restore_files')
        if self.fail_restore:self.fail_restore-=1;raise ValueError('uncertain restore')
    def restart_original(self):self.mark('restart_original')
    def original_health(self):self.mark('original_health');return self.original
    def finish(self,restored=False):
        assert self.frame_running
        self.mark('finished_original' if restored else 'finished_prepared')
    def failure(self,error,recovery=False):self.mark('recovery_failure' if recovery else 'failure')


class Workflow(unittest.TestCase):
    def test_cancellation_restores_running_original_before_exiting_without_retry(self):
        ports=Ports();ui=UI()
        ui.allow_recovery=lambda:ports.mark('allow_recovery')
        def cancel(action):ports.mark('prepare_'+action);raise KeyboardInterrupt()
        ports.prepare=cancel
        ui.choose_action=lambda *a,**kw:self.fail('A completed explicit cancellation must exit after recovery')
        self.assertEqual(flow.Workflow(ports,ui).run('install'),130)
        self.assertEqual(ports.events[-5:],['allow_recovery','restore_files','restart_original','original_health','finished_original'])
        self.assertIn('bridge_rolled_back',ui.messages)

    def test_success_stages_then_restarts_then_checks(self):
        ports=Ports();ui=UI();self.assertEqual(flow.Workflow(ports,ui).run('install'),0)
        self.assertEqual(ports.events,['begin','stop_pc','prepare_install','restart_prepared','prepared_health','finished_prepared'])

    def test_failed_health_automatically_restores_and_restarts_original(self):
        ports=Ports(prepared=False);ui=UI();self.assertEqual(flow.Workflow(ports,ui).run('install'),2)
        self.assertIn('bridge_rolled_back',ui.messages)
        self.assertEqual(ports.events[-4:],['restore_files','restart_original','original_health','finished_original'])

    def test_bad_picture_restores_without_manual_kill(self):
        ports=Ports();ui=UI(picture=False);self.assertEqual(flow.Workflow(ports,ui).run('repair'),2)
        self.assertIn('finished_original',ports.events)

    def test_failed_restore_offers_retry_and_cannot_claim_rolled_back(self):
        ports=Ports(prepared=False);ports.fail_restore=1;ui=UI(['retry','exit'])
        self.assertEqual(flow.Workflow(ports,ui).run('install'),2)
        self.assertEqual(ports.events.count('restore_files'),2)
        self.assertLess(ports.events.index('recovery_failure'),ports.events.index('restart_original'))
        self.assertEqual(ui.messages.count('bridge_rolled_back'),1)

    def test_unhealthy_original_keeps_recovery_incomplete(self):
        ports=Ports(prepared=False,original=False);ui=UI()
        self.assertEqual(flow.Workflow(ports,ui).run('install'),2)
        self.assertNotIn('bridge_rolled_back',ui.messages);self.assertNotIn('finished_original',ports.events)
        self.assertIn('bridge_recovery_incomplete',ui.messages)

    def test_slow_start_retry_observes_without_another_restart_or_rollback(self):
        ports=Ports();ui=UI(['retry'])
        results=iter([{'status':'not-ready','software_ready':False},{'status':'ready','software_ready':True}])
        ports.prepared_health=lambda:next(results)
        self.assertEqual(flow.Workflow(ports,ui).run('install'),0)
        self.assertEqual(ports.events.count('restart_prepared'),1)
        self.assertNotIn('restore_files',ports.events);self.assertIn('bridge_health_waiting',ui.messages)


if __name__=='__main__':
    if len(sys.argv)>1:unittest.main()
    # Existing installer cases are independently run by their own suite.
    own=unittest.TestSuite()
    for case in (Files,Workflow):
        own.addTests(case(name) for name in case.__dict__ if name.startswith('test_'))
    raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(own).wasSuccessful())
