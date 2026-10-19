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

    def test_explicit_ctrl_c_preserves_exact_real_pc_and_frame_operation_checkpoints(self):
        installer=b.LocalInstall(s,self.local);ports=Ports();ui=UI();snapshots={}
        def prepare(action):
            ports.mark('prepare_'+action)
            installer.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631,backup=True)
            with self.fake_generation():
                self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
            paths=(installer.checkpoint().path,self.unit.checkpoint().path,installer.receipt,
                   self.unit.tx.path,self.local.root/'pc-provider/bin/win64/driver_steam_link_native_passthrough.dll',
                   self.froot/b.LIBRARY,self.settings,self.fsettings)
            snapshots.update((path,s.read(path)) for path in paths)
            raise KeyboardInterrupt()
        ports.prepare=prepare
        ports.restore_files=lambda:self.fail('Explicit Ctrl+C must leave the actual checkpoints untouched')
        ports.restart_original=lambda:self.fail('Explicit Ctrl+C must not restart the original runtime')
        self.assertEqual(flow.Workflow(ports,ui).run('install'),130)
        self.assertEqual(len(snapshots),8)
        self.assertTrue(all(path.exists() and s.read(path)==raw for path,raw in snapshots.items()))
        self.assertIn('bridge_cancelled_retained',ui.messages)
        self.assertNotIn('finished_prepared',ports.events)
        self.assertNotIn('finished_original',ports.events)

    def test_picture_no_preserves_real_changes_until_explicit_undo_and_original_image_confirmation(self):
        installer=b.LocalInstall(s,self.local);ports=Ports();ui=UI(['rollback','exit'],picture=False)
        original_pc=s.parse(s.read(self.settings));original_frame=s.parse(s.read(self.fsettings))
        questions=[];offered=[];before_undo=[]
        def prepare(action):
            ports.mark('prepare_'+action)
            installer.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631,backup=True)
            with self.fake_generation():
                self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
            self.unit.tx.commit()
        def restore():
            ports.mark('restore_files')
            installer.checkpoint().restore();installer.checkpoint().verify_restored()
            self.unit.checkpoint().restore();self.unit.checkpoint().verify_restored()
        def confirm(key):
            questions.append(key)
            self.assertTrue(installer.checkpoint().path.exists())
            self.assertTrue(self.unit.checkpoint().path.exists())
            if key=='bridge_original_picture':
                self.assertEqual(s.parse(s.read(self.settings)),original_pc)
                self.assertEqual(s.parse(s.read(self.fsettings)),original_frame)
                self.assertIn('restart_original',ports.events)
                self.assertIn('original_health',ports.events)
                self.assertNotIn('finished_original',ports.events)
                return True
            return False
        def choose(actions,recommended=None):
            offered.append(actions)
            if 'rollback' in actions:
                # Assertions belong outside Workflow's ordinary failure handler;
                # a fixture assertion must not be mistaken for recovery consent.
                before_undo.append((ports.events.count('prepared_health'),'restore_files' in ports.events,
                                    installer.state() is not None,(self.froot/b.LIBRARY).exists()))
            return next(ui.choices)
        def finish(restored=False):
            self.assertTrue(restored)
            self.assertEqual(questions,['bridge_picture','bridge_original_picture'])
            installer.checkpoint().finish(restored=True);self.unit.checkpoint().finish(restored=True)
            ports.mark('finished_original')
        ports.prepare=prepare;ports.restore_files=restore;ports.finish=finish;ui.confirm=confirm;ui.choose_action=choose
        self.assertEqual(flow.Workflow(ports,ui).run('install'),2)
        self.assertEqual(offered,[['verify','rollback','exit'],['retry','exit']])
        self.assertEqual(before_undo,[(2,False,True,True)])
        self.assertEqual(ports.events.count('restart_original'),1)
        self.assertFalse(installer.checkpoint().path.exists());self.assertFalse(self.unit.checkpoint().path.exists())
        self.assertIn('bridge_rolled_back',ui.messages)

    def test_pending_confirmed_update_inherits_undo_and_has_one_restart_and_picture(self):
        self.version_bundle(pc='0.2.4',frame='0.2.5')
        installer=b.LocalInstall(s,self.local)
        installer.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631,backup=True)
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
        self.unit.tx.commit();self.version_bundle(pc='0.2.6',frame='0.2.6')
        updated=self.newer_bundle();choices=[];confirmations=[];commands=[];inventories=[];messages=[]
        frame_fixture=load('sfnp_pending_update_lifecycle_fixture',ROOT/'tests/test_setup_frame.py')
        system=frame_fixture.System()
        def lifecycle(*a):
            return frame_fixture.frame.FrameLifecycle(s,self.froot,runner=system,sleeper=system.sleep,
                monotonic=lambda:system.now,poll_attempts=4,poll_interval=.01,phase_timeout=1)
        lifecycle().restart()
        original_runtime_baseline=s.parse(s.read(self.froot/'lifecycle.json'))['baseline']
        old_runtime_actions=list(system.actions)
        self.active=True
        class Screen:
            def banner(inner,**kw):pass
            def status(inner,*a):pass
            def path(inner,*a,**kw):pass
            def say(inner,key):messages.append(key)
            def suspend(inner):return contextlib.nullcontext()
            def inventory(inner,**kw):inventories.append(kw)
            def choose_action(inner,actions,**kw):
                choices.append(actions)
                self.assertEqual(kw.get('recommended'),'update')
                if len(choices)>1:raise AssertionError('Confirmed Update must continue after recovery without asking again')
                return 'update'
            def confirm(inner,key):
                confirmations.append(key)
                if key=='bridge_picture':
                    journal=s.parse(s.read(self.froot/'lifecycle.json'))
                    self.assertEqual(journal['baseline'],original_runtime_baseline)
                    self.assertEqual(journal['activation']['continuation'],self.unit.checkpoint().value()['continuation']['id'])
                return True
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
                if command=='bridge-runtime-restart':
                    return lifecycle().restart(restoring=request.get('restoring')is True,
                        health_receipt={'baseline':{'current':'before-apply'},'checkpoint':{}})
                return b.remote_request(s,request,unit=self.unit)
            def close(inner):pass
        real=b.load_helper
        def helper(engine,name):
            if name=='steam_link_setup_pc':return types.SimpleNamespace(Lifecycle=Runtime)
            if name=='steam_link_setup_frame':return types.SimpleNamespace(FrameLifecycle=lifecycle)
            return real(engine,name)
        args=types.SimpleNamespace(bundle=self.bundle_root,check=False,dry_run=False,lang='en',frame='192.168.1.25',user='steamos',action=None)
        with mock.patch.object(s.sys.stdin,'isatty',return_value=True),mock.patch.object(s,'load_presentation',return_value=types.SimpleNamespace(Presenter=lambda **kw:Screen())),mock.patch.object(b,'local_address',return_value='192.168.1.4'),mock.patch.object(b,'load_helper',side_effect=helper),self.fake_generation():
            self.assertEqual(b.run(args,s,self.local,remote_factory=lambda *a,**kw:Peer()),0)
        self.assertIn('bridge_pending_apply',messages)
        self.assertNotIn('bridge_recovery_pending',messages)
        self.assertNotIn('bridge_recovery_incomplete',messages)
        self.assertEqual(len(choices),1)
        self.assertEqual(confirmations,['bridge_confirm','bridge_picture'])
        self.assertEqual(commands.count('bridge-runtime-restart'),1)
        self.assertNotIn('bridge_rolled_back',messages)
        self.assertEqual(system.actions,old_runtime_actions+[
            ('stop','steam.service'),('restart','steamvr.service'),('start','steam.service')])
        self.assertEqual(system.live_client_vr_restarts,0)
        self.assertFalse((self.froot/'lifecycle.json').exists())
        self.assertLess(commands.index('bridge-restore'),commands.index('bridge-prepare'))
        self.assertEqual(installer.state()['bundle'],updated.pin)
        self.assertEqual(self.unit.tx.state()['bundle'],updated.pin)
        self.assertTrue(inventories[-1]['pc']['installed']);self.assertTrue(inventories[-1]['frame']['installed'])
        self.assertFalse(installer.checkpoint().path.exists());self.assertFalse(self.unit.checkpoint().path.exists())
        records=[s.parse(line) for path in (self.local.root/'logs').glob('setup-*.jsonl') for line in s.read(path).splitlines()]
        picture=[item for item in records if item['stage']=='picture_check']
        self.assertEqual([item['status'] for item in picture],['started','ok'])
        self.assertTrue(all(item['action']=='update' for item in picture))
        self.assertTrue(all(item['versions']['frame']['installed_version']=='0.2.6' for item in picture))

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

    def test_continuation_checkpoint_keeps_exact_original_predecessors_across_new_update(self):
        installer=b.LocalInstall(s,self.local)
        installer.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631)
        stable_receipt=s.read(installer.receipt);stable_settings=s.parse(s.read(self.settings))
        image=self.local.root/'pc-provider/bin/win64/driver_steam_link_native_passthrough.dll'
        stable_image=s.read(image)
        installer.install(self.newer_bundle(),self.local.inventory(),self.pair,'192.168.1.4',47631,backup=True)
        checkpoint=installer.checkpoint();old=checkpoint.value()
        checkpoint.restore();checkpoint.verify_restored()
        newest=self.newer_bundle();identity=checkpoint.reconcile(newest.pin)
        ready=s.read(checkpoint.path);self.assertEqual(checkpoint.reconcile(newest.pin),identity)
        self.assertEqual(s.read(checkpoint.path),ready)
        installer.install(newest,self.local.inventory(),self.pair,'192.168.1.4',47631,backup=True)
        active=checkpoint.value();self.assertEqual(active['continuation']['phase'],'active')
        self.assertEqual(active['continuation']['id'],identity)
        self.assertEqual(active['registered'],old['registered'])
        for previous in old['operations']:
            current=next(item for item in active['operations'] if item['path']==previous['path'])
            self.assertEqual(current['before'],previous['before'])
        checkpoint.restore();checkpoint.verify_restored()
        self.assertEqual(s.read(installer.receipt),stable_receipt)
        self.assertEqual(s.read(image),stable_image)
        self.assertEqual(s.parse(s.read(self.settings)),stable_settings)
        self.assertTrue(checkpoint.path.exists(),'Disk restoration is not a completed UI recovery')

    def test_continuation_refuses_foreign_or_unrestored_state_and_keeps_journal(self):
        installer=b.LocalInstall(s,self.local)
        installer.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631,backup=True)
        checkpoint=installer.checkpoint();old=s.read(checkpoint.path)
        with self.assertRaisesRegex(s.SetupError,'changed before recovery'):checkpoint.reconcile('a'*64)
        self.assertEqual(s.read(checkpoint.path),old)
        checkpoint.restore();checkpoint.reconcile('a'*64);ready=s.read(checkpoint.path)
        with self.assertRaisesRegex(s.SetupError,'unfinished setup recovery'):
            checkpoint.begin('update',[],[],target='b'*64)
        self.assertEqual(s.read(checkpoint.path),ready)
        # A known path changed after the ready handoff is still foreign, even
        # if the next package is otherwise valid and targets the right ID.
        image=self.local.root/'pc-provider/bin/win64/driver_steam_link_native_passthrough.dll'
        s.put(image,b'foreign after handoff')
        with self.assertRaisesRegex(s.SetupError,'changed before recovery'):
            checkpoint.begin('update',[],[],target='a'*64)
        self.assertEqual(s.read(image),b'foreign after handoff')
        self.assertEqual(s.read(checkpoint.path),ready)

    def test_continuation_keeps_prior_owned_scope_even_when_new_package_does_not_write_it(self):
        installer=b.LocalInstall(s,self.local);checkpoint=installer.checkpoint()
        helper=b.load_helper(s,'steam_link_setup_checkpoint')
        older=self.local.root/'pairings'/('34'*16+'.json')
        newer=self.local.root/'pairings'/('56'*16+'.json')
        s.put(older,b'original owned predecessor')
        original_setting=checkpoint.setting(self.settings,b.SETTINGS_SECTION,'controlPort',[{'present':True,'value':9999}])
        checkpoint.begin('update',[checkpoint.operation(older,[helper.file_value(b'old attempted replacement')])],[original_setting],True)
        s.put(older,b'old attempted replacement',s.read(older));checkpoint.restore()
        checkpoint.reconcile('c'*64)
        checkpoint.begin('repair',[checkpoint.operation(newer,[helper.file_value(b'new replacement')])],[],True,target='c'*64)
        value=checkpoint.value()
        retained=next(item for item in value['operations'] if item['path']==str(older))
        self.assertEqual(retained['before'],helper.file_value(b'original owned predecessor'))
        self.assertEqual(retained['after'],[retained['before']])
        self.assertEqual(value['settings'][0]['before'],original_setting['before'])
        s.put(newer,b'new replacement');checkpoint.restore();checkpoint.verify_restored()
        self.assertEqual(s.read(older),b'original owned predecessor');self.assertFalse(newer.exists())

    def test_lost_atomic_handoff_reply_keeps_original_undo_authority(self):
        installer=b.LocalInstall(s,self.local);checkpoint=installer.checkpoint()
        helper=b.load_helper(s,'steam_link_setup_checkpoint')
        path=self.local.root/'pairings'/('78'*16+'.json');s.put(path,b'original')
        checkpoint.begin('update',[checkpoint.operation(path,[helper.file_value(b'earlier attempt')])],[])
        checkpoint.reconcile('d'*64)
        ready=s.read(checkpoint.path);write=s.put
        def lost_reply(target,raw,expected=None):
            write(target,raw,expected)
            if target==checkpoint.path and s.parse(raw).get('continuation',{}).get('phase')=='active':
                raise OSError('atomic journal published, reply lost')
        with mock.patch.object(s,'put',side_effect=lost_reply),self.assertRaises(OSError):
            checkpoint.begin('update',[checkpoint.operation(path,[helper.file_value(b'new attempt')])],[],target='d'*64)
        value=checkpoint.value();self.assertEqual(value['continuation']['phase'],'active')
        self.assertEqual(value['continuation']['previous'],s.sha(ready))
        self.assertEqual(value['operations'][0]['before'],helper.file_value(b'original'))
        self.assertEqual(s.read(path),b'original');checkpoint.restore();checkpoint.verify_restored()

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

    def seed_retired_bridge_files(self):
        # Historical receipt from the previous paired package. Never execute an
        # old shader generator; only the exact owned migration is under test.
        state=self.unit.tx.state();before=s.read(self.unit.tx.path)
        for name in sorted(b.RETIRED_FRAME_FILES):
            path=self.froot/name;raw=('owned obsolete '+name).encode()
            operation=self.unit.tx.operation(path,raw)
            self.unit.tx.replace(operation);state['operations'].append(operation)
        s.put(self.unit.tx.path,s.encoded(state),before)
        self.unit.tx.commit()

    def test_update_removes_retired_owned_files_and_undo_restores_exact_previous_version(self):
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.unit.tx.commit();self.seed_retired_bridge_files()
        receipt=s.read(self.unit.tx.path)
        previous={self.froot/name:s.read(self.froot/name) for name in b.RETIRED_FRAME_FILES}
        next_bundle=self.newer_bundle();self.active=True
        with self.fake_generation():
            self.unit.install(next_bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
        self.unit.tx.commit()
        self.assertTrue(all(not path.exists() for path in previous))
        self.assertTrue((self.froot/'coverage.spv').exists())
        self.assertTrue(self.unit.inventory(next_bundle)['files_current']);self.assertTrue(self.active)
        self.unit.checkpoint().restore();self.unit.checkpoint().verify_restored()
        self.assertEqual(s.read(self.unit.tx.path),receipt)
        self.assertTrue(all(s.read(path)==raw for path,raw in previous.items()))
        self.unit.checkpoint().finish(restored=True)

    def test_changed_retired_file_refuses_update_before_any_write(self):
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.unit.tx.commit();self.seed_retired_bridge_files();next_bundle=self.newer_bundle()
        path=self.froot/'legacy.spv';s.put(path,b'foreign changed shader',s.read(path))
        receipt=s.read(self.unit.tx.path)
        with self.fake_generation(),self.assertRaisesRegex(s.SetupError,'Changed Frame files'):
            self.unit.install(next_bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
        self.assertEqual(s.read(path),b'foreign changed shader');self.assertEqual(s.read(self.unit.tx.path),receipt)
        self.assertFalse(self.unit.checkpoint().path.exists())

    def test_update_refuses_a_retired_receiver_reactivated_after_bridge_install(self):
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.unit.tx.commit();next_bundle=self.newer_bundle()
        # Historical helper always used home/.config, independent of XDG.
        self.unit.config=self.fhome/'different-xdg-config'
        old=self.fhome/'.config/systemd/user/steamvr.service.d/80-steam-frame-native-passthrough.conf'
        s.put(old,b'[Service]\n# separately installed old receiver\n');receipt=s.read(self.unit.tx.path)
        image=s.read(self.froot/b.LIBRARY)
        with self.fake_generation(),self.assertRaisesRegex(s.SetupError,'retired Frame receiver is active'):
            self.unit.install(next_bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
        self.assertEqual(s.read(old),b'[Service]\n# separately installed old receiver\n')
        self.assertEqual(s.read(self.unit.tx.path),receipt);self.assertEqual(s.read(self.froot/b.LIBRARY),image)
        self.assertFalse(self.unit.checkpoint().path.exists())

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
        self.active=True;before=s.read(self.fsettings);initial_mode=self.fsettings.stat().st_mode&0o777
        with self.fake_generation(),mock.patch.object(self.unit,'cold',side_effect=AssertionError('UI must remain running')):
            self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
        self.unit.tx.commit();self.assertTrue(self.unit.checkpoint().path.exists())
        self.assertEqual(s.read(self.fsettings),before)
        self.assertEqual(self.fsettings.stat().st_mode&0o777,initial_mode)
        self.assertEqual(self.unit.checkpoint().value()['settings'],[])
        preferences=s.parse(s.read(self.fsettings));preferences['user']['later']=8
        s.put(self.fsettings,s.encoded(preferences),s.read(self.fsettings))
        later_mode=self.fsettings.stat().st_mode&0o777
        self.unit.checkpoint().restore();self.assertIsNone(self.unit.tx.state())
        self.unit.checkpoint().verify_restored()
        expected=s.parse(before);expected['user']['later']=8
        self.assertEqual(s.parse(s.read(self.fsettings)),expected)
        # Session-policy setup owns no persistent camera/file-mode edit. A
        # later preferences writer's mode must survive this unrelated Undo.
        self.assertEqual(self.fsettings.stat().st_mode&0o777,later_mode)
        self.assertEqual(self.camera_native_calls,[])
        self.assertFalse((self.froot/b.LIBRARY).exists());self.assertTrue(self.active)
        self.unit.checkpoint().finish(restored=True)
        self.assertFalse(self.unit.checkpoint().path.exists())
        self.assertFalse(any('stop' in cmd for cmd in self.commands))

    def test_failed_frame_repair_restores_existing_installation_not_uninstall(self):
        preferences=s.read(self.fsettings)
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.unit.tx.commit();receipt=s.read(self.unit.tx.path)
        missing=self.froot/'coverage.spv';missing.unlink();self.active=True
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
        self.assertTrue(missing.exists());self.unit.checkpoint().restore()
        self.assertFalse(missing.exists());self.assertEqual(s.read(self.unit.tx.path),receipt)
        self.assertTrue((self.froot/b.LIBRARY).exists())
        self.assertEqual(self.unit.tx.state()['settings']['policy'],b.SESSION_POLICY)
        self.assertEqual(s.read(self.fsettings),preferences)
        self.assertFalse(s.parse(s.read(self.fsettings))['camera']['forceSceneAlphaBlending'])
        self.assertEqual(self.camera_native_calls,[])
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
        questions=[]
        def confirm(key):questions.append(key);return True
        screen.confirm=confirm
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
        self.assertNotIn('bridge_original_picture',questions,'A pending operation failure must be resolved before asking for final Yes')

    def test_confirmed_finish_retry_reconciles_real_records_after_all_receipts_were_removed(self):
        # Actual production finalizer, checkpoint files and lifecycle methods;
        # only systemctl/process adapters are explicit no-device fixtures.
        pc_fixture=load('sfnp_finish_pc_fixture',ROOT/'tests/test_setup_pc.py')
        frame_fixture=load('sfnp_finish_frame_fixture',ROOT/'tests/test_setup_frame.py')
        installed=b.LocalInstall(s,self.local)
        installed.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631,backup=True)
        with self.fake_generation():
            self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
        self.unit.tx.commit()
        adapter=pc_fixture.ChildAdapter(self.runtime);self.addCleanup(adapter.close)
        runtime=pc_fixture.pc.Lifecycle(s,self.local,self.runtime,adapter=adapter);runtime.snapshot()
        system=frame_fixture.System()
        frame=frame_fixture.frame.FrameLifecycle(s,self.froot,runner=system,
            sleeper=system.sleep,monotonic=lambda:system.now,poll_attempts=4,poll_interval=.01,phase_timeout=1)
        frame.restart();original_actions=list(system.actions);commands=[]
        class Peer:
            def call(inner,request):
                commands.append(request['command'])
                if request['command']=='bridge-runtime-restored':
                    return frame.restored(health_ok=request['health_ok'])
                return b.remote_request(s,request,unit=self.unit)
        ui=UI(['retry']);ui.suspend=lambda:contextlib.nullcontext()
        ports=object.__new__(b.Automation)
        ports.e=s;ports.bundle=self.bundle;ports.local=self.local;ports.installed=installed
        ports.remote=Peer();ports.ui=ui;ports.runtime=runtime;ports.context={'log':None,'stage':'complete'}
        ports.address='192.168.1.25';ports.login='steamos'
        cache=s.remember_frame;remembered=[]
        def lost_final_reply(*args):
            remembered.append(1)
            if len(remembered)==1:raise OSError('owned final response lost after all receipts were removed')
            return cache(*args)
        with mock.patch.object(s,'remember_frame',side_effect=lost_final_reply):
            self.assertTrue(flow.Workflow(ports,ui).finish_confirmed(restored=False))
        self.assertEqual(commands,['bridge-runtime-restored','bridge-checkpoint-finish']*2)
        self.assertEqual(len(remembered),2)
        self.assertEqual(system.actions,original_actions)
        self.assertEqual(adapter.launches,0);self.assertEqual(adapter.graceful_calls,[])
        for path in (installed.checkpoint().path,self.unit.checkpoint().path,runtime.path,frame.path):
            self.assertFalse(path.exists(),str(path))
        self.assertTrue(s.parse(s.read(self.local.root/'frame-address.json'))['host']=='192.168.1.25')
        self.assertIsNotNone(installed.state());self.assertIsNotNone(self.unit.tx.state())
        self.assertIn('bridge_finalize_incomplete',ui.messages)

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
    def __init__(self,choices=(),picture=True):self.choices=iter(choices);self.messages=[];self.picture=picture;self.stopped=False
    def say(self,key):self.messages.append(key)
    def confirm(self,key):return True if key=='bridge_original_picture' else self.picture
    def choose_action(self,actions,recommended=None):return next(self.choices,'exit')
    def cancelled(self):return self.stopped


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
    def assert_retained_cancellation(self,ports,ui):
        self.assertNotIn('restore_files',ports.events)
        self.assertNotIn('restart_original',ports.events)
        self.assertNotIn('original_health',ports.events)
        self.assertNotIn('finished_original',ports.events)
        self.assertNotIn('finished_prepared',ports.events)
        self.assertNotIn('allow_recovery',ports.events)
        self.assertNotIn('bridge_rolled_back',ui.messages)
        self.assertIn('bridge_cancelled_retained',ui.messages)

    def test_cancel_during_pending_preparation_keeps_records_without_restart(self):
        ports=Ports();ui=UI()
        def reconcile():
            ports.mark('reconcile');raise KeyboardInterrupt()
        ports.reconcile_for_apply=reconcile
        self.assertEqual(flow.Workflow(ports,ui).run('update',reconcile=True),130)
        self.assert_retained_cancellation(ports,ui)
        self.assertNotIn('begin',ports.events);self.assertNotIn('restart_prepared',ports.events)

    def test_pending_preparation_restarts_only_the_prepared_package(self):
        ports=Ports();ui=UI();questions=[]
        ports.reconcile_for_apply=lambda:ports.mark('reconcile')
        ui.confirm=lambda key:questions.append(key) or True
        self.assertEqual(flow.Workflow(ports,ui).run('update',reconcile=True),0)
        self.assertEqual(ports.events.count('restart_prepared'),1)
        self.assertNotIn('restart_original',ports.events);self.assertNotIn('finished_original',ports.events)
        self.assertEqual(questions,['bridge_picture'])

    def test_picture_log_failure_cannot_turn_yes_into_rollback(self):
        ports=Ports();ui=UI()
        ports.picture_observation=lambda **kw:(_ for _ in ()).throw(OSError('log unavailable'))
        self.assertEqual(flow.Workflow(ports,ui).run('update'),0)
        self.assertEqual(ports.events.count('restart_prepared'),1)
        self.assertNotIn('restore_files',ports.events);self.assertNotIn('restart_original',ports.events)

    def test_keyboard_interrupt_at_each_stage_retains_checkpoints_without_undo_or_retry(self):
        class SessionCancelled(KeyboardInterrupt):pass
        stages=('begin','stop_pc','prepare','restart_prepared','prepared_health','picture')
        for exception in (KeyboardInterrupt,SessionCancelled):
            for stage in stages:
                with self.subTest(exception=exception.__name__,stage=stage):
                    ports=Ports();ui=UI()
                    ui.allow_recovery=lambda:ports.mark('allow_recovery')
                    ui.choose_action=lambda *a,**kw:self.fail('Explicit Ctrl+C must not offer retry or undo')
                    def cancel(*a,**kw):
                        ports.mark('cancel_'+stage)
                        raise exception()
                    if stage=='picture':ui.confirm=cancel
                    else:setattr(ports,stage,cancel)
                    self.assertEqual(flow.Workflow(ports,ui).run('install'),130)
                    self.assert_retained_cancellation(ports,ui)
                    self.assertEqual(ports.events.count('failure'),1)

    def test_cooperative_cancel_after_each_operation_stops_before_the_next_one(self):
        for stage in ('begin','stop_pc','prepare','restart_prepared','prepared_health'):
            with self.subTest(stage=stage):
                ports=Ports();ui=UI();original=getattr(ports,stage)
                ui.allow_recovery=lambda:ports.mark('allow_recovery')
                ui.choose_action=lambda *a,**kw:self.fail('Explicit Ctrl+C must not offer retry')
                def complete_then_cancel(*a,**kw):
                    result=original(*a,**kw);ui.stopped=True;return result
                setattr(ports,stage,complete_then_cancel)
                self.assertEqual(flow.Workflow(ports,ui).run('install'),130)
                self.assert_retained_cancellation(ports,ui)
                operations=[event for event in ports.events if event!='failure']
                self.assertEqual(operations[-1],'prepare_install' if stage=='prepare' else stage)

    def test_ctrl_c_modal_answer_is_not_treated_as_a_rejected_or_accepted_picture(self):
        for answer in (False,True):
            with self.subTest(modal_answer=answer):
                ports=Ports();ui=UI()
                def picture(key):ui.stopped=True;return answer
                ui.confirm=picture
                self.assertEqual(flow.Workflow(ports,ui).run('install'),130)
                self.assert_retained_cancellation(ports,ui)

    def test_cancel_before_begin_does_not_create_an_operation(self):
        ports=Ports();ui=UI();ui.stopped=True
        self.assertEqual(flow.Workflow(ports,ui).run('install'),130)
        self.assertEqual(ports.events,['failure'])
        self.assert_retained_cancellation(ports,ui)

    def test_cancel_during_slow_health_choice_does_not_start_rollback(self):
        ports=Ports(prepared={'status':'not-ready','software_ready':False});ui=UI()
        def choose(*a,**kw):ui.stopped=True;return 'rollback'
        ui.choose_action=choose
        self.assertEqual(flow.Workflow(ports,ui).run('install'),130)
        self.assert_retained_cancellation(ports,ui)

    def test_cancel_racing_a_genuine_failure_is_not_cleared_by_recovery(self):
        ports=Ports();ui=UI()
        ui.allow_recovery=lambda:ports.mark('allow_recovery')
        def failure(*a):ui.stopped=True;raise ValueError('ordinary error after Ctrl+C')
        ports.prepare=failure
        self.assertEqual(flow.Workflow(ports,ui).run('install'),130)
        self.assert_retained_cancellation(ports,ui)

    def test_explicit_recovery_cancel_does_not_continue_or_offer_retry(self):
        for stage in ('entry','allow_recovery','restore_files','restart_original','original_health'):
            with self.subTest(stage=stage):
                ports=Ports();ui=UI()
                ui.choose_action=lambda *a,**kw:self.fail('Interrupted recovery must stop without retry')
                if stage=='entry':ui.stopped=True
                elif stage=='allow_recovery':
                    def resume():ports.mark('allow_recovery');ui.stopped=True
                    ui.allow_recovery=resume
                else:
                    original=getattr(ports,stage)
                    def complete_then_cancel(*a,**kw):
                        result=original(*a,**kw);ui.stopped=True;return result
                    setattr(ports,stage,complete_then_cancel)
                with self.assertRaises(KeyboardInterrupt):flow.Workflow(ports,ui).recover()
                self.assertNotIn('finished_original',ports.events)
                self.assertNotIn('bridge_rolled_back',ui.messages)
                self.assertIn('bridge_cancelled_retained',ui.messages)
                self.assertEqual(ports.events.count('recovery_failure'),1)
                if stage=='restore_files':self.assertNotIn('restart_original',ports.events)
                if stage=='entry':self.assertEqual(ports.events,['recovery_failure'])

    def test_interrupt_during_genuine_failure_recovery_returns_130_once(self):
        ports=Ports(prepared=False);ui=UI()
        def interrupt():ports.mark('restore_files');raise KeyboardInterrupt()
        ports.restore_files=interrupt
        self.assertEqual(flow.Workflow(ports,ui).run('install'),130)
        self.assertNotIn('restart_original',ports.events)
        self.assertNotIn('finished_original',ports.events)
        self.assertEqual(ports.events.count('failure'),1)
        self.assertEqual(ports.events.count('recovery_failure'),1)
        self.assertEqual(ui.messages.count('bridge_cancelled_retained'),1)

    def test_yes_followed_by_partial_finalize_failure_never_undoes_or_retries_writes(self):
        ports=Ports();ui=UI(['exit']);questions=[]
        def confirm(key):questions.append(key);return True
        def finish(*,restored):
            ports.mark('partial_finish')
            raise OSError('owned checkpoint already partly finalized')
        ui.confirm=confirm;ports.finish=finish
        self.assertEqual(flow.Workflow(ports,ui).run('install'),2)
        self.assertEqual(questions,['bridge_picture'])
        self.assertEqual(ports.events.count('partial_finish'),1)
        self.assertEqual(ports.events.count('prepared_health'),1)
        self.assertNotIn('restore_files',ports.events)
        self.assertNotIn('restart_original',ports.events)
        self.assertNotIn('bridge_installed',ui.messages)
        self.assertIn('bridge_finalize_incomplete',ui.messages)

    def test_original_yes_finalize_failure_does_not_repeat_restore_or_restart(self):
        ports=Ports();ui=UI(['exit'])
        def finish(*,restored):
            self.assertTrue(restored);ports.mark('partial_finish')
            raise OSError('owned final recovery record unavailable')
        ports.finish=finish
        self.assertFalse(flow.Workflow(ports,ui).recover())
        self.assertEqual(ports.events.count('restore_files'),1)
        self.assertEqual(ports.events.count('restart_original'),1)
        self.assertEqual(ports.events.count('original_health'),1)
        self.assertEqual(ports.events.count('partial_finish'),1)
        self.assertIn('bridge_finalize_incomplete',ui.messages)
        self.assertNotIn('bridge_rolled_back',ui.messages)

    def test_yes_finalize_retry_repeats_only_reconciled_finish_and_not_the_operation(self):
        ports=Ports();ui=UI(['retry']);finish=ports.finish;calls=[]
        def incomplete(*,restored):
            calls.append(restored)
            if len(calls)==1:raise OSError('owned lost finalization reply')
            finish(restored=restored)
        ports.finish=incomplete
        self.assertEqual(flow.Workflow(ports,ui).run('install'),0)
        self.assertEqual(calls,[False,False])
        self.assertEqual(ports.events.count('begin'),1)
        self.assertEqual(ports.events.count('prepared_health'),1)
        self.assertEqual(ports.events.count('restart_prepared'),1)
        self.assertEqual(ports.events.count('prepare_install'),1)
        self.assertNotIn('restore_files',ports.events)

    def test_notice_failure_after_finalization_preserves_committed_success(self):
        ports=Ports();ui=UI()
        def say(key):
            if key=='bridge_pc_stopped':raise RuntimeError('owned notification error')
            ui.messages.append(key)
        ui.say=say
        self.assertEqual(flow.Workflow(ports,ui).run('install'),0)
        self.assertEqual(ports.events.count('finished_prepared'),1)
        self.assertNotIn('restore_files',ports.events)
        self.assertNotIn('restart_original',ports.events)
        self.assertIn('bridge_installed',ui.messages)

    def test_success_stages_then_restarts_then_checks(self):
        ports=Ports();ui=UI();self.assertEqual(flow.Workflow(ports,ui).run('install'),0)
        self.assertEqual(ports.events,['begin','stop_pc','prepare_install','restart_prepared','prepared_health','finished_prepared'])

    def test_failed_health_automatically_restores_and_restarts_original(self):
        ports=Ports(prepared=False);ui=UI();self.assertEqual(flow.Workflow(ports,ui).run('install'),2)
        self.assertIn('bridge_rolled_back',ui.messages)
        self.assertEqual(ports.events[-4:],['restore_files','restart_original','original_health','finished_original'])

    def test_bad_picture_offers_read_only_checks_and_explicit_restore_without_manual_kill(self):
        ports=Ports();ui=UI(['rollback','exit'],picture=False)
        offered=[]
        def choose(actions,recommended=None):
            offered.append((actions,recommended,ports.events[:]))
            return next(ui.choices)
        ui.choose_action=choose
        self.assertEqual(flow.Workflow(ports,ui).run('repair'),2)
        self.assertEqual(offered[0][:2],(['verify','rollback','exit'],'verify'))
        self.assertEqual(offered[0][2].count('prepared_health'),2)
        self.assertNotIn('restore_files',offered[0][2])
        self.assertNotIn('restart_original',offered[0][2])
        self.assertIn('finished_original',ports.events)

    def test_no_then_retry_rechecks_without_restarting_and_requires_a_new_yes(self):
        ports=Ports();ui=UI(['verify']);answers=iter([False,True]);questions=[]
        def confirm(key):questions.append(key);return next(answers)
        ui.confirm=confirm
        self.assertEqual(flow.Workflow(ports,ui).run('install'),0)
        self.assertEqual(questions,['bridge_picture','bridge_picture'])
        self.assertEqual(ports.events.count('prepared_health'),3)
        self.assertEqual(ports.events.count('restart_prepared'),1)
        self.assertNotIn('restore_files',ports.events)
        self.assertIn('finished_prepared',ports.events)

    def test_no_and_failed_recheck_never_automatically_restarts_or_restores(self):
        for result in (False,{'status':'not-ready','software_ready':False},OSError('read-only connection failed')):
            with self.subTest(result=type(result).__name__):
                ports=Ports();ui=UI(['exit'],picture=False);attempts=[]
                def health():
                    ports.mark('prepared_health');attempts.append(1)
                    if len(attempts)==1:return True
                    if isinstance(result,Exception):raise result
                    return result
                ports.prepared_health=health
                self.assertEqual(flow.Workflow(ports,ui).run('install'),130)
                self.assertEqual(len(attempts),2)
                self.assert_retained_cancellation(ports,ui)
                self.assertIn('bridge_picture_checks_unconfirmed',ui.messages)
                self.assertEqual(ports.events.count('restart_prepared'),1)

    def test_cancel_picture_is_not_no_and_never_checks_or_restores_again(self):
        ports=Ports();ui=UI();ui.confirm=lambda key:None
        self.assertEqual(flow.Workflow(ports,ui).run('install'),130)
        self.assertEqual(ports.events.count('prepared_health'),1)
        self.assert_retained_cancellation(ports,ui)

    def test_restore_keeps_records_until_original_picture_is_confirmed(self):
        ports=Ports(prepared=False);ui=UI(['verify','exit']);answers=iter([False,True]);questions=[]
        def picture(key):
            self.assertNotIn('finished_original',ports.events)
            questions.append(key);return next(answers)
        ui.confirm=picture
        self.assertEqual(flow.Workflow(ports,ui).run('install'),2)
        self.assertEqual(questions,['bridge_original_picture','bridge_original_picture'])
        self.assertEqual(ports.events.count('original_health'),3)
        self.assertEqual(ports.events.count('restart_original'),1)
        self.assertEqual(ports.events.count('finished_original'),1)

    def test_restore_with_bad_original_picture_cannot_claim_completed_or_discard_records(self):
        ports=Ports(prepared=False);ui=UI(['exit']);ui.confirm=lambda key:False
        self.assertEqual(flow.Workflow(ports,ui).run('install'),130)
        self.assertNotIn('finished_original',ports.events)
        self.assertNotIn('bridge_rolled_back',ui.messages)
        self.assertEqual(ports.events.count('restart_original'),1)
        self.assertEqual(ports.events.count('original_health'),2)

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
