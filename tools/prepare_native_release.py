#!/usr/bin/env python3
"""Build the owned receiver and derive its private shader on a compatible Frame.

Only requested local build/evidence files are written; there is no installation,
service/restart action or camera/settings change. Valve input and
derived SPIR-V stay local. verify_package is also the installer's cold pre-start
gate; a changed or unknown runtime must not publish its Vulkan manifest.
"""
from __future__ import annotations
import argparse
import ctypes
import contextlib
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import stat
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
COMPATIBILITY = ROOT / 'packaging/compatibility.json'
if not COMPATIBILITY.is_file():
    COMPATIBILITY = ROOT / 'compatibility.json'
SCHEMA = 'sfnp-native-package-v1'
COMPATIBILITY_SHA256 = 'd398e5cc534b981d738bbc22ffced805bb9609e92c11c47cab6d2a062da4c178'
BUILD_MARKER = b'SFNP_RELEASE_RECEIVER/1\0'
LIBRARY = 'libVkLayer_frame_pixel_snapshot.so'
CANDIDATE = 'scene-presenter.spv'
HELPERS = ('tools/prepare_native_release.py', 'tools/steamvr_camera_alpha.py',
           'tools/pixel_snapshot_session.py', 'pc/vrsettings_win.py')
ARTIFACTS = (LIBRARY, CANDIDATE, 'compatibility.json', 'source-manifest.json', *HELPERS)
MAX_METADATA = 65536


class SafetyError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise SafetyError(message)


def read_regular(path, limit, *, private=False):
    """Open no final symlink; bound both initial size and a growing file."""
    path = Path(path)
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        before = os.fstat(fd)
        require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1
                and 0 <= before.st_size <= limit, 'ordinary bounded single-link file required: ' + str(path))
        if private:
            require(before.st_uid == os.getuid() and stat.S_IMODE(before.st_mode) == 0o600,
                    'owned0600 file required: ' + str(path))
        chunks, total = [], 0
        while total <= limit:
            chunk = os.read(fd, min(65536, limit + 1 - total))
            if not chunk:
                break
            chunks.append(chunk); total += len(chunk)
        after = os.fstat(fd)
        require(total == before.st_size <= limit and
                (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) ==
                (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns),
                'file changed while read: ' + str(path))
        return b''.join(chunks)
    finally:
        os.close(fd)


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def json_bytes(value):
    return (json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + '\n').encode()


def parse_json(raw):
    def pairs(values):
        result = {}
        for key, value in values:
            require(key not in result, 'duplicate metadata key')
            result[key] = value
        return result
    return json.loads(raw, object_pairs_hook=pairs)


def known_compatibility():
    raw = read_regular(COMPATIBILITY, MAX_METADATA)
    require(sha(raw) == COMPATIBILITY_SHA256, 'reviewed compatibility manifest pin differs')
    profile = parse_json(raw)
    require(profile.get('schema') == 'sfnp-native-compatibility-v1' and profile.get('complete') is True,
            'reviewed runtime profile is incomplete; no release activation is supported')
    require(profile.get('architecture') == 'aarch64', 'known AArch64 profile required')
    for role, item in profile['runtime_files'].items():
        require(type(item.get('bytes')) is int and item['bytes'] > 0
                and type(item.get('sha256')) is str and len(item['sha256']) == 64,
                'missing exact runtime fingerprint: ' + role)
    return profile


def runtime_files(steamvr_root=Path('/opt/steamvr'), home=None):
    """Exact roles for installation and every launch. No SteamVR API call."""
    profile = known_compatibility()
    bases = {'steamvr': Path(steamvr_root), 'home': Path.home() if home is None else Path(home), 'absolute': Path('/')}
    return {role: bases[item['location']] / item['path'] for role, item in profile['runtime_files'].items()}


def verify_runtime(files=None, *, steamvr_root=Path('/opt/steamvr')):
    profile = known_compatibility()
    files = runtime_files(steamvr_root) if files is None else files
    require(set(files) == set(profile['runtime_files']), 'complete exact runtime role mapping required')
    for role, item in profile['runtime_files'].items():
        # System aliases are resolved for reading only. The complete resolved
        # target must have exact reviewed bytes and remain stable during read.
        path = Path(files[role]).resolve(strict=True)
        raw = read_regular(path, item['bytes'])
        require(len(raw) == item['bytes'] and sha(raw) == item['sha256'], 'unsupported runtime fingerprint: ' + role)
    return profile


def elf_arm64(raw):
    require(len(raw) >= 64 and raw[:7] == b'\x7fELF\x02\x01\x01'
            and struct.unpack_from('<HH', raw, 16) == (3, 183), 'AArch64 shared ELF required')
    require(raw.count(BUILD_MARKER) == 1, 'release receiver build marker missing or ambiguous')


def package_path(package, name):
    path = Path(package)
    for component in Path(name).parts[:-1]:
        path = path / component; info = path.lstat()
        require(stat.S_ISDIR(info.st_mode) and info.st_uid == os.getuid()
                and stat.S_IMODE(info.st_mode) == 0o700, 'ordinary owned0700 package subdirectory required')
    return Path(package) / name


