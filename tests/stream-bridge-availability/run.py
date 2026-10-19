#!/usr/bin/env python3
"""Capture the actual async Channel/Renderer availability gap; no device access.

The Renderer case intentionally remains REQUIREMENT RED on the current code.
It demonstrates why native decoded-picture exposure must be ordered after local
coverage publication, rather than claiming an ordinary opaque frame is success.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--source-root', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--vs', type=Path, required=True)
    p.add_argument('--ps', type=Path, required=True)
    p.add_argument('--coverage', type=Path, required=True)
    a = p.parse_args()
    root, out = a.source_root.resolve(), a.output.resolve()
    here = Path(__file__).resolve().parent
    cache = out / 'cache'
    cache.mkdir(parents=True, exist_ok=True)
    original = (root / 'tests/stream-bridge-renderer/fixture.cpp').read_text()
    needle = 'int main(int argc,char** argv)'
    assert original.count(needle) == 1
    fixture = cache / 'renderer.cpp'
    fixture.write_text(original.replace(needle, 'int inheritedRendererMain(int argc,char** argv)') + '\n' + (here / 'renderer_timing.cpp').read_text())
    flags = ['c++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-Wno-missing-field-initializers', '-I' + str(root), '-I' + str(root / 'layer'), '-pthread']
    shared = [root / f for f in ['bridge/stream_bridge_frame.cpp', 'bridge/stream_bridge_auth.cpp', 'bridge/stream_bridge_protocol.cpp', 'bridge/hevc_bridge_sei.cpp']]
    targets = {
        'channel': [here / 'channel_timing.cpp', root / 'receiver/stream_bridge_local.cpp', root / 'receiver/stream_bridge_native_decode.cpp', *shared],
        'renderer': [fixture, root / 'layer/stream_bridge_renderer.cpp', root / 'layer/stream_bridge_coverage_gpu.cpp', *shared],
    }
    results = {}
    try:
        for name, sources in targets.items():
            binary = cache / name
            with (out / (name + '-build.log')).open('w') as log:
                subprocess.run([*flags, *map(str, sources), '-ldl', '-o', str(binary)], stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
            args = [str(binary)]
            if name == 'renderer':
                args += [str(x.resolve()) for x in [a.vs, a.ps, a.coverage]]
            with (out / (name + '.log')).open('w') as log:
                code = subprocess.run(args, stdout=log, stderr=subprocess.STDOUT, timeout=20).returncode
            results[name] = {'exit_code': code}
            if name == 'channel' and code != 0:
                raise RuntimeError('actual Channel fixture failed')
            if name == 'renderer':
                text = (out / 'renderer.log').read_text()
                if code != 1 or 'active=32, unmasked=16' not in text or 'every active native frame must have its exact mask at first presentation' not in text:
                    raise RuntimeError('expected precise availability RED was not observed; review the changed production ordering')
                results[name]['meaning'] = 'User requirement fails in actual production Renderer when LocalPicture arrives after native recording, even if exact certificate precedes CAPTURE. This is evidence, not a passing presentation gate.'
        pins = {str(x): hashlib.sha256(x.read_bytes()).hexdigest() for x in [a.vs, a.ps, a.coverage]}
        (out / 'results.json').write_text(json.dumps({'results': results, 'private_input_pins': pins}, indent=2) + '\n')
        print(json.dumps(results))
    finally:
        # All child processes above have completed or been reaped by run().
        # This native standalone fixture has no Unity project caches.
        shutil.rmtree(cache)


if __name__ == '__main__':
    main()
