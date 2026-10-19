"""Owned-file, native-ABI and real-pipe controls; never load Frame libraries."""
import ctypes
import importlib.util
import io
import json
import os
from pathlib import Path
import queue
import sys
import tempfile
import threading
import unittest
from unittest import mock

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location('stream_bridge_camera', Path(__file__).resolve().parents[1] / 'tools/stream_bridge_camera.py')
tool = importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(tool)


class FakeCamera:
    def __init__(self, owner, value=False, persist=True, default=False):
        self.owner, self.value, self.persist, self.default = owner, value, persist, default
        self.calls = [];self.closed = False;self.fail_read = False;self.refuse = False
    def __enter__(self):return self
    def __exit__(self, *unused):self.closed = True
    def read(self):
        if self.fail_read:raise tool.SafetyError('simulated native generation loss')
        return self.value
    def change(self, value, remove=False):
        # Actual production ordering: ownership must precede the first True.
        if value is True and not (self.owner.root / tool.PAUSE).exists():
            assert (self.owner.root / tool.LEASE).exists()
        self.calls.append(('remove',) if remove else ('write',value))
        if self.refuse:return
        self.value = self.default if remove else value
        if self.persist:
            body = json.loads(self.owner.settings.read_text())
            if remove:
                body.get('camera', {}).pop(tool.KEY, None)
                if not body.get('camera'):body.pop('camera', None)
            else:body.setdefault('camera', {})[tool.KEY] = value
            self.owner.write_settings(body)
    def write(self, value):self.change(value)
    def remove_override(self):self.change(None, True)


