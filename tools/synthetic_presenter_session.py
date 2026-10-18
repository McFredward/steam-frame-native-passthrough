#!/usr/bin/env python3
"""Cold file staging for explicit output replacement, never passive.

The default remains the historical synthetic profile. The explicit scene
profile uses distinct executable pins, markers, receipts and status semantics.

No device approval, network, service action, restart, capture arm or image read is
performed here. A separately approved operator owns any eventual device use.
"""
from __future__ import annotations
import argparse
from collections import namedtuple
import importlib.util
import json
import os
from pathlib import Path
import re
import secrets
import struct
import sys

sys.dont_write_bytecode = True
_spec = importlib.util.spec_from_file_location('synthetic_presenter_base_session',
    Path(__file__).with_name('pixel_snapshot_session.py'))
base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(base)
SafetyError = base.SafetyError
SCHEMA = 'frame-synthetic-presenter-session-v1'
SCOPE = 'SYNTHETIC_OUTPUT_REPLACEMENT'
PROFILE = 'dense-asymmetric-registration-presenter-v2'
BUILD_MARKER = b'GHVR_SYNTHETIC_PRESENTER_COLD/1\0'
ENABLE_NAME, CANDIDATE_NAME = 'synthetic-presenter.enable', 'synthetic-presenter.spv'
STATUS_NAME, CONSUMED_NAME = 'synthetic-presenter-status.json', 'synthetic-presenter.consumed'
ENABLE = b'GHVR_SYNTHETIC_PRESENTER/1\n'
CONSUMED = b'GHVR_SYNTHETIC_PRESENTER_CONSUMED/1\n'
CANDIDATE_BYTES = 18416
CANDIDATE_SHA256 = '0aaf356d3789473cd32ff0d60cf5971bc1473813241657877db5c147bb1d4364'
# Historical staging ABI name; the exact executable pin now recognizes robust v3/v1.
SCENE_PROFILE = 'scene-contribution-atlas-v1'
SCENE_BUILD_MARKER = b'GHVR_SCENE_PRESENTER_COLD/1\0'
SCENE_CANDIDATE_BYTES = 31384
SCENE_CANDIDATE_SHA256 = '44c4a6c6bf7543b53d14ab6d350d8498143d6c03f70bfb45b9c6812e6b900a49'
_Profile = namedtuple('_Profile', 'name revision scope stem build_marker enable consumed candidate_bytes candidate_sha256 receiver_schema receiver_mode')


def _profile_spec(name):
    # No caller-supplied executable pins, filenames or marker values. A Session
    # freezes this immutable descriptor at its first operation, before any write.
    # This also preserves the old module-constant sandbox fixture interface.
    if type(name) is not str:
        raise SafetyError('explicit known presenter profile required')
    if name == PROFILE:
        return _Profile(PROFILE, 2, SCOPE, 'synthetic-presenter', BUILD_MARKER, ENABLE, CONSUMED,
            CANDIDATE_BYTES, CANDIDATE_SHA256, 'frame-synthetic-presenter-receiver-v1', 'synthetic_format_guarded_presenter')
    if name == SCENE_PROFILE:
        return _Profile(SCENE_PROFILE, 1, 'SCENE_CONTRIBUTION_OUTPUT_REPLACEMENT', 'scene-presenter', SCENE_BUILD_MARKER,
            b'GHVR_SCENE_PRESENTER/1\n', b'GHVR_SCENE_PRESENTER_CONSUMED/1\n', SCENE_CANDIDATE_BYTES,
            SCENE_CANDIDATE_SHA256, 'frame-scene-presenter-receiver-v1', 'scene_contribution_guarded_presenter')
    raise SafetyError('unknown presenter profile refused')


MAX_RECEIVER_STATUS = 1024
SOURCE_REASONS = {'disabled', 'directory', 'marker', 'module', 'pin', 'ready'}
PRIMITIVE_REASONS = {'none', 'artifact', 'api_version', 'original', 'identity', 'interface', 'graphics', 'dispatch',
    'native_creation', 'shader_creation', 'pipeline_creation', 'unexpected_handle'}