def verify_package(package, runtime_files=None):
    """Return checked manifest or raise SafetyError/OSError. Never trust its pins.

    runtime_files may map known role names to resolved namespace files for the
    service/operator; expected fingerprints always come from the shipped known
    profile, never this mapping or package metadata.
    """
    package = Path(package)
    info = package.lstat()
    require(stat.S_ISDIR(info.st_mode) and info.st_uid == os.getuid()
            and stat.S_IMODE(info.st_mode) == 0o700, 'owned0700 ordinary package required')
    profile = verify_runtime(runtime_files)
    manifest = parse_json(read_regular(package / 'package.json', MAX_METADATA, private=True))
    require(type(manifest) is dict and set(manifest) == {'schema', 'profile', 'build_marker', 'files', 'source_snapshot_sha256'}
            and manifest['schema'] == SCHEMA and manifest['profile'] == profile['profile']
            and manifest['build_marker'] == BUILD_MARKER[:-1].decode(), 'exact release manifest schema required')
    require(type(manifest['files']) is dict and set(manifest['files']) == set(ARTIFACTS), 'exact artifact inventory required')
    contents = {}
    for name in ARTIFACTS:
        record = manifest['files'][name]
        require(type(record) is dict and set(record) == {'bytes', 'sha256'} and type(record['bytes']) is int
                and 0 < record['bytes'] <= 32 * 1024 * 1024, 'bounded artifact record required')
        contents[name] = read_regular(package_path(package, name), record['bytes'], private=True)
        require(len(contents[name]) == record['bytes'] and sha(contents[name]) == record['sha256'], 'artifact hash mismatch: ' + name)
    require(parse_json(contents['compatibility.json']) == profile, 'package cannot change reviewed compatibility')
    require(sha(contents['compatibility.json']) == COMPATIBILITY_SHA256, 'package compatibility bytes differ')
    require(len(contents[CANDIDATE]) == profile['candidate']['bytes']
            and sha(contents[CANDIDATE]) == profile['candidate']['sha256'], 'exact locally derived scene candidate required')
    elf_arm64(contents[LIBRARY])
    require(sha(contents['source-manifest.json']) == manifest['source_snapshot_sha256'], 'source snapshot digest differs')
    sources = parse_json(contents['source-manifest.json'])
    require(type(sources) is dict, 'source snapshot object required')
    for name in HELPERS:
        require(sources.get(name) == sha(contents[name]) and
                contents[name] == read_regular(ROOT / name, 2 * 1024 * 1024),
                'helper differs from the running reviewed preparation source: ' + name)
    if (ROOT / 'CMakeLists.txt').is_file():
        require(sources == source_snapshot(), 'package source snapshot differs from this checkout')
    return manifest


def run_command(command, log, timeout=600):
    with Path(log).open('ab') as stream:
        process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            code = process.wait(timeout=timeout)
        except BaseException:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            raise
    require(code == 0, 'local build/validation failed; see preparation log: ' + str(log))


def source_snapshot():
    paths = [ROOT / 'CMakeLists.txt', COMPATIBILITY]
    for folder in ('layer', 'receiver', 'tools'):
        paths.extend(p for p in (ROOT / folder).rglob('*') if p.suffix in ('.h', '.cpp', '.py'))
    paths.extend(ROOT / p for p in HELPERS if ROOT / p not in paths)
    paths.extend(ROOT / ('docs/protocols/scene-contribution-atlas-v%d.json' % revision) for revision in (1, 2, 3))
    return {str(p.relative_to(ROOT)): sha(read_regular(p, 2 * 1024 * 1024)) for p in sorted(set(paths))}


def publish_directory(staged, output):
    # Linux renameat2 is atomic and refuses even an empty concurrently created
    # output. A check followed by ordinary rename could overwrite that directory.
    libc = ctypes.CDLL(None, use_errno=True)
    function = getattr(libc, 'renameat2', None)
    require(function is not None, 'atomic no-replace directory publication unavailable')
    function.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint)
    function.restype = ctypes.c_int
    if function(-100, os.fsencode(staged), -100, os.fsencode(output), 1):
        raise OSError(ctypes.get_errno(), 'exclusive package publication refused')