class Controls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='sfnp-camera-control-');self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name);self.root.chmod(0o700)
        self.settings = self.root / 'steamvr.vrsettings'
        self.write_settings({'camera':{tool.KEY:False,'keep':31},'unrelated':{'ok':True}})
        self.config = {'schema':tool.CONFIG_SCHEMA,'settings':str(self.settings),
            'library':{'path':str(self.root / 'libopenvr_api.so'),'bytes':249168,'sha256':'ab'*32}}
        self.write_config();self.camera = FakeCamera(self)
    def write_settings(self, value):
        self.settings.write_bytes(tool.encoded(value));self.settings.chmod(0o640)
    def write_config(self):
        path = self.root / tool.CONFIG;path.write_bytes(tool.encoded(self.config));path.chmod(0o600)
    def factory(self, pin):return self.camera
    def session(self):
        config, sha = tool.load_config(self.root)
        return tool.Session(self.root, config, sha, self.camera)
    def run_serve(self, commands):
        out = io.StringIO();tool.serve(self.root, input_stream=io.BytesIO(commands), output_stream=out, api_factory=self.factory)
        return out.getvalue()
    def legacy(self, desired=None, grant='11'*32):
        self.config['legacy_migration'] = {'id':grant,'section_present':desired is not None,
            'present':desired is not None,'value':desired,'installed':True}
        self.write_config()
    def dead_lease(self):
        session = self.session();session.enable()
        value = dict(session.lease);value.update(helper_pid=2147483646,helper_start_ticks=1);session.save(value)
        return session
    def test_enable_ack_disable_repeat_eof_and_unrelated_preservation(self):
        before = json.loads(self.settings.read_text())
        self.assertEqual(self.run_serve(b'E 1\nD 2\nE 3\n'), 'R 0\nA 1\nD 2\nA 3\n')
        self.assertEqual(json.loads(self.settings.read_text()), before)
        self.assertEqual(self.camera.calls,[('write',True),('write',False)]*2)
        self.assertFalse((self.root / tool.LEASE).exists());self.assertTrue(self.camera.closed)
        self.assertEqual(self.settings.stat().st_mode & 0o777,0o640)
    def test_absent_override_removes_only_owned_key_with_false_default(self):
        self.write_settings({'unrelated':42});self.camera.value = False
        self.run_serve(b'E 1\nD 2\n')
        self.assertEqual(json.loads(self.settings.read_text()),{'unrelated':42})
        self.assertEqual(self.camera.calls,[('write',True),('remove',)])
    def test_existing_user_true_is_preserved_without_writes(self):
        self.write_settings({'camera':{tool.KEY:True}});self.camera.value = True
        self.run_serve(b'E 1\nD 2\n');self.assertEqual(self.camera.calls,[])
    def test_receipt_precedes_enable_and_failed_ack_is_recoverable(self):
        self.camera.refuse = True
        with self.assertRaisesRegex(tool.SafetyError,'not acknowledged'):self.run_serve(b'E 1\n')
        self.assertEqual(self.camera.calls,[('write',True)])
        self.assertTrue((self.root / tool.LEASE).exists())
    def test_disable_ack_does_not_claim_unconfirmed_file_persistence(self):
        self.camera.persist = False
        self.assertEqual(self.run_serve(b'E 1\nD 2\n'),'R 0\nA 1\nD 2\n')
        self.assertTrue((self.root / tool.LEASE).exists());self.assertIs(self.camera.value,False)
    def test_eof_and_bad_sequences_restore_runtime(self):
        for suffix in (b'E 1\n',b'D 0\n',b'E 18446744073709551616\n',b'x\n',b'E 2\r\n'):
            with self.subTest(suffix=suffix):
                self.camera = FakeCamera(self)
                with self.assertRaises(tool.SafetyError):self.run_serve(b'E 1\n'+suffix)
                self.assertIs(self.camera.value,False)
    def test_file_runtime_disagreement_never_claims_ownership(self):
        self.camera.value = True
        with self.assertRaisesRegex(tool.SafetyError,'disagrees'):self.session().enable()
        self.assertFalse((self.root / tool.LEASE).exists());self.assertEqual(self.camera.calls,[])
    def test_changed_live_override_is_not_overwritten(self):
        session = self.session();session.enable();self.write_settings({'camera':{tool.KEY:False}})
        self.camera.value = None
        with self.assertRaises(tool.SafetyError):session.restore()
        self.assertIsNone(self.camera.value);self.assertTrue((self.root / tool.LEASE).exists())
    def test_live_restore_quiesces_even_live_owner_and_blocks_renewal(self):
        session = self.session();session.enable()
        self.assertTrue(tool.owner_alive(session.lease))
        self.assertEqual(tool.restore_owned(self.root,api_factory=self.factory,persist_timeout=0),
            {'status':'restored','persisted':True})
        self.assertTrue((self.root / tool.PAUSE).exists());self.assertFalse((self.root / tool.LEASE).exists())
        with self.assertRaisesRegex(tool.SafetyError,'paused'):session.enable()
        self.assertIs(self.camera.value,False)
    def test_live_restore_refuses_unpersisted_native_change_retains_receipt(self):
        session = self.session();session.enable();self.camera.persist = False
        with self.assertRaisesRegex(tool.SafetyError,'persistence'):tool.restore_owned(self.root,api_factory=self.factory,persist_timeout=0)
        self.assertIs(self.camera.value,False);self.assertTrue((self.root / tool.LEASE).exists())
        self.assertIs(tool.settings(self.settings)[1],True)
    def test_exact_checkpoint_native_restore_and_foreign_value_refusal(self):
        authority = {'schema':'sfnp-camera-override-authority-v1','settings':str(self.settings),
            'before':False,'desired':True,'section_present':True}
        self.assertTrue(tool.apply_owned_override(self.root,authority,api_factory=self.factory,persist_timeout=0)['persisted'])
        self.assertIs(self.camera.value,True)
        with self.assertRaises(tool.SafetyError):tool.apply_owned_override(self.root,authority,api_factory=self.factory,persist_timeout=0)
        self.assertEqual(self.camera.calls,[('write',True)])
    def test_checkpoint_absent_override_accepts_native_false_default(self):
        self.write_settings({})
        authority = {'schema':'sfnp-camera-override-authority-v1','settings':str(self.settings),
            'before':None,'desired':True,'section_present':False}
        tool.apply_owned_override(self.root,authority,api_factory=self.factory,persist_timeout=0)
        authority.update(before=True,desired=None)
        tool.apply_owned_override(self.root,authority,api_factory=self.factory,persist_timeout=0)
        self.assertEqual(json.loads(self.settings.read_text()),{});self.assertIs(self.camera.value,False)
    def test_startup_restores_dead_owned_lease_without_loading_api(self):
        self.dead_lease()
        with mock.patch.object(tool.NativeCamera,'__enter__',side_effect=AssertionError('startup API forbidden')):
            self.assertEqual(tool.startup(self.root,running=lambda:False),0)
        self.assertIs(tool.settings(self.settings)[1],False);self.assertFalse((self.root / tool.LEASE).exists())
    def test_startup_preserves_live_unknown_owner_and_running_runtime(self):
        session = self.session();session.enable();before = self.settings.read_bytes()
        for running,alive in ((False,True),(False,None),(True,False)):
            with self.subTest(running=running,alive=alive):
                tool.startup(self.root,running=lambda:running,alive=lambda _:alive)
                self.assertEqual(self.settings.read_bytes(),before);self.assertTrue((self.root / tool.LEASE).exists())
    def test_stale_pid_reuse_is_not_live_owner(self):
        lease={'helper_pid':os.getpid(),'helper_start_ticks':tool.own_start(os.getpid())+1}
        self.assertIs(tool.owner_alive(lease),False)
    def test_startup_clears_valid_pause_only_after_recovery_no_live_runtime(self):
        _,sha=tool.load_config(self.root);tool.pause(self.root,sha)
        tool.startup(self.root,running=lambda:True);self.assertTrue((self.root/tool.PAUSE).exists())
        tool.startup(self.root,running=lambda:False);self.assertFalse((self.root/tool.PAUSE).exists())
    def test_legacy_restore_one_time_and_same_grant_update_preserves_later_true(self):
        self.legacy();self.write_settings({'camera':{tool.KEY:True,'keep':17}})
        tool.startup(self.root,running=lambda:False)
        self.assertEqual(json.loads(self.settings.read_text()),{'camera':{'keep':17}})
        self.write_settings({'camera':{tool.KEY:True}})
        self.config['library']['sha256']='cd'*32;self.write_config()
        tool.startup(self.root,running=lambda:False)
        self.assertIs(tool.settings(self.settings)[1],True)
    def test_legacy_new_grant_after_undo_may_restore_new_owned_true(self):
        self.legacy(False);self.write_settings({'camera':{tool.KEY:True}})
        tool.startup(self.root,running=lambda:False)
        self.legacy(None,'22'*32);self.write_settings({'camera':{tool.KEY:True}})
        tool.startup(self.root,running=lambda:False)
        self.assertIsNone(tool.settings(self.settings)[1])
    def test_legacy_foreign_false_spends_grant_preserves_later_user_true(self):
        self.legacy(None);tool.startup(self.root,running=lambda:False)
        self.assertIs(tool.settings(self.settings)[1],False)
        self.write_settings({'camera':{tool.KEY:True}});tool.startup(self.root,running=lambda:False)
        self.assertIs(tool.settings(self.settings)[1],True)
    def test_pending_migration_changed_signature_preserves_user_true(self):
        self.legacy(None);self.write_settings({'camera':{tool.KEY:True}})
        tool.migration_begin(self.root,self.config)
        self.write_settings({'camera':{tool.KEY:True},'changed':1})
        tool.startup(self.root,running=lambda:False)
        self.assertIs(tool.settings(self.settings)[1],True)
    def test_live_legacy_restore_uses_only_api_and_records_consumption(self):
        self.legacy(None);self.write_settings({'camera':{tool.KEY:True},'keep':17});self.camera.value=True
        self.assertTrue(tool.restore_owned(self.root,api_factory=self.factory,persist_timeout=0)['persisted'])
        self.assertEqual(self.camera.calls,[('remove',)]);self.assertEqual(json.loads(self.settings.read_text()),{'keep':17})
        marker=json.loads((self.root/tool.MIGRATION).read_text());self.assertEqual(marker['phase'],'consumed')
    def test_private_config_and_parent_symlink_guards(self):
        config=self.root/tool.CONFIG;config.chmod(0o644)
        with self.assertRaises(tool.SafetyError):tool.load_config(self.root)
        config.chmod(0o600);other=self.root/'other';other.write_text('{}');config.unlink();config.symlink_to(other)
        with self.assertRaises(tool.SafetyError):tool.load_config(self.root)
        self.assertEqual(other.read_text(),'{}')
    def test_unknown_lease_and_duplicate_json_preserved(self):
        self.dead_lease();path=self.root/tool.LEASE;value=json.loads(path.read_text());value['config_sha256']='00'*32
        path.write_bytes(tool.encoded(value));before=self.settings.read_bytes()
        tool.startup(self.root,running=lambda:False);self.assertEqual(self.settings.read_bytes(),before)
        with self.assertRaises(tool.SafetyError):tool.parsed(b'{"camera":{},"camera":{}}')
    def test_pause_signature_survives_config_library_update(self):
        _,sha=tool.load_config(self.root);tool.pause(self.root,sha)
        self.config['library']['sha256']='cd'*32;self.write_config()
        tool.startup(self.root,running=lambda:False);self.assertFalse((self.root/tool.PAUSE).exists())
    def test_real_pipe_coalesced_enable_disable_no_prefetch_starvation(self):
        read,write=os.pipe();os.write(write,b'E 1\nD 2\n');os.close(write)
        with os.fdopen(read,'rb') as stream:
            out=io.StringIO();tool.serve(self.root,input_stream=stream,output_stream=out,api_factory=self.factory)
        self.assertEqual(out.getvalue(),'R 0\nA 1\nD 2\n');self.assertIs(self.camera.value,False)
    def test_real_pipe_wanted_health_loss_revokes_before_exit(self):
        read,write=os.pipe();out=io.StringIO();events=queue.Queue()
        def run():
            try:
                with os.fdopen(read,'rb') as stream:
                    tool.serve(self.root,input_stream=stream,output_stream=out,api_factory=self.factory)
            except BaseException as error:events.put(error)
        thread=threading.Thread(target=run);thread.start()
        try:
            os.write(write,b'E 1\n')
            # Synchronize on the actual A, not a guessed callback/frame duration.
            for _ in range(100):
                if 'A 1\n' in out.getvalue():break
                threading.Event().wait(.01)
            self.assertIn('A 1\n',out.getvalue());self.camera.fail_read=True
            error=events.get(timeout=2);self.assertIsInstance(error,tool.SafetyError)
            thread.join(timeout=2);self.assertFalse(thread.is_alive());self.assertIn('N 1\n',out.getvalue())
            self.assertTrue((self.root/tool.LEASE).exists())
        finally:os.close(write);thread.join(timeout=3)
    def test_real_ctypes_table_slots_and_error_five_only(self):
        state={'value':False,'error':0};calls=[]
        @tool._FN(ctypes.c_char_p,ctypes.c_int)
        def error_name(error):return b'error'
        @tool._FN(None,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_bool,tool._ERR)
        def set_bool(section,key,value,error):calls.append((section,key,value));state['value']=bool(value);error[0]=0
        @tool._FN(ctypes.c_bool,ctypes.c_char_p,ctypes.c_char_p,tool._ERR)
        def get_bool(section,key,error):error[0]=state['error'];return state['value']
        @tool._FN(None,ctypes.c_char_p,ctypes.c_char_p,tool._ERR)
        def remove(section,key,error):calls.append((section,key,'remove'));state['value']=False;error[0]=0
        camera=tool.NativeCamera({});camera.fn=tool.SettingsTable(error_name,set_bool,None,None,None,get_bool,None,None,None,None,remove)
        camera.health=lambda:None
        camera.write(True);self.assertIs(camera.read(),True);camera.remove_override();self.assertIs(camera.read(),False)
        state['error']=5;self.assertIsNone(camera.read())
        state['error']=2
        with self.assertRaises(tool.SafetyError):camera.read()
        self.assertEqual(calls,[(b'camera',b'forceSceneAlphaBlending',True),(b'camera',b'forceSceneAlphaBlending','remove')])
    def test_unrelated_pre_disable_file_save_is_not_restore_persistence(self):
        self.camera.persist=False;session=self.session();session.enable()
        value=json.loads(self.settings.read_text());value['unrelated-save-before-D']=True;self.write_settings(value)
        session.restore()
        self.assertIs(self.camera.value,False)
        self.assertTrue((self.root/tool.LEASE).exists())
        self.assertIsNotNone(session.lease['restore_signature'])
        # Only a later native file generation matching the original override
        # permits ownership removal; an old baseline file is not a D receipt.
        self.write_settings(value);session.restore()
        self.assertFalse((self.root/tool.LEASE).exists())
    def test_repeated_enable_cannot_overwrite_changed_active_native_value(self):
        session=self.session();session.enable();self.camera.value=False
        with self.assertRaisesRegex(tool.SafetyError,'readiness lost'):session.enable()
        self.assertEqual(self.camera.calls,[('write',True)])
        self.assertIs(self.camera.value,False)
    def test_native_generation_change_refuses_before_getter(self):
        camera=tool.NativeCamera({});camera.generation=((10,11,'vrserver'),)
        camera.fn=mock.Mock()
        with mock.patch.object(tool,'runtime_generation',return_value=((12,13,'vrserver'),)):
            with self.assertRaisesRegex(tool.SafetyError,'generation'):camera.read()
        camera.fn.GetBool.assert_not_called()
    def test_pinned_library_mismatch_refuses_before_cdll_or_init(self):
        library=self.root/'libopenvr_api.so'
        raw=bytearray(64);raw[:7]=b'\x7fELF\x02\x01\x01';raw[18:20]=(183).to_bytes(2,'little')
        library.write_bytes(raw);library.chmod(0o600)
        pin={'path':str(library),'bytes':64,'sha256':'00'*32}
        with mock.patch.object(tool.os,'uname',return_value=mock.Mock(machine='aarch64')):
            with mock.patch.object(tool.ctypes,'CDLL',side_effect=AssertionError('unverified library load')):
                with self.assertRaisesRegex(tool.SafetyError,'identity'):
                    with tool.NativeCamera(pin):pass
    def test_reconnect_adopts_only_dead_lease_and_preserves_initial_baseline(self):
        self.dead_lease()
        camera=FakeCamera(self,True);self.camera=camera
        self.assertEqual(self.run_serve(b'E 1\nD 2\n'),'R 0\nA 1\nD 2\n')
        self.assertIs(camera.value,False);self.assertIs(tool.settings(self.settings)[1],False)
        self.assertEqual(camera.calls,[('write',False),('write',True),('write',False)])
    def test_unknown_pause_and_impossible_baseline_are_preserved(self):
        pause=self.root/tool.PAUSE;pause.write_bytes(tool.encoded({'schema':'unknown'}));pause.chmod(0o600)
        before=self.settings.read_bytes();tool.startup(self.root,running=lambda:False)
        self.assertEqual(self.settings.read_bytes(),before);self.assertTrue(pause.exists())
        pause.unlink();session=self.dead_lease()
        value=dict(session.lease);value['write_intent']=False
        path=self.root/tool.LEASE;path.write_bytes(tool.encoded(value))
        with self.assertRaises(tool.SafetyError):tool.load_lease(self.root,session.sha)
    def test_non_aarch64_refuses_before_any_library_load(self):
        with mock.patch.object(tool.ctypes,'CDLL',side_effect=AssertionError('load forbidden')):
            with self.assertRaises(tool.SafetyError):
                with tool.NativeCamera(self.config['library']):pass


if __name__=='__main__':unittest.main()
