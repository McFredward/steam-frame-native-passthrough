#!/usr/bin/env python3
"""Private-input controls for the actual stream coverage shader generator."""
import hashlib
import ast
import os
import re
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import native_fragment_stream_coverage as coverage
import native_fragment_ssa_probe as ssa


def instructions(binary):
    words = struct.unpack('<%dI' % (len(binary) // 4), binary)
    out = []
    at = 5
    while at < len(words):
        count = words[at] >> 16
        if not count or at + count > len(words):
            raise AssertionError('invalid generated instruction framing')
        out.append((words[at] & 65535, words[at + 1:at + count]))
        at += count
    return out


class CoverageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = os.environ.get('SFNP_PRIVATE_NATIVE_FRAGMENT')
        if not path:
            raise RuntimeError('SFNP_PRIVATE_NATIVE_FRAGMENT must name the pinned private fragment')
        cls.original = Path(path).read_bytes()
        if hashlib.sha256(cls.original).hexdigest() != ssa.ORIGINAL_SHA256:
            raise RuntimeError('private original pin differs')

    def test_actual_scalar_interface_and_unchanged_image_accesses(self):
        scalar, record = coverage.transform(self.original)
        parsed = instructions(scalar)
        variables = {args[1]: args for op, args in parsed if op == 59}
        types = {args[0]: (op, args) for op, args in parsed if op in (28, 32)}
        constants = {args[1]: args[2] for op, args in parsed if op == 43}
        pointer = types[variables[289][0]][1]
        array = types[pointer[2]][1]
        self.assertEqual(constants[array[2]], 1)
        self.assertEqual(record['owned_binding9_descriptor_count'], 1)
        before = [args for op, args in instructions(self.original) if op == 65 and args[2] == 289]
        after = [args for op, args in parsed if op == 65 and args[2] == 289]
        self.assertEqual(after, before)
        self.assertEqual(len(after), 2)
        self.assertEqual(len(scalar), 7188)
        self.assertEqual(hashlib.sha256(scalar).hexdigest(),
                         '316ceb1cc3c50a6eef9c6e176d1001d1afd61e0747b628d87728439c3d2bfc6e')

    def test_native_picture_and_both_reconstruction_coordinates_are_unchanged(self):
        binary, record = coverage.transform(self.original)
        before = instructions(self.original)
        after = instructions(binary)
        # Every native executable instruction survives exactly, except its one
        # final output store where only alpha is inserted into original RGBA994.
        code = False
        for op, args in before:
            if op == 54:
                code = True
            if code and not (op == 62 and args == (504, 994)):
                self.assertIn((op, args), after)
        for coordinate in (538, 798):
            self.assertTrue(any(op == 88 and args[3] == coordinate for op, args in after))
        self.assertTrue(record['native_rgb_instructions_retained'])
        self.assertNotIn('atlas', {node.name for node in ast.walk(ast.parse(
            Path(coverage.__file__).read_text())) if isinstance(node, ast.alias)})

    def test_generated_fallback_is_opaque_and_keeps_native_rgb(self):
        binary, _ = coverage.transform(self.original)
        first_new = struct.unpack('<5I', self.original[:20])[3]
        for control in (0., 1.):
            for weight in (0., .25, 1.):
                values = {185: 1., 1008: weight, 899: .75,
                          994: (.125, .5, .875, .9)}
                output = None
                for op, args in instructions(binary):
                    if op not in (61, 88, 81, 131, 133, 129, 82) or args[1] < first_new:
                        continue
                    if op == 61 and args[0] == 8:
                        values[args[1]] = control
                    elif op == 88:
                        values[args[1]] = (.2 if args[3] == 538 else .6, 0., 0., 1.)
                    elif op == 81 and args[2] in values:
                        values[args[1]] = values[args[2]][args[3]]
                    elif op in (131, 133, 129) and args[2] in values and args[3] in values:
                        a, b = values[args[2]], values[args[3]]
                        values[args[1]] = a - b if op == 131 else a * b if op == 133 else a + b
                    elif op == 82 and args[2] in values and args[3] == 994:
                        self.assertEqual(args[4], 3)
                        output = (*values[994][:3], values[args[2]])
                self.assertIsNotNone(output)
                self.assertEqual(output[:3], values[994][:3])
                self.assertAlmostEqual(output[3], 1. if control == 0. else
                                       (.2 * (1. - weight) + .6 * weight) * .75)

    def test_both_generated_interfaces_validate(self):
        with tempfile.TemporaryDirectory(prefix='sfnp-coverage-count1-') as directory:
            for count in (1, 3):
                binary, _ = coverage.transform(self.original, count)
                path = Path(directory) / ('count%d.spv' % count)
                path.write_bytes(binary)
                result = subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(path)],
                                        capture_output=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stderr.decode('utf8', errors='replace'))
        old, _ = coverage.transform(self.original, 3)
        self.assertEqual(hashlib.sha256(old).hexdigest(),
                         '54760d17ec2933ca667923ca713f64d6810914f9b2b53503d9a6ebb68ee2bdc0')

    def test_unpinned_input_and_unknown_interfaces_refuse(self):
        wrong = bytearray(self.original)
        wrong[-1] ^= 1
        for binary, count in ((bytes(wrong), 1), (self.original, 0), (self.original, True)):
            with self.assertRaises(ValueError):
                coverage.transform(binary, count)

    def test_installer_runtime_and_generator_use_the_same_owned_pins(self):
        root = Path(__file__).resolve().parents[1]
        header = (root / 'layer/stream_bridge_artifacts.h').read_text()
        runtime = {}
        for name, filename in (('Coverage', 'coverage.spv'),):
            size = re.search(r'\b' + name + r'Bytes=(\d+);', header)
            digest = re.search(r'\b' + name + r'Sha256="([a-f0-9]{64})";', header)
            self.assertIsNotNone(size)
            self.assertIsNotNone(digest)
            runtime[filename] = {'bytes': int(size[1]), 'sha256': digest[1]}
        assignments = ast.parse((root / 'setup/stream_bridge_install.py').read_text()).body
        installed = [ast.literal_eval(node.value) for node in assignments
                     if isinstance(node, ast.Assign)
                     and any(isinstance(target, ast.Name) and target.id == 'GENERATED' for target in node.targets)]
        self.assertEqual(installed, [runtime])
        binary, _ = coverage.transform(self.original)
        self.assertEqual(runtime['coverage.spv'], {'bytes': len(binary),
                                                  'sha256': hashlib.sha256(binary).hexdigest()})


if __name__ == '__main__':
    unittest.main()