@contextlib.contextmanager
def preparation_run(output, snapshot):
    evidence = Path(tempfile.mkdtemp(prefix=output.name + '.preparation-', dir=output.parent))
    os.chmod(evidence, 0o700)
    cache = evidence / 'cache'; cache.mkdir(mode=0o700)
    log = evidence / 'prepare.log'; log.touch(mode=0o600)
    failure = None
    try:
        (evidence / 'source-manifest.json').write_bytes(json_bytes(snapshot))
        for name, expected in snapshot.items():
            raw = read_regular(ROOT / name, 2 * 1024 * 1024)
            require(sha(raw) == expected, 'source changed before build: ' + name)
            path = evidence / 'sources' / name; path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            path.write_bytes(raw); path.chmod(0o600)
        yield cache, log
    except BaseException as error:
        failure = str(error)[:16384]
        if isinstance(error, Exception):
            raise SafetyError(str(error) + '; retained preparation evidence: ' + str(evidence)) from error
        raise
    finally:
        # The child process groups have already been reaped by run_command.
        # Keep a bounded final log, exact source snapshot and outcome, but remove
        # every regenerable compiler/CMake/generated-module cache on all exits.
        size = log.stat().st_size
        if size > MAX_METADATA:
            with log.open('rb') as stream:
                stream.seek(size - MAX_METADATA); tail = stream.read(MAX_METADATA)
            log.write_bytes(tail)
        shutil.rmtree(cache)
        (evidence / 'result.json').write_bytes(json_bytes({'schema': 'sfnp-local-preparation-result-v1',
            'success': failure is None, 'failure': failure, 'log_original_bytes': size,
            'log_retained_bytes': log.stat().st_size, 'processes_reaped': True,
            'regenerable_cache_removed': True, 'installed': False, 'camera_or_service_changed': False}))
        for path in (evidence / 'source-manifest.json', evidence / 'result.json'):
            path.chmod(0o600)


def prepare(output, steamvr_root=Path('/opt/steamvr')):
    require(platform.system() == 'Linux' and platform.machine().lower() in ('aarch64', 'arm64'),
            'local Linux AArch64 build required; no package was installed')
    for name in ('cmake', 'g++', 'spirv-val'):
        require(shutil.which(name) is not None, 'required local build tool missing: ' + name)
    output = Path(output).absolute()
    require(not output.exists() and not output.is_symlink(), 'exclusive absent output package required')
    require(output.parent.is_dir(), 'output parent must already exist')
    profile = verify_runtime(steamvr_root=steamvr_root)
    files = runtime_files(steamvr_root)
    before = source_snapshot()
    # All processes are reaped before temporary generated modules/build caches
    # disappear. Originals and the caller's existing files are never modified.
    with preparation_run(output, before) as (scratch, log):
        build = scratch / 'build'
        run_command(['cmake', '-S', str(ROOT), '-B', str(build), '-DCMAKE_BUILD_TYPE=Release',
            '-DBUILD_TESTING=OFF', '-DFRAME_MASK_BUILD_PIXEL_SNAPSHOT=ON',
            '-DFRAME_PIXEL_SYNTHETIC_PRESENTER=ON', '-DFRAME_PIXEL_RELEASE_RECEIVER=ON'], log)
        run_command(['cmake', '--build', str(build), '--target', 'VkLayer_frame_pixel_snapshot', '--parallel', '2'], log)
        import native_fragment_scene_presenter as transform
        original = read_regular(Path(files['fragment']).resolve(strict=True), profile['runtime_files']['fragment']['bytes'])
        candidate, record = transform.transform_robust(original, True, 1)
        require(len(candidate) == profile['candidate']['bytes'] and sha(candidate) == profile['candidate']['sha256'], 'generated candidate pin differs')
        candidate_path = scratch / CANDIDATE; candidate_path.write_bytes(candidate)
        run_command(['spirv-val', '--target-env', 'vulkan1.1', str(candidate_path)], log, 30)
        library = read_regular(build / LIBRARY, 32 * 1024 * 1024); elf_arm64(library)
        require(source_snapshot() == before, 'source changed during build; package refused')
        verify_runtime(files)
        staged = scratch / 'package'; staged.mkdir(mode=0o700)
        artifacts = {LIBRARY: library, CANDIDATE: candidate, 'compatibility.json': read_regular(COMPATIBILITY, MAX_METADATA),
                     'source-manifest.json': json_bytes(before)}
        artifacts.update({name: read_regular(ROOT / name, 2 * 1024 * 1024) for name in HELPERS})
        for name, raw in artifacts.items():
            path = staged / name; path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            with path.open('xb') as stream:
                os.chmod(path, 0o600); stream.write(raw)
        manifest = {'schema': SCHEMA, 'profile': profile['profile'], 'build_marker': BUILD_MARKER[:-1].decode(),
            'source_snapshot_sha256': sha(artifacts['source-manifest.json']),
            'files': {name: {'bytes': len(raw), 'sha256': sha(raw)} for name, raw in artifacts.items()}}
        path = staged / 'package.json'
        with path.open('xb') as stream:
            os.chmod(path, 0o600); stream.write(json_bytes(manifest))
        verify_package(staged, files)
        publish_directory(staged, output)
        return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--steamvr-root', type=Path, default=Path('/opt/steamvr'))
    args = parser.parse_args(argv)
    try:
        result = prepare(args.output, args.steamvr_root)
        print(json.dumps({'schema': SCHEMA, 'profile': result['profile'], 'prepared': str(args.output),
                          'installed': False, 'device_changed': False}))
        return 0
    except (SafetyError, OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print('Steam Frame Native Passthrough preparation refused: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
