"""Actual owned retired files and native camera boundary; no device actions."""
import base64,importlib.util,os
from pathlib import Path
import unittest
from unittest import mock
ROOT=Path(__file__).resolve().parents[1]
def load(name,path):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
f=load('sfnp737_old_fixture',ROOT/'tests/test_stream_bridge_install.py');s,b=f.s,f.b
legacy=load('sfnp737_legacy',ROOT/'setup/steam_link_setup_legacy.py')
class Tests(f.Tests):
 def seed(self):
  self.olddata=self.fhome/'.local/share/SteamFrameNativePassthrough';self.oldcfg=self.fhome/'.config/SteamFrameNativePassthrough'
  self.olddrop=self.fhome/'.config/systemd/user/steamvr.service.d'/legacy.DROPIN
  self.oldmanifest=self.fhome/'.local/share/vulkan/implicit_layer.d'/legacy.MANIFEST
  files={self.olddata/'tools/native_passthrough_setup.py':b'owned old tool',self.olddata/'package/legacy.so':b'owned old library',self.olddrop:b'[Service]\nExecCondition=/oldhelper\n'}
  for p,raw in files.items():s.put(p,raw)
  self.oldmanifest.parent.mkdir(parents=True,exist_ok=True,mode=0o700)
  target=str(Path('/run/user')/str(os.getuid())/'steam-frame-native-passthrough/receiver.json');self.oldmanifest.symlink_to(target)
  s.put(self.oldcfg/'install.json',s.encoded({'schema':legacy.RECEIPT,'home':str(self.fhome),'data':str(self.olddata),'manifest_link':target,'package_sha256':'71'*32,'files':{str(p):s.sha(raw)for p,raw in files.items()}}))
  s.put(self.oldcfg/'control.json',s.encoded({'schema':'sfnp-user-control-v1','enabled':True}))
  before=s.read(self.fsettings);prefs=s.parse(before);prefs['camera']['forceSceneAlphaBlending']=True;prefs['camera']['keep']=7;s.put(self.fsettings,s.encoded(prefs),before)
  s.put(self.oldcfg/'camera-before.json',s.encoded({'schema':legacy.CAMERA,'settings':str(self.fsettings),'previous':{'present':False,'value':None},'write_intent':True}))
  pin=self.bundle.frame['runtime_files']['openvr'];self.unit._camera_payload=(self.bundle.files['frame/'+b.CAMERA_TOOL],{'path':str(self.fsvr/pin['path']),'bytes':pin['bytes'],'sha256':pin['sha256']})
  return files
 def test_pure_plan_proves_all_old_files_without_running_old_code(self):
  files=self.seed();all_before={str(p):p.read_bytes()for p in self.base.rglob('*')if p.is_file()and not p.is_symlink()}
  value=legacy.plan(self.unit)
  self.assertEqual(len(value['operations']),len(files)+4)
  self.assertTrue(all(op['after']is None for op in value['operations']))
  self.assertEqual(value['camera']['desired'],None)
  self.assertEqual(self.camera_native_calls,[])
  self.assertEqual(all_before,{str(p):p.read_bytes()for p in self.base.rglob('*')if p.is_file()and not p.is_symlink()})
 def test_real_camera_restore_removes_only_owned_override_then_exact_files(self):
  self.seed();value=legacy.plan(self.unit);legacy.apply_camera(self.unit,value)
  self.assertEqual(self.camera_native_calls,[None]);prefs=s.parse(s.read(self.fsettings))
  self.assertEqual(prefs['camera'],{'keep':7});self.assertEqual(prefs['user'],{'quality':3})
  for op in value['operations']:self.unit.tx.replace(op)
  self.assertFalse(self.olddrop.exists());self.assertFalse(os.path.lexists(self.oldmanifest))
  self.assertFalse((self.oldcfg/'install.json').exists());self.assertFalse((self.oldcfg/'camera-before.json').exists())
  self.assertIsNone(legacy.plan(self.unit))
 def test_changed_old_payload_refuses_before_any_camera_or_file_mutation(self):
  files=self.seed();path=next(p for p in files if p.suffix=='.so');s.put(path,b'foreign',s.read(path))
  with self.assertRaisesRegex(s.SetupError,'component changed'):legacy.plan(self.unit)
  self.assertEqual(self.camera_native_calls,[]);self.assertTrue(self.olddrop.exists());self.assertTrue(os.path.lexists(self.oldmanifest))
 def test_foreign_manifest_is_never_removed(self):
  self.seed();self.oldmanifest.unlink();s.put(self.oldmanifest,b'foreign real manifest')
  with self.assertRaisesRegex(s.SetupError,'layer link'):legacy.plan(self.unit)
  self.assertEqual(self.camera_native_calls,[])
 def test_unknown_camera_baseline_does_not_authorize_writes(self):
  self.seed();p=self.oldcfg/'camera-before.json';v=s.parse(s.read(p));v['previous']['value']='guess';s.put(p,s.encoded(v),s.read(p))
  with self.assertRaisesRegex(s.SetupError,'original camera preference'):legacy.plan(self.unit)
  self.assertEqual(self.camera_native_calls,[])
 def test_current_camera_change_is_preserved(self):
  self.seed();p=self.fsettings;v=s.parse(s.read(p));v['camera']['forceSceneAlphaBlending']=False;s.put(p,s.encoded(v),s.read(p));before=s.read(p)
  with self.assertRaisesRegex(s.SetupError,'changed outside'):legacy.plan(self.unit)
  self.assertEqual(s.read(p),before);self.assertEqual(self.camera_native_calls,[])
 def test_missing_legacy_receipt_never_reactivates_a_paired_predecessor(self):
  drop=self.fhome/'.config/systemd/user/steamvr.service.d'/legacy.DROPIN
  state={'operations':[{'path':str(drop),'before':{'kind':'file','bytes':base64.b64encode(b'owned predecessor').decode()},'after':None,'mode':0o600}]}
  with mock.patch.object(self.unit.tx,'state',return_value=state):
   with self.assertRaisesRegex(s.SetupError,'restoring a retired patch was refused'):legacy.plan(self.unit)
  self.assertFalse(drop.exists());self.assertEqual(self.camera_native_calls,[])
 def test_missing_camera_baseline_never_claims_a_complete_uninstall(self):
  self.seed();(self.oldcfg/'camera-before.json').unlink()
  with self.assertRaisesRegex(s.SetupError,'original camera preference was not recorded'):legacy.plan(self.unit)
  self.assertTrue(self.olddrop.exists());self.assertEqual(self.camera_native_calls,[])
 def test_orphan_camera_authority_never_disappears_behind_not_installed(self):
  self.seed();(self.oldcfg/'install.json').unlink();self.olddrop.unlink();self.oldmanifest.unlink()
  with self.assertRaisesRegex(s.SetupError,'no matching installation receipt'):legacy.plan(self.unit)
  self.assertEqual(self.camera_native_calls,[])
 def test_launch_hook_missing_from_receipt_refuses_instead_of_leaving_it_active(self):
  self.seed();path=self.oldcfg/'install.json';raw=s.read(path);v=s.parse(raw);v['files'].pop(str(self.olddrop));s.put(path,s.encoded(v),raw)
  with self.assertRaisesRegex(s.SetupError,'launch hook is missing'):legacy.plan(self.unit)
  self.assertTrue(self.olddrop.exists());self.assertEqual(self.camera_native_calls,[])
 def partial_uninstall(self):
  self.seed();plan=legacy.plan(self.unit)
  self.unit.backup('uninstall',plan['operations'],{'path':str(self.fsettings),'policy':b.SESSION_POLICY},[None],camera_override=plan['camera'])
  legacy.apply_camera(self.unit,plan)
  removed={self.oldcfg/'install.json',self.oldcfg/'camera-before.json',self.olddata/'package/legacy.so'}
  for op in plan['operations']:
   if Path(op['path'])in removed:self.unit.tx.replace(op)
  return self.unit.checkpoint().value()
 def test_interrupted_uninstall_uses_exact_checkpoint_without_reactivating_missing_files(self):
  checkpoint=self.partial_uninstall();before=s.read(self.unit.checkpoint().path)
  recovered=legacy.plan(self.unit,checkpoint=checkpoint)
  self.assertIsNone(recovered['camera']['desired']);self.assertIsNone(recovered['camera']['before'])
  self.assertFalse((self.oldcfg/'install.json').exists());self.assertFalse((self.olddata/'package/legacy.so').exists())
  self.assertEqual(s.read(self.unit.checkpoint().path),before)
  legacy.apply_camera(self.unit,recovered)
  for op in recovered['operations']:self.unit.tx.replace(op)
  self.assertIsNone(legacy.plan(self.unit));self.assertIsNone(self.unit.camera_value(self.fsettings))
 def test_checkpoint_reconstruction_never_admits_changed_remaining_payload(self):
  checkpoint=self.partial_uninstall();path=self.olddata/'tools/native_passthrough_setup.py';s.put(path,b'foreign change',s.read(path))
  calls=list(self.camera_native_calls)
  with self.assertRaisesRegex(s.SetupError,'component changed'):legacy.plan(self.unit,checkpoint=checkpoint)
  self.assertEqual(self.camera_native_calls,calls);self.assertTrue(self.olddrop.exists())
if __name__=='__main__':
 suite=unittest.TestSuite(Tests(name)for name in Tests.__dict__ if name.startswith('test_'))
 raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
