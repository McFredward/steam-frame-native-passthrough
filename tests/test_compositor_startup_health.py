"""Injected startup evidence and local proc/log fixtures; no Frame access."""
import copy
import datetime
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location("startup_health_fixture", Path(__file__).resolve().parents[1] / "tools/compositor_startup_health.py")
M = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(M)


def unit(invocation="b" * 32):
    return {"InvocationID": invocation, "ActiveState": "active", "SubState": "running", "MainPID": "100",
        "ControlGroup": "/user.slice/user-1000.slice/user@1000.service/app.slice/steamvr.service",
        "NRestarts": "0", "ExecMainStartTimestampMonotonic": "1000000"}


def process(pid, kind):
    return {"pid": pid, "start_ticks": pid * 100, "state": "S", "exe": M.COMPOSITOR if kind == "compositor" else M.WEBHELPER,
            "kind": kind, "libcef": kind != "compositor"}


def baseline():
    return {"schema": M.SCHEMA, "unit": unit("a" * 32), "processes": [process(10, "compositor")],
            "monotonic": 1.0, "wall_time": 100.0, "cursor": "s=old", "log_offsets": {}}


def line(message, wall=101):
    date = datetime.datetime.fromtimestamp(wall).strftime("%a %b %d %Y %H:%M:%S.%f")
    return date + " [Info] - " + message


def sample(now=2.0):
    return {"unit": unit(), "monotonic": now, "errors": [], "journal": [],
        "main_process": {"pid": 100, "start_ticks": 10000, "state": "S"},
        "processes": [process(101, "compositor"), process(102, "browser"), process(103, "zygote"), process(104, "renderer")],
        "logs": {"vrcompositor": "\n".join([line("vrcompositor 2.18.2 startup with PID=101, config=/native"),
            line("Done initializing CGraphicsDevice resources"), line("Compositor render thread started")]),
            "vrserver": "\n".join([line("New Connect message from " + M.WEBHELPER + " (VRApplication_WebHelper) (Args: -lang=en_us) 102 "),
                line("SetApplicationPid appkey=system.systemui pid=103, external transition")])}}


def coredump(pid="101"):
    # Actual Frame journal shape: the coredump logger has its own invocation,
    # and MESSAGE exceeded journalctl's default 4096-byte JSON field threshold.
    return {"MESSAGE": None, "_PID": "2700709", "__MONOTONIC_TIMESTAMP": "2000000",
        "_SYSTEMD_INVOCATION_ID": "c" * 32, "SYSLOG_IDENTIFIER": "systemd-coredump",
        "MESSAGE_ID": M.COREDUMP_MESSAGE_ID, "COREDUMP_PID": pid,
        "COREDUMP_EXE": M.COMPOSITOR, "COREDUMP_SIGNAL": "11", "COREDUMP_USER_UNIT": M.UNIT,
        "COREDUMP_CGROUP": unit()["ControlGroup"]}


def server(pid, *, start_ticks=None, group=None):
    return {"pid": pid, "start_ticks": pid * 100 if start_ticks is None else start_ticks, "state": "S",
        "exe": M.SERVER, "kind": "server", "libcef": False, "comm": "vrserver",
        "control_group": unit()["ControlGroup"] if group is None else group, "ui_compositor_tasks": None}


def forked_sample(now=2.0):
    s = sample(now)
    s["processes"] = [p for p in s["processes"] if p["kind"] != "renderer"]
    p = s["processes"][2]
    # Captured normal native systemui has these distinct tasks while proc argv
    # still says --type=zygote. Fixture start ticks remain injected test values.
    p["ui_compositor_tasks"] = [{"tid": 2702896 + index, "start_ticks": p["start_ticks"] + index,
        "state": "S", "comm": name} for index, name in enumerate(("Chrome_ChildIOT", "GpuMemoryThread", "Compositor"))]
    s["logs"]["vrserver"] += "\n" + line("New Connect message from " + M.WEBHELPER +
        " (VRApplication_WebHelper) (Args: --type=zygote --no-sandbox) 103 ")
    return s


def descriptor():
    result = {"schema": M.ISOLATION_SCHEMA, "target_exe": M.COMPOSITOR,
        "selector_path": "/reviewed/selector.so", "payload_path": "/reviewed/observer.so", "audit_path": "/reviewed/audit.so"}
    result["target_exec_environment"] = {"LD_PRELOAD": result["selector_path"] + " " + result["payload_path"],
        "LD_AUDIT": result["audit_path"], "GHVR_FRAME_COMPOSITOR_RAW_ENVELOPE": "/reviewed/envelope.bin",
        "GHVR_FRAME_COMPOSITOR_RAW_OUTPUT": "/reviewed/raw.jsonl", "FRAME_MASK_TRACE": "1",
        "FRAME_MASK_TRACE_BINDINGS": "1", "FRAME_MASK_TRACE_OUTPUT": "/reviewed/trace.jsonl"}
    return result


def isolated_report():
    config = descriptor()
    rows = []
    for p in sample()["processes"]:
        target = p["exe"] == M.COMPOSITOR
        rows.append({key: p[key] for key in ("pid", "start_ticks", "state", "exe")})
        rows[-1].update(ppid=100, selector_mapped=True, payload_mapped=target, audit_mapped=target,
            owned_mapping_deleted=False, loader_payload=target, loader_audit=target,
            diagnostic_keys=sorted(set(config["target_exec_environment"]) - {"LD_PRELOAD", "LD_AUDIT"}) if target else [],
            target_initial_environment_matches=True if target else None)
    return {"schema": M.ISOLATION_SCHEMA, "descriptor": config, "invocation": unit()["InvocationID"],
        "control_group": unit()["ControlGroup"], "status": "isolated", "reason": None, "processes": rows,
        "errors": [], "inventory_stable": True, "unit_stable": True, "target_env_view": "initial-exec-only",
        "runtime_environ_verified": False, "metadata_usage": {"samples": 1, "processes": 4, "metadata_bytes": 4000}}


def rejudge(report):
    report["status"], report["reason"] = M.isolation_verdict(report["processes"], report["errors"],
        inventory_stable=report["inventory_stable"], unit_stable=report["unit_stable"])
    return report


class Source:
    def __init__(self, samples): self.samples = samples; self.now = 2.0; self.calls = 0
    def sample(self):
        self.calls += 1
        item = copy.deepcopy(self.samples[min(self.calls - 1, len(self.samples) - 1)])
        if isinstance(item, Exception): raise item
        item["monotonic"] = self.now
        return item
    def clock(self): return self.now
    def sleep(self, seconds): self.now += seconds
    def checkpoint(self): return {"fixture": True}


class JournalMessageTests(unittest.TestCase):
    def test_all_binary_bytes_are_preserved_without_utf8_assumptions(self):
        raw = list(range(256))
        self.assertEqual(M.journal_message(raw).encode("latin-1"), bytes(raw))

    def test_string_semantics_and_exact_message_bounds_are_preserved(self):
        for value in ("", "\x1b[93mReady\x1b[m", "\u00e4" * M.JOURNAL_MESSAGE_LIMIT):
            with self.subTest(value_length=len(value)):
                self.assertEqual(M.journal_message(value), value)
        self.assertEqual(M.journal_message([65] * M.JOURNAL_MESSAGE_LIMIT), "A" * M.JOURNAL_MESSAGE_LIMIT)
        self.assertEqual(M.journal_message([]), "")
        for value in ("x" * (M.JOURNAL_MESSAGE_LIMIT + 1), [65] * (M.JOURNAL_MESSAGE_LIMIT + 1)):
            with self.subTest(type=type(value).__name__), self.assertRaises(RuntimeError):
                M.journal_message(value)

    def test_unknown_encodings_and_non_byte_array_elements_are_refused(self):
        for value in (None, 1, True, {}, {"bytes": [65]}, (65,), b"A",
                      ["A"], [[65]], [65, True], [65, False], [65, -1], [65, 256],
                      [65, 1.0], [65, None], [65, {}]):
            with self.subTest(value=value), self.assertRaises(RuntimeError):
                M.journal_message(value)


