"""Public finite harness controls; no private pixels, codecs or GPU execution."""
import builtins
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import os
import sys
import tempfile
from unittest import mock
import unittest

sys.dont_write_bytecode=True
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('stream',HERE/'run.py');stream=importlib.util.module_from_spec(spec);spec.loader.exec_module(stream)


class StreamContract(unittest.TestCase):
    def test_receipt_claim_preserves_existing_and_symlink_before_inputs(self):
        with tempfile.TemporaryDirectory(prefix='stream-receipt-') as tmp:
            directory=Path(tmp);existing=directory/'existing.json'
            existing.write_bytes(b'FOREIGN ORIGINAL RECEIPT\n')
            alias=directory/'alias.json';alias.symlink_to(existing)
            original=existing.read_bytes();identity=existing.stat().st_ino
            importer=builtins.__import__
            def no_optional(name,*args,**kwargs):
                if name.split('.')[0] in ('PIL','numpy'):
                    raise AssertionError('optional import before exclusive output claim')
                return importer(name,*args,**kwargs)
            for path in (existing,alias):
                inode=path.lstat().st_ino
                with mock.patch.object(stream.os,'write',side_effect=AssertionError('no transient output write')), \
                     mock.patch.object(Path,'read_text',side_effect=AssertionError('no input read')), \
                     mock.patch.object(builtins,'__import__',side_effect=no_optional), \
                     mock.patch.object(stream,'completion_cleanup',side_effect=AssertionError('no run to clean')), \
                     contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(FileExistsError):stream.claim_receipt(path)
                    args=['--producer','never-read','--original','never-read','--candidate','never-read',
                          '--historical-dual','never-read','--record',str(path)]
                    self.assertEqual(stream.main(args),1)
                self.assertEqual(path.lstat().st_ino,inode)
                self.assertEqual(existing.stat().st_ino,identity)
                self.assertEqual(existing.read_bytes(),original)
            owned=directory/'owned.json';fd=stream.claim_receipt(owned)
            try:
                with self.assertRaises(FileExistsError):stream.claim_receipt(owned)
                stream.finish_receipt(fd,owned,{'owned':True})
                self.assertEqual(owned.stat().st_mode&0o777,0o600)
                self.assertEqual(json.loads(owned.read_text()),{'owned':True})
            finally:os.close(fd)
            absent=stream.completion_cleanup(1,directory/'result',helper=directory/'absent-external-helper')
            self.assertFalse(absent['external_completion_api_available'])
            self.assertTrue(absent['processes_reaped'] and absent['owned_temporary_directory_cleanup'])

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
