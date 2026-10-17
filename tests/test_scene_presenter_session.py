"""Scene-only staging sandbox: no native loading, services, image reads or device."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[1]
SPEC=importlib.util.spec_from_file_location('scene_session',ROOT/'tools/synthetic_presenter_session.py')
mod=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(mod)
NONCE='0123456789abcdef0123456789abcdef'


def elf(marker,flags=4,load=True):
    length=120+len(marker);raw=bytearray(length);raw[:7]=b'\x7fELF\x02\x01\x01'
    struct.pack_into('<HHI',raw,16,3,183,1);struct.pack_into('<Q',raw,32,64)
    struct.pack_into('<HHH',raw,52,64,56,1)
    struct.pack_into('<IIQQQQQQ',raw,64,1,flags,0,0,0,length if load else 120,length,4096)
    raw[120:]=marker;return bytes(raw)


class SceneSessionTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='scene-session-');self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.home=self.root/'home';self.home.mkdir(mode=0o700)
        self.proc=self.root/'proc';self.proc.mkdir(mode=0o700)
        self.run=self.home/'.local/share/GloomhavenVR/SteamLinkMaskProbe/runs/scene-r1'
        self.session=mod.Session(self.home,self.run,proc_root=self.proc,host_fixture=True,profile=mod.SCENE_PROFILE)
        self.library=self.root/'source.so';self.write(self.library,elf(mod.SCENE_BUILD_MARKER));self.sha=mod.base.digest(self.library.read_bytes())
        self.candidate=self.root/'candidate.spv';self.code=b'\x03\x02\x23\x07'+b'owned-fixture'+b'\0'*(mod.SCENE_CANDIDATE_BYTES-17)
        self.assertEqual(len(self.code),mod.SCENE_CANDIDATE_BYTES);self.write(self.candidate,self.code)
        # Private sandbox code is not production authority. No CLI/host-fixture
        # option can replace the executable SHA; only this scoped mock does.
        patch=mock.patch.object(mod,'SCENE_CANDIDATE_SHA256',mod.base.digest(self.code));patch.start();self.addCleanup(patch.stop)

    def write(self,path,raw):path.write_bytes(raw);path.chmod(0o600)
    def prepare(self):return self.session.prepare(self.library,self.sha,self.candidate,capture_id=NONCE)
    def status_value(self,**kwargs):
        p=self.session.profile
        value={'schema':p.receiver_schema,'mode':p.receiver_mode,'capture_id':NONCE,'source_ready':True,'source_reason':'ready',
            'candidate_sha256':p.candidate_sha256,'candidate_bytes':p.candidate_bytes,'observed_device':123,'observed_api':4198400,
            'active_devices':1,'pipeline_attempts':6,'pipeline_replacements':2,'last_primitive_reason':'none','graphics_gate':'none',
            'attachment_format':0,'unsupported_dynamic':2147483647,'definition_history_failed':False,'revision':1,'updates':1,
            'budget_exhausted':False,'diagnostic_atomics_not_source_cohort':True,'native_frame_association_proven':False,'passthrough_authorized':False}
        value.update(kwargs);return value

    def test_receipt_before_seven_writes_and_scene_enable_last(self):
        seen=[];real=mod.base.write_new
        def writer(path,data):seen.append(path);return real(path,data)
        with mock.patch.object(mod.base,'write_new',side_effect=writer),mock.patch.object(mod.base.Session,'arm',side_effect=AssertionError('no capture')):
            result=self.prepare()
        self.assertEqual(len(seen),7);self.assertEqual(seen[0],self.session.receipt);self.assertEqual(seen[-1],self.session.enable)
        self.assertEqual(self.session.enable.name,'scene-presenter.enable');self.assertEqual(self.session.enable.read_bytes(),b'GHVR_SCENE_PRESENTER/1\n')
        self.assertEqual(self.session.candidate.name,'scene-presenter.spv');self.assertEqual(self.session.candidate.read_bytes(),self.code)
        receipt=json.loads(self.session.receipt.read_text());self.assertEqual(receipt['schema'],'frame-scene-presenter-session-v1')
        self.assertEqual(receipt['profile'],mod.SCENE_PROFILE);self.assertEqual(receipt['profile_revision'],1)
        self.assertEqual(result['schema'],'frame-scene-presenter-session-status-v1');self.assertTrue(result['integrity_valid'])
        self.assertFalse(result['automatic_restart']);self.assertFalse(result['image_acquisition_requested']);self.assertFalse(result['passthrough_authorized'])
        self.assertFalse(self.session.base.arm_file.exists());self.assertFalse((self.run/'synthetic-presenter.enable').exists())
        for path in seen:self.assertEqual(path.stat().st_mode&0o777,0o600)

    def test_immutable_profile_and_idempotent_inode_preservation(self):
        self.prepare();p=self.session.profile
        with self.assertRaises(AttributeError):p.candidate_sha256='f'*64
        paths=[self.session.receipt,self.session.enable,self.session.candidate,self.session.base.library,self.session.base.config,self.session.base.manifest]
        before=[(x.stat().st_ino,x.read_bytes()) for x in paths]
        with mock.patch.object(mod,'SCENE_CANDIDATE_SHA256','f'*64):self.prepare()
        self.assertIs(self.session.profile,p);self.assertEqual(before,[(x.stat().st_ino,x.read_bytes()) for x in paths])

    def test_real_pin_not_relaxed_by_host_fixture(self):
        with mock.patch.object(mod,'SCENE_CANDIDATE_SHA256','9ad1b56a20db039132ad6ec4557fe8ece3fff277316370bcefc9f70ec7aed367'):
            with self.assertRaises(mod.SafetyError):self.prepare()
        self.assertFalse(self.session.receipt.exists());self.assertFalse(self.session.enable.exists())

    def test_scene_readonly_elf_marker_and_cross_profile_refusal(self):
        for raw in [elf(mod.BUILD_MARKER),elf(mod.SCENE_BUILD_MARKER,flags=6),elf(mod.SCENE_BUILD_MARKER,load=False),elf(mod.SCENE_BUILD_MARKER*2)]:
            self.write(self.library,raw)
            with self.assertRaises(mod.SafetyError):self.session.prepare(self.library,mod.base.digest(raw),self.candidate)
            self.assertFalse(self.session.receipt.exists())
        # A multi-profile compiled library is permitted, but run ownership and
        # the selected runtime enable marker still have exactly one profile.
        self.write(self.library,elf(mod.SCENE_BUILD_MARKER+mod.BUILD_MARKER))
        self.sha=mod.base.digest(self.library.read_bytes());self.prepare()
        other=mod.Session(self.home,self.run,proc_root=self.proc,host_fixture=True)
        with self.assertRaises((mod.SafetyError,OSError)):other.status()
        with self.assertRaises((mod.SafetyError,OSError)):other.rollback()
        self.assertTrue(self.session.enable.exists())

    def test_mixed_files_refused_preserved_and_owned_rollback_still_runs(self):
        self.prepare();foreign=self.run/'synthetic-presenter.enable';self.write(foreign,mod.ENABLE)
        self.assertFalse(self.session.status()['integrity_valid'])
        with self.assertRaises(mod.SafetyError):self.prepare()
        result=self.session.rollback();self.assertFalse(result['complete']);self.assertIn('foreign profile files preserved',result['errors'])
        self.assertEqual(foreign.read_bytes(),mod.ENABLE);self.assertFalse(self.session.enable.exists())
        self.assertFalse(self.session.base.manifest.exists());self.assertFalse(self.session.base.library.exists())

    def test_foreign_other_profile_receipt_before_stage_never_adopted(self):
        self.run.parent.mkdir(parents=True,mode=0o700);foreign=self.run.parent/(self.run.name+'.synthetic-presenter-session.json');self.write(foreign,b'{}')
        with self.assertRaises(mod.SafetyError):self.prepare()
        self.assertFalse(self.session.receipt.exists());self.assertEqual(foreign.read_bytes(),b'{}')

    def test_interrupted_staging_resumes_with_same_owned_profile(self):
        real=mod.base.write_new
        def writer(path,data):
            if path==self.session.candidate:raise OSError('owned interruption')
            return real(path,data)
        with mock.patch.object(mod.base,'write_new',side_effect=writer),self.assertRaises(OSError):self.prepare()
        self.assertTrue(self.session.receipt.exists());self.assertFalse(self.session.enable.exists())
        self.prepare();self.assertTrue(self.session.enable.exists());self.assertTrue(self.session.rollback()['complete'])
        with self.assertRaises(mod.SafetyError):self.prepare()

    def test_changed_input_and_receipt_are_not_repaired(self):
        self.prepare();self.write(self.session.candidate,b'foreign')
        with self.assertRaises(mod.SafetyError):self.prepare()
        self.assertEqual(self.session.candidate.read_bytes(),b'foreign');result=self.session.rollback()
        self.assertFalse(result['complete']);self.assertFalse(self.session.enable.exists());self.assertEqual(self.session.candidate.read_bytes(),b'foreign')
        value=json.loads(self.session.receipt.read_text());value['profile']=mod.PROFILE;self.write(self.session.receipt,mod.base.encoded(value))
        with self.assertRaises(mod.SafetyError):self.session.rollback()

    def test_loaded_library_manual_reversal_is_not_claimed_immediate(self):
        self.prepare();proc=self.proc/'101';proc.mkdir(mode=0o700)
        self.write(proc/'maps',('1000-2000 r-xp 00000000 00:00 0 '+str(self.session.base.library)+'\n').encode())
        r=self.session.rollback();self.assertFalse(r['complete']);self.assertFalse(r['running_replacements_revoked'])
        self.assertFalse(r['automatic_restart']);self.assertFalse(self.session.enable.exists());self.assertTrue(self.session.base.library.exists())
        self.assertTrue(self.session.consumed.read_bytes().startswith(b'GHVR_SCENE_PRESENTER_CONSUMED/1\n'))
        self.write(proc/'maps',b'');self.assertTrue(self.session.rollback()['complete']);self.assertTrue(self.session.rollback()['complete'])
        self.assertFalse(self.session.base.library.exists());self.assertTrue(self.session.candidate.exists())

    def test_receiver_scope_pin_cohort_and_no_image_reads(self):
        self.prepare();self.write(self.session.receiver_status,mod.base.encoded(self.status_value()))
        (self.run/'snapshot.p010').symlink_to(self.candidate)
        self.assertEqual(self.session.status()['receiver_status']['state'],'bounded_diagnostic_only')
        for patch in [{'schema':'frame-synthetic-presenter-receiver-v1'},{'mode':'synthetic_format_guarded_presenter'},
                      {'capture_id':'f'*32},{'candidate_sha256':'f'*64},{'candidate_bytes':True},{'updates':33},
                      {'native_frame_association_proven':True},{'extra':True}]:
            self.write(self.session.receiver_status,mod.base.encoded(self.status_value(**patch)))
            self.assertFalse(self.session.status()['integrity_valid'])
        self.assertTrue((self.run/'snapshot.p010').is_symlink())

    def test_actual_cli_scene_prepare_status_rollback_and_wrong_profile(self):
        def cli(command,*extra,profile=mod.SCENE_PROFILE):
            with contextlib.redirect_stdout(io.StringIO()) as out,contextlib.redirect_stderr(io.StringIO()):
                code=mod.main([command,'--home',str(self.home),'--run-dir',str(self.run),'--proc-root',str(self.proc),'--host-fixture','--profile',profile,*extra])
            return code,json.loads(out.getvalue()) if out.getvalue() else None
        code,value=cli('prepare','--library',str(self.library),'--library-sha256',self.sha,'--candidate',str(self.candidate),'--capture-id',NONCE)
        self.assertEqual(code,0);self.assertEqual(value['profile'],mod.SCENE_PROFILE)
        self.assertEqual(cli('status')[0],0);self.assertEqual(cli('status',profile=mod.PROFILE)[0],2)
        self.assertEqual(cli('rollback')[0],0);self.assertEqual(cli('rollback')[0],0)
        self.assertTrue(self.session.candidate.exists());self.assertTrue(self.session.receipt.exists())

    def test_foreign_scan_failure_does_not_block_owned_manual_rollback(self):
        self.prepare()
        with mock.patch.object(self.session,'_foreign_files',side_effect=OSError('foreign scan unavailable')):
            result=self.session.rollback()
        self.assertFalse(result['complete']);self.assertFalse(self.session.enable.exists())
        self.assertFalse(self.session.base.library.exists());self.assertFalse(self.session.base.manifest.exists())
        self.assertTrue(any(x.startswith('foreign profile scan failed:') for x in result['errors']))

    def test_profile_input_is_finite_selector_not_pin_override(self):
        for value in [True,None,{},'scene-contribution-atlas-v2','../../scene']:
            with self.assertRaises(mod.SafetyError):mod.Session(self.home,self.run,proc_root=self.proc,host_fixture=True,profile=value)
        with self.assertRaises((mod.SafetyError,OSError)):mod.verify_candidate(self.code,mod.PROFILE)


class PrivateCandidateTests(unittest.TestCase):
    def test_exact_generated_scene_candidate_matches_stager_production_pin(self):
        path=os.environ.get('FRAME_PRIVATE_SCENE_CANDIDATE')
        if not path:self.skipTest('explicit private scene candidate absent')
        raw=Path(path).read_bytes();mod.verify_candidate(raw,mod.SCENE_PROFILE)
        bad=bytearray(raw);bad[-1]^=1
        with self.assertRaises(mod.SafetyError):mod.verify_candidate(bytes(bad),mod.SCENE_PROFILE)
        with self.assertRaises(mod.SafetyError):mod.verify_candidate(raw,mod.PROFILE)


if __name__=='__main__':unittest.main()
