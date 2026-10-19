#!/usr/bin/env python3
"""Offline AMF ABI controls; no driver installation, GPU or SDK download.

The caller supplies reviewed AMD public headers. Build output must be a new
private directory. Results/source pins survive completion; only own cache goes.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import shutil
import subprocess

SDK_COMMIT = "57ee0cfb0e1a08d9041f77435653794c1b012be0"
SDK_PINS = {'components/Component.h': '4dda42a6e17247a0ce20803e500a869adb36fb33aedda8d314dd9fcbac499ef4',
 'core/Data.h': 'eaac4d80dd3215638c70f8a4ca01791d26d9df77586f665a772bb51907a8035b',
 'core/PropertyStorage.h': '0059b787e78cd7c4e57212ccaab56ad7e17e9474a31337da0eac5b2d056a5dd8',
 'core/Variant.h': 'be4e3db8f1c11b737e22367b8d16b170bfd9a178fdcd912d63f4a1b18e5f7ae1',
 'core/Interface.h': '304f6d6077e9b0733859d44a5e43a343ae374c8fe72c7d79741c0aca89c21162',
 'core/Result.h': 'a469ff33875484b5762199da757819056ff4a7535bfdcec1f4cf2ffc403b98f2',
 'core/Platform.h': 'c060dc2466d0d886d1a09e5fea891aa3bc2efd08dc8d5ef59faa4d0a7f42abd4',
 'core/PropertyStorageEx.h': '8e2f86af773360c2a7572eb157e7ebf017b7c87db9f4b672ef7644567bf6b890',
 'core/Surface.h': '262cfecd26ac501e12251e7cd6131e40d312f57f49c1b19aed797440da007f63',
 'core/Plane.h': '5ad5165e9de2afcfc2da828e8704c838b1f82e8ead7422fbcfc32c758672e1f3',
 'core/Context.h': '3fea2ed1b3ea85146757f156124e4fbc48dbe51a1f08b13e67cf667a60d98f90',
 'core/Buffer.h': '3e52733b46e168f1b0118381df1ce17eed17353ca89b39c77394f59e254aef67',
 'core/AudioBuffer.h': '74b4bbc22c66520c233f258dba0642c3f4de66ae02d16201c3e224f9238b5d5d',
 'core/Compute.h': '2d8c675431ab1697303b492c8ac94921aaa8fb6e1dae24166b4b9dca0874fbc3',
 'core/ComputeFactory.h': '50acf69fb670bc6cf093384d1b698bf9e44cda8dd49e02ef24eacfb86ad8ccae',
 'components/ComponentCaps.h': '2479eb2a6174a1b63552c509abac1b701eddcab5a5eac5a7f4d9f40274d588b4',
 'components/VideoEncoderHEVC.h': 'a7f0463d583c442ee305840e324d74f8228accb82142f134a1f80cbf34a2e49c',
 'components/ColorSpace.h': '5f6757244c6aac90c613e42e34a94cad2b8fb60c790768ac1e998573e2685a13',
 'components/PreAnalysis.h': '801a197f578104c4481d21f68c3dd6ea8fbe2f9774424277499c5de70d213662'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--amf-include", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--retention-checkout", type=Path)
    parser.add_argument("--compiler", default="c++")
    parser.add_argument("--cross-compiler")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    sdk = args.amf_include.resolve(strict=True)
    for name, expected in SDK_PINS.items():
        path = sdk / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise SystemExit("AMF SDK header differs from reviewed source: " + name)
    output = args.output.absolute()
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    cache = output / "cache"
    cache.mkdir(mode=0o700)
    sources = [repo / "pc/stream-bridge-amf-boundary.h", repo / "pc/stream-bridge-amf-boundary.cpp",
               repo / "tests/stream_bridge_amf_boundary_test.cpp", Path(__file__).resolve()]
    result = {"scope": "Public AMF SDK interface fixture; no AMD GPU, installed Steam hook or image-carriage proof",
              "sdk_commit": SDK_COMMIT, "sdk_pins": SDK_PINS,
              "source_sha256": {str(p.relative_to(repo)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
              "commands": []}
    status = 1
    try:
        flags = ["-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-isystem", str(sdk), "-I", str(repo)]
        jobs = [("compile", [args.compiler] + flags + [str(sources[2]), str(sources[1]), "-o", str(cache / "test-amf")]),
                ("controls", [str(cache / "test-amf")])]
        if args.cross_compiler:
            jobs.append(("win64-compile", [args.cross_compiler] + flags + ["-c", str(sources[1]), "-o", str(cache / "amf-win64.o")]))
        for name, command in jobs:
            process = subprocess.run(command, cwd=repo, text=True, capture_output=True, timeout=60)
            (output / (name + ".stdout")).write_text(process.stdout)
            (output / (name + ".stderr")).write_text(process.stderr)
            result["commands"].append({"name": name, "command": command, "exit": process.returncode})
            if process.returncode:
                raise RuntimeError(name + " failed: " + process.stderr[:2000])
            if name == "controls":
                print(process.stdout, end="")
        status = 0
    finally:
        result["exit"] = status
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        # subprocess.run has reaped every process before completion cleanup.
        cleanup = {"api": "not available in standalone fork", "native_cache_removed": False}
        if args.retention_checkout:
            checkout = args.retention_checkout.resolve(strict=True)
            script = checkout / "scripts/town_run_retention.py"
            spec = importlib.util.spec_from_file_location("town_retention", script)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            cleanup["completion_api"] = module.cleanup_completed_run(
                "steamlink-amf-boundary", status, output / "controls.stdout", checkout, process_reaped=True)
        # The shared API recognizes managed/Unity caches. This native cache is
        # ours alone, under the exclusively created output directory.
        if cache.exists():
            if cache.is_symlink() or cache.parent != output or (cache / ".keep").exists():
                raise RuntimeError("Owned native cache identity changed")
            shutil.rmtree(cache)
            cleanup["native_cache_removed"] = True
        (output / "cleanup.json").write_text(json.dumps(cleanup, indent=2) + "\n")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
