"""Frozen v2 windmill and dual-profile native guard; private originals explicit."""
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import native_fragment_scene_presenter as scene
spec=importlib.util.spec_from_file_location('scene_scalar_oracle',ROOT/'tests/test_native_fragment_scene_presenter.py')
oracle=importlib.util.module_from_spec(spec);spec.loader.exec_module(oracle)


class FullProfile(unittest.TestCase):
    def test_exact_final_windmill_profile(self):
        p=scene.load_profile(scene.SPEC_V2)
        self.assertEqual(p['canvas'],[3168,3168]);self.assertEqual(p['scene'],[560,560,2048,2048])
        self.assertEqual(p['coverage']['data_origins'],[[576,32],[2624,576],[544,2624],[32,544]])
        self.assertEqual(p['coverage']['rotation_quarter_turns'],[0,1,0,1])
        self.assertEqual(p['guard']['codes'],[104,14,149,250,167,249,185,25,121,144,38,152])
        self.assertEqual(hashlib.sha256(json.dumps(p,sort_keys=True,separators=(',',':')).encode()).hexdigest(),scene.PROFILE_V2_PIN)
        for key,value in [('canvas',[3168,3169]),('revision',1)]:
            bad=scene.load_profile(scene.SPEC_V2);bad[key]=value
            with self.assertRaises(scene.presenter.atlas.probe.ProbeError):scene.profile_contract(bad)


class FullShader(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        name=os.environ.get('FRAME_PRIVATE_ORIGINAL_PS')
        if not name:raise unittest.SkipTest('private original absent')
        cls.original=Path(name).read_bytes()
        cls.binary,cls.record=scene.transform(cls.original,scene.load_profile(scene.SPEC_V2))
        cls.h,cls.ins=scene.parse_module(cls.binary);cls.definitions=oracle.defs(cls.ins)

    def test_rotated_cell_centres_all_tiles_and_edges(self):
        points=[0.,.5/2048,.1,.249,.25,.2505,.499,.5,.5005,.749,.75,.7505,.9,1-.5/2048,1.]
        ids=self.record['new_ssa_ids'];p=scene.load_profile(scene.SPEC_V2)
        for x in points:
            for y in points:
                q=[oracle.f32(x),oracle.f32(y)]
                px,py=[oracle.f32(oracle.f32(min(oracle.f32(1-.5/2048),max(oracle.f32(.5/2048),v))*2048)-.5) for v in q]
                tile=min(3,math.floor(max(0,py)/512));local=oracle.f32(py-tile*512)
                a,b=(local,oracle.f32(2047-px)) if tile%2 else (px,local)
                ox,oy=p['coverage']['data_origins'][tile]
                expected=[oracle.f32(oracle.f32(oracle.f32(ox+a)+.5)/3168),oracle.f32(oracle.f32(oracle.f32(oy+b)+.5)/3168)]
                self.assertEqual(oracle.evaluate(ids['mapped_qa'],self.definitions,{883:q}),expected)
                self.assertEqual(oracle.evaluate(ids['mask_tile_base'],self.definitions,{883:q}),tile*512)

    def test_actual_modules_and_unchanged_default(self):
        for count in (1,3):
            single,_=scene.transform(self.original,scene.load_profile(scene.SPEC_V2),descriptor_count=count)
            dual,record=scene.transform_dual(self.original,descriptor_count=count)
            for binary in (single,dual):
                with tempfile.TemporaryDirectory(prefix='full-atlas-val-') as d:
                    p=Path(d)/'candidate.spv';p.write_bytes(binary)
                    v=subprocess.run(['spirv-val','--target-env','vulkan1.1',str(p)],capture_output=True,text=True)
                    self.assertEqual(v.returncode,0,v.stdout+v.stderr)
            self.assertEqual(scene.transform_dual(self.original,False,count)[0],self.original)
            self.assertEqual(record['supported_profiles'],[1,2]);self.assertFalse(record['alpha_threshold'])
        old,_=scene.transform(self.original,scene.load_profile())
        self.assertEqual(len(old),19224)

    def test_dual_original_prefix_reads_and_exact_fallback(self):
        dual,record=scene.transform_dual(self.original);_,ins=scene.parse_module(dual)
        h,native=scene.ssa.parse_module(self.original);_,store=scene.ssa.profile_structure(h,native)
        n=next(i for i,x in enumerate(native) if x.opcode==54);d=next(i for i,x in enumerate(ins) if x.opcode==54)
        self.assertEqual(ins[d:d+store-n],native[n:store])
        for op in (61,65,87):self.assertEqual([x for x in ins if x.opcode==op],[x for x in native if x.opcode==op])
        self.assertEqual(sum(x==scene.ssa.Instruction(62,(504,994)) for x in ins),3)
        self.assertEqual(sum(x.opcode==88 for x in ins),32)
        self.assertEqual(len(record['v2_guard_cells']),12)
        self.assertEqual(record['guard_sample_budget'],{'v2':12,'legacy_v1':13,'ordinary_first_cells_mismatch':2})


if __name__=='__main__':unittest.main()