class BoundedCommandTests(unittest.TestCase):
    def run_python(self, source, *, timeout=5):
        return M.bounded_command([sys.executable, "-c", source], check=True,
            capture_output=True, text=True, timeout=timeout)

    def test_complete_output_at_exact_joint_byte_limit_is_preserved(self):
        result = self.run_python("import os; os.write(1, b'x' * " + str(M.LIMIT - 4) + "); os.write(2, b'last')")
        self.assertEqual(len(result.stdout.encode()), M.LIMIT - 4)
        self.assertEqual(result.stderr, "last")

    def test_joint_overflow_stops_and_reaps_actual_child(self):
        children = []
        popen = subprocess.Popen
        def tracked(*args, **kwargs):
            child = popen(*args, **kwargs); children.append(child); return child
        with mock.patch.object(M.subprocess, "Popen", side_effect=tracked):
            with self.assertRaisesRegex(RuntimeError, "output bound exceeded"):
                self.run_python("import os,time; os.write(1, b'x' * " + str(M.LIMIT) + "); os.write(2, b'y'); time.sleep(30)")
        self.assertEqual(len(children), 1)
        self.assertIsNotNone(children[0].poll())

    def test_timeout_stops_and_reaps_actual_child(self):
        children = []
        popen = subprocess.Popen
        def tracked(*args, **kwargs):
            child = popen(*args, **kwargs); children.append(child); return child
        with mock.patch.object(M.subprocess, "Popen", side_effect=tracked):
            with self.assertRaises(subprocess.TimeoutExpired):
                self.run_python("import time; time.sleep(30)", timeout=0.1)
        self.assertIsNotNone(children[0].poll())

    def test_nonzero_utf8_and_multibyte_limits_are_not_silent_success(self):
        with self.assertRaises(subprocess.CalledProcessError): self.run_python("raise SystemExit(7)")
        with self.assertRaises(UnicodeDecodeError): self.run_python("import os; os.write(1, b'\\xff')")
        with self.assertRaisesRegex(RuntimeError, "output bound exceeded"):
            self.run_python("import os; os.write(1, '\\u00e4'.encode() * " + str(M.LIMIT // 2 + 1) + ")")

    def test_default_native_source_uses_incremental_bound(self):
        source = M.NativeEvidenceSource("/unused")
        self.assertIs(source.run, M.bounded_command)
        with self.assertRaisesRegex(RuntimeError, "output bound exceeded"):
            source.command([sys.executable, "-c", "import os; os.write(1, b'x' * " + str(M.LIMIT + 1) + ")"])


class GateTests(unittest.TestCase):
    def test_real_forking_unit_materializes_main_identity_without_restart(self):
        invocation = "6db652c4220148b8bcee29555cab6245"
        early = sample(); early["unit"].update(InvocationID=invocation, MainPID="0",
            ExecMainStartTimestampMonotonic="0", ActiveState="activating", SubState="start")
        early["main_process"] = None; early["processes"] = early["processes"][:1]
        gate = M.StartupGate(baseline())
        self.assertEqual(gate.consume(early, stable_seconds=0)["status"], "not-ready")
        self.assertEqual(set(gate.pinned), {"compositor"})
        # Exact unit values from the retained startup/recheck, with injected
        # process start ticks and UI logs; this does not inspect the device.
        final = sample(3); final["unit"].update(InvocationID=invocation, MainPID="1028030",
            ExecMainStartTimestampMonotonic="34789695511")
        final["main_process"]["pid"] = 1028030
        result = gate.consume(final, stable_seconds=0)
        self.assertTrue(result["ready"])
        self.assertEqual(gate.unit_identity, (invocation, "1028030", "34789695511", "0"))
        self.assertFalse(result["automatic_restart"])

    def test_forking_identity_fields_can_materialize_in_either_order(self):
        sequences = ((("0","0"),("100","0"),("100","1000000")),
                     (("0","0"),("0","1000000"),("100","1000000")),
                     (("100","0"),("100","1000000")),
                     (("0","1000000"),("100","1000000")))
        for sequence in sequences:
            gate = M.StartupGate(baseline())
            for index, (pid, start) in enumerate(sequence):
                current = sample(2+index)
                current["unit"].update(MainPID=pid, ExecMainStartTimestampMonotonic=start)
                if pid == "0": current["main_process"] = None
                with self.subTest(sequence=sequence, index=index):
                    self.assertEqual(gate.consume(current)["status"], "not-ready")
                    if pid == "0" or start == "0": self.assertIsNone(gate.ready_since)
            # Stable timing begins only when both actual fields and roles exist;
            # no new grace period or sleep is added for zero-field publication.
            final = sample(2+len(sequence)+.1); final["logs"] = {}
            self.assertTrue(gate.consume(final)["ready"])

    def test_zero_startstamp_never_proves_ready_even_with_complete_live_roles(self):
        current = sample(); current["unit"]["ExecMainStartTimestampMonotonic"] = "0"
        gate = M.StartupGate(baseline())
        self.assertEqual(gate.consume(current, stable_seconds=0)["status"], "not-ready")
        self.assertIsNone(gate.ready_since)
        self.assertEqual(set(gate.pinned), {"service_main","compositor","ui_browser","system_ui","ui_renderer"})

    def test_unknown_forking_identity_roundtrips_checkpoint_without_losing_pins(self):
        for pid, start in (("0","0"),("100","0"),("0","1000000")):
            early = sample(); early["unit"].update(MainPID=pid, ExecMainStartTimestampMonotonic=start)
            if pid == "0": early["main_process"] = None
            gate = M.StartupGate(baseline()); gate.consume(early, stable_seconds=0)
            before = copy.deepcopy(gate.pinned)
            resumed = M.StartupGate.resume(json.loads(json.dumps(gate.checkpoint())))
            result = resumed.consume(sample(3), stable_seconds=0)
            with self.subTest(pid=pid, start=start):
                self.assertTrue(result["ready"])
                for role, pin in before.items(): self.assertEqual(resumed.pinned[role], pin)

    def test_materialization_does_not_admit_unit_restart_or_known_field_change(self):
        cases = (("0","0", {"InvocationID":"c"*32}),
                 ("0","0", {"NRestarts":"1"}),
                 ("100","0", {"MainPID":"200"}),
                 ("100","0", {"MainPID":"0"}),
                 ("0","1000000", {"ExecMainStartTimestampMonotonic":"2000000"}),
                 ("0","1000000", {"ExecMainStartTimestampMonotonic":"0"}))
        for pid, start, change in cases:
            early = sample(); early["unit"].update(MainPID=pid, ExecMainStartTimestampMonotonic=start)
            if pid == "0": early["main_process"] = None
            gate = M.StartupGate(baseline()); self.assertEqual(gate.consume(early)["status"],"not-ready")
            later = sample(3); later["unit"].update(change)
            if "MainPID" in change: later["main_process"] = None if change["MainPID"] == "0" else {"pid":200,"start_ticks":20000,"state":"S"}
            with self.subTest(pid=pid, start=start, change=change):
                result = gate.consume(later, stable_seconds=0)
                self.assertEqual(result["status"], "fault")
                self.assertIn("restarted", result["reason"])
                self.assertEqual(gate.consume(sample(4), stable_seconds=0)["status"], "fault")

    def test_materialization_never_replaces_or_discards_pinned_compositor(self):
        for change in ("exit", "replace"):
            early = sample(); early["unit"].update(MainPID="0", ExecMainStartTimestampMonotonic="0")
            early["main_process"] = None
            gate = M.StartupGate(baseline()); self.assertEqual(gate.consume(early)["status"], "not-ready")
            later = sample(3)
            if change == "exit": later["processes"].pop(0)
            else: later["processes"][0]["start_ticks"] += 1
            with self.subTest(change=change):
                self.assertEqual(gate.consume(later, stable_seconds=0)["status"], "fault")

    def test_ready_checkpoint_cannot_contain_unknown_or_untyped_unit_identity(self):
        gate = M.StartupGate(baseline()); self.assertTrue(gate.consume(sample(), stable_seconds=0)["ready"])
        for index, value in ((1,"0"),(2,"0"),(1,100),(2,None),(3,False),(3,"unknown")):
            saved = copy.deepcopy(gate.checkpoint()); identity = list(saved["unit_identity"])
            identity[index] = value; saved["unit_identity"] = identity
            with self.subTest(index=index, value=value), self.assertRaises(RuntimeError): M.StartupGate.resume(saved)

    def test_missing_actual_main_process_cannot_pass_service_active(self):
        s = sample(); s["main_process"] = None
        self.assertFalse(M.StartupGate(baseline()).consume(s, stable_seconds=0)["ready"])

    def test_process_duplicate_bad_types_and_nan_are_rejected(self):
        for variant in ("duplicate", "bool_pid", "bad_kind", "nan", "huge_clock"):
            s = sample()
            if variant == "duplicate": s["processes"].append(copy.deepcopy(s["processes"][0]))
            if variant == "bool_pid": s["processes"][0]["pid"] = True
            if variant == "bad_kind": s["processes"][0]["kind"] = {}
            if variant == "nan": s["monotonic"] = float("nan")
            if variant == "huge_clock": s["monotonic"] = 2**1024
            with self.subTest(variant=variant): self.assertEqual(M.StartupGate(baseline()).consume(s)["status"], "fault")

    def test_corrupt_ready_checkpoints_never_bypass_health_sampling(self):
        gate = M.StartupGate(baseline()); gate.consume(sample(), stable_seconds=0)
        for key, value in (("markers", {}), ("pinned", {}), ("unit_identity", None),
                           ("ready", 1), ("last_progress", float("nan")), ("invocation", "foreign")):
            saved = copy.deepcopy(gate.checkpoint()); saved[key] = value
            with self.subTest(key=key), self.assertRaises(RuntimeError): M.StartupGate.resume(saved)

    def test_failed_or_inactive_baseline_still_permits_fresh_recovery_start(self):
        b = baseline(); b["unit"].update(InvocationID="", ActiveState="failed", SubState="failed", ControlGroup="", MainPID="0")
        self.assertTrue(M.StartupGate(b).consume(sample(), stable_seconds=0)["ready"])

    def test_valid_startup_requires_positive_ui_and_stable_live_processes(self):
        gate = M.StartupGate(baseline()); first = gate.consume(sample())
        self.assertFalse(first["ready"]); self.assertEqual(first["status"], "not-ready")
        s = sample(3.1); s["logs"] = {}; result = gate.consume(s)
        self.assertTrue(result["ready"]); self.assertFalse(result["screen_pixels_verified"])
        self.assertEqual(set(result["process_identities"]), {"service_main", "compositor", "ui_browser", "system_ui", "ui_renderer"})

    def test_active_compositor_with_actual_static_tls_ui_failure_is_rejected(self):
        gate = M.StartupGate(baseline()); s = sample(); s["processes"] = s["processes"][:1]
        s["journal"] = [{"MESSAGE": "./vrwebhelper: error while loading shared libraries: " + M.LIBCEF + ": cannot allocate memory in static TLS block",
            "_PID": "368028", "_SYSTEMD_INVOCATION_ID": "b" * 32, "__MONOTONIC_TIMESTAMP": "2000000"}]
        result = gate.consume(s); self.assertEqual(result["status"], "fault")
        self.assertIn("static TLS", result["reason"])
        self.assertEqual(gate.consume(sample(4))["status"], "fault")

    def test_active_service_and_compositor_prepare_are_not_ui_readiness(self):
        s = sample(); s["processes"] = s["processes"][:1]; s["logs"]["vrserver"] = "rawPrepare succeeded"
        self.assertFalse(M.StartupGate(baseline()).consume(s, stable_seconds=0)["ready"])

    def test_known_gamescope_and_input_warning_are_not_fatal_alone(self):
        s = sample(); s["journal"] = [{"MESSAGE": "[Gamescope WSI] Failed to connect to gamescope socket",
            "_PID": "102", "__MONOTONIC_TIMESTAMP": "2000000"}]
        s["logs"]["vrserver"] += "\n" + line("[Error] - [Input] LoadActionManifest failed. Could not find action manifest for vrwebhelper_systemui")
        self.assertTrue(M.StartupGate(baseline()).consume(s, stable_seconds=0)["ready"])

    def test_actual_shaped_ansi_binary_journal_warning_is_not_a_startup_fault(self):
        # Same systemd byte-array shape and text as the captured Frame warning.
        message = (b'\x1b[0;93m2026-10-14 18:37:44.453260815 [W:onnxruntime:, device_discovery.cc:211 '
            b'DiscoverDevicesForPlatform] GPU device discovery failed: device_discovery.cc:91 '
            b'ReadFileContents Failed to open file: "/sys/class/drm/card0/device/vendor"\x1b[m')
        s = sample(); s["journal"] = [{"MESSAGE": list(message), "_PID": "102",
            "_SYSTEMD_INVOCATION_ID": "b" * 32, "__MONOTONIC_TIMESTAMP": "2000000"}]
        result = M.StartupGate(baseline()).consume(s, stable_seconds=0)
        self.assertTrue(result["ready"])
        self.assertFalse(result["screen_pixels_verified"])

    def test_binary_journal_fatal_evidence_is_scanned_and_stays_failed(self):
        for message in (b"./vrwebhelper: error while loading shared libraries: " + M.LIBCEF.encode() +
                        b": cannot allocate memory in static TLS block",
                        b"VK_ERROR_DEVICE_LOST", b"symbol lookup error", b"renderer process crashed"):
            s = sample(); s["journal"] = [{"MESSAGE": list(b"\xff\x00\x1b[93m" + message + b"\x1b[m"),
                "_PID": "102", "_SYSTEMD_USER_INVOCATION_ID": "b" * 32,
                "__MONOTONIC_TIMESTAMP": "2000000"}]
            gate = M.StartupGate(baseline())
            with self.subTest(message=message):
                result = gate.consume(s, stable_seconds=0)
                self.assertEqual(result["status"], "fault")
                self.assertIn("Fresh startup failure:", result["reason"])
                self.assertIn(message.decode(), result["reason"])
                self.assertEqual(gate.consume(sample(4))["status"], "fault")

    def test_ansi_control_codes_cannot_hide_fatal_text_in_either_encoding(self):
        for text in ("\x1b[93mVK_ERROR_DEVICE_LOST\x1b[m", "cannot allocate memory in \x1b[93mstatic TLS block\x1b[m"):
            for message in (text, list(text.encode())):
                s = sample(); s["journal"] = [{"MESSAGE": message, "_PID": "102",
                    "__MONOTONIC_TIMESTAMP": "2000000"}]
                with self.subTest(text=text, encoding=type(message).__name__):
                    result = M.StartupGate(baseline()).consume(s)
                    self.assertEqual(result["status"], "fault")
                    self.assertEqual(result["reason"], "Fresh startup failure: " + text)

    def test_malformed_or_oversize_binary_messages_are_not_ignored_or_truncated(self):
        for message in ([65, True], [65, -1], [65, 256], [65, 1.0], [65, None],
                        [[65]], ["cannot allocate memory in static TLS block"],
                        [65] * M.JOURNAL_MESSAGE_LIMIT + list(b"VK_ERROR_DEVICE_LOST"),
                        {"bytes": [65]}):
            s = sample(); s["journal"] = [{"MESSAGE": message, "_PID": "102",
                "_SYSTEMD_INVOCATION_ID": "b" * 32, "__MONOTONIC_TIMESTAMP": "2000000"}]
            gate = M.StartupGate(baseline())
            with self.subTest(type=type(message).__name__, length=len(message)):
                result = gate.consume(s, stable_seconds=0)
                self.assertEqual(result["status"], "fault")
                self.assertEqual(result["reason"], "Unsupported current journal evidence")
                self.assertEqual(gate.consume(sample(4))["status"], "fault")

    def test_binary_journal_keeps_invocation_timestamp_and_old_pid_filters(self):
        for updates in ({"_SYSTEMD_INVOCATION_ID": "a" * 32}, {"_PID": "10"},
                        {"__MONOTONIC_TIMESTAMP": "999999"}, {"MESSAGE": list(b"Process 10 (vrcompositor) core dumped")}):
            s = sample(); row = {"MESSAGE": list(b"segmentation fault core dumped"),
                "_PID": "102", "__MONOTONIC_TIMESTAMP": "2000000"}; row.update(updates)
            s["journal"] = [row]
            with self.subTest(updates=updates):
                self.assertTrue(M.StartupGate(baseline()).consume(s, stable_seconds=0)["ready"])
        s = sample(); s["journal"] = [{"MESSAGE": list(b"cannot allocate memory in static TLS block"),
            "_PID": "10", "_SYSTEMD_INVOCATION_ID": "b" * 32, "__MONOTONIC_TIMESTAMP": "2000000"}]
        self.assertEqual(M.StartupGate(baseline()).consume(s, stable_seconds=0)["status"], "fault")

    def test_binary_journal_does_not_relax_required_metadata_or_row_bounds(self):
        for updates in ({"_PID": [102]}, {"_PID": "102\n"}, {"__MONOTONIC_TIMESTAMP": True},
                        {"__MONOTONIC_TIMESTAMP": "2.0"}):
            s = sample(); row = {"MESSAGE": [65], "_PID": "102", "__MONOTONIC_TIMESTAMP": "2000000"}
            row.update(updates); s["journal"] = [row]
            with self.subTest(updates=updates):
                self.assertEqual(M.StartupGate(baseline()).consume(s)["status"], "fault")
        s = sample(); s["journal"] = [[65]]
        self.assertEqual(M.StartupGate(baseline()).consume(s)["status"], "fault")
        s["journal"] = [{"MESSAGE": [65], "_PID": "102", "__MONOTONIC_TIMESTAMP": "2000000"}] * 512
        result = M.StartupGate(baseline()).consume(s)
        self.assertEqual(result["status"], "fault")
        self.assertEqual(result["reason"], "Invalid startup journal bound")

    def test_old_invocation_pid_or_pre_baseline_journal_errors_are_not_current(self):
        for updates in ({"_SYSTEMD_INVOCATION_ID": "a" * 32}, {"_PID": "10"}, {"__MONOTONIC_TIMESTAMP": "999999"}):
            s = sample(); row = {"MESSAGE": "segmentation fault core dumped", "_PID": "102", "__MONOTONIC_TIMESTAMP": "2000000"}; row.update(updates)
            s["journal"] = [row]
            with self.subTest(updates=updates): self.assertTrue(M.StartupGate(baseline()).consume(s, stable_seconds=0)["ready"])

    def test_old_core_exit_report_from_manager_is_not_new_ui_crash(self):
        s = sample(); s["journal"] = [{"MESSAGE": "Process 10 (vrcompositor) core dumped", "_PID": "1592", "__MONOTONIC_TIMESTAMP": "2000000"}]
        self.assertTrue(M.StartupGate(baseline()).consume(s, stable_seconds=0)["ready"])

    def test_current_invocation_fault_is_not_hidden_by_reused_old_pid(self):
        s = sample(); s["journal"] = [{"MESSAGE": "cannot allocate memory in static TLS block",
            "_PID": "10", "_SYSTEMD_USER_INVOCATION_ID": "b" * 32,
            "__MONOTONIC_TIMESTAMP": "2000000"}]
        result = M.StartupGate(baseline()).consume(s, stable_seconds=0)
        self.assertEqual(result["status"], "fault")
        self.assertIn("static TLS", result["reason"])

    def test_current_structured_coredump_is_fatal_with_null_or_oversize_message(self):
        for message in (None, "x" * (M.JOURNAL_MESSAGE_LIMIT + 1), "no fatal words", list(b"renderer crash")):
            s = sample(); row = coredump(); row["MESSAGE"] = message; s["journal"] = [row]
            with self.subTest(message_type=type(message).__name__):
                gate = M.StartupGate(baseline()); result = gate.consume(s, stable_seconds=0)
                self.assertEqual(result["status"], "fault")
                self.assertEqual(result["reason"], "Fresh startup coredump: pid=101 signal=11 exe=" + M.COMPOSITOR)
                self.assertEqual(gate.consume(sample(4))["status"], "fault")

    def test_delayed_baseline_coredump_null_message_does_not_poison_restoration(self):
        s = sample(); s["journal"] = [coredump("10")]
        self.assertTrue(M.StartupGate(baseline()).consume(s, stable_seconds=0)["ready"])

    def test_current_crashed_pid_reuse_and_old_logger_pid_are_not_hidden(self):
        b = baseline(); b["processes"] = [process(101, "compositor")]; b["processes"][0]["start_ticks"] = 1
        s = sample(); row = coredump(); row["_PID"] = "101"; s["journal"] = [row]
        self.assertEqual(M.StartupGate(b).consume(s)["status"], "fault")
        b = baseline(); b["processes"] += [process(100, "browser")]
        s = sample(); s["journal"] = [coredump("100")]
        self.assertEqual(M.StartupGate(b).consume(s)["status"], "fault")

    def test_structured_core_scope_and_identity_are_required_before_old_pid_skip(self):
        for updates in ({"COREDUMP_PID": True}, {"COREDUMP_PID": "0"}, {"COREDUMP_PID": "2147483648"},
                        {"COREDUMP_SIGNAL": "0"}, {"COREDUMP_SIGNAL": "65"}, {"COREDUMP_SIGNAL": [11]},
                        {"COREDUMP_USER_UNIT": None}, {"COREDUMP_CGROUP": None},
                        {"COREDUMP_EXE": "../vrcompositor"}, {"MESSAGE_ID": "unknown"},
                        {"SYSLOG_IDENTIFIER": "foreign"}):
            s = sample(); row = coredump("10"); row.update(updates); s["journal"] = [row]
            with self.subTest(updates=updates):
                result = M.StartupGate(baseline()).consume(s)
                self.assertEqual(result["status"], "fault")
                self.assertIn("structured startup coredump", result["reason"])
        for updates in ({"COREDUMP_USER_UNIT": "other.service"}, {"COREDUMP_CGROUP": "/other.service"}):
            s = sample(); row = coredump("10"); row.update(updates); s["journal"] = [row]
            with self.subTest(updates=updates):
                result = M.StartupGate(baseline()).consume(s)
                self.assertEqual(result["status"], "fault")
                self.assertIn("scope disagrees", result["reason"])

    def test_prior_or_proven_unrelated_core_is_not_current_ui_crash(self):
        for updates in ({"__MONOTONIC_TIMESTAMP": "999999"},
                        {"COREDUMP_USER_UNIT": "other.service", "COREDUMP_CGROUP": "/other.service"}):
            s = sample(); row = coredump(); row.update(updates); s["journal"] = [row]
            with self.subTest(updates=updates):
                self.assertTrue(M.StartupGate(baseline()).consume(s, stable_seconds=0)["ready"])

    def test_ordinary_current_null_message_is_not_replaced_with_empty(self):
        s = sample(); s["journal"] = [{"MESSAGE": None, "_PID": "102", "__MONOTONIC_TIMESTAMP": "2000000"}]
        self.assertEqual(M.StartupGate(baseline()).consume(s)["reason"], "Unsupported current journal evidence")
        for updates in ({"_SYSTEMD_INVOCATION_ID": "a" * 32}, {"__MONOTONIC_TIMESTAMP": "999999"}):
            s["journal"][0].update(updates)
            self.assertTrue(M.StartupGate(baseline()).consume(s, stable_seconds=0)["ready"])

    def test_stale_rotated_native_readiness_and_error_lines_are_not_admitted(self):
        s = sample()
        s["logs"] = {key: "\n".join(line(row.split("[Info] - ", 1)[1], wall=99) for row in value.splitlines()) for key, value in s["logs"].items()}
        s["logs"]["vrserver"] += "\n" + line("error while loading shared libraries", wall=99)
        result = M.StartupGate(baseline()).consume(s, stable_seconds=0)
        self.assertFalse(result["ready"]); self.assertEqual(result["status"], "not-ready")

    def test_each_required_process_or_marker_or_mapping_is_needed(self):
        for index in range(4):
            s = sample(); s["processes"].pop(index)
            with self.subTest(missing_process=index): self.assertFalse(M.StartupGate(baseline()).consume(s, stable_seconds=0)["ready"])
        for index in range(1, 4):
            s = sample(); s["processes"][index]["libcef"] = False
            with self.subTest(missing_libcef=index): self.assertFalse(M.StartupGate(baseline()).consume(s, stable_seconds=0)["ready"])
        for marker in ("Done initializing CGraphicsDevice resources", "Compositor render thread started", "system.systemui", "New Connect"):
            s = sample(); s["logs"] = {k: "\n".join(row for row in v.splitlines() if marker not in row) for k, v in s["logs"].items()}
            with self.subTest(missing_marker=marker): self.assertFalse(M.StartupGate(baseline()).consume(s, stable_seconds=0)["ready"])

    def test_unreadable_required_process_never_passes(self):
        s = sample(); s["errors"] = [{"pid": 102, "reason": "PermissionError"}]
        self.assertFalse(M.StartupGate(baseline()).consume(s, stable_seconds=0)["ready"])

    def test_premature_exit_identity_replacement_or_service_restart_fails(self):
        for variant in ("exit", "replace", "invocation", "old_invocation", "restarts", "main", "failed", "zombie", "main_exit", "main_replace"):
            gate = M.StartupGate(baseline()); gate.consume(sample(), stable_seconds=0)
            s = sample(4)
            if variant == "exit": s["processes"].pop(1)
            if variant == "replace": s["processes"][1]["start_ticks"] += 1
            if variant == "invocation": s["unit"]["InvocationID"] = "c" * 32
            if variant == "old_invocation": s["unit"]["InvocationID"] = "a" * 32
            if variant == "restarts": s["unit"]["NRestarts"] = "1"
            if variant == "main": s["unit"]["MainPID"] = "200"
            if variant == "failed": s["unit"]["ActiveState"] = "failed"
            if variant == "zombie": s["processes"][1]["state"] = "Z"
            if variant == "main_exit": s["main_process"] = None
            if variant == "main_replace": s["main_process"]["start_ticks"] += 1
            with self.subTest(variant=variant): self.assertEqual(gate.consume(s)["status"], "fault")

    def test_missing_libcef_after_ready_is_not_ready(self):
        gate = M.StartupGate(baseline()); gate.consume(sample(), stable_seconds=0)
        s = sample(4); s["processes"][1]["libcef"] = False
        result = gate.consume(s)
        self.assertFalse(result["ready"]); self.assertEqual(result["status"], "fault")
        self.assertEqual(gate.consume(sample(5))["status"], "fault")

    def test_stopped_or_dead_required_process_is_never_ready(self):
        for state in "TtXxZ":
            for index in range(5):
                s = sample()
                target = s["main_process"] if index == 4 else s["processes"][index]
                target["state"] = state
                with self.subTest(state=state, initial_role=index):
                    self.assertFalse(M.StartupGate(baseline()).consume(s, stable_seconds=0)["ready"])
                gate = M.StartupGate(baseline()); gate.consume(sample(), stable_seconds=0)
                s["monotonic"] = 4
                with self.subTest(state=state, monitored_role=index):
                    self.assertEqual(gate.consume(s)["status"], "fault")

    def test_baseline_managed_read_gap_is_retained_and_cannot_prove_ready(self):
        b = baseline(); b["errors"] = [{"pid": 10, "reason": "PermissionError"}]
        gate = M.StartupGate(b)
        self.assertEqual(gate.checkpoint()["baseline"]["errors"], b["errors"])
        result = gate.consume(sample(), stable_seconds=0)
        self.assertEqual(result["status"], "fault"); self.assertIn("Baseline", result["reason"])

    def test_same_pid_old_start_identity_cannot_satisfy_new_readiness(self):
        b = baseline(); b["processes"] = copy.deepcopy(sample()["processes"])
        self.assertFalse(M.StartupGate(b).consume(sample(), stable_seconds=0)["ready"])

    def test_wait_is_bounded_and_requires_progress(self):
        s = sample(); s["logs"] = {}; source = Source([s])
        result = M.wait_ready(source, baseline(), timeout=2, no_progress_seconds=1, clock=source.clock, sleep=source.sleep)
        self.assertEqual(result["status"], "not-ready"); self.assertIn("stopped advancing", result["reason"])
        self.assertLessEqual(source.calls, 6)

    def test_missing_or_unreadable_evidence_times_out_without_restart(self):
        source = Source([PermissionError("managed metadata denied")])
        result = M.wait_ready(source, baseline(), timeout=1, no_progress_seconds=1, clock=source.clock, sleep=source.sleep)
        self.assertFalse(result["ready"]); self.assertLessEqual(source.calls, 5)

    def test_wait_and_resumable_monitor_detect_ui_loss(self):
        source = Source([sample()]); result = M.wait_ready(source, baseline(), timeout=3, no_progress_seconds=3,
            clock=source.clock, sleep=source.sleep)
        self.assertTrue(result["ready"])
        gate = M.StartupGate.resume(json.loads(json.dumps(result["gate_checkpoint"])))
        s = sample(); s["processes"].pop(1); source.samples = [s]
        result = M.monitor(source, gate, seconds=1, clock=source.clock, sleep=source.sleep)
        self.assertEqual(result["status"], "fault"); self.assertEqual(result["monitor_samples"], 1)
        self.assertFalse(result["automatic_restart"])

    def test_monitor_unreadable_evidence_is_sticky_without_restarting(self):
        gate = M.StartupGate(baseline()); gate.consume(sample(), stable_seconds=0)
        source = Source([PermissionError("UI process unreadable")])
        result = M.monitor(source, gate, seconds=1, clock=source.clock, sleep=source.sleep)
        self.assertFalse(result["ready"]); self.assertEqual(result["monitor_samples"], 1)
        self.assertEqual(gate.consume(sample(5))["status"], "fault")

    def test_poll_limits_and_monotonic_regression_fail_closed(self):
        for key, value in (("timeout", 31), ("poll_seconds", 0), ("stable_seconds", 4), ("no_progress_seconds", 21)):
            with self.subTest(key=key), self.assertRaises(RuntimeError): M.wait_ready(Source([sample()]), baseline(), **{key: value})
        gate = M.StartupGate(baseline()); self.assertEqual(gate.consume(sample(0.5))["status"], "fault")

    def test_critical_current_renderer_failure_detected(self):
        for message in ("vkCreateQueryPool invalid device", "VK_ERROR_DEVICE_LOST", "renderer process crashed", "FATAL graphics initialization failed", "symbol lookup error"):
            s = sample(); s["journal"] = [{"MESSAGE": message, "_PID": "102", "__MONOTONIC_TIMESTAMP": "2000000"}]
            with self.subTest(message=message): self.assertEqual(M.StartupGate(baseline()).consume(s)["status"], "fault")


class ServerBaselineGateTests(unittest.TestCase):
    def test_exact_old_server_identity_prevents_the_observed_teardown_false_alarm(self):
        # The captured teardown PID/executable and monotonic timestamps are real;
        # start_ticks and positive UI roles below are synthetic fixture identities.
        b = baseline(); b["monotonic"] = 10340.269872196
        b["processes"].append(server(327846, start_ticks=4460000))
        s = sample(10350.0); s["unit"]["ExecMainStartTimestampMonotonic"] = "10347501181"
        row = coredump("327846"); row.update(COREDUMP_EXE=M.SERVER, __MONOTONIC_TIMESTAMP="10342931025")
        s["journal"] = [row]
        self.assertTrue(M.StartupGate(b).consume(s, stable_seconds=0)["ready"])
        missing = copy.deepcopy(b); missing["processes"].pop()
        result = M.StartupGate(missing).consume(s, stable_seconds=0)
        self.assertEqual(result["status"], "fault")
        self.assertEqual(result["reason"], "Fresh startup coredump: pid=327846 signal=11 exe=" + M.SERVER)

    def test_unknown_current_and_same_pid_reused_servers_remain_fatal(self):
        for variant in ("unknown", "current", "reused", "dead_reused", "baseline_other_exe"):
            b = baseline(); b["processes"] = [server(10)]
            s = sample(); row = coredump("10"); row["COREDUMP_EXE"] = M.SERVER
            if variant == "unknown": row["COREDUMP_PID"] = "200"
            if variant == "current": s["processes"].append(server(200)); row["COREDUMP_PID"] = "200"
            if variant in ("reused", "dead_reused"):
                current = server(10, start_ticks=1001)
                if variant == "dead_reused": current["state"] = "Z"
                s["processes"].append(current)
            if variant == "baseline_other_exe": b["processes"] = [process(10, "compositor")]
            s["journal"] = [row]
            with self.subTest(variant=variant):
                result = M.StartupGate(b).consume(s, stable_seconds=0)
                self.assertEqual(result["status"], "fault")
                self.assertIn("Fresh startup coredump", result["reason"])

    def test_old_server_pid_never_hides_a_different_executable_or_group(self):
        b = baseline(); b["processes"] = [server(10)]
        for updates in ({"COREDUMP_EXE": M.COMPOSITOR}, {"COREDUMP_EXE": "/unrelated/vrserver"},
                        {"COREDUMP_CGROUP": unit()["ControlGroup"] + "/different-child"}):
            s = sample(); row = coredump("10"); row["COREDUMP_EXE"] = M.SERVER; row.update(updates); s["journal"] = [row]
            with self.subTest(updates=updates):
                self.assertEqual(M.StartupGate(b).consume(s)["status"], "fault")

    def test_complete_server_identity_and_native_scope_are_required(self):
        for updates in ({"comm": "vrwebhelper"}, {"kind": "browser"}, {"libcef": True},
                        {"control_group": "/other/steamvr.service"}, {"control_group": "../steamvr.service"},
                        {"start_ticks": True}, {"ui_compositor_tasks": []}):
            b = baseline(); item = server(10); item.update(updates); b["processes"] = [item]
            with self.subTest(updates=updates), self.assertRaises(RuntimeError): M.StartupGate(b)
        for key in ("comm", "control_group", "start_ticks"):
            b = baseline(); item = server(10); item.pop(key); b["processes"] = [item]
            with self.subTest(missing=key), self.assertRaises(RuntimeError): M.StartupGate(b)

    def test_descendant_server_identity_is_exact_and_checkpointed(self):
        child = unit()["ControlGroup"] + "/native-server"
        b = baseline(); b["processes"] = [server(10, group=child)]
        s = sample(); row = coredump("10"); row.update(COREDUMP_EXE=M.SERVER, COREDUMP_CGROUP=child); s["journal"] = [row]
        gate = M.StartupGate(b); gate = M.StartupGate.resume(gate.checkpoint())
        self.assertTrue(gate.consume(s, stable_seconds=0)["ready"])
        self.assertEqual(gate.baseline["processes"][-1]["control_group"], child)
        fresh = copy.deepcopy(s); fresh["journal"][0]["COREDUMP_PID"] = "200"
        self.assertEqual(gate.consume(fresh)["status"], "fault")

    def test_server_is_not_a_substitute_for_required_positive_ui_roles(self):
        s = sample(); s["processes"] = [server(200)]
        self.assertFalse(M.StartupGate(baseline()).consume(s, stable_seconds=0)["ready"])

    def test_observed_server_pid_reuse_stays_current_after_exit_and_resume(self):
        b = baseline(); b["processes"] = [server(10)]
        first = sample(); first["processes"].append(server(10, start_ticks=1001))
        gate = M.StartupGate(b); self.assertTrue(gate.consume(first, stable_seconds=0)["ready"])
        saved = gate.checkpoint(); self.assertEqual(saved["reused_old_pids"], [10])
        gate = M.StartupGate.resume(saved)
        later = sample(4); row = coredump("10"); row["COREDUMP_EXE"] = M.SERVER; later["journal"] = [row]
        result = gate.consume(later)
        self.assertEqual(result["status"], "fault"); self.assertIn("Fresh startup coredump", result["reason"])

    def test_reuse_checkpoint_cannot_invent_pids_or_drop_new_server_history(self):
        b = baseline(); b["processes"] = [server(10)]
        saved = M.StartupGate(b).checkpoint()
        for value in ([True], [200], [10, 10], {}, "10"):
            bad = copy.deepcopy(saved); bad["reused_old_pids"] = value
            with self.subTest(value=value), self.assertRaises(RuntimeError): M.StartupGate.resume(bad)
        saved.pop("reused_old_pids")
        with self.assertRaises(RuntimeError): M.StartupGate.resume(saved)
        legacy = M.StartupGate(baseline()).checkpoint(); legacy.pop("reused_old_pids")
        self.assertFalse(M.StartupGate.resume(legacy).reused_old_pids)

    def test_duplicate_baseline_pid_cannot_authenticate_two_native_roles(self):
        b = baseline(); b["processes"].append(server(10))
        with self.assertRaisesRegex(RuntimeError, "Duplicate startup baseline process identity"): M.StartupGate(b)


class NativeSourceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="compositor-health-fixture-"); self.root = Path(self.temp.name)
        self.home = self.root / "home"; self.home.mkdir(); self.proc = self.root / "proc"; self.proc.mkdir()
        self.now = 1.0; self.commands = []; self.journal_rows = []
        self.source = M.NativeEvidenceSource(self.home, proc_root=self.proc, run=self.command,
            clock=lambda: self.now, wall_clock=lambda: 100.0)
        self.logdir = self.home / ".local/share/Steam/logs"; self.logdir.mkdir(parents=True)
        for name in self.source.logs: (self.logdir / (name + ".txt")).write_text("old line\n")
    def tearDown(self): self.temp.cleanup()
    def command(self, args, **kwargs):
        self.commands.append(args)
        if args[0] == "systemctl": raw = "\n".join(f"{k}={v}" for k, v in unit().items()) + "\n"
        else:
            rows = self.journal_rows if any(arg.startswith("--after-cursor") for arg in args) else []
            raw = "\n".join(json.dumps(row) for row in rows) + "\n-- cursor: s=current\n"
        return subprocess.CompletedProcess(args, 0, raw, "")
    def procfile(self, pid, kind="browser", *, group=None, libcef=True):
        p = self.proc / str(pid); p.mkdir()
        exe = M.COMPOSITOR if kind == "compositor" else M.SERVER if kind == "server" else M.WEBHELPER
        (p / "comm").write_text(Path(exe).name + "\n")
        (p / "cgroup").write_text("0::" + (group or unit()["ControlGroup"]) + "\n")
        cols = ["S"] + ["0"] * 18 + [str(pid * 100)]
        (p / "stat").write_text(str(pid) + " (vrwebhelper) " + " ".join(cols))
        (p / "exe").symlink_to(exe)
        (p / "cmdline").write_bytes((exe + ("\0--type=" + kind if kind not in ("browser", "compositor", "server") else "") + "\0").encode())
        (p / "maps").write_text("1000-2000 r-xp 00000000 00:00 0 " + (M.LIBCEF if libcef else "/not-libcef.so") + "\n")
        return p
    def test_log_offsets_read_only_new_bytes_and_rotation_uses_current_file(self):
        self.source.baseline()
        with (self.logdir / "vrserver.txt").open("a") as stream: stream.write("fresh line\n")
        self.assertEqual(self.source.sample()["logs"]["vrserver"], "fresh line")
        (self.logdir / "vrserver.txt").rename(self.logdir / "vrserver.previous.txt")
        (self.logdir / "vrserver.txt").write_text("new invocation\n")
        self.assertEqual(self.source.sample()["logs"]["vrserver"], "new invocation")
        self.assertNotIn("previous", str(self.commands))

    def test_delayed_stream_log_snapshot_uses_bounded_reads_and_keeps_budget(self):
        self.source.baseline()
        # The failed hardware arm resumed at 265191 cumulative bytes, with a
        # later vrserver delta larger than one 128-KiB read but within 512 KiB.
        checkpoint = copy.deepcopy(self.source.checkpoint())
        checkpoint["total"] = 265191
        self.source.resume(checkpoint)
        raw = b"stream connected\n" * 9500
        self.assertGreater(len(raw), M.LOG_READ_CHUNK)
        with (self.logdir / "vrserver.txt").open("ab") as stream: stream.write(raw)
        requests = []; original = M.os.fdopen
        class BoundedReader:
            def __init__(self, stream): self.stream = stream
            def __enter__(self): return self
            def __exit__(self, *args): return self.stream.__exit__(*args)
            def __getattr__(self, name): return getattr(self.stream, name)
            def read(self, size):
                requests.append(size)
                return self.stream.read(size)
        with mock.patch.object(M.os, "fdopen", side_effect=lambda *args, **kwargs: BoundedReader(original(*args, **kwargs))):
            text = self.source.log_read("vrserver")
        self.assertEqual(text, raw.decode().removesuffix("\n"))
        self.assertGreater(len(requests), 1)
        self.assertLessEqual(max(requests), M.LOG_READ_CHUNK)
        self.assertEqual(self.source.total, checkpoint["total"] + len(raw))
        observed = sample(); observed["logs"]["vrserver"] += "\n" + text
        self.assertTrue(M.StartupGate(baseline()).consume(observed, stable_seconds=0)["ready"])
        resumed = M.NativeEvidenceSource(self.home, proc_root=self.proc, run=self.command,
            clock=lambda: self.now, wall_clock=lambda: 100.0)
        resumed.resume(copy.deepcopy(self.source.checkpoint()))
        self.assertEqual(resumed.log_read("vrserver"), "")
        self.assertEqual(resumed.total, self.source.total)

    def test_chunk_boundary_does_not_split_utf8_or_reemit_pending_lines(self):
        self.source.baseline()
        path = self.logdir / "vrserver.txt"
        with path.open("ab") as stream: stream.write(b"pending ")
        self.assertEqual(self.source.log_read("vrserver"), "")
        saved = copy.deepcopy(self.source.checkpoint()); self.source.resume(saved)
        raw = b"x\n" * ((M.LOG_READ_CHUNK - 2) // 2) + b"x" + "ä\n".encode()
        self.assertEqual(raw[M.LOG_READ_CHUNK - 1:M.LOG_READ_CHUNK + 1], "ä".encode())
        with path.open("ab") as stream: stream.write(raw)
        text = self.source.log_read("vrserver")
        self.assertTrue(text.startswith("pending x\n"))
        self.assertTrue(text.endswith("xä"))
        self.assertEqual(self.source.total, len(b"pending ") + len(raw))
        self.assertEqual(self.source.log_read("vrserver"), "")

    def test_failure_after_first_chunk_still_revokes_ready_gate(self):
        self.source.baseline()
        gate = M.StartupGate(baseline())
        self.assertTrue(gate.consume(sample(), stable_seconds=0)["ready"])
        raw = (line("stream progress") + "\n") * 3000 + line("FATAL graphics initialization failed") + "\n"
        self.assertGreater(raw.index("FATAL"), M.LOG_READ_CHUNK)
        with (self.logdir / "vrserver.txt").open("a") as stream: stream.write(raw)
        observed = sample(3.0)
        observed["logs"] = {"vrserver": self.source.log_read("vrserver"), "vrcompositor": ""}
        result = gate.consume(observed, stable_seconds=0)
        self.assertEqual(result["status"], "fault")
        self.assertIn("Fresh native startup failure", result["reason"])

    def test_all_log_files_share_cumulative_budget_and_refuse_before_reading(self):
        self.source.baseline()
        checkpoint = copy.deepcopy(self.source.checkpoint())
        checkpoint["total"] = M.LIMIT - 200000; self.source.resume(checkpoint)
        for name in self.source.logs:
            with (self.logdir / (name + ".txt")).open("ab") as stream: stream.write(b"x\n" * 80000)
        self.source.log_read("vrcompositor")
        before = copy.deepcopy(self.source.checkpoint())
        # The second file cannot silently spend a fresh budget or skip its gap.
        with mock.patch.object(M.os, "fdopen", wraps=M.os.fdopen) as opened, \
                self.assertRaisesRegex(RuntimeError, "evidence byte bound"):
            self.source.log_read("vrserver")
        self.assertEqual(self.source.checkpoint(), before)
        self.assertEqual(opened.call_count, 1)
        oversized = sample()
        oversized["logs"] = {"vrserver": "x" * (M.LIMIT // 2 + 1), "vrcompositor": "x" * (M.LIMIT // 2)}
        self.assertEqual(M.StartupGate(baseline()).consume(oversized)["status"], "fault")

    def test_replaced_snapshot_keeps_consumed_bytes_without_advancing_offset(self):
        self.source.baseline()
        path = self.logdir / "vrserver.txt"
        raw = b"x\n" * 80000
        with path.open("ab") as stream: stream.write(raw)
        before = copy.deepcopy(self.source.checkpoint()); original = M.os.fdopen
        class ReplacedReader:
            def __init__(self, stream): self.stream = stream; self.replaced = False
            def __enter__(self): return self
            def __exit__(self, *args): return self.stream.__exit__(*args)
            def __getattr__(self, name): return getattr(self.stream, name)
            def read(self, size):
                data = self.stream.read(size)
                if not self.replaced:
                    self.replaced = True
                    path.rename(path.with_suffix(".old")); path.write_bytes(b"new log\n")
                return data
        with mock.patch.object(M.os, "fdopen", side_effect=lambda *args, **kwargs: ReplacedReader(original(*args, **kwargs))), \
                self.assertRaisesRegex(RuntimeError, "rotated during sampling"):
            self.source.log_read("vrserver")
        self.assertEqual(self.source.total, before["total"] + len(raw))
        self.assertEqual(self.source.offsets["vrserver"], before["log_offsets"]["vrserver"])

    def test_truncated_snapshot_charges_short_read_and_keeps_old_offset(self):
        self.source.baseline()
        path = self.logdir / "vrserver.txt"
        with path.open("ab") as stream: stream.write(b"x\n" * 80000)
        before = copy.deepcopy(self.source.checkpoint()); original = M.os.fdopen
        class TruncatedReader:
            def __init__(self, stream): self.stream = stream; self.truncated = False
            def __enter__(self): return self
            def __exit__(self, *args): return self.stream.__exit__(*args)
            def __getattr__(self, name): return getattr(self.stream, name)
            def read(self, size):
                data = self.stream.read(min(size, 19))
                if not self.truncated:
                    self.truncated = True
                    path.write_bytes(b"")
                return data
        with mock.patch.object(M.os, "fdopen", side_effect=lambda *args, **kwargs: TruncatedReader(original(*args, **kwargs))), \
                self.assertRaisesRegex(RuntimeError, "truncated during sampling"):
            self.source.log_read("vrserver")
        self.assertEqual(self.source.total, before["total"] + 19)
        self.assertEqual(self.source.offsets["vrserver"], before["log_offsets"]["vrserver"])

    def test_journal_and_delayed_log_snapshot_share_one_evidence_budget(self):
        self.source.baseline()
        checkpoint = copy.deepcopy(self.source.checkpoint())
        checkpoint["total"] = M.LIMIT - 200000; self.source.resume(checkpoint)
        self.journal_rows = [{"MESSAGE": "x" * 25000, "__CURSOR": "s=one"},
                             {"MESSAGE": "x" * 25000, "__CURSOR": "s=two"}]
        self.source.journal()
        self.assertGreater(self.source.total, checkpoint["total"] + 40000)
        with (self.logdir / "vrserver.txt").open("ab") as stream: stream.write(b"x\n" * 80000)
        before = copy.deepcopy(self.source.checkpoint())
        with self.assertRaisesRegex(RuntimeError, "evidence byte bound"):
            self.source.log_read("vrserver")
        self.assertEqual(self.source.checkpoint(), before)

    def test_partial_lines_are_only_emitted_once_complete(self):
        self.source.baseline()
        path = self.logdir / "vrserver.txt"
        with path.open("a") as stream: stream.write("part")
        self.assertEqual(self.source.sample()["logs"]["vrserver"], "")
        with path.open("a") as stream: stream.write("ial\n")
        self.assertEqual(self.source.sample()["logs"]["vrserver"], "partial")
    def test_proc_reads_only_managed_required_executable_metadata(self):
        self.procfile(101, "compositor"); self.procfile(102); self.procfile(103, "renderer")
        self.procfile(200, group="/not/steamvr.service")
        p = self.proc / "1"; p.mkdir(); (p / "comm").write_text("systemd\n")
        processes, errors = self.source.processes(unit())
        self.assertEqual({p["pid"] for p in processes}, {101, 102, 103}); self.assertFalse(errors)
        self.assertTrue(next(p for p in processes if p["pid"] == 103)["libcef"])

    def test_server_baseline_carries_exact_identity_without_reading_maps(self):
        p = self.procfile(327846, "server")
        original = self.source.proc_read
        def no_maps(path, limit):
            if path == p / "maps": raise AssertionError("Server mappings are not needed")
            return original(path, limit)
        with mock.patch.object(self.source, "proc_read", side_effect=no_maps):
            processes, errors = self.source.processes(unit())
        self.assertFalse(errors); self.assertEqual(processes, [server(327846)])
        M.require_process(processes[0], group=unit()["ControlGroup"])
        b = baseline(); b["processes"] += processes
        s = sample(); row = coredump("327846"); row["COREDUMP_EXE"] = M.SERVER; s["journal"] = [row]
        self.assertTrue(M.StartupGate(b).consume(s, stable_seconds=0)["ready"])

    def test_server_exact_resolved_executable_and_descendant_group(self):
        child = unit()["ControlGroup"] + "/native-server"
        p = self.procfile(200, "server", group=child)
        p.joinpath("exe").unlink(); p.joinpath("exe").symlink_to("/resolved/native/vrserver")
        self.source.native_paths[M.SERVER] = "/resolved/native/vrserver"
        processes, errors = self.source.processes(unit())
        self.assertFalse(errors); self.assertEqual(processes, [server(200, group=child)])
        p.joinpath("exe").unlink(); p.joinpath("exe").symlink_to("/unrelated/vrserver")
        self.assertEqual(self.source.processes(unit()), ([], []))

    def test_server_identity_changes_during_collection_are_explicit_gaps(self):
        for variant in ("start", "exe", "comm", "group", "cmdline"):
            with self.subTest(variant=variant), tempfile.TemporaryDirectory(prefix="server-proc-") as tmp:
                old_proc = self.proc; self.proc = Path(tmp); self.source.proc = self.proc
                try:
                    p = self.procfile(200, "server")
                    original = self.source.proc_read; reads = {"stat": 0}
                    def change(path, limit):
                        if path == p / "stat":
                            reads["stat"] += 1
                            if reads["stat"] == 2:
                                if variant == "start": p.joinpath("stat").write_text("200 (vrserver) S " + "0 " * 18 + "20001")
                                if variant == "exe": p.joinpath("exe").unlink(); p.joinpath("exe").symlink_to("/foreign/vrserver")
                                if variant == "comm": p.joinpath("comm").write_text("other\n")
                                if variant == "group": p.joinpath("cgroup").write_text("0::/foreign/steamvr.service\n")
                                if variant == "cmdline": p.joinpath("cmdline").write_bytes(b"changed\0")
                        return original(path, limit)
                    with mock.patch.object(M.NativeEvidenceSource, "proc_read", side_effect=change):
                        processes, errors = self.source.processes(unit())
                    self.assertFalse(processes); self.assertEqual(errors, [{"pid": 200, "reason": "RuntimeError"}])
                finally:
                    self.proc = old_proc; self.source.proc = old_proc

    def test_server_mismatched_comm_or_unreadable_identity_cannot_enter_baseline(self):
        p = self.procfile(200, "server"); p.joinpath("comm").write_text("vrwebhelper\n")
        processes, errors = self.source.processes(unit())
        self.assertFalse(processes); self.assertEqual(errors, [{"pid": 200, "reason": "RuntimeError"}])
        p.joinpath("comm").write_text("vrserver\n")
        original = self.source.proc_read
        def deny(path, limit):
            if path == p / "stat": raise PermissionError("denied")
            return original(path, limit)
        with mock.patch.object(M.NativeEvidenceSource, "proc_read", side_effect=deny):
            processes, errors = self.source.processes(unit())
        self.assertFalse(processes); self.assertEqual(errors, [{"pid": 200, "reason": "PermissionError"}])
        for other in (M.COMPOSITOR, M.WEBHELPER):
            p.joinpath("exe").unlink(); p.joinpath("exe").symlink_to(other)
            with self.subTest(exe=other):
                self.assertEqual(self.source.processes(unit()), ([], []))
    def test_exact_resolved_native_paths_are_normalized_without_filename_guessing(self):
        p = self.procfile(102, "renderer")
        p.joinpath("exe").unlink(); p.joinpath("exe").symlink_to("/resolved/steamvr/vrwebhelper")
        p.joinpath("maps").write_text("1000-2000 r-xp 00000000 00:00 0 /resolved/steamvr/libcef.so\n")
        self.source.native_paths[M.WEBHELPER] = "/resolved/steamvr/vrwebhelper"
        self.source.native_paths[M.LIBCEF] = "/resolved/steamvr/libcef.so"
        processes, errors = self.source.processes(unit())
        self.assertFalse(errors); self.assertEqual(processes[0]["exe"], M.WEBHELPER); self.assertTrue(processes[0]["libcef"])
        p.joinpath("exe").unlink(); p.joinpath("exe").symlink_to("/unrelated/vrwebhelper")
        self.assertFalse(self.source.processes(unit())[0])

    def test_cgroup_is_checked_before_unrelated_comm_and_read_errors(self):
        unrelated = self.procfile(200, group="/not/steamvr.service")
        managed = self.procfile(102)
        original = self.source.proc_read
        seen = []
        def selective(path, limit):
            seen.append(path)
            if path in (unrelated / "comm", managed / "comm"): raise PermissionError("comm denied")
            return original(path, limit)
        with mock.patch.object(self.source, "proc_read", side_effect=selective):
            processes, errors = self.source.processes(unit())
        self.assertFalse(processes)
        self.assertNotIn(unrelated / "comm", seen)
        self.assertEqual(errors, [{"pid": 102, "reason": "PermissionError"}])
        with mock.patch.object(self.source, "proc_read", side_effect=selective):
            b = self.source.baseline()
        self.assertTrue(any(item["pid"] == 102 for item in b["errors"]))

    def test_scope_unknown_unrelated_read_error_is_not_managed(self):
        p = self.procfile(200, group="/not/steamvr.service")
        original = self.source.proc_read
        def deny(path, limit):
            if path == p / "cgroup": raise PermissionError("group denied")
            return original(path, limit)
        with mock.patch.object(self.source, "proc_read", side_effect=deny):
            processes, errors = self.source.processes(unit())
        self.assertFalse(processes); self.assertFalse(errors)
        u = unit(); u["MainPID"] = "200"
        with mock.patch.object(self.source, "proc_read", side_effect=deny):
            _, errors = self.source.processes(u)
        self.assertEqual(errors, [{"pid": 200, "reason": "PermissionError"}])

    def test_inactive_empty_cgroup_does_not_scan_all_user_processes(self):
        self.procfile(102)
        u = unit(); u.update(ControlGroup="", ActiveState="inactive", MainPID="0")
        with mock.patch.object(self.source, "proc_read", side_effect=AssertionError("unexpected proc read")):
            self.assertEqual(self.source.processes(u), ([], []))

    def test_unreadable_managed_required_process_is_explicit(self):
        p = self.procfile(102)
        original = self.source.proc_read
        def deny(path, limit):
            if path == p / "maps": raise PermissionError("maps denied")
            return original(path, limit)
        with mock.patch.object(self.source, "proc_read", side_effect=deny): processes, errors = self.source.processes(unit())
        self.assertFalse(processes); self.assertEqual(errors, [{"pid": 102, "reason": "PermissionError"}])
    def test_symlink_log_and_oversize_delta_refused(self):
        self.source.baseline(); p = self.logdir / "vrserver.txt"; p.unlink(); p.symlink_to(self.logdir / "vrcompositor.txt")
        with self.assertRaises((OSError, RuntimeError)): self.source.sample()
        p.unlink(); p.write_text("x" * (128 * 1024 + 1))
        with self.assertRaises(RuntimeError): self.source.sample()
    def test_journal_requires_cursor_and_bounded_delta(self):
        with mock.patch.object(self.source, "command", return_value=""):
            with self.assertRaises(RuntimeError): self.source.baseline()
        self.source.baseline(); self.journal_rows = [{"MESSAGE": "x", "__CURSOR": "s=x"}] * 512
        with self.assertRaises(RuntimeError): self.source.sample()

    def test_journal_requests_complete_fields_without_private_core_payloads(self):
        self.source.baseline()
        self.journal_rows = [{"MESSAGE": "x" * 9000, "_PID": "102", "__MONOTONIC_TIMESTAMP": "2000000"}]
        self.assertEqual(self.source.journal(), self.journal_rows)
        calls = [args for args in self.commands if args[0] == "journalctl"]
        for args in calls:
            self.assertIn("--all", args)
            fields = next(arg for arg in args if arg.startswith("--output-fields=")).split("=", 1)[1].split(",")
            self.assertEqual(set(fields), set(M.JOURNAL_FIELDS))
            self.assertNotIn("COREDUMP_ENVIRON", fields)
            self.assertNotIn("COREDUMP_PROC_MAPS", fields)
            self.assertNotIn("COREDUMP_OPEN_FDS", fields)
        s = sample(); s["journal"] = self.source.journal()
        self.assertTrue(M.StartupGate(baseline()).consume(s, stable_seconds=0)["ready"])

    def test_json_byte_arrays_keep_current_invocation_and_budget_checks(self):
        self.source.baseline()
        self.journal_rows = [
            {"MESSAGE": list(b"\x1b[93mcannot allocate memory in static TLS block\x1b[m"),
             "_PID": "10", "_SYSTEMD_USER_INVOCATION_ID": "a" * 32, "__MONOTONIC_TIMESTAMP": "2000000"},
            {"MESSAGE": list(b"\x1b[0;93m[W:onnxruntime:] GPU device discovery failed\x1b[m"),
             "_PID": "102", "_SYSTEMD_USER_INVOCATION_ID": "b" * 32, "__MONOTONIC_TIMESTAMP": "2000000"}]
        s = sample(); s["journal"] = self.source.journal()
        self.assertEqual(s["journal"], self.journal_rows)
        self.assertTrue(M.StartupGate(baseline()).consume(s, stable_seconds=0)["ready"])
        self.journal_rows[1]["MESSAGE"] = list(b"\x1b[93mVK_ERROR_DEVICE_LOST\x1b[m")
        s["journal"] = self.source.journal()
        self.assertEqual(M.StartupGate(baseline()).consume(s)["status"], "fault")
        self.source.total = M.LIMIT
        with self.assertRaises(RuntimeError):
            self.source.journal()
    def test_empty_journal_delta_retains_actual_cursor(self):
        self.source.baseline()
        previous = self.source.cursor
        self.assertEqual(self.source.journal(), [])
        self.assertEqual(self.source.cursor, previous)
        # No invented success for an absent cursor, even when stdout is empty.
        with mock.patch.object(self.source, "command", return_value=""):
            with self.assertRaises(RuntimeError): self.source.journal()

    def test_checkpoint_resume_preserves_cumulative_evidence_budget(self):
        self.source.baseline()
        self.source.total = M.LIMIT - 1
        checkpoint = copy.deepcopy(self.source.checkpoint())
        resumed = M.NativeEvidenceSource(self.home, proc_root=self.proc, run=self.command)
        resumed.resume(checkpoint)
        self.assertEqual(resumed.total, M.LIMIT - 1)
        with self.assertRaises(RuntimeError): resumed.journal()
        for invalid in (-1, M.LIMIT + 1, True):
            checkpoint["total"] = invalid
            with self.subTest(total=invalid), self.assertRaises(RuntimeError): resumed.resume(checkpoint)
        checkpoint.pop("total")
        with self.assertRaises(RuntimeError): resumed.resume(checkpoint)

    def test_resume_rejects_malformed_offsets(self):
        self.source.baseline(); checkpoint = self.source.checkpoint()
        checkpoint["log_offsets"]["vrserver"]["offset"] = -1
        with self.assertRaises(RuntimeError): self.source.resume(checkpoint)


class CameraHealthTests(unittest.TestCase):
    """Camera observations are separate from the UI startup/recovery verdict."""
    def setUp(self):
        self.fixture=NativeSourceTests();self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.source=self.fixture.source
        self.baseline=self.source.baseline()
        self.fixture.now=2.0
        self.commands=[]

    @staticmethod
    def row(message, *, monotonic=2000000, identifier="XRService"):
        return {"MESSAGE":message,"__MONOTONIC_TIMESTAMP":str(monotonic),"SYSLOG_IDENTIFIER":identifier}

    def journal(self,system=(),user=(),*,denied=False,partial=False):
        def run(args,**kwargs):
            self.commands.append(args)
            if "--user" not in args and denied:
                raise subprocess.CalledProcessError(1,args,stderr="private denied details")
            rows=user if "--user" in args else system
            raw="\n".join(json.dumps(row) for row in rows)+"\n"
            return subprocess.CompletedProcess(args,0,raw,"Not seeing messages from other users" if partial and "--user" not in args else "")
        self.source.run=run

    def test_actual_camera_fault_signatures_warn_without_changing_ready_ui(self):
        messages=("[ICP] Failed to receive a firmware response to the IPE_FRAME_PROCESS command",
            "[ICP] Frame processing failure detected after running hardware recovery.",
            "Skipping compositor shared frame update after ICP frame processing failure",
            "icp-sm8650 ac00000.icp: Unexpected ICP IRQ (irq=280|280)")
        self.journal([self.row(message,identifier="kernel" if index==3 else "XRService") for index,message in enumerate(messages)])
        result=self.source.camera_health(self.baseline)
        self.assertEqual(result["status"],"warning")
        self.assertEqual(set(result["reason_codes"]),{"firmware-response-missing","hardware-recovery-failed","camera-update-skipped","icp-interrupt"})
        self.assertFalse(result["camera_pixels_verified"])
        self.assertFalse(result["automatic_restart"]);self.assertFalse(result["automatic_rollback"])
        # The same native camera failure must not turn a healthy UI into an
        # automatic rollback trigger in the independent existing gate.
        value=sample();value["journal"]=[{**self.row(messages[0]),"_PID":"2001583","_SYSTEMD_INVOCATION_ID":"c"*32}]
        self.assertTrue(M.StartupGate(baseline()).consume(value,stable_seconds=0)["ready"])

    def test_old_faults_are_excluded_and_service_or_init_success_does_not_claim_camera_pixels(self):
        self.journal([self.row("[ICP] Failed to receive a firmware response to the IPE_FRAME_PROCESS command",monotonic=500000),
                      self.row("[CTrackedCamera] Late initialization successful")])
        result=self.source.camera_health(self.baseline)
        self.assertEqual(result["status"],"no-errors-observed")
        self.assertEqual(result["reason_codes"],[])
        self.assertFalse(result["camera_pixels_verified"])
        self.assertNotIn("healthy",json.dumps(result))
        for args in self.commands:
            self.assertIn("--boot=0",args)
            self.assertIn("--after-cursor="+self.baseline["cursor"],args)
            self.assertNotIn("--user-unit="+M.UNIT,args)

    def test_permission_denied_uses_user_relay_and_never_exposes_raw_identity(self):
        message="Skipping compositor shared frame update after ICP frame processing failure serial=SECRET-DEVICE"
        self.journal(user=[self.row(message)],denied=True)
        result=self.source.camera_health(self.baseline)
        self.assertEqual(result["status"],"warning")
        self.assertEqual(result["observations"]["system_journal"]["status"],"unavailable")
        self.assertEqual(result["observations"]["user_journal"]["counts"],{"camera-update-skipped":1})
        text=json.dumps(result)
        self.assertNotIn("SECRET",text);self.assertNotIn("private denied",text);self.assertNotIn("_PID",text)
        self.assertTrue(any("--user" in args for args in self.commands))

    def test_permission_gap_without_faults_is_unavailable_not_camera_success(self):
        for mode in ("denied","partial"):
            with self.subTest(mode=mode):
                self.journal(denied=mode=="denied",partial=mode=="partial")
                result=self.source.camera_health(self.baseline)
                self.assertEqual(result["status"],"unavailable")
                self.assertFalse(result["camera_pixels_verified"])

    def test_native_log_fallback_uses_baseline_offset_and_time_without_consuming_startup_source(self):
        path=self.fixture.logdir/"vrcompositor.txt"
        with path.open("a") as stream:
            stream.write(line("Skipping compositor shared frame update after ICP frame processing failure",wall=99)+"\n")
            stream.write(line("Skipping compositor shared frame update after ICP frame processing failure",wall=101)+"\n")
        self.journal(denied=True)
        before=copy.deepcopy(self.source.checkpoint())
        result=self.source.camera_health(self.baseline)
        self.assertEqual(result["status"],"warning")
        self.assertEqual(result["observations"]["compositor_log"]["counts"],{"camera-update-skipped":1})
        self.assertEqual(self.source.checkpoint(),before)
        self.assertIn("Skipping compositor",self.source.log_read("vrcompositor"))

    def test_binary_ansi_message_preserves_fault_and_output_is_only_fixed_metadata(self):
        text="\x1b[91m[ICP] Failed to receive a firmware response to the IPE_FRAME_PROCESS command\x1b[0m"
        self.journal([self.row(list(text.encode()))])
        result=self.source.camera_health(self.baseline)
        self.assertEqual(result["reason_codes"],["firmware-response-missing"])
        self.assertNotIn("MESSAGE",json.dumps(result))

    def test_record_and_byte_bounds_are_explicit_and_never_silently_no_errors(self):
        rows=[self.row("[ICP] Failed to receive a firmware response to the IPE_FRAME_PROCESS command")]*128
        self.journal(rows)
        result=self.source.camera_health(self.baseline)
        self.assertEqual(result["status"],"warning")
        self.assertEqual(result["observations"]["system_journal"]["status"],"bounded")
        self.assertLessEqual(result["observations"]["system_journal"]["counts"]["firmware-response-missing"],128)
        self.journal([self.row("x"*(M.LIMIT+1))])
        result=self.source.camera_health(self.baseline)
        self.assertEqual(result["status"],"unavailable")

    def test_native_fault_before_baseline_offset_cannot_leak_into_current_observation(self):
        with (self.fixture.logdir/"vrcompositor.txt").open("a") as stream:
            stream.write(line("Skipping compositor shared frame update after ICP frame processing failure",wall=101)+"\n")
        current=self.source.baseline()
        self.journal()
        result=self.source.camera_health(current)
        self.assertEqual(result["status"],"no-errors-observed")
        self.assertEqual(result["observations"]["compositor_log"]["counts"],{})

    def test_valid_fault_is_retained_when_later_journal_row_is_unreadable(self):
        self.journal([self.row("[ICP] Failed to receive a firmware response to the IPE_FRAME_PROCESS command"),
                      {"MESSAGE":"untrusted malformed row"}])
        result=self.source.camera_health(self.baseline)
        self.assertEqual(result["status"],"warning")
        self.assertEqual(result["observations"]["system_journal"]["status"],"unavailable")
        self.assertEqual(result["reason_codes"],["firmware-response-missing"])
        self.assertNotIn("untrusted",json.dumps(result))

    def test_unknown_or_incomplete_baseline_does_not_read_any_current_or_historical_camera_data(self):
        self.journal()
        for broken in ({},{**self.baseline,"cursor":"not a safe cursor"},{**self.baseline,"monotonic":True}):
            with self.subTest(keys=sorted(broken)):
                before=len(self.commands)
                self.assertEqual(self.source.camera_health(broken)["status"],"unavailable")
                self.assertEqual(len(self.commands),before)


class ForkedRendererGateTests(unittest.TestCase):
    def test_actual_native_fork_shape_retains_all_required_positive_ui_proofs(self):
        gate = M.StartupGate(baseline())
        result = gate.consume(forked_sample(), stable_seconds=0)
        self.assertTrue(result["ready"])
        self.assertEqual(result["process_identities"]["ui_renderer"], result["process_identities"]["system_ui"])
        self.assertIn("libcef_renderer_live", result["evidence"])
        self.assertFalse(result["screen_pixels_verified"])

    def test_native_connection_and_registration_can_arrive_in_different_polls(self):
        gate = M.StartupGate(baseline()); first = forked_sample()
        first["logs"]["vrserver"] = "\n".join(row for row in first["logs"]["vrserver"].splitlines() if "system.systemui" not in row)
        self.assertFalse(gate.consume(first, stable_seconds=0)["ready"])
        second = forked_sample(3); second["logs"]["vrserver"] = line("SetApplicationPid appkey=system.systemui pid=103, external transition")
        self.assertTrue(gate.consume(second, stable_seconds=0)["ready"])

    def test_unused_zygote_utility_and_missing_fresh_connection_do_not_pass(self):
        for variant in ("utility", "browser", "unused_zygote", "missing_connection", "old_connection", "old_systemui"):
            s = forked_sample(); b = baseline()
            if variant in ("utility", "browser"): s["processes"][2]["kind"] = variant
            if variant == "unused_zygote":
                p = process(105, "zygote"); s["processes"].append(p)
                s["logs"]["vrserver"] = s["logs"]["vrserver"].replace("system.systemui pid=103", "system.systemui pid=105")
            if variant == "missing_connection":
                s["logs"]["vrserver"] = "\n".join(row for row in s["logs"]["vrserver"].splitlines() if "Args: --type=zygote" not in row)
            if variant == "old_connection":
                s["logs"]["vrserver"] = "\n".join(line(row.split("[Info] - ", 1)[1], wall=99) if "Args: --type=zygote" in row else row
                    for row in s["logs"]["vrserver"].splitlines())
            if variant == "old_systemui": b["processes"].append(copy.deepcopy(s["processes"][2]))
            with self.subTest(variant=variant): self.assertFalse(M.StartupGate(b).consume(s, stable_seconds=0)["ready"])

    def test_missing_partial_stopped_or_dead_tasks_are_non_proof(self):
        for variant in (None, [], "partial", "T", "Z", "X"):
            s = forked_sample(); tasks = s["processes"][2]["ui_compositor_tasks"]
            if variant is None or variant == []: s["processes"][2]["ui_compositor_tasks"] = variant
            elif variant == "partial": tasks.pop()
            else: tasks[0]["state"] = variant
            with self.subTest(variant=variant): self.assertFalse(M.StartupGate(baseline()).consume(s, stable_seconds=0)["ready"])

    def test_invalid_task_metadata_and_foreign_steam_renderer_are_refused(self):
        for variant in ("duplicate_tid", "duplicate_role", "bool_tid", "old_start", "foreign_exe", "foreign_role"):
            s = forked_sample(); p = s["processes"][2]; tasks = p["ui_compositor_tasks"]
            if variant == "duplicate_tid": tasks[0]["tid"] = tasks[1]["tid"]
            if variant == "duplicate_role": tasks[0]["comm"] = tasks[1]["comm"]
            if variant == "bool_tid": tasks[0]["tid"] = True
            if variant == "old_start": tasks[0]["start_ticks"] = p["start_ticks"] - 1
            if variant == "foreign_exe": p["exe"] = "/home/steamos/.local/share/Steam/steamrtarm64/steamwebhelper"
            if variant == "foreign_role": tasks[0]["comm"] = "Chrome_IOThread"
            with self.subTest(variant=variant): self.assertEqual(M.StartupGate(baseline()).consume(s)["status"], "fault")

    def test_ready_resume_monitor_requires_same_native_renderer_task_roles(self):
        gate = M.StartupGate(baseline()); self.assertTrue(gate.consume(forked_sample(), stable_seconds=0)["ready"])
        saved = json.loads(json.dumps(gate.checkpoint())); resumed = M.StartupGate.resume(saved)
        self.assertTrue(resumed.consume(forked_sample(3), stable_seconds=0)["ready"])
        missing = forked_sample(4); missing["processes"][2]["ui_compositor_tasks"].pop()
        self.assertEqual(resumed.consume(missing)["status"], "fault")
        for bad in ({"103": True}, {"foreign": 10300}, {"103": 0}, []):
            saved["webhelper_connections"] = bad
            with self.subTest(bad=bad), self.assertRaises(RuntimeError): M.StartupGate.resume(saved)


class ForkedRendererSourceTests(unittest.TestCase):
    setUp = NativeSourceTests.setUp
    tearDown = NativeSourceTests.tearDown
    command = NativeSourceTests.command
    procfile = NativeSourceTests.procfile

    def native_ui(self):
        process = self.procfile(103, "zygote")
        taskdir = process / "task"; taskdir.mkdir()
        for index, name in enumerate(("Chrome_ChildIOT", "GpuMemoryThread", "Compositor")):
            p = taskdir / str(2702896 + index); p.mkdir()
            (p / "comm").write_text(name + "\n")
            cols = ["S"] + ["0"] * 18 + [str(10300 + index)]
            (p / "stat").write_text(p.name + " (" + name + ") " + " ".join(cols))
            (p / "cgroup").write_text("0::" + unit()["ControlGroup"] + "\n")
        return process

    def test_collects_only_exact_managed_libcef_native_fork_tasks(self):
        self.native_ui()
        processes, errors = self.source.processes(unit()); self.assertFalse(errors)
        p = processes[0]
        self.assertEqual(p["kind"], "zygote")
        self.assertEqual({task["comm"] for task in p["ui_compositor_tasks"]}, M.UI_COMPOSITOR_TASK_NAMES)
        self.assertTrue(M.forked_ui_compositor_live(p))
        self.assertEqual(self.source.ui_task_usage["tasks"], 6)
        self.assertGreater(self.source.ui_task_usage["metadata_bytes"], 0)
        self.assertGreater(self.source.ui_task_usage["elapsed_ns"], 0)

    def test_real_proc_fixture_collection_drives_native_fork_gate(self):
        self.native_ui(); self.procfile(101, "compositor"); self.procfile(102); self.procfile(100)
        self.source.cursor = "s=old"
        for name, text in forked_sample()["logs"].items():
            (self.logdir / (name + ".txt")).write_text(text + "\n")
        collected = self.source.sample()
        result = M.StartupGate(baseline()).consume(collected, stable_seconds=0)
        self.assertTrue(result["ready"])
        self.assertEqual(result["process_identities"]["ui_renderer"]["pid"], 103)
        self.assertFalse(result["screen_pixels_verified"])

    def test_unreadable_missing_wrong_group_and_symlink_task_are_non_proof(self):
        for variant in ("missing", "foreign_group", "symlink"):
            with self.subTest(variant=variant):
                self.tearDown(); self.setUp(); p = self.native_ui() / "task/2702896"
                if variant == "missing": (p / "stat").unlink()
                if variant == "foreign_group": (p / "cgroup").write_text("0::/unrelated\n")
                if variant == "symlink":
                    (p / "comm").unlink(); (p / "comm").symlink_to(p / "stat")
                processes, errors = self.source.processes(unit())
                self.assertEqual(len(processes), 1); self.assertFalse(errors)
                self.assertIsNone(processes[0]["ui_compositor_tasks"])

    def test_changed_task_identity_and_process_role_cannot_pass(self):
        for variant in ("task_start", "task_name", "process_role"):
            with self.subTest(variant=variant):
                self.tearDown(); self.setUp(); p = self.native_ui()
                original = self.source.ui_task_read; changed = False
                def mutate(path, limit):
                    nonlocal changed
                    data = original(path, limit)
                    if not changed and path.name == "stat":
                        changed = True
                        if variant == "task_start":
                            prefix, ticks = path.read_text().rsplit(" ", 1)
                            path.write_text(prefix + " " + str(int(ticks) + 1))
                        if variant == "task_name": path.with_name("comm").write_text("Chrome_IOThread\n")
                        if variant == "process_role": (p / "cmdline").write_bytes((M.WEBHELPER + "\0--type=utility\0").encode())
                    return data
                with mock.patch.object(self.source, "ui_task_read", side_effect=mutate):
                    processes, errors = self.source.processes(unit())
                if variant == "process_role": self.assertFalse(processes); self.assertTrue(errors)
                else:
                    self.assertEqual(len(processes), 1); self.assertFalse(errors)
                    self.assertIsNone(processes[0]["ui_compositor_tasks"])

    def test_changed_required_task_role_inventory_cannot_pass(self):
        p = self.native_ui() / "task"; original = self.source.ui_task_read
        def mutate(path, limit):
            data = original(path, limit)
            if path.name == "stat" and not (p / "3000001").exists():
                extra = p / "3000001"; extra.mkdir(); (extra / "comm").write_text("Compositor\n")
            return data
        with mock.patch.object(self.source, "ui_task_read", side_effect=mutate):
            processes, errors = self.source.processes(unit())
        self.assertEqual(len(processes), 1); self.assertFalse(errors)
        self.assertIsNone(processes[0]["ui_compositor_tasks"])

    def test_task_sample_read_count_time_and_resume_budgets_do_not_reset(self):
        self.native_ui(); self.source.processes(unit())
        self.source.cursor = "s=current"
        saved = json.loads(json.dumps(self.source.checkpoint()))
        resumed = M.NativeEvidenceSource(self.home, proc_root=self.proc, run=self.command)
        # Supply baseline log offsets before testing the independent UI budget.
        saved["log_offsets"] = {name: None for name in self.source.logs}
        resumed.resume(saved)
        self.assertEqual(resumed.ui_task_usage, self.source.ui_task_usage)
        for key, limit in M.UI_TASK_LIMITS.items():
            resumed.ui_task_usage = dict(self.source.ui_task_usage); resumed.ui_task_usage[key] = limit
            if key == "samples":
                with self.assertRaisesRegex(RuntimeError, "sample budget exhausted"): resumed.processes(unit())
            else:
                processes, errors = resumed.processes(unit())
                self.assertEqual(len(processes), 1); self.assertFalse(errors)
                self.assertIsNone(processes[0]["ui_compositor_tasks"])
            bad = copy.deepcopy(saved); bad["ui_task_usage"][key] = True
            with self.subTest(key=key), self.assertRaises(RuntimeError): resumed.resume(bad)

    def test_per_process_inventory_and_sample_time_are_hard_bounds(self):
        p = self.native_ui() / "task"
        for index in range(M.UI_TASKS_PER_PROCESS + 1): (p / str(3000000 + index)).mkdir()
        processes, errors = self.source.processes(unit())
        self.assertEqual(len(processes), 1); self.assertFalse(errors); self.assertIsNone(processes[0]["ui_compositor_tasks"])
        self.tearDown(); self.setUp(); self.native_ui()
        original = self.source.ui_task_read
        def expire(path, limit):
            self.source.ui_task_deadline = 0
            return original(path, limit)
        with mock.patch.object(self.source, "ui_task_read", side_effect=expire):
            processes, errors = self.source.processes(unit())
        self.assertEqual(len(processes), 1); self.assertFalse(errors); self.assertIsNone(processes[0]["ui_compositor_tasks"])

    def test_unrelated_task_birth_death_and_non_ascii_names_preserve_required_roles(self):
        p = self.native_ui() / "task"; original = self.source.ui_task_read
        ordinary = p / "3000000"; ordinary.mkdir(); (ordinary / "comm").write_text("WorkerThread\n")
        changed = False
        def mutate(path, limit):
            nonlocal changed
            data = original(path, limit)
            if not changed and path.name == "stat":
                changed = True
                # Birth/death of unneeded tasks must not become a renderer loss.
                (ordinary / "comm").unlink(); ordinary.rmdir()
                extra = p / "3000001"; extra.mkdir(); (extra / "comm").write_bytes(b"Worker_\xff\n")
            return data
        with mock.patch.object(self.source, "ui_task_read", side_effect=mutate):
            processes, errors = self.source.processes(unit())
        self.assertEqual(len(processes), 1); self.assertFalse(errors)
        self.assertTrue(M.forked_ui_compositor_live(processes[0]))

    def test_optional_task_gap_preserves_one_shot_native_connection_until_role_ready(self):
        self.native_ui(); self.procfile(101, "compositor"); self.procfile(102); self.procfile(100)
        self.source.cursor = "s=old"
        for name, text in forked_sample()["logs"].items(): (self.logdir / (name + ".txt")).write_text(text + "\n")
        gate = M.StartupGate(baseline())
        with mock.patch.object(self.source, "ui_compositor_tasks", side_effect=RuntimeError("temporarily changing task roles")):
            first = self.source.sample()
        self.assertFalse(first["errors"])
        self.assertFalse(gate.consume(first, stable_seconds=0)["ready"])
        self.source.now = 3
        # Cursor/log offsets advanced; no invented/replayed native events.
        second = self.source.sample()
        self.assertTrue(all(not text for text in second["logs"].values()))
        self.assertTrue(gate.consume(second, stable_seconds=0)["ready"])

    def test_unrelated_optional_task_gap_does_not_revoke_an_explicit_renderer(self):
        self.native_ui(); self.procfile(101, "compositor"); self.procfile(102); self.procfile(104, "renderer"); self.procfile(100)
        self.source.cursor = "s=old"
        for name, text in sample()["logs"].items(): (self.logdir / (name + ".txt")).write_text(text + "\n")
        gate = M.StartupGate(baseline()); self.assertTrue(gate.consume(self.source.sample(), stable_seconds=0)["ready"])
        self.source.now = 3
        with mock.patch.object(self.source, "ui_compositor_tasks", side_effect=RuntimeError("ordinary CEF worker churn")):
            second = self.source.sample()
        self.assertTrue(gate.consume(second, stable_seconds=0)["ready"])

    def test_required_fork_task_proof_loss_still_revokes_readiness(self):
        self.native_ui(); self.procfile(101, "compositor"); self.procfile(102); self.procfile(100)
        self.source.cursor = "s=old"
        for name, text in forked_sample()["logs"].items(): (self.logdir / (name + ".txt")).write_text(text + "\n")
        gate = M.StartupGate(baseline()); self.assertTrue(gate.consume(self.source.sample(), stable_seconds=0)["ready"])
        self.source.now = 3
        with mock.patch.object(self.source, "ui_compositor_tasks", side_effect=RuntimeError("required task unavailable")):
            second = self.source.sample()
        self.assertEqual(gate.consume(second, stable_seconds=0)["status"], "fault")

class IsolationGateTests(unittest.TestCase):
    def setUp(self):
        self.b = baseline(); self.b["isolation_descriptor"] = descriptor()
        self.s = sample(); self.s["isolation"] = isolated_report()

    def test_reviewed_isolation_is_required_in_addition_to_actual_ui_health(self):
        self.assertFalse(M.StartupGate(self.b).consume(sample(), stable_seconds=0)["ready"])
        result = M.StartupGate(self.b).consume(self.s, stable_seconds=0)
        self.assertTrue(result["ready"]); self.assertTrue(result["isolation_verified"])
        self.assertFalse(result["screen_pixels_verified"])
        self.assertEqual(result["isolation"]["target_env_view"], "initial-exec-only")
        self.assertFalse(result["isolation"]["runtime_environ_verified"])
        self.s["processes"].pop(1)
        self.assertFalse(M.StartupGate(self.b).consume(self.s, stable_seconds=0)["ready"])

    def test_each_non_target_mapping_or_initial_flag_contaminates_and_fault_is_sticky(self):
        for key in ("payload_mapped", "audit_mapped", "loader_payload", "loader_audit", "diagnostic_keys"):
            s = copy.deepcopy(self.s)
            s["isolation"]["processes"][1][key] = ["FRAME_MASK_TRACE"] if key == "diagnostic_keys" else True
            rejudge(s["isolation"])
            gate = M.StartupGate(self.b)
            with self.subTest(key=key):
                result = gate.consume(s, stable_seconds=0)
                self.assertEqual(result["status"], "fault"); self.assertFalse(result["isolation_verified"])
                self.assertEqual(gate.consume(self.s)["status"], "fault")

    def test_unknown_unreadable_changing_and_possible_fork_inventory_never_proves_isolation(self):
        for variant in ("unreadable", "inventory", "unit", "duplicate_target", "target_mapping", "target_state"):
            s = copy.deepcopy(self.s); report = s["isolation"]
            if variant == "unreadable": report["errors"] = [{"pid": 200, "reason": "PermissionError"}]
            if variant == "inventory": report["inventory_stable"] = False
            if variant == "unit": report["unit_stable"] = False
            if variant == "duplicate_target":
                child = copy.deepcopy(report["processes"][0]); child.update(pid=200, start_ticks=20000, ppid=101)
                report["processes"].append(child); report["metadata_usage"]["processes"] += 1
            if variant == "target_mapping": report["processes"][0]["audit_mapped"] = False
            if variant == "target_state": report["processes"][0]["state"] = "Z"
            rejudge(report)
            with self.subTest(variant=variant):
                result = M.StartupGate(self.b).consume(s, stable_seconds=0)
                self.assertFalse(result["ready"]); self.assertEqual(result["status"], "not-ready")

    def test_target_initial_input_mismatch_and_deleted_mapping_refuse(self):
        for key in ("target_initial_environment_matches", "owned_mapping_deleted"):
            s = copy.deepcopy(self.s)
            s["isolation"]["processes"][0][key] = key == "owned_mapping_deleted"
            rejudge(s["isolation"])
            with self.subTest(key=key): self.assertEqual(M.StartupGate(self.b).consume(s)["status"], "fault")

    def test_ready_monitor_cannot_reuse_missing_isolation_or_reset_budget(self):
        for variant in ("missing", "unreadable", "budget", "cgroup"):
            gate = M.StartupGate(self.b); self.assertTrue(gate.consume(self.s, stable_seconds=0)["ready"])
            s = copy.deepcopy(self.s); s["monotonic"] = 4
            if variant == "missing": s.pop("isolation")
            if variant == "unreadable":
                s["isolation"]["errors"] = [{"pid": 200, "reason": "PermissionError"}]; rejudge(s["isolation"])
            if variant == "budget": s["isolation"]["metadata_usage"]["metadata_bytes"] -= 1
            if variant == "cgroup":
                s["unit"]["ControlGroup"] = "/changed/steamvr.service"
                s["isolation"]["control_group"] = s["unit"]["ControlGroup"]
            with self.subTest(variant=variant):
                result = gate.consume(s)
                self.assertEqual(result["status"], "fault"); self.assertFalse(result["isolation_verified"])

    def test_exact_isolated_target_must_be_the_actual_ui_compositor(self):
        report = self.s["isolation"]
        report["processes"][0]["pid"] += 10
        self.assertEqual(M.StartupGate(self.b).consume(self.s)["status"], "fault")

    def test_reports_do_not_self_attest_isolation_or_runtime_environment(self):
        for variant in ("verdict", "descriptor", "runtime", "invocation", "unknown", "duplicate", "zero_budget"):
            s = copy.deepcopy(self.s); report = s["isolation"]
            if variant == "verdict": report["processes"][1]["payload_mapped"] = True
            if variant == "descriptor": report["descriptor"]["payload_path"] = "/unreviewed/observer.so"
            if variant == "runtime": report["runtime_environ_verified"] = True
            if variant == "invocation": report["invocation"] = "c" * 32
            if variant == "unknown": report["extra"] = True
            if variant == "duplicate": report["processes"].append(copy.deepcopy(report["processes"][0]))
            if variant == "zero_budget": report["metadata_usage"]["samples"] = 0
            with self.subTest(variant=variant): self.assertEqual(M.StartupGate(self.b).consume(s)["status"], "fault")

    def test_checkpoint_retains_required_isolation_and_rejects_invented_proof(self):
        gate = M.StartupGate(self.b); gate.consume(self.s, stable_seconds=0)
        saved = json.loads(json.dumps(gate.checkpoint()))
        self.assertTrue(M.StartupGate.resume(saved).consume(self.s, stable_seconds=0)["ready"])
        for variant in ("missing", "false", "unscoped", "bad_budget", "target_identity"):
            broken = copy.deepcopy(saved)
            if variant == "missing": broken["isolation"] = None
            if variant == "false": broken["isolation"]["processes"][1]["payload_mapped"] = True
            if variant == "unscoped": broken["baseline"].pop("isolation_descriptor")
            if variant == "bad_budget": broken["isolation"]["metadata_usage"]["samples"] = M.ISOLATION_SAMPLE_LIMIT + 1
            if variant == "target_identity": broken["isolation"]["processes"][0]["start_ticks"] += 1
            with self.subTest(variant=variant), self.assertRaises(RuntimeError): M.StartupGate.resume(broken)


class IsolationSourceTests(unittest.TestCase):
    command = NativeSourceTests.command
    procfile = NativeSourceTests.procfile
    def setUp(self):
        NativeSourceTests.setUp(self)
        self.config = descriptor()
        self.cgroot = self.root / "cgroups"; self.cgroot.mkdir()
        self.group = self.cgroot / unit()["ControlGroup"].lstrip("/")
        self.group.mkdir(parents=True); (self.group / "cgroup.procs").write_text("100\n101\n102\n")
        self.source.isolation_descriptor = M.require_isolation_descriptor(self.config)
        self.source.cgroup_root = self.cgroot
        self.managed(100, exe="/usr/bin/launcher")
        self.managed(101, exe=M.COMPOSITOR, env=self.config["target_exec_environment"], full=True)
        self.managed(102, exe="/usr/bin/unrecognized-service-helper")
    def tearDown(self): NativeSourceTests.tearDown(self)
    def managed(self, pid, *, exe, env=None, full=False, group=None):
        p = self.procfile(pid, "compositor" if exe == M.COMPOSITOR else "browser", group=group)
        (p / "exe").unlink(); (p / "exe").symlink_to(exe)
        cols = ["S", "100" if pid != 100 else "1"] + ["0"] * 17 + [str(pid * 100)]
        (p / "stat").write_text(str(pid) + " (helper) " + " ".join(cols))
        env = env if env is not None else {"LD_PRELOAD": self.config["selector_path"], "PRIVATE_USER_VALUE": "never-log-this"}
        (p / "environ").write_bytes(b"".join(key.encode() + b"=" + value.encode() + b"\0" for key, value in env.items()))
        paths = [self.config["selector_path"]]
        if full: paths += [self.config["payload_path"], self.config["audit_path"]]
        (p / "maps").write_text("\n".join("1000-2000 r-xp 00000000 00:00 0 " + path for path in paths) + "\n")
        return p
    def report(self): return self.source.isolation(unit())

    def test_every_managed_executable_is_checked_with_only_sanitized_evidence(self):
        report = self.report(); M.require_isolation_report(report, self.config, unit())
        self.assertEqual(report["status"], "isolated")
        self.assertEqual({row["pid"] for row in report["processes"]}, {100, 101, 102})
        self.assertNotIn("never-log-this", json.dumps(report))
        self.assertFalse(report["runtime_environ_verified"])
        self.assertEqual(report["target_env_view"], "initial-exec-only")
        self.assertGreater(report["metadata_usage"]["metadata_bytes"], 0)

    def test_unknown_helper_environment_and_mappings_cannot_be_skipped_by_name(self):
        p = self.proc / "102"
        for case in ("preload", "audit", "flag", "payload_map", "audit_map", "deleted_map", "prefix"):
            original_env, original_maps = (p / "environ").read_bytes(), (p / "maps").read_bytes()
            if case == "preload": (p / "environ").write_bytes(b"LD_PRELOAD=/foreign.so:" + self.config["payload_path"].encode() + b"\0")
            if case == "audit": (p / "environ").write_bytes(b"LD_AUDIT=" + self.config["audit_path"].encode() + b"\0")
            if case == "flag": (p / "environ").write_bytes(b"FRAME_MASK_TRACE=0\0")
            if case == "prefix": (p / "environ").write_bytes(b"GHVR_FRAME_COMPOSITOR_RAW_UNREVIEWED=secret\0")
            if case.endswith("map"):
                key = "audit_path" if case == "audit_map" else "payload_path"
                (p / "maps").write_text("1000-2000 r-xp 00000000 00:00 0 " + self.config[key] + (" (deleted)" if case == "deleted_map" else "") + "\n")
            with self.subTest(case=case): self.assertEqual(self.report()["status"], "contaminated")
            (p / "environ").write_bytes(original_env); (p / "maps").write_bytes(original_maps)

    def test_descriptor_matches_exact_paths_and_target_initial_seven_values(self):
        target = self.proc / "101"
        original = (target / "environ").read_bytes()
        (target / "environ").write_bytes(original + b"FRAME_MASK_TRACE_DISABLE=1\0")
        self.assertEqual(self.report()["status"], "contaminated")
        (target / "environ").write_bytes(original.replace(b"FRAME_MASK_TRACE=1", b"FRAME_MASK_TRACE=0"))
        self.assertEqual(self.report()["status"], "contaminated")
        (target / "environ").write_bytes(original)
        p = self.proc / "102"
        (p / "environ").write_bytes(b"LD_PRELOAD=/unrelated/observer.so /unrelated/audit.so\0")
        (p / "maps").write_text("1000-2000 r-xp 00000000 00:00 0 /unrelated/observer.so\n")
        self.assertEqual(self.report()["status"], "isolated")

    def test_managed_descendants_are_included_and_unrelated_pid_metadata_is_not_read(self):
        child = self.group / "nested"; child.mkdir(); (child / "cgroup.procs").write_text("200\n")
        self.managed(200, exe="/usr/bin/nested-helper", group=unit()["ControlGroup"] + "/nested")
        unrelated = self.managed(201, exe="/unrelated/helper", group="/other.service")
        (unrelated / "environ").unlink()
        result = self.report()
        self.assertEqual(result["status"], "isolated")
        self.assertEqual({row["pid"] for row in result["processes"]}, {100, 101, 102, 200})

    def test_unknown_unreadable_managed_members_are_non_proof_never_skipped(self):
        for file in ("environ", "maps", "exe", "cgroup", "stat"):
            path = self.proc / "102" / file; rename = path.with_name(file + ".held")
            path.rename(rename)
            result = self.report()
            with self.subTest(file=file):
                self.assertEqual(result["status"], "not-isolated")
                self.assertTrue(any(error["pid"] == 102 for error in result["errors"]))
            rename.rename(path)
        (self.group / "cgroup.procs").write_text("100\n101\n102\n200\n")
        self.assertEqual(self.report()["status"], "not-isolated")

    def test_permission_denial_of_any_managed_helper_is_explicit_non_proof(self):
        original = self.source.isolation_read
        def deny(path, limit):
            if path == self.proc / "102" / "maps": raise PermissionError("managed mapping denied")
            return original(path, limit)
        with mock.patch.object(self.source, "isolation_read", side_effect=deny): result = self.report()
        self.assertEqual(result["status"], "not-isolated")
        self.assertIn({"pid": 102, "reason": "PermissionError"}, result["errors"])

    @unittest.skipUnless(sys.platform == "linux", "Linux proc initial environment contract")
    def test_real_proc_environment_is_initial_exec_input_not_late_runtime_getenv(self):
        # The actual kernel interface retains the initial range after unsetenv.
        # Reading it cannot certify that a compositor's late getenv is frozen.
        code = ("import os\nfrom pathlib import Path\nimport sys\n"
                "os.environ.pop('FRAME_MASK_TRACE')\nos.environ.pop('GHVR_FRAME_COMPOSITOR_RAW_OUTPUT')\n"
                "assert os.getenv('FRAME_MASK_TRACE') is None\n"
                "sys.stdout.buffer.write(Path('/proc/self/environ').read_bytes())\n")
        result = subprocess.run([sys.executable, "-c", code], env={"FRAME_MASK_TRACE": "1",
            "GHVR_FRAME_COMPOSITOR_RAW_OUTPUT": "/reviewed/raw.jsonl"}, check=True, capture_output=True, timeout=5)
        evidence = self.source.isolation_environment(result.stdout, target=False)
        self.assertEqual(set(evidence["diagnostic_keys"]), {"FRAME_MASK_TRACE", "GHVR_FRAME_COMPOSITOR_RAW_OUTPUT"})
        self.assertIsNone(evidence["target_initial_environment_matches"])

    def test_uid_pid_start_exe_cgroup_and_inventory_are_bracketed(self):
        for variant in ("uid", "start", "exe", "group", "inventory", "unit"):
            with self.subTest(variant=variant):
                original = self.source.isolation_read
                changed = False
                def mutate(path, limit):
                    nonlocal changed
                    data = original(path, limit)
                    if path == self.proc / "102" / "maps" and not changed:
                        changed = True
                        if variant == "start":
                            p = self.proc / "102" / "stat"; p.write_text(p.read_text().replace("10200", "10201"))
                        if variant == "exe":
                            p = self.proc / "102" / "exe"; p.unlink(); p.symlink_to("/changed/helper")
                        if variant == "group": (self.proc / "102" / "cgroup").write_text("0::/other.service\n")
                        if variant == "inventory": (self.group / "cgroup.procs").write_text("100\n101\n")
                    return data
                if variant == "uid":
                    self.source.uid = os.getuid() + 1
                    result = self.report(); self.source.uid = os.getuid()
                elif variant == "unit":
                    with mock.patch.object(self.source, "unit", return_value=unit("c" * 32)): result = self.report()
                else:
                    with mock.patch.object(self.source, "isolation_read", side_effect=mutate): result = self.report()
                self.assertEqual(result["status"], "not-isolated")
                if variant in ("start", "exe", "group"):
                    for p in (self.proc / "102",):
                        for child in p.iterdir(): child.unlink()
                        p.rmdir()
                    self.managed(102, exe="/usr/bin/unrecognized-service-helper")
                (self.group / "cgroup.procs").write_text("100\n101\n102\n")

    def test_non_exec_fork_copies_are_ambiguous_and_not_called_unloaded(self):
        self.managed(200, exe=M.COMPOSITOR, env=self.config["target_exec_environment"], full=True)
        (self.group / "cgroup.procs").write_text("100\n101\n102\n200\n")
        result = self.report()
        self.assertEqual(result["status"], "not-isolated"); self.assertIn("fork", result["reason"])
        (self.proc / "200" / "exe").unlink(); (self.proc / "200" / "exe").symlink_to("/usr/bin/forked-shell")
        self.assertEqual(self.report()["status"], "contaminated")

    def test_source_checkpoint_preserves_budget_descriptor_and_never_resets_on_resume(self):
        self.source.baseline(); self.report()
        saved = json.loads(json.dumps(self.source.checkpoint()))
        resumed = M.NativeEvidenceSource(self.home, proc_root=self.proc, run=self.command, isolation=self.config, cgroup_root=self.cgroot)
        resumed.resume(saved)
        self.assertEqual(resumed.isolation_usage, saved["isolation_usage"])
        resumed.isolation(unit())
        self.assertGreater(resumed.isolation_usage["samples"], saved["isolation_usage"]["samples"])
        self.assertGreater(resumed.isolation_usage["metadata_bytes"], saved["isolation_usage"]["metadata_bytes"])
        for key, value in (("samples", M.ISOLATION_SAMPLE_LIMIT + 1), ("processes", True), ("metadata_bytes", -1)):
            bad = copy.deepcopy(saved); bad["isolation_usage"][key] = value
            with self.subTest(key=key), self.assertRaises(RuntimeError): resumed.resume(bad)
        bad = copy.deepcopy(saved); bad["isolation_descriptor"]["audit_path"] = "/unreviewed/audit.so"
        with self.assertRaises(RuntimeError): resumed.resume(bad)

    def test_cumulative_limits_and_per_process_extents_fail_without_metadata_leak(self):
        self.source.isolation_usage["samples"] = M.ISOLATION_SAMPLE_LIMIT
        with self.assertRaises(RuntimeError): self.report()
        self.source.isolation_usage["samples"] = 0
        self.source.isolation_usage["processes"] = M.ISOLATION_PROCESS_LIMIT
        self.assertEqual(self.report()["status"], "not-isolated")
        self.source.isolation_usage = {"samples": 0, "processes": 0, "metadata_bytes": M.ISOLATION_BYTE_LIMIT - 1}
        self.assertEqual(self.report()["status"], "not-isolated")
        self.assertEqual(self.source.isolation_usage["metadata_bytes"], M.ISOLATION_BYTE_LIMIT)
        self.source.isolation_usage = {"samples": 0, "processes": 0, "metadata_bytes": 0}
        (self.proc / "102" / "environ").write_bytes(b"PRIVATE=secret" + b"x" * M.ISOLATION_ENV_LIMIT + b"\0")
        report = self.report(); self.assertEqual(report["status"], "not-isolated")
        self.assertNotIn("secret", json.dumps(report))
        (self.proc / "102" / "environ").write_bytes(b"LD_PRELOAD=" + self.config["selector_path"].encode() + b"\0")
        (self.proc / "102" / "maps").write_bytes(b"x" * (M.ISOLATION_MAP_LIMIT + 1))
        self.assertEqual(self.report()["status"], "not-isolated")

    def test_cgroup_inventory_bounds_prevent_unbounded_process_or_directory_scan(self):
        (self.group / "cgroup.procs").write_text("\n".join(str(pid) for pid in range(1, M.MAX_PROCESSES + 2)) + "\n")
        result = self.report(); self.assertEqual(result["status"], "not-isolated")
        self.assertFalse(result["processes"])
        (self.group / "cgroup.procs").write_text("100\n101\n102\n")
        for index in range(M.ISOLATION_GROUP_LIMIT):
            child = self.group / str(index); child.mkdir(); (child / "cgroup.procs").write_text("")
        result = self.report(); self.assertEqual(result["status"], "not-isolated")
        self.assertFalse(result["processes"])

    def test_unsafe_descriptors_and_scope_paths_are_refused_without_guessing(self):
        for variant in ("path", "relative", "aliases", "keys", "loader", "flag", "foreign"):
            config = descriptor()
            if variant == "path": config["selector_path"] = "/reviewed/../selector.so"
            if variant == "relative": config["target_exe"] = "vrcompositor"
            if variant == "aliases": config["audit_path"] = config["payload_path"]
            if variant == "keys": config["target_exec_environment"].pop("LD_AUDIT")
            if variant == "loader": config["target_exec_environment"]["LD_AUDIT"] = "/unrelated/audit.so"
            if variant == "flag": config["target_exec_environment"]["FRAME_MASK_TRACE"] = "0"
            if variant == "foreign": config["extra"] = "unreviewed"
            with self.subTest(variant=variant), self.assertRaises(RuntimeError): M.require_isolation_descriptor(config)
        u = unit(); u["ControlGroup"] = "/user.slice/../steamvr.service"
        self.assertEqual(self.source.isolation(u)["status"], "not-isolated")
        original = self.group / "cgroup.procs"; original.unlink(); original.symlink_to(self.proc / "102" / "environ")
        self.assertEqual(self.report()["status"], "not-isolated")


if __name__ == "__main__": unittest.main()
