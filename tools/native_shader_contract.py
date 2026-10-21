"""Recognize unchanged native shader logic across ID/debug/compiler metadata changes.

The declaration order/operand roles are numeric metadata, not native shader code.
Every rewritten module must still equal the independently reviewed logic digest.
"""
import hashlib
import json
from pathlib import Path
import struct

_METADATA = json.loads(Path(__file__).with_suffix('.json').read_text())
_RULES = {(r['op'], r['operands']): r for r in _METADATA['rules']}
_DEBUG = frozenset((0, 2, 3, 4, 5, 6, 7, 8, 317, 330))


def normalize(raw, stage='fragment'):
    profile = _METADATA[stage]
    if hashlib.sha256(raw).hexdigest() == profile['sha256']:
        return raw
    if len(raw) < 20 or len(raw) > 65536 or len(raw) % 4:
        raise ValueError('unsupported native shader framing')
    words = struct.unpack('<%dI' % (len(raw) // 4), raw)
    if words[0] != 0x07230203 or words[1] != profile['header'][1] or words[4] or not 0 < words[3] <= 65536:
        raise ValueError('unsupported native shader header')
    instructions, mapping, defined, cursor = [], {}, set(), 5
    for_count = 0
    while cursor < len(words):
        count, op = words[cursor] >> 16, words[cursor] & 65535
        if not count or count > len(words) - cursor:
            raise ValueError('unsupported native shader instruction framing')
        operands = words[cursor + 1:cursor + count]
        if op in _DEBUG:
            if op == 7:
                if not operands or not 0 < operands[0] < words[3] or operands[0] in defined:
                    raise ValueError('duplicate native shader result')
                defined.add(operands[0])
        else:
            rule = _RULES.get((op, count - 1))
            if rule is None:
                raise ValueError('native shader opcode contract changed')
            if rule['result'] >= 0:
                ident = operands[rule['result']]
                if not 0 < ident < words[3] or ident in defined or for_count >= len(profile['result_ids']):
                    raise ValueError('native shader result contract changed')
                defined.add(ident);mapping[ident] = profile['result_ids'][for_count];for_count += 1
            instructions.append((words[cursor], operands, rule))
        cursor += count
    if for_count != len(profile['result_ids']):
        raise ValueError('native shader declaration contract changed')
    result = list(profile['header'])
    for opcode, operands, rule in instructions:
        result.append(opcode)
        for i, word in enumerate(operands):
            if rule['id_mask'] & (1 << i):
                if word not in mapping:
                    raise ValueError('undefined native shader dependency')
                word = mapping[word]
            result.append(word)
    canonical = struct.pack('<%dI' % len(result), *result)
    if len(canonical) != profile['bytes'] or hashlib.sha256(canonical).hexdigest() != profile['sha256']:
        raise ValueError('native shader logic or interface changed')
    return canonical
