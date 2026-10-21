"""Own SPIR-V fixtures; public operand roles, no native executable bodies."""
import copy
import hashlib
import importlib.util
import struct
from pathlib import Path
import unittest
from unittest.mock import patch
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('shader_contract', ROOT / 'tools/native_shader_contract.py')
tool = importlib.util.module_from_spec(spec);spec.loader.exec_module(tool)

def packed(words):return struct.pack('<%dI' % len(words), *words)

class Contract(unittest.TestCase):
    def setUp(self):
        self.canonical = [0x07230203,0x10000,0,8,0,(3<<16)|22,2,32,(4<<16)|43,2,3,19]
        self.input = [0x07230203,0x10000,991,32,0,(3<<16)|22,19,32,(4<<16)|43,19,20,19]
        metadata = copy.deepcopy(tool._METADATA)
        metadata['fragment'] = {'header':self.canonical[:5], 'result_ids':[2,3], 'bytes':48,
            'sha256':hashlib.sha256(packed(self.canonical)).hexdigest()}
        self.patch = patch.object(tool,'_METADATA',metadata);self.patch.start();self.addCleanup(self.patch.stop)
    def test_ids_and_compiler_metadata_change_but_literal_does_not(self):
        self.assertEqual(tool.normalize(packed(self.input)),packed(self.canonical))
    def test_debug_added_removed_without_executed_change(self):
        changed = self.input[:5]+[(3<<16)|5,19,0x006f6f66]+self.input[5:]
        self.assertEqual(tool.normalize(packed(changed)),packed(self.canonical))
    def test_arithmetic_literal_change_cannot_be_mistaken_for_an_id(self):
        changed = self.input[:];changed[-1] = 20
        with self.assertRaisesRegex(ValueError,'logic or interface'):tool.normalize(packed(changed))
    def test_debug_string_cannot_claim_an_existing_result(self):
        changed = self.input[:]+[(3<<16)|7,19,0]
        with self.assertRaisesRegex(ValueError,'duplicate'):tool.normalize(packed(changed))
    def test_unknown_interface_or_dependency_is_refused(self):
        for position,value in ((9,31),(0,0),(5,(3<<16)|65535),(10,19),(3,65537)):
            changed = self.input[:];changed[position] = value
            with self.subTest(position=position),self.assertRaises(ValueError):tool.normalize(packed(changed))
    def test_truncation_and_oversized_input_are_bounded(self):
        for raw in (b'',packed(self.input)[:-1],packed(self.input)+bytes(65536)):
            with self.assertRaises(ValueError):tool.normalize(raw)

if __name__=='__main__':unittest.main()
