#!/usr/bin/env python3
"""Restore the original missing metadata budget and require the native QoS witness to fail."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--bridge-library', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    core = root / 'pc/stream-bridge-sender-provider-core.cpp'
    source = core.read_text()
    controls = {
        'unaccounted-metadata': (
            'encoder,applyBudget?&budgeted:params',
            'encoder,params',
            'v2 reserves the actual 70000 added bytes at 120Hz inside the original video budget'),
    }
    controls.update({
        'unbounded-video-buffer': (
            'budgetConfig.rcParams.vbvBufferSize=buffer.bits;budgetConfig.rcParams.vbvInitialDelay=buffer.delay;',
            '(void)buffer;',
            'v2 bounds the complete-frame burst instead of keeping the native video-only VBV'),
        'averaged-loading-spike': (
            'const auto metadata=s->wireBudget.reserve(authority,contract.frameRateNum,contract.frameRateDen);',
            'const auto metadata=s->wireBudget.average(authority,contract.frameRateNum,contract.frameRateDen);',
            'one scene-transition metadata spike reserves its full peak at the next native QoS call'),
    })
    results = {}
    with tempfile.TemporaryDirectory(prefix='native-continuity-', dir=output) as temporary:
        temporary = Path(temporary)
        for name, (old, new, expected) in controls.items():
            if source.count(old) != 1:
                raise RuntimeError('Native causal anchor changed: ' + name)
            implementation = temporary / (name + '.cpp')
            implementation.write_text(source.replace(old, new))
            binary = temporary / name
            command = ['c++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Wpedantic',
                       '-Werror', '-Wno-unused-but-set-variable', '-pthread', '-I' + str(root), '-I' + str(root / 'pc'),
                       '-isystem', str(args.sdk.resolve()),
                       str(root / 'tests/test_stream_bridge_sender_provider_input.cpp'),
                       str(implementation), str(root / 'pc/stream-bridge-sender.cpp'),
                       str(args.bridge_library.resolve()), '-o', str(binary)]
            with (output / (name + '-build.log')).open('w') as log:
                subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=90)
            with (output / (name + '.log')).open('w') as log:
                result = subprocess.run([str(binary)], stdout=log, stderr=subprocess.STDOUT, timeout=20)
            if result.returncode != 1 or expected not in (output / (name + '.log')).read_text():
                raise RuntimeError('Restored barrier did not fail at its exact coverage assertion: ' + name)
            results[name] = {'exit_code': result.returncode, 'expected_failure': expected}
    (output / 'results.json').write_text(json.dumps({
        'production_core_sha256': hashlib.sha256(core.read_bytes()).hexdigest(),
        'controls': results,
        'device_access': False,
    }, indent=2) + '\n')
    print('Unaccounted rate, unbounded VBV and averaged loading spikes each fail their production QoS witness.')


if __name__ == '__main__':
    main()
