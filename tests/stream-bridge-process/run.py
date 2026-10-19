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
    parser.add_argument("--decoded-control", action="store_true")
    parser.add_argument("--handoff-only", action="store_true")
    parser.add_argument("--handoff-control", choices=("early-receipt", "single-packet", "stale-receipt", "camera-receipt", "retired-lease", "unavailable-lease"))
    parser.add_argument("--camera-control", choices=("future-metadata", "early-none", "old-epoch"))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = args.source_root.resolve()
    output = args.output.resolve() if args.output else None
    if output:
        output.mkdir(parents=True, exist_ok=True)
    fixture = Path(__file__).with_name("lifecycle_tests.cpp")
    source = (root / "layer/stream_bridge_process.cpp").read_bytes()
    header = (root / "layer/stream_bridge_process.h").read_bytes()
    if args.causal:
        old = b"if(!start&&state!=local::State::Off){"
        if source.count(old) != 1:
            raise RuntimeError("process consumer gate changed; control needs review")
        source = source.replace(old, b"if(false){")
    if args.decoded_control:
        old = b"if(wantsDecoded&&consumer.decodedCoverage)"
        if source.count(old) != 1:
            raise RuntimeError("decoded dispatch causal anchor changed")
        source = source.replace(old, b"if(false&&wantsDecoded&&consumer.decodedCoverage)")
    controls = {
        "unavailable-lease": (b"p.unavailable[lease.slot]=lease;p.released.notify_all();", b"p.released.notify_all();",
                              "terminal exact unavailable disposition releases native DQ without the emergency deadline"),
        "retired-lease": (b"std::lock_guard<std::mutex> lock(p.gate);p.released.notify_all();", b"(void)p;",
                          "actual worker capture notification releases retired lease without emergency timeout"),
        "early-receipt": (b"if(needsCoverage&&(!any||!all||!p.camera.ready()))continue;", b"(void)any;(void)all;",
                          "active certificate and picture do not acknowledge unpublished exact mask storage"),
        "single-packet": (b"n<native::MaxBuffers*4&&p.channel.receive(message)", b"n<1&&p.channel.receive(message)",
                          "one actual worker cycle drains certificate/capture/certificate/capture burst without artificial 2-ms serialization"),
        "stale-receipt": (b"expected.binding!=message.binding", b"false",
                          "foreign binding/source/frame/capture/kernel and stale epoch receipts cannot expose native DQ"),
        "camera-receipt": (b"||!p.camera.ready()", b"",
                           "published exact GPU mask cannot expose native DQ before camera API acknowledgment"),
    }
    if args.handoff_control:
        old, new, _ = controls[args.handoff_control]
        if source.count(old) != 1:
            raise RuntimeError("handoff causal source anchor changed: " + args.handoff_control)
        source = source.replace(old, new)
    camera_controls = {
        "future-metadata": (b"p.cameraPeer=true;", b"p.cameraPeer=true;p.camera.request(certificate.coverageEncoding!=sb::CoverageEncoding::None);",
                            "future signed None certificate cannot revoke camera from the current exact decoded active picture"),
        "early-none": (b"const bool wanted=p.cameraPeer&&(p.cameraDemand||activePicture);", b"const bool wanted=p.cameraPeer&&p.cameraDemand;",
                       "decoded opaque picture defers global camera withdrawal while an exact active native buffer is still current"),
        "old-epoch": (b"requestCamera(p,false,Flow::CameraLease);p.currentLeases.reset();", b"requestCamera(p,false,Flow::CameraLease);",
                      "actual source epoch loss revokes once and clears the obsolete epoch observation"),
    }
    if args.camera_control:
        old, new, _ = camera_controls[args.camera_control]
        if source.count(old) != 1:
            raise RuntimeError("camera causal source anchor changed: " + args.camera_control)
        source = source.replace(old, new)
    print(json.dumps({"process_sha256": hashlib.sha256(source).hexdigest(),
                      "process_header_sha256": hashlib.sha256(header).hexdigest(),
                      "causal": args.causal, "decoded_control": args.decoded_control, "handoff_control": args.handoff_control, "camera_control": args.camera_control,
                      "sanitized": args.sanitized}), flush=True)
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
                   "stream_bridge_native_images", "stream_bridge_local", "stream_bridge_camera_client")]
        libraries = [args.libraries / ("libsfnp_stream_bridge" + name + ".a") for name in ("_control", "")]
        command = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-pthread", *flags,
                   "-I" + str(root), "-I" + str(root / "layer"),
                   '-DSFNP_PROCESS_SOURCE="' + str(implementation) + '"', str(fixture), *map(str, native),
                   "-Wl,--start-group", *map(str, libraries), "-Wl,--end-group", "-ldl", "-o", str(executable)]
        if output:
            with (output / "build.log").open("w") as log:
                subprocess.run(command, check=True, timeout=120, stdout=log, stderr=subprocess.STDOUT)
        else:
            subprocess.run(command, check=True, timeout=120)
        result = subprocess.run([str(executable)] + (["handoff"] if args.handoff_only else []), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30)
        print(result.stdout, end="", flush=True)
        if output:
            (output / "test.log").write_text(result.stdout)
            (output / "results.json").write_text(json.dumps({
                "process_sha256": hashlib.sha256(source).hexdigest(),
                "process_header_sha256": hashlib.sha256(header).hexdigest(),
                "fixture_sha256": hashlib.sha256(fixture.read_bytes()).hexdigest(),
                "local_transport_sha256": hashlib.sha256((root / "receiver/stream_bridge_local.cpp").read_bytes()).hexdigest(),
                "native_runtime_sha256": hashlib.sha256((root / "receiver/stream_bridge_native_runtime.cpp").read_bytes()).hexdigest(),
                "native_runtime_header_sha256": hashlib.sha256((root / "receiver/stream_bridge_native_runtime.h").read_bytes()).hexdigest(),
                "sanitized": args.sanitized, "handoff_only": args.handoff_only,
                "causal": args.causal, "decoded_control": args.decoded_control, "handoff_control": args.handoff_control, "camera_control": args.camera_control,
                "exit_status": result.returncode,
                "scope": "Actual Process source, authenticated Unix channels, AU signing/inspection and shared exact leases; final GPU consumer publication is an explicit test port. Not a Frame picture test.",
                "cache": "TemporaryDirectory removes owned executables after child processes are reaped.",
            }, indent=2) + "\n")
        if args.causal:
            if result.returncode != 1 or "last consumer disappearance immediately revokes old picture connection" not in result.stdout:
                raise RuntimeError("missing-consumer causal mutation was not caught at intended assertion")
        elif args.decoded_control:
            if result.returncode != 1 or "authenticated compositor expands once and dispatches decoded consumers" not in result.stdout:
                raise RuntimeError("decoded reuse causal mutation was not caught at intended assertion")
        elif args.handoff_control:
            if result.returncode != 1 or controls[args.handoff_control][2] not in result.stdout:
                raise RuntimeError("handoff causal mutation was not caught at intended assertion: " + args.handoff_control)
        elif args.camera_control:
            if result.returncode != 1 or camera_controls[args.camera_control][2] not in result.stdout:
                raise RuntimeError("camera causal mutation was not caught at intended assertion: " + args.camera_control)
        elif result.returncode:
            raise RuntimeError("actual process lifecycle fixture failed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
