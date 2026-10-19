#!/usr/bin/env python3
"""Execute the real wrapper against bounded fake public Vulkan APIs."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Wrapper(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="sfnp-wrapper-")
        cls.directory = Path(cls.temp.name)
        cls.source = (ROOT / "layer/stream_bridge_vulkan.cpp").read_text()
        shutil.copy2(ROOT / "tests/stream-bridge-vulkan/renderer_fixture.h", cls.directory / "stream_bridge_renderer.h")
        shutil.copy2(ROOT / "tests/stream-bridge-vulkan/wrapper_tests.cpp", cls.directory / "wrapper_tests.cpp")
        (cls.directory / "stream_bridge_vulkan.cpp").write_text(cls.source)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def build(self, name, source=None, sanitized=False):
        if source is None:
            source = self.source
        (self.directory / "stream_bridge_vulkan.cpp").write_text(source)
        executable = self.directory / name
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if sanitized else []
        result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                                 "-Wno-missing-field-initializers", "-pthread", *flags,
                                 str(self.directory / "wrapper_tests.cpp"), "-o", str(executable)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout)
        return executable

    def run_fixture(self, executable):
        environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1")
        return subprocess.run([str(executable)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, env=environment, timeout=30)

    def test_actual_wrapper(self):
        result = self.run_fixture(self.build("wrapper"))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("Vulkan wrapper checks passed", result.stdout)
        print(result.stdout.strip())

    def test_actual_wrapper_sanitized(self):
        result = self.run_fixture(self.build("wrapper-sanitized", sanitized=True))
        self.assertEqual(result.returncode, 0, result.stdout)
        print(result.stdout.strip())

    def test_causal_controls(self):
        controls = [
            ("failed-descriptor-pool-reset", "if(result==VK_SUCCESS)d->renderer.descriptorPoolRetired(pool);", "d->renderer.descriptorPoolRetired(pool);",
             "failed native pool reset preserves prior descriptor authority"),
            ("descriptor-allocation-not-observed", "if(result==VK_SUCCESS&&info&&sets)d->renderer.descriptorsAllocated(*info,sets);", "(void)sets;",
             "successful original allocation publishes actual owning pool and native handles"),
            ("reset-retirement", "else d.renderer.commandReset(c.value);", "else d.renderer.commandRetired(c.value);",
             "pool reset preserves allocations"),
            ("protected-pool", "if(pool&&!pool->protectedMemory)", "if(pool)",
             "protected pool is never renderer eligible"),
            ("failed-begin", "if(result==VK_SUCCESS&&d->tracking.load(std::memory_order_acquire))", "if(d->tracking.load(std::memory_order_acquire))",
             "failed native begin cannot authorize recording"),
        ]
        for name, old, new, expected in controls:
            with self.subTest(control=name):
                self.assertEqual(self.source.count(old), 1)
                result = self.run_fixture(self.build(name, self.source.replace(old, new)))
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn(expected, result.stdout)

    def test_destroy_dispatch_causal_control(self):
        old = "owned=remove(r.devices,dispatch);"
        self.assertEqual(self.source.count(old), 1)
        mutant = self.source.replace(old, "(void)dispatch;owned=remove(r.devices,key(value));")
        result = self.run_fixture(self.build("destroy-dispatch", mutant, sanitized=True))
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("heap-use-after-free", result.stdout)

    def test_strict_production_interface(self):
        # Use the actual production renderer declarations, not the fake API.
        header = ROOT / "layer/stream_bridge_renderer.h"
        if not header.is_file():
            self.skipTest("renderer interface is supplied by the integration checkout")
        target = self.directory / "production.o"
        result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                                 "-Wno-missing-field-initializers", "-fPIC", "-I" + str(ROOT / "layer"),
                                 "-c", str(ROOT / "layer/stream_bridge_vulkan.cpp"), "-o", str(target)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main()
