"""Frozen robust-v3/v1 format composition; private native input stays explicit."""
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import native_fragment_scene_presenter as scene
spec = importlib.util.spec_from_file_location('robust_scalar', ROOT / 'tests/test_native_fragment_scene_presenter.py')
scalar = importlib.util.module_from_spec(spec); spec.loader.exec_module(scalar)


class RobustProfile(unittest.TestCase):
    def test_exact_profile_and_unchanged_full_resolution_geometry(self):
        p = scene.load_profile(scene.SPEC_V3)
        old = scene.load_profile(scene.SPEC_V2)
        for name in ('canvas', 'scene', 'coordinates', 'coverage', 'source_transfer', 'rgb_convention', 'output'):
            self.assertEqual(p[name], old[name])
        self.assertEqual(p['revision'], 3)
        self.assertEqual(p['guard']['origins'], [[48,64],[208,64],[368,64],[48,224],[208,224],[368,224],
            [2672,2704],[2832,2704],[2992,2704],[2672,2864],[2832,2864],[2992,2864]])
        self.assertEqual(p['guard']['size'], [128,128])
        self.assertEqual(p['guard']['code_levels'], [32,96,160,224])
        self.assertEqual(p['guard']['code_tolerance'], 16)
        self.assertEqual(p['guard']['codes'], [32,224,224,96,96,160,96,224,160,224,96,96])
        self.assertEqual(hashlib.sha256(json.dumps(p, sort_keys=True, separators=(',', ':')).encode()).hexdigest(), scene.PROFILE_V3_PIN)
        for i, origin in enumerate(p['guard']['origins']):
            self.assertEqual(scene.guard_center(p, i), [origin[0]+64, origin[1]+64])

    def test_signature_derivation_and_immutable_admission(self):
        p = scene.load_profile(scene.SPEC_V3)
        base = copy.deepcopy(p)
        for key in ('signature_preimage', 'signature_sha256', 'codes'): del base['guard'][key]
        preimage = 'GloomhavenVR/SteamLink/scene-contribution-atlas-v3\n' + json.dumps(base, sort_keys=True, separators=(',', ':')) + '\n'
        digest = hashlib.sha256(preimage.encode()).digest()
        self.assertEqual(preimage, p['guard']['signature_preimage'])
        self.assertEqual(digest.hex(), p['guard']['signature_sha256'])
        self.assertEqual([p['guard']['code_levels'][v & 3] for v in digest[:12]], p['guard']['codes'])
        sealed = scene.profile_contract(p); p['guard']['origins'][0][0] += 1
        self.assertEqual(sealed['guard']['origins'][0], [48,64])
        for key, value in [('code_tolerance',17), ('size',[129,128]), ('codes',[32]*12), ('origins',[[0,0]]*12)]:
            bad = scene.load_profile(scene.SPEC_V3); bad['guard'][key] = value
            with self.assertRaises(scene.presenter.atlas.probe.ProbeError): scene.profile_contract(bad)


