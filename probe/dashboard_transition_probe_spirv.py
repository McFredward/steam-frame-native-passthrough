#!/usr/bin/env python3
"""Instrument the exact shipping scene fragment without changing its output.

This diagnostic records existing native registers and the guard samples ALREADY
performed by the shipping fragment. It adds no image read, image output, native
descriptor read, camera or runtime feature. A separately owned Set1 SSBO, actual
enabled fragmentStoresAndAtomics and exact lifetime/queue proof are mandatory.
It is not a deployable shader override for the ordinary release receiver.
"""
from pathlib import Path
import argparse
import hashlib
import json
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import native_fragment_ssa_probe as ssa
import native_fragment_scene_presenter as scene

SOURCE_SHA = '44c4a6c6bf7543b53d14ab6d350d8498143d6c03f70bfb45b9c6812e6b900a49'
SOURCE_BYTES = 31384
WORDS_PER_SLOT = 64
SLOT_COUNT = 6
CONTROL_WORDS = 16
BUFFER_BYTES = (CONTROL_WORDS + WORDS_PER_SLOT * SLOT_COUNT) * 4
MAGIC = 0x44545031


def parse(raw):
    ssa.require(len(raw) >= 20 and len(raw) % 4 == 0 and len(raw) <= 64 * 1024,
                'bounded SPIR-V word stream required')
    words = list(struct.unpack('<%dI' % (len(raw) // 4), raw))
    ssa.require(words[0] == 0x07230203, 'SPIR-V header required')
    result, cursor = [], 5
    while cursor < len(words):
        count, opcode = words[cursor] >> 16, words[cursor] & 0xffff
        ssa.require(count > 0 and count <= len(words) - cursor, 'SPIR-V framing')
        result.append(ssa.Instruction(opcode, tuple(words[cursor + 1:cursor + count])))
        cursor += count
    return words[:5], result


def instrument(raw):
    require = ssa.require
    require(len(raw) == SOURCE_BYTES and hashlib.sha256(raw).hexdigest() == SOURCE_SHA,
            'exact current shipping scene fragment required')
    header, original = parse(raw)
    make = lambda op, *args: ssa.Instruction(op, tuple(args))
    definitions = {scene.result_id(i): i for i in original if scene.result_id(i) is not None}
    require(definitions[693].opcode == 61 and definitions[709].opcode == 124,
            'original current eye selector and flags must be present')
    stores = [i for i in original if i.opcode == 62 and i.operands[0] == 504]
    decoded = [i.operands[1] for i in stores if i.operands[1] != 994]
    require(len(decoded) == 2 and len(set(decoded)) == 2,
            'exact v3/v1 output stores required')
    # The frozen robust generator inserts the v3 branch before the old v1 body.
    kinds = {994: 0, decoded[0]: 3, decoded[1]: 1}
    require(not any(i.opcode == 71 and len(i.operands) == 3
                    and i.operands[1] == 34 and i.operands[2] != 0 for i in original),
            'original scene uses only Set0')
    # Exact existing v3 sampling IDs come from the independently regenerated
    # provenance; a diagnostic must never add a new read at guessed coordinates.
    # Reconstruct only ID provenance from the pinned prefix/profile transform,
    # using the original instruction prefix frozen inside the shipping module.
    # The guard sample pattern itself is sufficient to identify those existing
    # instructions: all explicit samples in the v3 guard have frozen coordinates
    # and occur before its first v3 decoded store.
    guard_values = []
    first_v3_store = next(n for n, i in enumerate(original) if i == make(62, 504, decoded[0]))
    for n, i in enumerate(original[:first_v3_store]):
        if i.opcode != 88:
            continue
        ident = scene.result_id(i)
        following = original[n + 1]
        if following.opcode == 81 and following.operands[0] == 8 \
                and following.operands[2:] == (ident, 1):
            guard_values.append(following.operands[1])
    require(len(guard_values) == 12 and len(set(guard_values)) == 12,
            'twelve existing current-image v3 guard values required')

    bound = header[3]
    def fresh():
        nonlocal bound
        value = bound
        bound += 1
        return value
    declarations, annotations, constants = [], [], {}
    def constant(value):
        if value not in constants:
            constants[value] = fresh()
            declarations.append(make(43, 6, constants[value], value))
        return constants[value]
    def floating(value):
        bits = struct.unpack('<I', struct.pack('<f', value))[0]
        key = ('float', bits)
        if key not in constants:
            constants[key] = fresh()
            declarations.append(make(43, 8, constants[key], bits))
        return constants[key]
    zero, one, two, three = [constant(x) for x in (0, 1, 2, 3)]
    uint_array, block, block_pointer, word_pointer, ssbo = [fresh() for _ in range(5)]
    local_array, local_pointer, local_word, guard_local = [fresh() for _ in range(4)]
    guard_count = constant(12)
    annotations += [make(71, uint_array, 6, 4), make(71, block, 3),
                    make(72, block, 0, 35, 0), make(71, ssbo, 34, 1),
                    make(71, ssbo, 33, 0)]
    declarations += [make(29, uint_array, 6), make(30, block, uint_array),
                     make(32, block_pointer, 2, block), make(32, word_pointer, 2, 6),
                     make(59, block_pointer, ssbo, 2),
                     make(28, local_array, 6, guard_count),
                     make(32, local_pointer, 7, local_array),
                     make(32, local_word, 7, 6)]
    unknown = constant(0x7fc00001)
    blank_guards = fresh()
    declarations += [make(44, local_array, blank_guards, *([unknown] * 12))]
    pipeline_tag = constant(0x44545000)

    def record(rgba, kind):
        body = []
        def emit(op, typ, *args):
            value = fresh()
            body.append(make(op, typ, value, *args))
            return value
        phase_pointer = emit(65, word_pointer, ssbo, zero, zero)
        phase = emit(61, 6, phase_pointer)
        eligible = emit(176, 96, phase, three)
        eligible = emit(167, 96, eligible, emit(176, 96, 693, two))
        coordinate = emit(61, 9, 487)
        for axis in (0, 1):
            value = emit(109, 6, emit(81, 8, coordinate, axis))
            grid = emit(199, 6, value, constant(31))
            eligible = emit(167, 96, eligible, emit(170, 96, grid, zero))
        # Never let a faster distortion-border fallback steal the sole slot
        # before the actual central video contribution runs its guard.
        valid_r1 = None
        central = None
        for axis in (0, 1):
            low = emit(81, 8, 871, axis)
            high = emit(81, 8, 871, axis + 2)
            ordered = emit(186, 96, high, low)
            valid_r1 = ordered if valid_r1 is None else emit(167, 96, valid_r1, ordered)
            q = emit(81, 8, 883, axis)
            inside = emit(167, 96, emit(190, 96, q, floating(.45)), emit(188, 96, q, floating(.55)))
            central = inside if central is None else emit(167, 96, central, inside)
        # Invalid R1 has no meaningful centre. Preserve a distinct invalid-R1
        # receipt instead of misclassifying its unread guards as a rejected atlas.
        eligible = emit(167, 96, eligible, emit(166, 96, central, emit(168, 96, valid_r1)))
        selected, claimed, claimed_end, end = [fresh() for _ in range(4)]
        body += [make(247, end, 0), make(250, eligible, selected, end), make(248, selected)]
        slot = emit(128, 6, emit(132, 6, phase, two), 693)
        claim = emit(65, word_pointer, ssbo, zero, emit(128, 6, slot, one))
        previous = emit(230, 6, claim, one, zero, zero, one, zero)
        won = emit(170, 96, previous, zero)
        body += [make(247, claimed_end, 0), make(250, won, claimed, claimed_end), make(248, claimed)]
        words = [constant(MAGIC), one, phase, 693, constant(kind), 709, pipeline_tag]
        for value, size in [(538, 2), (coordinate, 2), (871, 4), (777, 4),
                            (797, 2), (907, 3), (994, 4), (rgba, 4)]:
            words += [emit(124, 6, emit(81, 8, value, axis)) for axis in range(size)]
        for index in range(12):
            ptr = emit(65, local_word, guard_local, constant(index))
            words.append(emit(61, 6, ptr))
        words += [zero] * (WORDS_PER_SLOT - len(words) - 1)
        words += [emit(169, 6, valid_r1, one, zero)]
        require(len(words) == WORDS_PER_SLOT, 'fixed bounded sparse record')
        offset = emit(128, 6, constant(CONTROL_WORDS), emit(132, 6, slot, constant(WORDS_PER_SLOT)))
        for index, value in enumerate(words):
            ptr = emit(65, word_pointer, ssbo, zero, emit(128, 6, offset, constant(index)))
            body.append(make(62, ptr, value))
        body += [make(249, claimed_end), make(248, claimed_end), make(249, end), make(248, end)]
        return body

    # Build bodies first: their new constants belong before the first function.
    bodies = {n: record(i.operands[1], kinds[i.operands[1]]) for n, i in enumerate(original)
              if i.opcode == 62 and i.operands[0] == 504}
    first_type = next(n for n, i in enumerate(original) if i.opcode in ssa.TYPE_OPS)
    first_function = next(n for n, i in enumerate(original) if i.opcode == 54)
    first_label = next(n for n in range(first_function, len(original)) if original[n].opcode == 248)
    output = []
    for n, item in enumerate(original):
        if n == first_type:
            output += annotations
        if n == first_function:
            output += declarations
        if item.opcode == 62 and item.operands[0] == 504:
            output += bodies[n]
        output.append(item)
        if n == first_label:
            output += [make(59, local_pointer, guard_local, 7), make(62, guard_local, blank_guards)]
        if scene.result_id(item) in guard_values:
            index = guard_values.index(scene.result_id(item))
            pointer, bits = fresh(), fresh()
            output += [make(65, local_word, pointer, guard_local, constant(index)),
                       make(124, 6, bits, scene.result_id(item)), make(62, pointer, bits)]
    # All indices0..11 have already been interned by record(). No late declaration.
    header[3] = bound
    all_words = [*header, *(word for item in output for word in item.words())]
    binary = struct.pack('<%dI' % len(all_words), *all_words)
    parsed_header, parsed = parse(binary)
    require(sum(i.opcode in range(87, 98) for i in parsed)
            == sum(i.opcode in range(87, 98) for i in original), 'no additional image reads')
    require([i for i in parsed if i.opcode == 62 and i.operands[0] == 504] == stores,
            'all original color output stores remain identical')
    index, instructions = 5, []
    for item in parsed:
        if item == make(43, 6, pipeline_tag, 0x44545000):
            instructions.append(index + 3)
        index += 1 + len(item.operands)
    require(len(instructions) == 1 and len(binary) <= 64 * 1024, 'one bounded cold pipeline tag patch')
    record = {'schema': 'native-dashboard-transition-probe-spv-v1',
              'source_sha256': SOURCE_SHA, 'source_bytes': SOURCE_BYTES,
              'output_sha256': hashlib.sha256(binary).hexdigest(), 'output_bytes': len(binary),
              'pipeline_tag_word': instructions[0], 'pipeline_tag_template': 0x44545000,
              'descriptor_set': 1, 'binding': 0, 'slots': SLOT_COUNT,
              'words_per_slot': WORDS_PER_SLOT, 'control_words': CONTROL_WORDS,
              'buffer_bytes': BUFFER_BYTES, 'initial_disabled_phase': 3,
              'guard_values_are_existing_reads': True, 'new_image_reads': 0,
              'native_output_stores_unchanged': True, 'native_descriptor_reads_added': 0,
              'requires_actually_enabled_fragment_stores': True,
              'sample_region': 'existing normalized native q883 in [.45,.55]^2, grid32',
              'missing_slot_is_incomplete_not_guard_rejection': True,
              'invalid_r1_record_flag_word': 63,
              'unobserved_guard_word': 0x7fc00001,
              'runtime_or_device_permission': False}
    return binary, record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    args = parser.parse_args()
    binary, record = instrument(args.source.read_bytes())
    args.output.write_bytes(binary)
    args.manifest.write_text(json.dumps(record, indent=2) + '\n')


if __name__ == '__main__':
    main()
