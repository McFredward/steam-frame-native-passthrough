"""Public cross-language contract; no private native shader inputs required."""
import ast
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ArtifactPins(unittest.TestCase):
    def test_installer_and_production_loader_admit_the_same_generated_outputs(self):
        header = (ROOT / 'layer/stream_bridge_artifacts.h').read_text()
        native = {}
        for name, filename in (('Coverage', 'coverage.spv'), ('CameraOnly','camera-only.spv')):
            size = re.search(r'\b' + name + r'Bytes=(\d+);', header)
            digest = re.search(r'\b' + name + r'Sha256="([a-f0-9]{64})";', header)
            self.assertIsNotNone(size)
            self.assertIsNotNone(digest)
            native[filename] = {'bytes': int(size[1]), 'sha256': digest[1]}
        tree = ast.parse((ROOT / 'setup/stream_bridge_install.py').read_text())
        generated = [ast.literal_eval(node.value) for node in tree.body
                     if isinstance(node, ast.Assign) and any(
                         isinstance(target, ast.Name) and target.id == 'GENERATED' for target in node.targets)]
        self.assertEqual(generated, [native], 'Installed outputs and the actual native loader must share exact pins.')

    def test_product_runtime_has_no_atlas_decoder_selection(self):
        for name in ('layer/stream_bridge_renderer.cpp', 'layer/stream_bridge_process.cpp',
                     'layer/stream_bridge_process.h', 'layer/stream_bridge_artifacts.h'):
            text = (ROOT / name).read_text()
            for forbidden in ('readLegacyFragmentCold', 'legacyWords', 'LegacyBytes',
                              'LegacySha256', 'legacy.spv', 'entry->legacy'):
                self.assertNotIn(forbidden, text, name)
        tree = ast.parse((ROOT / 'tools/native_fragment_stream_coverage.py').read_text())
        imported = {item.name for node in tree.body if isinstance(node, ast.Import) for item in node.names}
        self.assertIn('native_fragment_count1_interface', imported)
        self.assertFalse(imported & {'native_fragment_scene_presenter', 'native_fragment_packed_output',
                                    'native_fragment_atlas_probe'})


if __name__ == '__main__':
    unittest.main()