class RobustShader(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        name = os.environ.get('FRAME_PRIVATE_ORIGINAL_PS')
        if not name: raise unittest.SkipTest('explicit private original absent')
        cls.original = Path(name).read_bytes()
        cls.single, cls.single_record = scene.transform(cls.original, scene.load_profile(scene.SPEC_V3))
        cls.binary, cls.record = scene.transform_robust(cls.original)
        _, cls.single_instructions = scene.parse_module(cls.single)
        cls.definitions = scalar.defs(cls.single_instructions)
        cls.header, cls.instructions = scene.parse_module(cls.binary)

    def test_native_prefix_and_load_sample_bindings_preserved(self):
        h, native = scene.ssa.parse_module(self.original)
        _, store = scene.ssa.profile_structure(h, native)
        start = next(i for i,x in enumerate(native) if x.opcode == 54)
        new = next(i for i,x in enumerate(self.instructions) if x.opcode == 54)
        self.assertEqual(self.instructions[new:new+store-start], native[start:store])
        for op in (61,65,87):
            self.assertEqual([x for x in self.instructions if x.opcode == op], [x for x in native if x.opcode == op])
        self.assertEqual(sum(x == scene.ssa.Instruction(62,(504,994)) for x in self.instructions), 3)
        self.assertEqual(sum(x.opcode == 88 for x in self.instructions), 32)
        self.assertEqual(self.record['guard_sample_budget'], {'v3':12,'legacy_v1':13,'ordinary_first_cells_mismatch':2})
        self.assertEqual(self.record['supported_profiles'], [1,3])
        self.assertNotIn('profile_v2', self.record)
        self.assertEqual(len(self.record['v3_guard_cells']), 12)
        self.assertFalse(self.record['extra_pixel_planes']); self.assertFalse(self.record['alpha_threshold'])
        self.assertEqual(self.record['candidate_marker'], scene.MARKER_ROBUST)

    def test_all_twelve_independent_inclusive_windows(self):
        ids = self.single_record['guard_ssa']; d = self.definitions
        good = {c['g']:scalar.f32(c['code']/255) for c in ids['cells']}
        for i, cell in enumerate(ids['cells']):
            for delta in (-17,-16,16,17):
                values = {**good,cell['g']:scalar.f32((cell['code']+delta)/255)}
                self.assertEqual(scalar.evaluate(ids['signature_valid'],d,values), abs(delta) <= 16, (i,delta))
            values = {**good,cell['g']:float('nan')}
            self.assertFalse(scalar.evaluate(ids['signature_valid'],d,values))
            self.assertEqual(cell['center_canvas'], scene.guard_center(scene.load_profile(scene.SPEC_V3),i))
        for grey in range(256):
            self.assertFalse(scalar.evaluate(ids['signature_valid'], d, {c['g']:scalar.f32(grey/255) for c in ids['cells']}))

    def test_same_full_resolution_coordinates_as_frozen_v2(self):
        old, record = scene.transform(self.original, scene.load_profile(scene.SPEC_V2))
        _, old_instructions = scene.parse_module(old); old_defs = scalar.defs(old_instructions)
        points = [0.,.5/2048,.1,.249,.25,.499,.5,.749,.75,.9,1-.5/2048,1.]
        for x in points:
            for y in points:
                q = [scalar.f32(x),scalar.f32(y)]
                for key in ('mapped_qc','mapped_qa','mask_tile_base'):
                    self.assertEqual(scalar.evaluate(self.single_record['new_ssa_ids'][key],self.definitions,{883:q}),
                        scalar.evaluate(record['new_ssa_ids'][key],old_defs,{883:q}))

    def test_robust_cli_records_actual_frozen_source(self):
        with tempfile.TemporaryDirectory(prefix='robust-native-cli-') as directory:
            path = Path(directory); original = path/'original.spv'; original.write_bytes(self.original)
            out, record = path/'out.spv', path/'record.json'
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(scene.main(['--original',str(original),'--output',str(out),
                    '--record',str(record),'--robust-profile']),0)
            self.assertEqual(out.read_bytes(),self.binary)
            receipt = json.loads(record.read_text())
            self.assertEqual(receipt['profile_file_sha256'],hashlib.sha256(scene.SPEC_V3.read_bytes()).hexdigest())
            self.assertEqual(receipt['supported_profiles'],[1,3])

    def test_exact_shipping_and_historical_modules(self):
        self.assertEqual(len(self.binary),31384)
        self.assertEqual(hashlib.sha256(self.binary).hexdigest(),'44c4a6c6bf7543b53d14ab6d350d8498143d6c03f70bfb45b9c6812e6b900a49')
        for count, length, digest in [(1,31960,'9ad1b56a20db039132ad6ec4557fe8ece3fff277316370bcefc9f70ec7aed367'),
                                     (3,31864,'79f18ddc2584918360d4316f249f58712c50285c83648dd9aa005e66daf00461')]:
            historical, record = scene.transform_dual(self.original,descriptor_count=count)
            self.assertEqual(len(historical),length); self.assertEqual(hashlib.sha256(historical).hexdigest(),digest)
            self.assertEqual(record['supported_profiles'],[1,2])
            robust, _ = scene.transform_robust(self.original,descriptor_count=count)
            self.assertEqual(scene.transform_robust(self.original,False,count)[0],self.original)
            for candidate in (historical,robust):
                with tempfile.TemporaryDirectory(prefix='robust-native-val-') as directory:
                    path = Path(directory)/'candidate.spv'; path.write_bytes(candidate)
                    result = subprocess.run(['spirv-val','--target-env','vulkan1.1',str(path)],capture_output=True,text=True,timeout=15)
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        for kwargs in ({'enabled':1},{'descriptor_count':True},{'descriptor_count':2}):
            with self.assertRaises(scene.presenter.atlas.probe.ProbeError): scene.transform_robust(self.original,**kwargs)


if __name__ == '__main__': unittest.main()
