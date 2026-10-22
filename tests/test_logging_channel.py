"""Channel changes only logging policy/data, never native or installer code."""
import importlib.util
import io
import json
from pathlib import Path
import unittest
import zipfile

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('channel',ROOT/'tools/publish_logging_channel.py');p=importlib.util.module_from_spec(spec);spec.loader.exec_module(p)

def archive(files):
    files=dict(files)
    files['bootstrap-package.json']=p.encoded({'schema':'sfnp-bootstrap-stream-bridge-v1','files':{n:p.pin(b) for n,b in files.items()}})
    out=io.BytesIO()
    with zipfile.ZipFile(out,'w') as z:
        for name,data in files.items():z.writestr(name,data)
    return out.getvalue()

def members(raw):
    with zipfile.ZipFile(io.BytesIO(raw)) as z:return {n:z.read(n) for n in z.namelist()}

class Tests(unittest.TestCase):
    def fixture(self):
        files={'pc-provider/bin/win64/driver_steam_link_native_passthrough.dll':b'pc-test-fixture',
               'frame/libVkLayer_steam_link_bridge.so':b'frame-test-fixture','frame/libsfnp_stream_launch.so':b'launcher-test-fixture',
               'setup/steam_link_setup.py':b'unchanged setup fixture','README.txt':b'Logging default: Debug (dev).\n'}
        files.update({n:b'Debug\n' for n in p.DEFAULTS})
        logging={'channel':'dev','level':'Debug'}
        bundle={'logging':logging,'files':{n:p.pin(b) for n,b in files.items() if n.startswith(('pc-provider/','frame/'))}}
        source={'logging':logging,'native_artifacts':{role:p.pin(files[name]) for role,name in {
            'pc_provider':'pc-provider/bin/win64/driver_steam_link_native_passthrough.dll','frame_layer':'frame/libVkLayer_steam_link_bridge.so','frame_selector':'frame/libsfnp_stream_launch.so'}.items()}}
        files['stream-bridge-bundle.json']=p.encoded(bundle);files['stream-bridge-package-source.json']=p.encoded(source)
        return archive(files)

    def check_roundtrip(self,raw):
        old=members(raw);quiet,changed=p.retarget(raw,'main');self.assertTrue(changed)
        new=members(quiet);self.assertEqual(set(old),set(new))
        for n in p.DEFAULTS:self.assertEqual(new[n],b'Important\n')
        for n in old:
            if n.endswith(('.dll','.so','.exe','.py','.sh')):self.assertEqual(old[n],new[n],n)
        self.assertEqual(json.loads(new['stream-bridge-bundle.json'])['logging'],{'channel':'main','level':'Important'})
        same,changed=p.retarget(quiet,'main');self.assertFalse(changed);self.assertEqual(same,quiet)
        verbose,changed=p.retarget(quiet,'dev');self.assertTrue(changed)
        for n in p.DEFAULTS:self.assertEqual(members(verbose)[n],b'Debug\n')
        outer=json.loads(new['bootstrap-package.json']);self.assertEqual(outer['files'],{n:p.pin(b) for n,b in new.items() if n!='bootstrap-package.json'})

    def test_fixed_native_setup_bytes_and_validated_outer_pins(self):self.check_roundtrip(self.fixture())
    def test_wrong_channel_rejected(self):
        with self.assertRaises(ValueError):p.retarget(self.fixture(),'feature/guess')
    def test_changed_payload_refused_even_on_matching_channel(self):
        raw=members(self.fixture());raw.pop('bootstrap-package.json');raw['frame/libVkLayer_steam_link_bridge.so']=b'changed'
        for channel in ('dev','main'):
            with self.assertRaises(ValueError):p.retarget(archive(raw),channel)
    def test_actual_published_package_when_logging_capable(self):
        raw=(ROOT/p.ARCHIVE).read_bytes();logging=json.loads(members(raw)['stream-bridge-bundle.json']).get('logging')
        if logging is None:self.skipTest('Prior package; source candidate has not been packaged yet')
        if logging['channel']=='main':raw,_=p.retarget(raw,'dev')
        self.check_roundtrip(raw)

if __name__=='__main__':unittest.main()