GRAPHICS_GATES = {'none', 'contract', 'blend', 'dynamic', 'color_write', 'attachment', 'format', 'multisample', 'stages'}
RECEIVER_FIELDS = {'schema', 'mode', 'capture_id', 'source_ready', 'source_reason', 'candidate_sha256', 'candidate_bytes',
    'observed_device', 'observed_api', 'active_devices', 'pipeline_attempts', 'pipeline_replacements', 'last_primitive_reason',
    'definition_history_failed', 'revision', 'updates', 'budget_exhausted', 'diagnostic_atomics_not_source_cohort',
    'graphics_gate', 'attachment_format', 'unsupported_dynamic',
    'native_frame_association_proven', 'passthrough_authorized'}


def verify_mode(raw, profile=PROFILE):
    return _verify_mode(raw, _profile_spec(profile))


def _verify_mode(raw, profile):
    """Authenticate literal scope in exactly one read-only file-backed load span.

    The explicit whole-library digest and ELF admission must already have passed.
    This never loads the library or substitutes a caller's mode Boolean.
    """
    locations = [match.start() for match in re.finditer(re.escape(profile.build_marker), raw)]
    if len(locations) != 1:
        raise SafetyError('functional build marker missing or ambiguous; passive artifact refused')
    offset = locations[0]
    phoff = struct.unpack_from('<Q', raw, 32)[0]
    phsize, phnum = struct.unpack_from('<HH', raw, 54)
    spans = []
    for index in range(phnum):
        kind, flags, start, rva, _, length, _, _ = struct.unpack_from('<IIQQQQQQ', raw, phoff + index * phsize)
        if kind == 1 and start <= offset and offset + len(profile.build_marker) <= start + length:
            spans.append((flags, rva + offset - start))
    if (len(spans) != 1 or spans[0][0] not in (4, 5)
            or not 0 <= spans[0][1] <= 0xffffffffffffffff - len(profile.build_marker)):
        raise SafetyError('functional build marker must occupy one read-only file-backed load segment')
    return {'literal': profile.build_marker[:-1].decode(), 'rva': spans[0][1], 'bytes': len(profile.build_marker)}


def verify_candidate(raw, profile=PROFILE):
    return _verify_candidate(raw, _profile_spec(profile))


def _verify_candidate(raw, profile):
    if (len(raw) != profile.candidate_bytes or raw[:4] != b'\x03\x02\x23\x07'
            or base.digest(raw) != profile.candidate_sha256):
        raise SafetyError('exact private guarded candidate bytes/hash required')


def strict_json(path, limit):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result: raise SafetyError('duplicate JSON key')
            result[key] = value
        return result
    def nonfinite(value): raise SafetyError('nonfinite JSON value refused')
    result = json.loads(base.read_file(path, limit, mode=0o600), object_pairs_hook=pairs, parse_constant=nonfinite)
    if type(result) is not dict: raise SafetyError('bounded JSON object required')
    return result


