#!/usr/bin/env python3
"""Focused per-companion sparse-record tests; no runtime or headset access."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import tempfile

import dashboard_transition_probe_spirv as transform
import dashboard_transition_probe_session as session

checks = 0


def check(value, message):
    global checks
    checks += 1
    if not value:
        raise ValueError(message)


def rejects(action, message):
    try:
        action()
    except (ValueError, transform.ssa.recovery.probe.ProbeError):
        check(True, message)
    else:
        check(False, message)


def test_dominance():
    make = lambda op, *args: transform.ssa.Instruction(op, tuple(args))
    sample = lambda ident: make(87, 9, ident, 1, 2)
    entry = [make(248, 5), sample(603), sample(629), make(250, 100, 6, 7)]
    tail = [make(248, 6), make(62, 504, 994), make(249, 8),
            make(248, 7), make(62, 504, 994), make(249, 8), make(248, 8), make(253), make(56)]
    check(transform.source_sample_dominance(entry+tail)['output_store_paths'] == 2,
          'both original samples dominate both conditional stores')
    branch_only = [item for item in entry if item != sample(629)] + tail
    branch_only.insert(branch_only.index(make(248, 6))+1, sample(629))
    rejects(lambda: transform.source_sample_dominance(branch_only),
            'a genuinely unexecuted source sample is refused, never copied from an undominated SSA')
    late = [make(248, 5), make(62, 504, 994), sample(603), sample(629), make(253)]
    rejects(lambda: transform.source_sample_dominance(late), 'later same-block sample cannot justify earlier store')


def test_index_arithmetic(binary, manifest):
    _, instructions = transform.parse(binary)
    def ident(item):
        return item.operands[1] if item.opcode in (128, 130, 132) else transform.scene.result_id(item)
    definitions = {ident(item): item for item in instructions if ident(item) is not None}

    def evaluate(ident, phase, tag, eye, body, old_shared=False):
        if ident == 693:
            return eye
        if ident == manifest['phase_value_ssa'][body]:
            return phase
        if ident == manifest['pipeline_tag_ssa']:
            return tag
        if old_shared and ident == manifest['companion_slot_index_ssa'][body]:
            return 0
        item = definitions[ident]
        if item.opcode == 43:
            return item.operands[2]
        values = [evaluate(value, phase, tag, eye, body, old_shared) for value in item.operands[2:]]
        if item.opcode == 128:
            return (values[0]+values[1]) & 0xffffffff
        if item.opcode == 130:
            return (values[0]-values[1]) & 0xffffffff
        if item.opcode == 132:
            return (values[0]*values[1]) & 0xffffffff
        raise ValueError('unexpected arithmetic opcode')

    for body, ident in enumerate(manifest['record_slot_ssa']):
        seen = set()
        for phase in range(3):
            for companion in range(8):
                for eye in range(2):
                    index = evaluate(ident, phase, 0x44545001+companion, eye, body)
                    check(index == ((phase*8+companion)*2+eye), 'actual SPIR-V arithmetic keeps every companion/context/selector distinct')
                    seen.add(index)
        check(seen == set(range(48)), 'all and only48 allocated slots are addressable')
        for tag in [0x44545000, 0x44545009, 0xffffffff, 0]:
            route = evaluate(manifest['companion_slot_index_ssa'][body], 0, tag, 0, body)
            check(not route < 8, 'invalid tag cannot pass actual unsigned companion bound')
        check(evaluate(ident, 1, 0x44545001, 0, body, True) ==
              evaluate(ident, 1, 0x44545002, 0, body, True),
              'causal old shared-index mutant recreates the exact companion collision')


def test_collection():
    route_schema = struct.Struct('<Q4I32s32s3Q')
    words = [0] * (session.BUFFER_BYTES//4)
    routes = bytearray(64*route_schema.size)
    for companion in range(2):
        route_schema.pack_into(routes, companion*route_schema.size,
                               1000+companion, session.TAG_FIRST+companion, 0, 0, 1,
                               b'v'*32, b'f'*32, 2, 2, 2)
        for phase in range(3):
            for eye in range(2):
                index = (phase*8+companion)*2+eye
                words[index+1] = 1
                offset = session.CONTROL_WORDS+session.WORDS_PER_SLOT*index
                record = [0]*64
                record[:7] = [0x44545031, 1, phase, eye, 3 if companion == 0 else 0,
                              0x2018018, session.TAG_FIRST+companion]
                record[32:44] = [0x3f000000]*12 if companion == 0 else [0x3d000000]+[session.UNOBSERVED]*11
                record[44:52] = [0x3f800000+companion, 1, 2, 3, 4, 5, 6, 7]
                record[63] = 1
                words[offset:offset+64] = record
    with tempfile.TemporaryDirectory(prefix='dashboard-route-parser-') as temporary:
        root = Path(temporary)
        root.chmod(0o700)
        (root/'dashboard-transition-prepared.json').write_text(json.dumps({'capture_id':'owned-fixture','files':{}}))
        (root/'dashboard-transition-terminal.json').write_text(json.dumps({'collected':True,'queued_phase':3}))
        (root/'dashboard-transition-routes.bin').write_bytes(routes)
        (root/'dashboard-transition-unknown-routes.bin').write_bytes(struct.pack('<3Q',0,0,0))

        def collect():
            (root/'dashboard-transition-records.bin').write_bytes(struct.pack('<%uI'%len(words),*words))
            return session.collect(root)

        result = collect()
        check(len(result['slots']) == 48 and len(result['companions']) == 2,
              'unused companion slots remain absent, not fake completed observations')
        for companion in result['companions']:
            check(companion['active_contexts_complete'], 'both independently submitted companions are complete')
            for context in companion['contexts']:
                first = context['slots'][0]
                check(first['branch_kind'] == (3 if companion['companion_index'] == 0 else 0),
                      'simultaneous opposite guard outcomes remain independently visible')
                check(first['guard_existing_reads_observed'] == (12 if companion['companion_index'] == 0 else 1),
                      'unexecuted remaining guard reads keep their exact sentinel')
                check(first['source603_rgba_words'][0] == 0x3f800000+companion['companion_index'],
                      'original already-executed source values remain route-specific')
        check('sparse_context_complete' not in result, 'no global six-slot completeness can hide a missing active route')
        # Remove one active route's second eye; the other route must not cover it.
        missing = (1*8+1)*2+1
        words[missing+1] = 0
        partial = collect()
        check(partial['companions'][0]['active_contexts_complete'] and
              not partial['companions'][1]['active_contexts_complete'],
              'one companion never completes a different companion with absent data')
        words[missing+1] = 1
        offset = session.CONTROL_WORDS+session.WORDS_PER_SLOT*missing
        words[offset+6] = session.TAG_FIRST
        malformed = collect()
        check(malformed['slots'][missing]['slot_state'] == 'malformed', 'wrong companion identity cannot masquerade as valid slot')
        (root/'dashboard-transition-records.bin').write_bytes(bytes(1600))
        rejects(lambda: session.collect(root), 'old shared six-slot schema is explicitly refused')
    check(not root.exists(), 'owned parser fixture removed after completion')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--shipping', type=Path, required=True)
    parser.add_argument('--record', type=Path, required=True)
    args = parser.parse_args()
    source = args.shipping.read_bytes()
    binary, manifest = transform.instrument(source)
    _, before = transform.parse(source)
    _, after = transform.parse(binary)
    cursor = 0
    for instruction in after:
        if cursor < len(before) and instruction == before[cursor]:
            cursor += 1
    check(cursor == len(before), 'every original instruction remains an exact ordered subsequence')
    check([item for item in before if item.opcode == 62 and item.operands[0] == 504] ==
          [item for item in after if item.opcode == 62 and item.operands[0] == 504],
          'all original actual output stores remain byte-identical')
    check(sum(87 <= item.opcode <= 97 for item in before) == sum(87 <= item.opcode <= 97 for item in after),
          'diagnostic adds no image read')
    check(manifest['source_sample_proof']['output_store_paths'] == 5 and manifest['buffer_bytes'] == 12544,
          'existing raw samples dominate every exact shipping output path; owned buffer stays bounded')
    test_dominance()
    test_index_arithmetic(binary, manifest)
    # Recreate the prior shared first-winner bug in actual generated arithmetic,
    # rather than just comparing two independent host indexing formulas.
    header, instructions = transform.parse(binary)
    changed=[]
    for item in instructions:
        if item.opcode == 130 and item.operands[1] in manifest['companion_slot_index_ssa']:
            item=transform.ssa.Instruction(128,(6,item.operands[1],manifest['uint_zero_ssa'],manifest['uint_zero_ssa']))
        changed.append(item)
    packed=struct.pack('<%uI'%(len(header)+sum(len(item.words()) for item in changed)),
                       *header,*(word for item in changed for word in item.words()))
    rejects(lambda: test_index_arithmetic(packed,manifest), 'old shared first-winner executable arithmetic is causally detected')
    test_collection()
    args.record.write_text(json.dumps({'schema':'dashboard-route-focused-v1','status':'PASS','checks':checks,
        'diagnostic_shader_sha256':hashlib.sha256(binary).hexdigest(),'slots':48,'buffer_bytes':12544,
        'source_sample_dominance':manifest['source_sample_proof'],
        'new_image_reads':0,'actual_gpu_or_hardware_executed':False},indent=2)+'\n')
    print(json.dumps({'status':'PASS','checks':checks,'record':str(args.record)}))


if __name__ == '__main__':
    main()
