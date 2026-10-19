#!/usr/bin/env python3
"""Focused immutable R8 pool checks; no game/private Valve inputs or device actions."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
OWNED = ['layer/stream_bridge_coverage_gpu.h', 'layer/stream_bridge_coverage_gpu.cpp',
         'tests/stream-bridge-coverage-gpu/pool_tests.cpp',
         'tests/stream-bridge-coverage-gpu/gpu_fixture.cpp',
         'tests/stream-bridge-coverage-gpu/fixture.vert',
         'tests/stream-bridge-coverage-gpu/fixture.frag',
         'tests/stream-bridge-coverage-gpu/run.py']
FLAGS = ['-std=c++17', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-pthread']


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--suite', choices=['all', 'core', 'gpu', 'scheduled-controls'], default='all')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    commands = []
    result = {'scope': args.suite, 'hardware': False, 'commands': commands,
              'source_pins': {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in OWNED}}
    def run(name, command, expected=0, message=None, env=None):
        before = time.monotonic()
        completed = subprocess.run([str(x) for x in command], text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, timeout=90, env=env)
        text = completed.stdout
        (args.output / (name + '.log')).write_text(text)
        commands.append({'name': name, 'command': [str(x) for x in command],
                         'exit': completed.returncode, 'seconds': round(time.monotonic()-before, 3)})
        if (expected == 0 and completed.returncode != 0) or (expected != 0 and completed.returncode == 0):
            raise RuntimeError(name + ' unexpected exit:\n' + text[-6000:])
        if message and message not in text:
            raise RuntimeError(name + ' missing intended causal assertion:\n' + text[-6000:])
        return text
    try:
        with tempfile.TemporaryDirectory(prefix='sfnp-coverage-gpu-') as temp:
            work = Path(temp)
            source = ROOT / 'layer/stream_bridge_coverage_gpu.cpp'
            core = ROOT / 'tests/stream-bridge-coverage-gpu/pool_tests.cpp'
            if args.suite in ('all', 'core'):
                binary = work / 'pool-tests'
                run('strict-core-build', ['g++', *FLAGS, core, source, '-o', binary])
                result['core'] = json.loads(run('core', [binary]))
                san = work / 'pool-sanitized'
                run('sanitized-build', ['g++', *FLAGS, '-fsanitize=address,undefined', '-fno-omit-frame-pointer', core, source, '-o', san])
                env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1')
                result['sanitized'] = json.loads(run('sanitized', [san], env=env))
                run('arm64-strict', ['aarch64-linux-gnu-g++', *FLAGS, '-I/usr/include', '-c', source, '-o', work / 'pool-arm64.o'])
                # Force the early GPU-ref observation stale, while retaining
                # the real post-writer-claim check. The fixture populates actual
                # submitted refs; no guessed/model GPU counter is injected.
                early = '&&!s.gpuRefs.load(std::memory_order_acquire)'
                late = 'if(selected->gpuRefs.load(std::memory_order_acquire))'
                text = source.read_text()
                if text.count(early) != 1 or text.count(late) != 1:
                    raise RuntimeError('writer GPU-reference causal anchors changed')
                stale = text.replace(early, '')
                variant = work / 'stale-gpu-precheck.cpp'
                variant.write_text(stale)
                run('stale-gpu-precheck-build', ['g++', *FLAGS, '-I' + str(ROOT / 'layer'), core, variant, '-o', work / 'stale-gpu-precheck'])
                result['stale_gpu_precheck'] = json.loads(run('stale-gpu-precheck', [work / 'stale-gpu-precheck']))
                variant.write_text(stale.replace(late, 'if(false)'))
                run('missing-post-claim-gpu-build', ['g++', *FLAGS, '-I' + str(ROOT / 'layer'), core, variant, '-o', work / 'missing-post-claim-gpu'])
                run('missing-post-claim-gpu', [work / 'missing-post-claim-gpu'], expected=1,
                    message='post-claim GPU ownership prevents any cold resource replacement')
                result['post_claim_gpu_control'] = 'stale early observation remains safe; removing the post-claim check fails'
                mutants = [
                    ('cold-overwrites-in-flight-copy', '(s.state==Impl::State::Free||s.state==Impl::State::Ready)',
                     '(s.state==Impl::State::Free||s.state==Impl::State::Ready||s.state==Impl::State::Submitting)', 'Submitting slot cannot be recycled'),
                    ('bind-before-copy', 'selected->state=ok?Impl::State::Pending:Impl::State::Free',
                     'selected->state=ok?Impl::State::Ready:Impl::State::Free', 'pending is never sampled'),
                    ('retire-before-fence', 'if(r==VK_SUCCESS){for(std::size_t i=0;i<MaximumSlots;',
                     'if(r==VK_SUCCESS||r==VK_NOT_READY){for(std::size_t i=0;i<MaximumSlots;', 'tail fence required'),
                    ('stale-release', 'for(unsigned tries=0;tries<8;++tries){if((refs>>16)!=l.generation||!(refs&RefMask))return;',
                     'for(unsigned tries=0;tries<8;++tries){if(!(refs&RefMask))return;', 'stale release cannot decrement new slot'),
                ]
                result['negative_controls'] = []
                for name, old, new, intended in mutants:
                    text = source.read_text()
                    if text.count(old) != 1:
                        raise RuntimeError('mutant source selector changed: ' + name)
                    mutation = work / (name + '.cpp')
                    mutation.write_text(text.replace(old, new))
                    mutant = work / name
                    run(name + '-build', ['g++', *FLAGS, '-I' + str(ROOT / 'layer'), core, mutation, '-o', mutant])
                    run(name, [mutant], expected=1, message=intended)
                    result['negative_controls'].append({'name': name, 'assertion': intended, 'rejected': True})
            if args.suite in ('all', 'core', 'scheduled-controls'):
                scheduled_mutants = [
                    ('scheduled-before-copy-submit',
                     's.state==State::Ready||(s.state==State::Copying)',
                     's.state==State::Ready||(s.state==State::Copying||s.state==State::Pending)',
                     'first pending upload cannot enable a native sample'),
                ]
                result.setdefault('negative_controls', [])
                for name, old, new, intended in scheduled_mutants:
                    text = source.read_text()
                    if text.count(old) != 1:
                        raise RuntimeError('scheduled source selector changed: ' + name)
                    mutation = work / (name + '.cpp')
                    mutation.write_text(text.replace(old, new))
                    mutant = work / name
                    run(name + '-build', ['g++', *FLAGS, '-I' + str(ROOT / 'layer'), core, mutation, '-o', mutant])
                    run(name, [mutant], expected=1, message=intended)
                    result['negative_controls'].append({'name': name, 'assertion': intended, 'rejected': True})
            if args.suite in ('all', 'gpu'):
                vertex, fragment = work / 'fixture.vert.spv', work / 'fixture.frag.spv'
                for suffix, target in [('vert', vertex), ('frag', fragment)]:
                    run('shader-' + suffix, ['glslc', ROOT / ('tests/stream-bridge-coverage-gpu/fixture.' + suffix), '-o', target])
                    run('validate-' + suffix, ['spirv-val', '--target-env', 'vulkan1.0', target])
                gpu = work / 'gpu-fixture'
                run('strict-gpu-build', ['g++', *FLAGS, ROOT / 'tests/stream-bridge-coverage-gpu/gpu_fixture.cpp', source, '-lvulkan', '-o', gpu])
                gpu_env = dict(os.environ, MESA_SHADER_CACHE_DISABLE='true', XDG_CACHE_HOME=str(work / 'gpu-cache'))
                result['gpu'] = json.loads(run('gpu', [gpu, vertex, fragment], env=gpu_env))
            result['result'] = 'PASS'
    except BaseException as error:
        result['result'] = 'FAIL'
        result['error'] = str(error)
        raise
    finally:
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        # All subprocesses above have been synchronously reaped. No generated
        # compiler/Vulkan cache lives outside this helper-owned temporary tree.
        cleanup = {'processes_reaped': True, 'temporary_tree_removed': True}
        retention = Path('/home/claw/gloomhaven_vr/scripts')
        if retention.is_dir():
            import sys
            sys.path.insert(0, str(retention))
            from town_run_retention import cleanup_completed_run
            cleanup['completion_api'] = cleanup_completed_run(
                'stream-bridge-coverage-gpu', 0 if result.get('result') == 'PASS' else 1,
                args.output, ROOT, process_reaped=True)
        (args.output / 'cleanup.json').write_text(json.dumps(cleanup, indent=2, default=str) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
