"""Public finite harness controls; no private pixels, codecs or GPU execution."""
import importlib.util
import json
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode=True
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('stream',HERE/'run.py');stream=importlib.util.module_from_spec(spec);spec.loader.exec_module(stream)


class StreamContract(unittest.TestCase):
    def test_native_channels_are_not_rgb_and_roundtrip(self):
        for rgb in ((0,0,0),(1,1,1),(.2,.3,.4),(.1,.05,.075),(1,0,.2)):
            encoded=stream.inverse_native(rgb)
            for actual,expected in zip(stream.decode_native(encoded),rgb):self.assertAlmostEqual(actual,expected,places=12)
        self.assertNotEqual(stream.inverse_native((.2,.3,.4)),[.2,.3,.4])
        for level in (0,.25,.5,.75,1):self.assertAlmostEqual(stream.inverse_native((level,level,level))[1],level)

    def test_both_true_texture_extents_keep_quarter_coordinates(self):
        for width in (1152,576):
            for eye in (0,1):
                u,v=stream.chart((.7,.5),eye)
                self.assertAlmostEqual(u*width,.7*width,places=3)
                self.assertEqual(v*(width*4),(1 if eye==0 else 3)*width+width/2)
        self.assertEqual(stream.CASE.size,160)

    def test_full_mask_rotated_mapping_is_not_a_new_fitted_chart(self):
        profile=json.loads((stream.ROOT/'docs/protocols/scene-contribution-atlas-v3.json').read_text())
        for i,q in enumerate(((.25,.125),(.25,.375),(.25,.625),(.25,.875))):
            _,mask=stream.panel_coordinates(q,profile);ox,oy=profile['coverage']['data_origins'][i]
            px,py=q[0]*2048-.5,q[1]*2048-.5-i*512
            x,y=(py,2047-px) if i%2 else (px,py)
            self.assertAlmostEqual(mask[0],(ox+x+.5)/3168,places=6)
            self.assertAlmostEqual(mask[1],(oy+y+.5)/3168,places=6)

    def test_only_explicit_lossy_codec_and_extent_matrix(self):
        for width in (1152,576):
            for codec in ('libx264','libx265'):
                for qp in (23,32):
                    command=stream.codec_args(codec,qp,width,Path('owned.mkv'))
                    self.assertIn('yuv420p',command);self.assertIn('bt709',command);self.assertIn(str(qp),command)
                    self.assertNotIn('lossless',str(command));self.assertEqual(command[-1],'owned.mkv')
        for bad in (('libx265',0,1152),('copy',23,1152),('libx264',23,577)):
            with self.assertRaises(ValueError):stream.codec_args(*bad,Path('refused.mkv'))


if __name__=='__main__':unittest.main()
