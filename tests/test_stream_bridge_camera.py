"""Owned-file, native-ABI and real-pipe controls; never load Frame libraries."""
import ctypes
from contextlib import contextmanager
import errno
import importlib.util
import io
import json
import os
from pathlib import Path
import queue
import struct
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

    @contextmanager
    def proc_view(self, *, failed_pid=103, point='comm', number=errno.ESRCH, active=True):
        proc=self.root/'proc';proc.mkdir(exist_ok=True)
        for pid,name,start in ((101,'vrserver',11),(102,'vrcompositor',22),(103,'vrserver' if point=='start' else 'unrelated',33),(os.getpid(),'helper',44)):
            path=proc/str(pid);path.mkdir(exist_ok=True)
            (path/'comm').write_text(name+'\n');(path/'stat').write_text(str(pid)+' ('+name+') S '+'0 '*18+str(start)+' 0\n')
        state={'active':active};real_path=Path;real_stat=Path.stat;real_read=Path.read_text
        target=proc/str(failed_pid)
        if point!='stat':target/='stat' if point=='start' else 'comm'
        def mapped(value):
            value=str(value)
            return proc/ value.removeprefix('/proc/') if value.startswith('/proc/') else proc if value=='/proc' else real_path(value)
        def fail(path):
            if state['active'] and path==target:raise OSError(number,'private fixture path and key must never be logged',str(target))
        def checked_stat(path,*args,**kw):fail(path);return real_stat(path,*args,**kw)
        def checked_read(path,*args,**kw):fail(path);return real_read(path,*args,**kw)
        with mock.patch.object(tool,'Path',side_effect=mapped),mock.patch.object(Path,'stat',autospec=True,side_effect=checked_stat),mock.patch.object(Path,'read_text',autospec=True,side_effect=checked_read):
            yield state

    def test_proc_disappearance_errno_does_not_invalidate_other_runtime_members(self):
        for number in (errno.ENOENT,errno.ESRCH):
            for point in ('stat','comm','start'):
                with self.subTest(number=number,point=point),self.proc_view(point=point,number=number):
                    self.assertEqual(tool.runtime_generation(),((101,11,'vrserver'),(102,22,'vrcompositor')))

    def test_proc_churn_during_real_pipe_health_poll_keeps_valid_session(self):
        with self.proc_view(active=False) as state:
            native=tool.NativeCamera({});native.generation=tool.runtime_generation();original=self.camera.read
            def read():
                native.health();result=original()
                if self.camera.value is True:state['active']=True
                return result
            self.camera.read=read
            source,write=os.pipe();os.write(write,b'E 1\n');out=io.StringIO();polls=[]
            def select(reads,*args):
                polls.append(1)
                if len(polls)==2:return [],[],[]
                if len(polls)==3:os.write(write,b'D 2\n');os.close(write)
                return reads,[],[]
            try:
                with os.fdopen(source,'rb') as stream,mock.patch.object(tool.select,'select',side_effect=select):
                    tool.serve(self.root,input_stream=stream,output_stream=out,api_factory=self.factory)
                self.assertEqual(out.getvalue(),'R 0\nA 1\nD 2\n');self.assertGreaterEqual(len(polls),3)
                self.assertFalse((self.root/tool.LEASE).exists());self.assertIs(self.camera.value,False)
            finally:
                try:os.close(write)
                except OSError:pass

    def test_proc_permission_and_unknown_errors_remain_generation_fatal(self):
        for number in (errno.EACCES,errno.EPERM,errno.EIO):
            for point in ('stat','comm','start'):
                with self.subTest(number=number,point=point),self.proc_view(point=point,number=number):
                    with self.assertRaisesRegex(tool.SafetyError,'runtime state unavailable') as caught:tool.runtime_generation()
                    self.assertEqual(caught.exception.__cause__.errno,number)

    def test_real_pipe_health_revokes_for_runtime_disappearance_or_unknown_permission(self):
        for pid,number,reason in ((101,errno.ESRCH,'native runtime generation changed'),(103,errno.EACCES,'runtime state unavailable')):
            with self.subTest(pid=pid),self.proc_view(failed_pid=pid,number=number,active=False) as state:
                self.camera=FakeCamera(self);native=tool.NativeCamera({});native.generation=tool.runtime_generation();original=self.camera.read
                def read():
                    native.health();result=original()
                    if self.camera.value is True:state['active']=True
                    return result
                self.camera.read=read;source,write=os.pipe();os.write(write,b'E 1\n');out=io.StringIO();polls=[]
                def select(reads,*args):
                    polls.append(1);return (reads,[],[]) if len(polls)==1 else ([],[],[])
                try:
                    with os.fdopen(source,'rb') as stream,mock.patch.object(tool.select,'select',side_effect=select),mock.patch.object(tool,'_warnings',0),mock.patch.object(tool.sys,'stderr',new_callable=io.StringIO) as log:
                        with self.assertRaisesRegex(tool.SafetyError,reason):tool.serve(self.root,input_stream=stream,output_stream=out,api_factory=self.factory)
                    self.assertEqual(out.getvalue(),'R 0\nA 1\nN 1\n');self.assertTrue((self.root/tool.LEASE).exists())
                    self.assertIn('phase=health; reason='+reason,log.getvalue())
                    if number==errno.EACCES:self.assertIn('errno=permission-denied(13)',log.getvalue())
                    self.assertNotIn(str(self.root),log.getvalue());self.assertNotIn('private fixture',log.getvalue())
                finally:os.close(write)
            (self.root/tool.LEASE).unlink();self.write_settings({'camera':{tool.KEY:False}})

    def test_real_runtime_disappearance_or_restart_still_revokes_before_getter(self):
        for pid in (101,102):
            for point in ('stat','comm','start'):
                with self.subTest(pid=pid,point=point),self.proc_view(failed_pid=pid,point=point,active=False) as state:
                    camera=tool.NativeCamera({});camera.generation=tool.runtime_generation();camera.fn=mock.Mock()
                    state['active']=True
                    with self.assertRaisesRegex(tool.SafetyError,'generation changed'):camera.read()
                    camera.fn.GetBool.assert_not_called()
        with self.proc_view(active=False):
            camera=tool.NativeCamera({});camera.generation=tool.runtime_generation();camera.fn=mock.Mock()
            path=self.root/'proc/101/stat';path.write_text('101 (vrserver) S '+'0 '*18+'99 0\n')
            with self.assertRaisesRegex(tool.SafetyError,'generation changed'):camera.read()
            camera.fn.GetBool.assert_not_called()

    def test_malformed_process_identity_remains_refused(self):
        with self.proc_view(active=False):
            path=self.root/'proc/101/stat';path.write_text('invalid private process data')
            with self.assertRaisesRegex(tool.SafetyError,'process identity unavailable'):tool.runtime_generation()

    def test_missing_helper_identity_cannot_create_a_lease_or_grant(self):
        for number in (errno.ENOENT,errno.ESRCH):
            with self.subTest(number=number),self.proc_view(failed_pid=os.getpid(),point='start',number=number):
                with self.assertRaisesRegex(tool.SafetyError,'process identity unavailable'):self.session().enable()
                self.assertFalse((self.root/tool.LEASE).exists());self.assertEqual(self.camera.calls,[])

    def test_failure_diagnostics_allowlist_errno_privacy_and_existing_budget(self):
        with mock.patch.object(tool,'_warnings',0),mock.patch.object(tool.sys,'stderr',new_callable=io.StringIO) as log:
            known=tool.SafetyError('runtime state unavailable');known.__cause__=PermissionError(errno.EACCES,'secret errno text',str(self.root))
            tool.warn_failure('health',known);tool.warn_failure('health',known)
            tool.warn_failure('enable',tool.SafetyError('camera enable not acknowledged'))
            tool.warn_failure('restore',OSError(errno.ESRCH,'secret text',str(self.root)))
            tool.warn_failure('ipc-read',BrokenPipeError(errno.EPIPE,'secret text'))
            tool.warn_failure('ipc-command',tool.SafetyError('private secret key '+str(self.root)))
            for _ in range(12):tool.warn_failure('untrusted private phase',ValueError('secret config payload'))
            rows=log.getvalue().splitlines();self.assertEqual(len(rows),8)
            self.assertIn('phase=health; reason=runtime state unavailable; errno=permission-denied(13)',rows[0])
            self.assertIn('phase=enable; reason=camera enable not acknowledged',rows[1])
            self.assertIn('phase=restore; reason=os-error; errno=process-gone(3)',rows[2])
            self.assertIn('phase=ipc-read; reason=os-error; errno=pipe-closed',rows[3])
            self.assertIn('reason=safety-check-refused',rows[4]);self.assertIn('phase=serve; reason=invalid-data',rows[5])
            self.assertNotIn('secret',log.getvalue());self.assertNotIn(str(self.root),log.getvalue())
            self.assertTrue(all(len(row)<300 for row in rows))

    def test_actual_main_reports_enable_and_ipc_failure_phases_without_payloads(self):
        real=tool.serve
        for commands,expected in ((b'E 1\n','enable'),(b'private-secret-command\n','ipc-command')):
            with self.subTest(phase=expected):
                self.camera=FakeCamera(self);self.camera.refuse=True;output=io.StringIO()
                def serve(root):return real(root,input_stream=io.BytesIO(commands),output_stream=output,api_factory=self.factory)
                with mock.patch.object(tool,'serve',side_effect=serve),mock.patch.object(tool.signal,'signal'),mock.patch.object(tool,'_warnings',0),mock.patch.object(tool.sys,'stderr',new_callable=io.StringIO) as log:
                    self.assertEqual(tool.main(['serve','--root',str(self.root)]),2)
                self.assertIn('phase='+expected+';',log.getvalue());self.assertNotIn('private-secret',log.getvalue())
                self.assertNotIn(str(self.root),log.getvalue());self.assertNotIn('Traceback',log.getvalue())
                if (self.root/tool.LEASE).exists():(self.root/tool.LEASE).unlink()
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
    def test_paused_serve_never_constructs_or_initializes_native_api(self):
        _,sha=tool.load_config(self.root);tool.pause(self.root,sha)
        factory=mock.Mock(side_effect=AssertionError('paused native initialization forbidden'))
        output=io.StringIO()
        with self.assertRaisesRegex(tool.SafetyError,'paused'):
            tool.serve(self.root,input_stream=io.BytesIO(b'E 1\n'),output_stream=output,api_factory=factory)
        factory.assert_not_called();self.assertEqual(output.getvalue(),'')
        self.assertFalse((self.root/tool.LEASE).exists())
    def test_unknown_config_or_invalid_settings_refuse_before_api_factory(self):
        factory=mock.Mock(side_effect=AssertionError('invalid session native initialization forbidden'))
        for invalid in ('schema','private-mode','settings'):
            with self.subTest(invalid=invalid):
                self.write_config();self.write_settings({'camera':{tool.KEY:False}})
                if invalid=='schema':
                    self.config['schema']='unknown';self.write_config();self.config['schema']=tool.CONFIG_SCHEMA
                elif invalid=='private-mode':(self.root/tool.CONFIG).chmod(0o644)
                else:self.write_settings({'camera':{tool.KEY:'unknown'}})
                with self.assertRaises(tool.SafetyError):
                    tool.serve(self.root,input_stream=io.BytesIO(b''),output_stream=io.StringIO(),api_factory=factory)
        factory.assert_not_called();self.assertFalse((self.root/tool.LEASE).exists())
    def test_pause_created_during_native_setup_is_still_refused_by_locked_guard(self):
        def factory(pin):
            _,sha=tool.load_config(self.root);tool.pause(self.root,sha)
            return self.camera
        output=io.StringIO()
        with self.assertRaisesRegex(tool.SafetyError,'paused'):
            tool.serve(self.root,input_stream=io.BytesIO(b'E 1\n'),output_stream=output,api_factory=factory)
        self.assertTrue(self.camera.closed);self.assertEqual(self.camera.calls,[])
        self.assertEqual(output.getvalue(),'');self.assertFalse((self.root/tool.LEASE).exists())
    def test_live_restore_refuses_unpersisted_native_change_retains_receipt(self):
        session = self.session();session.enable();self.camera.persist = False
        with self.assertRaisesRegex(tool.SafetyError,'persistence'):tool.restore_owned(self.root,api_factory=self.factory,persist_timeout=0)
        self.assertIs(self.camera.value,False);self.assertTrue((self.root / tool.LEASE).exists())
        self.assertIs(tool.settings(self.settings)[1],True)
    def test_restore_without_pending_ownership_never_opens_native_api(self):
        factory=mock.Mock(side_effect=AssertionError('unowned native initialization forbidden'))
        config_before=(self.root/tool.CONFIG).read_bytes()
        for value in (False,True,None):
            with self.subTest(value=value):
                self.write_settings({'camera':{'keep':31,**({tool.KEY:value} if value is not None else {})},'unrelated':42})
                before=self.settings.read_bytes()
                self.assertEqual(tool.restore_owned(self.root,api_factory=factory,persist_timeout=0),
                    {'status':'unchanged','persisted':True})
                self.assertEqual(self.settings.read_bytes(),before)
                self.assertEqual((self.root/tool.CONFIG).read_bytes(),config_before)
                self.assertFalse((self.root/tool.LEASE).exists())
                self.assertEqual(json.loads((self.root/tool.PAUSE).read_text()),
                    {'schema':tool.PAUSE_SCHEMA,'settings':str(self.settings)})
        factory.assert_not_called()
    def test_consumed_legacy_restore_preserves_later_user_choice_without_native_api(self):
        self.legacy(None);self.write_settings({'camera':{tool.KEY:True,'keep':31}})
        tool.startup(self.root,running=lambda:False)
        marker=(self.root/tool.MIGRATION).read_bytes()
        self.write_settings({'camera':{tool.KEY:True,'keep':42}});before=self.settings.read_bytes()
        factory=mock.Mock(side_effect=AssertionError('consumed grant native initialization forbidden'))
        self.assertEqual(tool.restore_owned(self.root,api_factory=factory,persist_timeout=0),
            {'status':'unchanged','persisted':True})
        factory.assert_not_called();self.assertEqual(self.settings.read_bytes(),before)
        self.assertEqual((self.root/tool.MIGRATION).read_bytes(),marker)
    def test_cleanup_pending_tracks_real_migration_lifecycle_and_new_grants(self):
        def pending():
            config,sha=tool.load_config(self.root)
            with tool.locked(self.root):return tool.cleanup_pending(self.root,config,sha)
        self.assertFalse(pending())
        self.legacy(None);self.write_settings({'camera':{tool.KEY:True,'keep':31}})
        self.assertTrue(pending());self.assertFalse((self.root/tool.MIGRATION).exists())
        tool.migration_begin(self.root,self.config)
        marker=(self.root/tool.MIGRATION).read_bytes();self.assertTrue(pending())
        self.assertEqual((self.root/tool.MIGRATION).read_bytes(),marker)
        tool.startup(self.root,running=lambda:False)
        self.assertFalse(pending());self.assertEqual(json.loads((self.root/tool.MIGRATION).read_text())['phase'],'consumed')
        consumed=(self.root/tool.MIGRATION).read_bytes()
        self.legacy(None,'22'*32);self.assertTrue(pending())
        self.assertEqual((self.root/tool.MIGRATION).read_bytes(),consumed)
    def test_cleanup_pending_validates_lease_even_after_consumed_migration(self):
        self.legacy(None);self.write_settings({'camera':{tool.KEY:True,'keep':31}})
        tool.startup(self.root,running=lambda:False)
        self.session().enable();config,sha=tool.load_config(self.root)
        path=self.root/tool.LEASE;valid=path.read_bytes();before=self.settings.read_bytes()
        with tool.locked(self.root):self.assertTrue(tool.cleanup_pending(self.root,config,sha))
        self.assertEqual(path.read_bytes(),valid)
        invalid=json.loads(valid);invalid['config_sha256']='00'*32;path.write_bytes(tool.encoded(invalid))
        factory=mock.Mock(side_effect=AssertionError('unverified lease native initialization forbidden'))
        with self.assertRaisesRegex(tool.SafetyError,'lease'):
            tool.restore_owned(self.root,api_factory=factory,persist_timeout=0)
        factory.assert_not_called();self.assertEqual(self.settings.read_bytes(),before)
        self.assertEqual(json.loads(path.read_text()),invalid)
    def test_restore_owned_pending_lease_still_requires_verified_native_restore(self):
        self.session().enable();lease=(self.root/tool.LEASE).read_bytes();before=self.settings.read_bytes()
        unavailable=mock.Mock(side_effect=tool.SafetyError('unverified native ABI'))
        with self.assertRaisesRegex(tool.SafetyError,'unverified native ABI'):
            tool.restore_owned(self.root,api_factory=unavailable,persist_timeout=0)
        unavailable.assert_called_once_with(self.config['library'])
        self.assertEqual((self.root/tool.LEASE).read_bytes(),lease)
        self.assertEqual(self.settings.read_bytes(),before);self.assertTrue((self.root/tool.PAUSE).exists())
        available=mock.Mock(return_value=self.camera)
        self.assertEqual(tool.restore_owned(self.root,api_factory=available,persist_timeout=0),
            {'status':'restored','persisted':True})
        available.assert_called_once_with(self.config['library'])
        self.assertFalse((self.root/tool.LEASE).exists());self.assertIs(tool.settings(self.settings)[1],False)
        self.assertEqual(self.camera.calls,[('write',True),('write',False)])
    def test_prepare_offline_restore_durably_pauses_without_touching_owned_state(self):
        session=self.dead_lease();lease=(self.root/tool.LEASE).read_bytes();before=self.settings.read_bytes()
        calls=list(self.camera.calls)
        with mock.patch.object(tool.NativeCamera,'__enter__',side_effect=AssertionError('offline native API forbidden')) as native:
            self.assertEqual(tool.prepare_offline_restore(self.root),{'status':'deferred','persisted':False})
            pause=(self.root/tool.PAUSE).read_bytes()
            self.assertEqual(tool.prepare_offline_restore(self.root),{'status':'deferred','persisted':False})
            self.assertFalse(tool.offline_cleanup_ready(self.root));native.assert_not_called()
        self.assertEqual((self.root/tool.PAUSE).read_bytes(),pause)
        self.assertEqual(json.loads(pause),{'schema':tool.PAUSE_SCHEMA,'settings':str(self.settings)})
        self.assertEqual((self.root/tool.PAUSE).stat().st_mode&0o777,0o600)
        self.assertEqual((self.root/tool.LEASE).read_bytes(),lease);self.assertEqual(self.settings.read_bytes(),before)
        with self.assertRaisesRegex(tool.SafetyError,'paused'):session.enable()
        self.assertEqual(self.camera.calls,calls)
    def test_offline_cleanup_ready_waits_for_actual_startup_restore_and_runtime_stop(self):
        original=json.loads(self.settings.read_text());self.dead_lease()
        with mock.patch.object(tool.NativeCamera,'__enter__',side_effect=AssertionError('startup native API forbidden')) as native:
            self.assertFalse(tool.offline_cleanup_ready(self.root))
            tool.prepare_offline_restore(self.root);self.assertFalse(tool.offline_cleanup_ready(self.root))
            lease=(self.root/tool.LEASE).read_bytes();pause=(self.root/tool.PAUSE).read_bytes()
            self.assertEqual(tool.startup(self.root,running=lambda:True),0)
            self.assertFalse(tool.offline_cleanup_ready(self.root));self.assertIs(tool.settings(self.settings)[1],True)
            self.assertEqual((self.root/tool.LEASE).read_bytes(),lease);self.assertEqual((self.root/tool.PAUSE).read_bytes(),pause)
            self.assertEqual(tool.startup(self.root,running=lambda:False),0)
            self.assertTrue(tool.offline_cleanup_ready(self.root));native.assert_not_called()
        self.assertEqual(json.loads(self.settings.read_text()),original)
        self.assertEqual(self.settings.stat().st_mode&0o777,0o640)
        self.assertFalse((self.root/tool.LEASE).exists());self.assertFalse((self.root/tool.PAUSE).exists())
    def test_offline_startup_preserves_live_or_uncertain_lease_owner(self):
        self.dead_lease();tool.prepare_offline_restore(self.root)
        before=self.settings.read_bytes();lease=(self.root/tool.LEASE).read_bytes();pause=(self.root/tool.PAUSE).read_bytes()
        with mock.patch.object(tool.NativeCamera,'__enter__',side_effect=AssertionError('uncertain owner native API forbidden')) as native:
            for alive in (True,None):
                with self.subTest(alive=alive):
                    self.assertEqual(tool.startup(self.root,running=lambda:False,alive=lambda _:alive),0)
                    self.assertFalse(tool.offline_cleanup_ready(self.root))
                    self.assertEqual(self.settings.read_bytes(),before)
                    self.assertEqual((self.root/tool.LEASE).read_bytes(),lease);self.assertEqual((self.root/tool.PAUSE).read_bytes(),pause)
            native.assert_not_called()
    def test_offline_startup_restores_pending_migration_before_readiness(self):
        self.legacy(None);self.write_settings({'camera':{tool.KEY:True,'keep':31},'unrelated':42})
        tool.migration_begin(self.root,self.config);pending=(self.root/tool.MIGRATION).read_bytes()
        with mock.patch.object(tool.NativeCamera,'__enter__',side_effect=AssertionError('migration native API forbidden')) as native:
            self.assertEqual(tool.prepare_offline_restore(self.root),{'status':'deferred','persisted':False})
            self.assertFalse(tool.offline_cleanup_ready(self.root));self.assertIs(tool.settings(self.settings)[1],True)
            self.assertEqual((self.root/tool.MIGRATION).read_bytes(),pending)
            self.assertEqual(tool.startup(self.root,running=lambda:False),0)
            self.assertTrue(tool.offline_cleanup_ready(self.root));native.assert_not_called()
        self.assertEqual(json.loads(self.settings.read_text()),{'camera':{'keep':31},'unrelated':42})
        self.assertEqual(json.loads((self.root/tool.MIGRATION).read_text())['phase'],'consumed')
    def test_offline_prepare_without_cleanup_waits_for_startup_to_clear_pause(self):
        before=self.settings.read_bytes();self.assertTrue(tool.offline_cleanup_ready(self.root))
        with mock.patch.object(tool.NativeCamera,'__enter__',side_effect=AssertionError('unowned native API forbidden')) as native:
            self.assertEqual(tool.prepare_offline_restore(self.root),{'status':'deferred','persisted':False})
            self.assertFalse(tool.offline_cleanup_ready(self.root))
            self.assertEqual(tool.startup(self.root,running=lambda:True),0);self.assertFalse(tool.offline_cleanup_ready(self.root))
            self.assertEqual(tool.startup(self.root,running=lambda:False),0);self.assertTrue(tool.offline_cleanup_ready(self.root))
            native.assert_not_called()
        self.assertEqual(self.settings.read_bytes(),before);self.assertFalse((self.root/tool.MIGRATION).exists())
    def test_offline_restore_preserves_changed_pending_migration_for_retry(self):
        self.legacy(None);self.write_settings({'camera':{tool.KEY:True}});tool.migration_begin(self.root,self.config)
        self.write_settings({'camera':{tool.KEY:True},'user-change':42})
        before=self.settings.read_bytes();marker=(self.root/tool.MIGRATION).read_bytes()
        with mock.patch.object(tool.NativeCamera,'__enter__',side_effect=AssertionError('changed migration native API forbidden')) as native:
            tool.prepare_offline_restore(self.root);pause=(self.root/tool.PAUSE).read_bytes()
            self.assertEqual(tool.startup(self.root,running=lambda:False),0)
            self.assertFalse(tool.offline_cleanup_ready(self.root));native.assert_not_called()
        self.assertEqual(self.settings.read_bytes(),before);self.assertEqual((self.root/tool.MIGRATION).read_bytes(),marker)
        self.assertEqual((self.root/tool.PAUSE).read_bytes(),pause)
    def test_offline_restore_spends_foreign_legacy_value_without_rewriting_it(self):
        self.legacy(None);before=self.settings.read_bytes()
        with mock.patch.object(tool.NativeCamera,'__enter__',side_effect=AssertionError('foreign value native API forbidden')) as native:
            tool.prepare_offline_restore(self.root);self.assertFalse(tool.offline_cleanup_ready(self.root))
            self.assertEqual(tool.startup(self.root,running=lambda:False),0)
            self.assertTrue(tool.offline_cleanup_ready(self.root));native.assert_not_called()
        self.assertEqual(self.settings.read_bytes(),before)
        self.assertEqual(json.loads((self.root/tool.MIGRATION).read_text())['phase'],'consumed')
    def test_offline_helpers_refuse_invalid_config_before_pause_or_native_api(self):
        original=(self.root/tool.CONFIG).read_bytes();before=self.settings.read_bytes()
        factory=mock.Mock(side_effect=AssertionError('invalid config native initialization forbidden'))
        guard=mock.patch.object(tool.NativeCamera,'__enter__',side_effect=AssertionError('invalid config native API forbidden'))
        native=guard.start();self.addCleanup(guard.stop)
        for invalid in ('schema','private-mode','relative-settings','root-mode'):
            with self.subTest(invalid=invalid):
                value=json.loads(original)
                if invalid=='schema':value['schema']='unknown'
                if invalid=='relative-settings':value['settings']='relative'
                (self.root/tool.CONFIG).write_bytes(tool.encoded(value));(self.root/tool.CONFIG).chmod(0o644 if invalid=='private-mode' else 0o600)
                self.root.chmod(0o755 if invalid=='root-mode' else 0o700)
                try:
                    with self.assertRaises(tool.SafetyError):tool.prepare_offline_restore(self.root)
                    with self.assertRaises(tool.SafetyError):tool.offline_cleanup_ready(self.root)
                    with self.assertRaises(tool.SafetyError):tool.restore_owned(self.root,api_factory=factory,persist_timeout=0)
                    self.assertFalse((self.root/tool.PAUSE).exists());self.assertEqual(self.settings.read_bytes(),before)
                finally:self.root.chmod(0o700)
        factory.assert_not_called();native.assert_not_called()
    def test_offline_helpers_preserve_invalid_lease_and_migration_records(self):
        self.legacy(None);before=self.settings.read_bytes()
        guard=mock.patch.object(tool.NativeCamera,'__enter__',side_effect=AssertionError('invalid record native API forbidden'))
        native=guard.start();self.addCleanup(guard.stop)
        for name in (tool.LEASE,tool.MIGRATION):
            for mode in (0o600,0o644):
                with self.subTest(name=name,mode=mode):
                    path=self.root/name;path.write_bytes(tool.encoded({'schema':'unknown'}));path.chmod(mode)
                    raw=path.read_bytes()
                    with self.assertRaises(tool.SafetyError):tool.prepare_offline_restore(self.root)
                    with self.assertRaises(tool.SafetyError):tool.offline_cleanup_ready(self.root)
                    self.assertEqual(path.read_bytes(),raw);self.assertEqual(self.settings.read_bytes(),before)
                    self.assertFalse((self.root/tool.PAUSE).exists());path.unlink()
        native.assert_not_called()
    def test_offline_startup_preserves_foreign_camera_value_and_lease(self):
        self.dead_lease();self.write_settings({'camera':{tool.KEY:'foreign'},'user-change':42})
        before=self.settings.read_bytes();lease=(self.root/tool.LEASE).read_bytes()
        with mock.patch.object(tool.NativeCamera,'__enter__',side_effect=AssertionError('foreign override native API forbidden')) as native:
            tool.prepare_offline_restore(self.root);pause=(self.root/tool.PAUSE).read_bytes()
            self.assertEqual(tool.startup(self.root,running=lambda:False),0)
            self.assertFalse(tool.offline_cleanup_ready(self.root));native.assert_not_called()
        self.assertEqual(self.settings.read_bytes(),before);self.assertEqual((self.root/tool.LEASE).read_bytes(),lease)
        self.assertEqual((self.root/tool.PAUSE).read_bytes(),pause)
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
    def test_changed_provider_requires_public_interface_before_any_settings_write(self):
        raw=bytearray(256);raw[:7]=b'\x7fELF\x02\x01\x01'
        struct.pack_into('<HH',raw,16,3,183);struct.pack_into('<Q',raw,32,64)
        struct.pack_into('<HH',raw,54,56,1);struct.pack_into('<IIQQQQQ',raw,64,1,5,0,0,0,256,256)
        library=self.root/'libopenvr_api.so';library.write_bytes(raw);library.chmod(0o600)
        pin={'path':str(library),'bytes':64,'sha256':'00'*32,'interface':'FnTable:IVRSettings_003'}
        writes=[]
        @tool._FN(None,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_bool,tool._ERR)
        def set_bool(section,key,value,error):writes.append(value);error[0]=0
        @tool._FN(ctypes.c_bool,ctypes.c_char_p,ctypes.c_char_p,tool._ERR)
        def get_bool(section,key,error):error[0]=0;return False
        @tool._FN(None,ctypes.c_char_p,ctypes.c_char_p,tool._ERR)
        def remove(section,key,error):writes.append('remove');error[0]=0
        fn=tool.SettingsTable(tool._FN(ctypes.c_char_p,ctypes.c_int)(),set_bool,None,None,None,get_bool,None,None,None,None,remove)
        api=mock.Mock();api.VR_GetGenericInterface.return_value=ctypes.addressof(fn)
        with mock.patch.object(tool.os,'uname',return_value=mock.Mock(machine='aarch64')),mock.patch.object(tool,'runtime_generation',return_value=((10,11,'vrserver'),)),mock.patch.object(tool.ctypes,'CDLL',return_value=api) as load:
            with tool.NativeCamera(pin) as camera:self.assertIs(camera.read(),False)
            self.assertTrue(load.call_args.args[0].startswith('/proc/self/fd/'))
            self.assertEqual(api.VR_GetGenericInterface.call_args.args[0],b'FnTable:IVRSettings_003')
            api.VR_ShutdownInternal.assert_called_once();self.assertEqual(writes,[])
            api.reset_mock();api.VR_GetGenericInterface.return_value=0
            with self.assertRaisesRegex(tool.SafetyError,'interface unavailable'):
                with tool.NativeCamera(pin):pass
            api.VR_ShutdownInternal.assert_called_once();self.assertEqual(writes,[])
        for position,value in ((18,62),(68,7),(54,55)):
            broken=raw[:];struct.pack_into('<H',broken,position,value)
            self.assertFalse(tool.provider_elf_compatible(broken))
        self.assertFalse(tool.provider_elf_compatible(raw[:80]))

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


