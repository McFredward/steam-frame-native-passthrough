#!/usr/bin/env python3
"""Actual software-HEVC round trip; not a Steam Link or native GPU test."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import shutil


def nals(data):
    # Independent small Annex-B inspector for preservation assertions. It does
    # not import or duplicate the C++ owned-SEI decoder under test.
    starts = []
    i = 0
    while i < len(data) - 2:
        if data[i:i + 4] == b"\x00\x00\x00\x01":
            starts.append((i, 4)); i += 4
        elif data[i:i + 3] == b"\x00\x00\x01":
            starts.append((i, 3)); i += 3
        else:
            i += 1
    result = []
    for index, (start, prefix) in enumerate(starts):
        end = starts[index + 1][0] if index + 1 < len(starts) else len(data)
        if end - start < prefix + 2:
            raise ValueError("Truncated generated NAL")
        result.append((start, end, (data[start + prefix] >> 1) & 63))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cleanup-checkout", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="codec-", dir=args.output.resolve()))
    cache = out / "owned-cache"; cache.mkdir()
    runs = []; code = 1; source = Path(__file__).resolve().parents[1]
    log = out / "run.log"

    def run(name, command, expected=0):
        target = out / (name + ".log")
        with target.open("wb") as sink:
            child = subprocess.Popen([str(x) for x in command], cwd=source,
                                     stdout=sink, stderr=subprocess.STDOUT,
                                     start_new_session=True)
            try:
                result = child.wait(timeout=45)
            except BaseException:
                if child.poll() is None: os.killpg(child.pid, signal.SIGKILL)
                child.wait(); raise
        runs.append({"name": name, "exit_code": result, "expected": expected,
                     "log": target.name})
        if result != expected:
            raise RuntimeError(name + " failed: " + target.read_text(errors="replace")[-4000:])

    try:
        original = out / "original.hevc"
        run("encode", [args.ffmpeg, "-nostdin", "-hide_banner", "-v", "error",
                       "-f", "lavfi", "-i", "testsrc2=size=128x96:rate=3",
                       "-frames:v", "4", "-threads", "1", "-c:v", "libx265",
                       "-preset", "ultrafast", "-x265-params",
                       "aud=1:repeat-headers=1:bframes=0:pools=none:frame-threads=1:keyint=4:log-level=error",
                       "-f", "hevc", original])
        data = original.read_bytes()
        nal_list = nals(data)
        auds = [start for start, _, kind in nal_list if kind == 35]
        assert len(auds) == 4 and auds[0] == 0, "Need four complete generated AUs"
        boundaries = auds + [len(data)]
        wrapped = []
        for index in range(4):
            au = out / f"original-{index}.hevc"
            au.write_bytes(data[boundaries[index]:boundaries[index + 1]])
            # An opaque per-AU payload with hand-checkable bytes and zero runs.
            # Protocol fixtures separately exercise actual HELLO/ACK/COMMIT;
            # a codec payload is deliberately not an authenticated handshake.
            payload = out / f"payload-{index}.bin"
            body = bytes(range(31)) + bytes([index + 1]) + bytes(48)
            assert len(body) == 80
            payload.write_bytes(body)
            coded = out / f"wrapped-{index}.hevc"
            extracted = out / f"extracted-{index}.bin"
            restored = out / f"restored-{index}.hevc"
            run(f"append-{index}", [args.tool, "append", au, payload, coded])
            run(f"inspect-{index}", [args.tool, "inspect", coded, extracted])
            run(f"remove-{index}", [args.tool, "remove", coded, restored])
            assert extracted.read_bytes() == body
            assert restored.read_bytes() == au.read_bytes(), "Original complete AU changed"
            source_nals = [au.read_bytes()[a:b] for a, b, _ in nals(au.read_bytes())]
            # Every original NAL, not just VCL, must survive in order verbatim.
            after_nals = [coded.read_bytes()[a:b] for a, b, _ in nals(coded.read_bytes())]
            original_cursor = 0
            for chunk in after_nals:
                if original_cursor < len(source_nals) and chunk == source_nals[original_cursor]:
                    original_cursor += 1
            assert original_cursor == len(source_nals)
            assert len(after_nals) == len(source_nals) + 1
            refused = cache / f"duplicate-{index}.hevc"
            run(f"duplicate-refusal-{index}", [args.tool, "append", coded, payload, refused], 3)
            assert not refused.exists()
            wrapped.append(coded.read_bytes())
        combined = out / "wrapped.hevc"; combined.write_bytes(b"".join(wrapped))
        for label, image in (("original", original), ("wrapped", combined)):
            run("decode-" + label, [args.ffmpeg, "-nostdin", "-hide_banner", "-v", "error",
                                    "-threads", "1", "-i", image, "-f", "framemd5",
                                    cache / (label + ".md5")])
        def frames(path):
            return [line for line in path.read_text().splitlines() if line and not line.startswith("#")]
        a, b = frames(cache / "original.md5"), frames(cache / "wrapped.md5")
        assert len(a) == len(b) == 4 and a == b, "Decoded frame content/cadence differs"
        (out / "decoded-frame-hashes.txt").write_text("\n".join(a) + "\n")
        code = 0
    except BaseException as error:
        runs.append({"failure": str(error)}); print(str(error), file=sys.stderr)
    finally:
        record = {"scope": "ACTUAL_LOCAL_SOFTWARE_HEVC_ONLY", "exit_code": code,
                  "runs": runs, "steam_link_observed": False, "gpu_or_native_decoder": False,
                  "live_peer_authentication": False, "passthrough_enabled": False}
        record["preserved_evidence"] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                         for p in out.iterdir() if p.is_file() and p.suffix in (".hevc", ".bin")}
        (out / "result.json").write_text(json.dumps(record, indent=2) + "\n")
        log.write_text("All codec/tool children terminated and reaped.\n")
        spec = importlib.util.spec_from_file_location("retention", args.cleanup_checkout / "scripts/town_run_retention.py")
        module = importlib.util.module_from_spec(spec); sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        receipt = module.cleanup_completed_run("stream-bridge-codec", code, log,
                                               args.cleanup_checkout, process_reaped=True)
        shutil.rmtree(cache)
        (out / "cleanup.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print("evidence: " + str(out))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