class Session:
    def __init__(self, home, run_dir, *, proc_root=Path('/proc'), host_fixture=False,
                 read_protected_maps=False, protected_map_reader=None, profile=PROFILE):
        if type(host_fixture) is not bool or type(read_protected_maps) is not bool:
            raise SafetyError('explicit Boolean fixture/protected-read policy required')
        selected = _profile_spec(profile)
        self._profile_name, self._profile = profile, None
        self.base = base.Session(home, run_dir, proc_root=proc_root, host_fixture=host_fixture,
            read_protected_maps=read_protected_maps, protected_map_reader=protected_map_reader)
        self.run = self.base.run
        self.runs = self.base.base / 'runs'
        if self.run.parent != self.runs or not re.fullmatch(r'[a-z0-9][a-z0-9_.-]{0,63}', self.run.name):
            raise SafetyError('functional run must be a direct canonical bounded child of runs')
        # Outside run: the unchanged base requires an initially empty run folder.
        # This receipt reserves ownership BEFORE base.prepare can be interrupted.
        self.receipt = self.runs / (self.run.name + '.' + selected.stem + '-session.json')
        self.enable, self.candidate = self.run / (selected.stem + '.enable'), self.run / (selected.stem + '.spv')
        self.receiver_status, self.consumed = self.run / (selected.stem + '-status.json'), self.run / (selected.stem + '.consumed')

    @property
    def profile(self):
        if self._profile is None:
            self._profile = _profile_spec(self._profile_name)
        return self._profile

    def _foreign_files(self):
        other = 'synthetic-presenter' if self.profile.name == SCENE_PROFILE else 'scene-presenter'
        paths = [self.run / (other + suffix) for suffix in ('.enable', '.spv', '-status.json', '.consumed')]
        paths.append(self.runs / (self.run.name + '.' + other + '-session.json'))
        return [str(path) for path in paths if base.exists(path)]

    def _reject_foreign(self):
        if self._foreign_files():
            raise SafetyError('mixed/cross-profile presenter files preserved')

    def _plan(self, capture, wanted, machine, marker):
        original, config, manifest = self.base.plan(capture, wanted, machine)
        group = base.digest(base.encoded({'base_receipt': original, 'candidate_sha256': self.profile.candidate_sha256,
            'candidate_bytes': self.profile.candidate_bytes, 'build_marker': marker, 'scope': self.profile.scope, 'profile': self.profile.name}))
        spent = self.profile.consumed + group.encode() + b'\n'
        receipt = {'schema': 'frame-' + self.profile.stem + '-session-v1', 'scope': self.profile.scope, 'profile': self.profile.name, 'profile_revision': self.profile.revision,
            'base_receipt': original, 'base_receipt_sha256': base.digest(base.encoded(original)),
            'candidate_file': self.candidate.name, 'candidate_sha256': self.profile.candidate_sha256, 'candidate_bytes': self.profile.candidate_bytes,
            'enable_file': self.enable.name, 'enable_sha256': base.digest(self.profile.enable), 'build_marker': marker,
            'receiver_status_file': self.receiver_status.name, 'receiver_status_max_bytes': MAX_RECEIVER_STATUS,
            'code_group_sha256': group, 'consumed_file': self.consumed.name, 'consumed_sha256': base.digest(spent),
            'initial_files': 'all_absent', 'automatic_restart': False, 'native_frame_association_proven': False,
            'passthrough_authorized': False}
        return receipt, original, config, manifest, spent

    def _receipt(self, *, validate_base=True):
        os.close(base.directory(self.runs, private=True))
        os.close(base.directory(self.run, private=True))
        actual = base.load_json(self.receipt)
        original = actual.get('base_receipt')
        marker = actual.get('build_marker')
        if type(original) is not dict or type(marker) is not dict or set(marker) != {'literal', 'rva', 'bytes'}:
            raise SafetyError('functional receipt shape invalid')
        if (marker['literal'] != self.profile.build_marker[:-1].decode() or type(marker['rva']) is not int
                or not 0 <= marker['rva'] <= 0xffffffffffffffff or type(marker['bytes']) is not int
                or marker['bytes'] != len(self.profile.build_marker)):
            raise SafetyError('functional receipt marker invalid')
        planned = self._plan(original.get('capture_id', ''), original.get('library_sha256'), original.get('library_machine'), marker)
        # Encoding comparison is type sensitive (False must not equal integer0).
        if base.encoded(actual) != base.encoded(planned[0]):
            raise SafetyError('changed/foreign functional receipt preserved')
        if validate_base and base.exists(self.base.receipt):
            if base.encoded(self.base.receipt_plan()[0]) != base.encoded(planned[1]):
                raise SafetyError('functional receipt does not own this base session')
        return planned

    def _spent(self):
        return any(base.exists(self.run / name) for name in
            (self.consumed.name, 'disarmed.marker', 'rollback-complete.json', 'capture.arm', 'launch.claim', self.receiver_status.name,
             'snapshot.p010', 'snapshot.json', 'ssbo-original-invocation.bin'))

    def prepare(self, library, library_sha256, candidate, *, capture_id=None):
        raw = base.read_file(base.absolute(library), base.MAX_LIBRARY)
        machine = base.verify_library(raw, library_sha256, host_fixture=self.base.host_fixture)
        self._reject_foreign()
        marker = _verify_mode(raw, self.profile)
        candidate_raw = base.read_file(base.absolute(candidate), self.profile.candidate_bytes)
        _verify_candidate(candidate_raw, self.profile)
        if base.exists(self.receipt):
            receipt, original, _, _, _ = self._receipt()
            if (original['library_sha256'] != library_sha256 or original['library_machine'] != machine
                    or marker != receipt['build_marker'] or (capture_id is not None and capture_id != original['capture_id'])):
                raise SafetyError('functional session cannot change code group or nonce')
        else:
            os.close(base.directory(self.base.home))
            os.close(base.directory(self.base.base, create=True, private=True))
            os.close(base.directory(self.runs, create=True, private=True))
            runfd = base.directory(self.run, create=True, private=True)
            try:
                if os.listdir(runfd): raise SafetyError('nonempty or passive run cannot be adopted as functional')
            finally: os.close(runfd)
            if any(base.exists(path) for path in (self.base.config, self.base.manifest, self.base.receipt, self.base.library)):
                raise SafetyError('existing passive/foreign session preserved')
            receipt, original, _, _, _ = self._plan(capture_id or secrets.token_hex(16), library_sha256, machine, marker)
            base.write_new(self.receipt, base.encoded(receipt))
        if self._spent(): raise SafetyError('consumed, used, capture-armed or rolled-back functional session cannot be reused')
        # Without enable the separately marked runtime remains original/stock.
        self.base.prepare(library, library_sha256, capture_id=original['capture_id'])
        receipt, original, config, manifest, _ = self._receipt()
        for path, expected in ((self.base.library, original['library_sha256']),
                (self.base.config, base.digest(config)), (self.base.manifest, base.digest(manifest))):
            if base.digest(base.read_file(path, base.MAX_LIBRARY, mode=0o600)) != expected:
                raise SafetyError('changed base files prevent functional enable')
        if base.exists(self.candidate):
            _verify_candidate(base.read_file(self.candidate, self.profile.candidate_bytes, mode=0o600), self.profile)
        else: base.write_new(self.candidate, candidate_raw)
        scan = self.base.active()
        if scan['uncertain'] or scan['owned_mapping_pids'] or scan['foreign_mapping_pids']:
            raise SafetyError('functional enable requires complete scan and no loaded helper')
        self._reject_foreign()
        # Final file operation: an incomplete stage cannot opt in accidentally.
        if base.exists(self.enable):
            if base.read_file(self.enable, len(self.profile.enable), mode=0o600) != self.profile.enable:
                raise SafetyError('foreign enable marker preserved')
        else: base.write_new(self.enable, self.profile.enable)
        return self.status()

    @staticmethod
    def _file(path, wanted, limit):
        try:
            if not base.exists(path): return 'absent'
            return 'owned_exact' if base.digest(base.read_file(path, limit, mode=0o600)) == wanted else 'changed_preserved'
        except (OSError, SafetyError): return 'unreadable_or_unsafe_preserved'

    def _receiver(self, capture):
        if not base.exists(self.receiver_status): return {'state': 'absent'}
        try:
            result = strict_json(self.receiver_status, MAX_RECEIVER_STATUS)
            if (set(result) != RECEIVER_FIELDS or result.get('schema') != self.profile.receiver_schema
                    or result.get('mode') != self.profile.receiver_mode or result.get('capture_id') != capture
                    or result.get('native_frame_association_proven') is not False or result.get('passthrough_authorized') is not False
                    or result.get('diagnostic_atomics_not_source_cohort') is not True):
                raise SafetyError('receiver status scope/session/authority mismatch')
            if type(result.get('source_ready')) is not bool or type(result.get('definition_history_failed')) is not bool:
                raise SafetyError('receiver status Boolean shape invalid')
            for key in ('candidate_bytes', 'observed_device', 'observed_api', 'active_devices', 'pipeline_attempts', 'pipeline_replacements', 'revision', 'updates'):
                if type(result.get(key)) is not int or not 0 <= result[key] <= 0xffffffffffffffff:
                    raise SafetyError('receiver status bounded numeric shape invalid')
            if type(result.get('budget_exhausted')) is not bool or type(result.get('source_reason')) is not str or len(result['source_reason']) > 96:
                raise SafetyError('receiver status finite shape invalid')
            if (result['source_reason'] not in SOURCE_REASONS or result.get('last_primitive_reason') not in PRIMITIVE_REASONS
                    or result['source_ready'] != (result['source_reason'] == 'ready') or result['updates'] > 32
                    or result['active_devices'] > 0xffffffff or result['observed_api'] > 0xffffffff):
                raise SafetyError('receiver status reason/budget/API shape invalid')
            if type(result.get('graphics_gate')) is not str or result['graphics_gate'] not in GRAPHICS_GATES:
                raise SafetyError('receiver graphics gate name invalid')
            for key in ('attachment_format', 'unsupported_dynamic'):
                if type(result.get(key)) is not int or not 0 <= result[key] <= 0xffffffff:
                    raise SafetyError('receiver graphics diagnostic uint32 invalid')
            if result.get('candidate_sha256') != self.profile.candidate_sha256 or result['candidate_bytes'] != self.profile.candidate_bytes:
                raise SafetyError('receiver status candidate pin mismatch')
            return {'state': 'bounded_diagnostic_only', 'value': result}
        except (OSError, SafetyError, ValueError, TypeError) as error:
            return {'state': 'invalid_preserved', 'error': str(error)[:256]}

    def status(self):
        receipt, original, _, _, spent = self._receipt()
        if base.exists(self.base.receipt):
            ordinary = self.base.status()
            # Keep this wrapper small; original bounded creation progress remains
            # available through the unchanged base helper, not duplicated here.
            ordinary.pop('progress', None)
            ordinary['progress_text_omitted'] = True
        else:
            ordinary = {'state': 'not_prepared', 'active_mappings': self.base.active(), 'files': {}}
        files = {str(path): self._file(path, wanted, limit) for path, wanted, limit in
            ((self.enable, receipt['enable_sha256'], len(self.profile.enable)), (self.candidate, self.profile.candidate_sha256, self.profile.candidate_bytes),
             (self.consumed, base.digest(spent), len(spent)))}
        receiver = self._receiver(original['capture_id'])
        all_files = list(files.values()) + list(ordinary['files'].values())
        valid = not any(value not in ('absent', 'owned_exact') for value in all_files) and receiver['state'] != 'invalid_preserved'
        scan = ordinary['active_mappings']
        valid = valid and not self._foreign_files() and not scan['uncertain'] and not scan['foreign_mapping_pids'] and not base.exists(self.base.arm_file)
        return {'schema': 'frame-' + self.profile.stem + '-session-status-v1', 'scope': self.profile.scope, 'profile': self.profile.name,
            'capture_id': original['capture_id'], 'code_group_sha256': receipt['code_group_sha256'],
            'files': files, 'base_session': ordinary, 'receiver_status': receiver, 'integrity_valid': valid,
            'consumed': base.exists(self.consumed), 'capture_arm_present': base.exists(self.base.arm_file),
            'manual_restart_required': bool(scan['uncertain'] or scan['owned_mapping_pids'] or scan['foreign_mapping_pids']),
            'activation_requires_new_native_process': True, 'running_replacements_revoked': False,
            'automatic_restart': False, 'image_acquisition_requested': False,
            'native_frame_association_proven': False, 'passthrough_authorized': False}

    def rollback(self):
        receipt, original, _, _, spent = self._receipt(validate_base=False)
        errors = []
        try:
            if self._foreign_files(): errors.append('foreign profile files preserved')
        except (OSError, SafetyError) as error:
            # Unknown foreign state must not block disabling our exact marker
            # and attempting the independently owned base rollback.
            errors.append('foreign profile scan failed: ' + str(error)[:256])
        # Consume first: even an interrupted rollback can never restage this code group.
        try:
            if base.exists(self.consumed):
                if base.read_file(self.consumed, len(spent), mode=0o600) != spent: raise SafetyError('changed consumed marker preserved')
            else: base.write_new(self.consumed, spent)
        except (OSError, SafetyError) as error: errors.append(str(error))
        try: base.remove_exact(self.enable, receipt['enable_sha256'])
        except (OSError, SafetyError) as error: errors.append(str(error))
        candidate_state = self._file(self.candidate, self.profile.candidate_sha256, self.profile.candidate_bytes)
        if candidate_state not in ('absent', 'owned_exact'): errors.append('changed candidate preserved')
        if base.exists(self.base.receipt):
            try:
                ordinary = self.base.rollback()
                errors.extend(ordinary['errors'])
            except (OSError, SafetyError, ValueError, TypeError, KeyError) as error:
                ordinary = {'state': 'unsafe_base_preserved', 'error': str(error)[:256]}
                errors.append('base rollback refused: ' + str(error)[:256])
        else:
            scan = self.base.active()
            ordinary = {'state': 'base_not_prepared', 'active_mappings': scan}
            if any(base.exists(path) for path in (self.base.library, self.base.config, self.base.manifest)):
                errors.append('base receipt missing; unexpected base files preserved')
            if scan['uncertain'] or scan['owned_mapping_pids'] or scan['foreign_mapping_pids']:
                errors.append('active or unknown mappings require manual review/restart')
        return {'schema': 'frame-' + self.profile.stem + '-session-rollback-v1', 'scope': self.profile.scope,
            'capture_id': original['capture_id'], 'code_group_sha256': receipt['code_group_sha256'],
            'complete': not errors, 'errors': errors, 'base_rollback': ordinary,
            'candidate_status_receipts_preserved': True, 'running_replacements_revoked': False,
            'automatic_restart': False, 'native_frame_association_proven': False, 'passthrough_authorized': False}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('prepare', 'status', 'rollback'))
    parser.add_argument('--profile', choices=(PROFILE, SCENE_PROFILE), default=PROFILE)
    parser.add_argument('--home', type=Path, default=Path('/home/steamos'))
    parser.add_argument('--run-dir', type=Path, required=True)
    parser.add_argument('--proc-root', type=Path, default=Path('/proc'))
    parser.add_argument('--host-fixture', action='store_true', help='Local sandbox fixtures only, never a device approval bypass')
    parser.add_argument('--read-protected-maps', action='store_true', help='Explicit bounded read-only sudo -n maps scan; no prompt')
    parser.add_argument('--library', type=Path)
    parser.add_argument('--library-sha256')
    parser.add_argument('--candidate', type=Path)
    parser.add_argument('--capture-id')
    args = parser.parse_args(argv)
    try:
        session = Session(args.home, args.run_dir, proc_root=args.proc_root, host_fixture=args.host_fixture,
            read_protected_maps=args.read_protected_maps, profile=args.profile)
        if args.command == 'prepare':
            if args.library is None or args.library_sha256 is None or args.candidate is None:
                raise SafetyError('prepare requires exact library/hash and private candidate')
            result = session.prepare(args.library, args.library_sha256, args.candidate, capture_id=args.capture_id)
        else:
            if any(value is not None for value in (args.library, args.library_sha256, args.candidate, args.capture_id)):
                raise SafetyError('source/code selection applies only to prepare')
            result = getattr(session, args.command)()
        print(json.dumps(result, sort_keys=True, indent=2))
        return 0 if result.get('complete', result.get('integrity_valid', True)) else 2
    except (OSError, SafetyError, ValueError, TypeError, KeyError, struct.error) as error:
        print('OUTPUT REPLACEMENT session: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