class InactiveSettingsControls(unittest.TestCase):
    """The native runtime and its obsolete config are independent files."""
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='sfnp-inactive-camera-');self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name);self.home.chmod(0o700)
        self.root = self.home / 'owned';self.root.mkdir(mode=0o700)
        self.active = self.home / '.config/openvr/config/steamvr.vrsettings'
        self.obsolete = self.home / '.local/share/Steam/config/steamvr.vrsettings'
        self.registry = self.home / '.config/openvr/openvrpaths.vrpath'
        for path in (self.active, self.obsolete):
            path.parent.mkdir(parents=True, mode=0o700)
        self.active.write_bytes(tool.encoded({'camera':{'keep':7}, 'actual':True}))
        self.active.chmod(0o640)
        self.obsolete.write_bytes(tool.encoded({'camera':{tool.KEY:True, 'keep':8}}))
        self.obsolete.chmod(0o640)
        self.config = {'schema':tool.CONFIG_SCHEMA, 'settings':str(self.obsolete),
            'library':{'path':str(self.root / 'libopenvr_api.so'), 'bytes':249168, 'sha256':'ab'*32},
            'legacy_migration':{'id':'11'*32, 'section_present':False,
                'present':False, 'value':None, 'installed':True}}
        self.write_config()
        self.write_registry([str(self.active.parent), str(self.obsolete.parent)])
        self.env = mock.patch.dict(os.environ, {'VR_CONFIG_PATH':'', 'VR_PATHREG_OVERRIDE':'', 'XDG_CONFIG_HOME':''})
        self.env.start();self.addCleanup(self.env.stop)
        self.native = mock.patch.object(tool.NativeCamera, '__enter__', side_effect=AssertionError('inactive recovery must not call any native API'))
        self.native.start();self.addCleanup(self.native.stop)
        self.active_before = self.active.read_bytes()
    def write_config(self):
        path=self.root/tool.CONFIG;path.write_bytes(tool.encoded(self.config));path.chmod(0o600)
    def write_registry(self, paths):
        self.registry.write_bytes(tool.encoded({'jsonid':'vrpathreg','version':1,'config':paths}))
        self.registry.chmod(0o600)
    def restore(self):return tool.restore_inactive(self.root,self.active,home=self.home)
    def authority(self, before=True, desired=None):
        return {'schema':'sfnp-camera-override-authority-v1','settings':str(self.obsolete),
            'before':before,'desired':desired,'section_present':False}
    def lease(self, write_intent=False):
        _,sha=tool.load_config(self.root)
        value={'schema':tool.LEASE_SCHEMA,'config_sha256':sha,'helper_pid':os.getpid(),
            'helper_start_ticks':tool.own_start(os.getpid()),'override':False if write_intent else True,
            'effective':False if write_intent else True,'section_present':True,'write_intent':write_intent,
            'settings_signature':tool.signature(self.obsolete.stat()),'restore_signature':None}
        path=self.root/tool.LEASE;path.write_bytes(tool.encoded(value));path.chmod(0o600)
    def test_registry_front_is_active_even_with_historical_second_path(self):
        self.assertEqual(tool.resolve_settings(self.home),self.active)
        self.write_registry([str(self.obsolete.parent),str(self.active.parent)])
        with self.assertRaisesRegex(tool.SafetyError,'active OpenVR'):self.restore()
    def test_registry_and_config_env_priority_match_native(self):
        with mock.patch.dict(os.environ,{'VR_CONFIG_PATH':str(self.active.parent),'VR_PATHREG_OVERRIDE':str(self.home/'missing')}):
            self.assertEqual(tool.resolve_settings(self.home),self.active)
        alternate=self.home/'alternate';alternate.mkdir(mode=0o700)
        other=alternate/'paths.json';other.write_bytes(self.registry.read_bytes())
        with mock.patch.dict(os.environ,{'VR_PATHREG_OVERRIDE':str(other),'XDG_CONFIG_HOME':str(self.home/'missing')}):
            self.assertEqual(tool.resolve_settings(self.home),self.active)
        with mock.patch.dict(os.environ,{'XDG_CONFIG_HOME':str(self.home/'.config')}):
            self.assertEqual(tool.resolve_settings(self.home),self.active)
    def test_pending_old_wrong_target_restores_without_api_or_waiting_on_active_file(self):
        tool.migration_begin(self.root,self.config)
        self.assertEqual(self.restore(),{'status':'restored','persisted':True})
        self.assertIsNone(tool.settings(self.obsolete)[1])
        self.assertEqual(self.active.read_bytes(),self.active_before)
        self.assertEqual(json.loads(self.obsolete.read_text()),{'camera':{'keep':8}})
        self.assertEqual(json.loads((self.root/tool.MIGRATION).read_text())['phase'],'consumed')
        self.assertTrue((self.root/tool.PAUSE).exists())
        self.assertEqual(self.restore(),{'status':'unchanged','persisted':True})
    def test_false_baseline_and_writeless_true_lease_are_restored_offline(self):
        self.config['legacy_migration'].update(section_present=True,present=True,value=False)
        self.write_config();self.lease()
        self.restore()
        self.assertIs(tool.settings(self.obsolete)[1],False)
        self.assertFalse((self.root/tool.LEASE).exists())
        self.assertEqual(self.active.read_bytes(),self.active_before)
    def test_native_write_intent_lease_cannot_be_inferred_from_inactive_file(self):
        self.lease(write_intent=True);before=self.obsolete.read_bytes()
        with self.assertRaisesRegex(tool.SafetyError,'live restoration'):self.restore()
        with self.assertRaisesRegex(tool.SafetyError,'live restoration'):
            tool.apply_owned_inactive(self.root,self.authority(),self.active,home=self.home)
        self.assertEqual(self.obsolete.read_bytes(),before);self.assertEqual(self.active.read_bytes(),self.active_before)
        self.assertTrue((self.root/tool.LEASE).exists())
        self.assertFalse((self.root/tool.PAUSE).exists())
    def test_inactive_checkpoint_delta_and_undo_preserve_actual_native_settings(self):
        self.assertEqual(tool.apply_owned_inactive(self.root,self.authority(),self.active,home=self.home),
            {'status':'restored','persisted':True})
        tool.apply_owned_inactive(self.root,self.authority(None,True),self.active,home=self.home)
        self.assertIs(tool.settings(self.obsolete)[1],True)
        self.assertEqual(self.active.read_bytes(),self.active_before)
        with self.assertRaisesRegex(tool.SafetyError,'checkpoint'):
            tool.apply_owned_inactive(self.root,self.authority(None,True),self.active,home=self.home)
    def test_only_known_historical_target_and_current_registry_are_authority(self):
        with self.assertRaisesRegex(tool.SafetyError,'active OpenVR'):
            tool.restore_inactive(self.root,self.obsolete,home=self.home)
        self.config['settings']=str(self.active);self.write_config()
        with self.assertRaisesRegex(tool.SafetyError,'historical'):self.restore()
        self.assertEqual(self.active.read_bytes(),self.active_before)
    def test_unknown_changed_pending_value_or_signature_preserves_receipts(self):
        tool.migration_begin(self.root,self.config)
        self.obsolete.write_bytes(tool.encoded({'camera':{tool.KEY:True},'changed':1}))
        before=self.obsolete.read_bytes()
        with self.assertRaisesRegex(tool.SafetyError,'pending migration'):self.restore()
        self.assertEqual(self.obsolete.read_bytes(),before)
        self.assertEqual(json.loads((self.root/tool.MIGRATION).read_text())['phase'],'pending')
        self.assertEqual(self.active.read_bytes(),self.active_before)
    def test_invalid_missing_first_or_symlink_registry_never_falls_back_to_history(self):
        for paths in ([],['relative'],[str(self.home/'missing'),str(self.obsolete.parent)],['x']*33):
            self.write_registry(paths)
            with self.assertRaises((tool.SafetyError,FileNotFoundError)):self.restore()
        self.registry.unlink();other=self.home/'other';other.write_text('{}');self.registry.symlink_to(other)
        with self.assertRaisesRegex(tool.SafetyError,'symlink'):self.restore()
        self.assertEqual(self.active.read_bytes(),self.active_before)
    def test_completed_target_transfer_stays_paused_until_new_config_startup(self):
        self.restore();pause=self.root/tool.PAUSE
        self.assertEqual(tool.prepare_target(self.root,self.active,home=self.home),
            {'status':'restored','persisted':True})
        self.assertEqual(json.loads(pause.read_text())['settings'],str(self.active))
        self.assertEqual(tool.prepare_target(self.root,self.active,home=self.home),
            {'status':'unchanged','persisted':True})
        _,sha=tool.load_config(self.root)
        camera=mock.Mock()
        with self.assertRaisesRegex(tool.SafetyError,'paused'):
            tool.Session(self.root,self.config,sha,camera).enable()
        camera.read.assert_not_called()
        tool.startup(self.root,running=lambda:True)
        self.assertTrue(pause.exists())
        self.config['settings']=str(self.active);self.config.pop('legacy_migration');self.write_config()
        tool.startup(self.root,running=lambda:False)
        self.assertFalse(pause.exists());self.assertEqual(self.active.read_bytes(),self.active_before)
    def test_target_transfer_rollback_before_publication_restores_old_pause(self):
        self.restore()
        tool.apply_owned_inactive(self.root,self.authority(None,True),self.active,home=self.home)
        tool.prepare_target(self.root,self.active,home=self.home)
        self.assertEqual(self.restore(),{'status':'unchanged','persisted':True})
        self.assertEqual(json.loads((self.root/tool.PAUSE).read_text())['settings'],str(self.obsolete))
        self.assertIs(tool.settings(self.obsolete)[1],True)
        self.assertEqual(self.active.read_bytes(),self.active_before)
    def test_configless_completed_recovery_can_transfer_pause_for_fresh_install(self):
        self.restore();(self.root/tool.CONFIG).unlink()
        tool.prepare_target(self.root,self.active,home=self.home)
        self.assertEqual(json.loads((self.root/tool.PAUSE).read_text())['settings'],str(self.active))
        self.config['settings']=str(self.active);self.config.pop('legacy_migration');self.write_config()
        tool.startup(self.root,running=lambda:False)
        self.assertFalse((self.root/tool.PAUSE).exists())
        self.assertEqual(self.active.read_bytes(),self.active_before)
    def test_target_transfer_refuses_pending_migration_any_lease_or_unknown_pause(self):
        tool.migration_begin(self.root,self.config);_,sha=tool.load_config(self.root);tool.pause(self.root,sha)
        original=(self.root/tool.PAUSE).read_bytes()
        with self.assertRaisesRegex(tool.SafetyError,'completed'):
            tool.prepare_target(self.root,self.active,home=self.home)
        self.restore();self.lease()
        with self.assertRaisesRegex(tool.SafetyError,'no session lease'):
            tool.prepare_target(self.root,self.active,home=self.home)
        (self.root/tool.LEASE).unlink()
        (self.root/tool.PAUSE).write_bytes(tool.encoded({'schema':tool.PAUSE_SCHEMA,'settings':str(self.home/'foreign')}))
        with self.assertRaisesRegex(tool.SafetyError,'unknown camera pause'):
            tool.prepare_target(self.root,self.active,home=self.home)
        (self.root/tool.PAUSE).write_bytes(original)
        self.config['legacy_migration']['id']='22'*32;self.write_config()
        with self.assertRaisesRegex(tool.SafetyError,'completed'):
            tool.prepare_target(self.root,self.active,home=self.home)
        self.assertEqual((self.root/tool.PAUSE).read_bytes(),original)
        self.assertEqual(self.active.read_bytes(),self.active_before)
    def test_configless_transfer_rejects_unconsumed_or_invalid_marker(self):
        self.restore();(self.root/tool.CONFIG).unlink()
        path=self.root/tool.MIGRATION;valid=json.loads(path.read_text())
        for value in ({**valid,'phase':'pending'}, {**valid,'unknown':True}):
            path.write_bytes(tool.encoded(value))
            with self.assertRaises(tool.SafetyError):
                tool.prepare_target(self.root,self.active,home=self.home)
        self.assertEqual(json.loads((self.root/tool.PAUSE).read_text())['settings'],str(self.obsolete))
        self.assertEqual(self.active.read_bytes(),self.active_before)
    def test_no_grant_pause_transfer_needs_no_new_settings_authority(self):
        self.config.pop('legacy_migration');self.write_config()
        before=self.obsolete.read_bytes()
        self.assertEqual(self.restore(),{'status':'unchanged','persisted':True})
        self.assertFalse((self.root/tool.MIGRATION).exists())
        tool.prepare_target(self.root,self.active,home=self.home)
        self.assertEqual(json.loads((self.root/tool.PAUSE).read_text())['settings'],str(self.active))
        self.restore()
        self.assertEqual(json.loads((self.root/tool.PAUSE).read_text())['settings'],str(self.obsolete))
        self.assertEqual(self.obsolete.read_bytes(),before);self.assertEqual(self.active.read_bytes(),self.active_before)
        (self.root/tool.CONFIG).unlink()
        tool.prepare_target(self.root,self.active,home=self.home)
        self.assertEqual(json.loads((self.root/tool.PAUSE).read_text())['settings'],str(self.active))
        self.assertEqual(self.obsolete.read_bytes(),before);self.assertEqual(self.active.read_bytes(),self.active_before)
    def test_no_config_grant_does_not_discard_pending_foreign_marker(self):
        tool.migration_begin(self.root,self.config)
        self.config.pop('legacy_migration');self.write_config()
        _,sha=tool.load_config(self.root);tool.pause(self.root,sha)
        for present in (True,False):
            if not present:(self.root/tool.CONFIG).unlink()
            with self.assertRaisesRegex(tool.SafetyError,'completed'):
                tool.prepare_target(self.root,self.active,home=self.home)
        self.assertEqual(json.loads((self.root/tool.PAUSE).read_text())['settings'],str(self.obsolete))
        self.assertEqual(self.active.read_bytes(),self.active_before)


if __name__=='__main__':unittest.main()
