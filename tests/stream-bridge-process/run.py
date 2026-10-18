#!/usr/bin/env python3
"""Exercise the actual Process worker cycle without starting its worker thread."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--libraries", type=Path, required=True)
    parser.add_argument("--sanitized", action="store_true")
    parser.add_argument("--causal", action="store_true")
    args = parser.parse_args()
    root = args.source_root.resolve()
    fixture = Path(__file__).with_name("lifecycle_tests.cpp")
    source = (root / "layer/stream_bridge_process.cpp").read_bytes()
    header = (root / "layer/stream_bridge_process.h").read_bytes()
    if args.causal:
        old = b"if(!start&&state!=local::State::Off){"
        if source.count(old) != 1:
            raise RuntimeError("process consumer gate changed; control needs review")
        source = source.replace(old, b"if(false){")
    print(json.dumps({"process_sha256": hashlib.sha256(source).hexdigest(),
                      "process_header_sha256": hashlib.sha256(header).hexdigest(),
                      "causal": args.causal, "sanitized": args.sanitized}), flush=True)
    with tempfile.TemporaryDirectory(prefix="sfnp-process-fixture-") as directory:
        temp = Path(directory)
        (temp / "layer").mkdir()
        implementation = temp / "layer/stream_bridge_process.cpp"
        implementation.write_bytes(source)
        (temp / "layer/stream_bridge_process.h").write_bytes(header)
        for name in ("receiver", "bridge"):
            (temp / name).symlink_to(root / name, target_is_directory=True)
        executable = temp / "process-fixture"
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitized else []
        native = [root / ("receiver/" + name + ".cpp") for name in
                  ("stream_bridge_native_decode", "stream_bridge_native_loader", "stream_bridge_native_runtime",
                   "stream_bridge_native_images", "stream_bridge_local")]
        libraries = [args.libraries / ("libsfnp_stream_bridge" + name + ".a") for name in ("_control", "")]
        command = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-pthread", *flags,
                   "-I" + str(root), "-I" + str(root / "layer"),
                   '-DSFNP_PROCESS_SOURCE="' + str(implementation) + '"', str(fixture), *map(str, native),
                   "-Wl,--start-group", *map(str, libraries), "-Wl,--end-group", "-ldl", "-o", str(executable)]
        subprocess.run(command, check=True, timeout=120)
        result = subprocess.run([str(executable)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30)
        print(result.stdout, end="", flush=True)
        if args.causal:
            if result.returncode != 1 or "last consumer disappearance immediately revokes old picture connection" not in result.stdout:
                raise RuntimeError("missing-consumer causal mutation was not caught at intended assertion")
        elif result.returncode:
            raise RuntimeError("actual process lifecycle fixture failed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
