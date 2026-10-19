#!/usr/bin/env python3
"""Locally derive a full-colour native fragment with a separate coverage sampler.

Only the exact installed native module is accepted. Native RGB reconstruction,
reprojection UV and every original instruction are retained. Generated native
code is private installation output and must never be distributed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

import native_fragment_ssa_probe as ssa
import native_fragment_count1_interface as count1


def transform(original, descriptor_count=1):
    ssa.require(type(descriptor_count) is int and descriptor_count in (1, 3),
                'explicit owned descriptor count1 or3 required')
    ssa.require(len(original) == ssa.ORIGINAL_BYTES and
                hashlib.sha256(original).hexdigest() == ssa.ORIGINAL_SHA256,
                "unsupported native fragment")
    # Reuse the established original-only constant-zero B9 proof before adding
    # our explicit-LOD mask reads. No native layout or executed access changes.
    owned_original = (count1.specialize_owned_binding9_count1(original, original)
                      if descriptor_count == 1 else original)
    native_header, instructions = ssa.parse_module(owned_original)
    header = list(native_header)
    _, store_index = ssa.profile_structure(header, instructions)
    ssa.require(instructions[store_index].operands == (504, 994), "native output changed")
    # IDs 538 (the actual native coded-video sample UV), 899 (native aperture),
    # and 994 (unchanged original RGBA) dominate this exact output store.
    ids = iter(range(header[3], header[3] + 22))
    pointer, variable, loaded, sample, second, value, inner, outer_weight, a, b, total, alpha, output, control_struct, control_pointer, control_variable, float_pointer, valid_address, valid, inverse_valid, ordinary_alpha, chosen_alpha = list(ids)
    make = lambda op, *args: ssa.Instruction(op, tuple(args))
    decorations = [make(71, variable, 34, 1), make(71, variable, 33, 0),
                   make(71, control_struct, 2), make(72, control_struct, 0, 35, 0),
                   make(71, control_variable, 34, 2), make(71, control_variable, 33, 0)]
    declarations = [make(32, pointer, 0, 297), make(59, pointer, variable, 0),
                    make(30, control_struct, 8),
                    make(32, control_pointer, 2, control_struct),
                    make(32, float_pointer, 2, 8),
                    make(59, control_pointer, control_variable, 2)]
    # Explicit LOD zero, same original full-video UV. No atlas/crop/resize,
    # copied eye matrix, eye index inference or RGB multiplication by alpha.
    composition = [make(61, 297, loaded, variable),
                   make(88, 9, sample, loaded, 538, 2, 210),
                   make(88, 9, second, loaded, 798, 2, 210),
                   make(81, 8, value, sample, 0),
                   make(81, 8, inner, second, 0),
                   make(131, 8, outer_weight, 185, 1008),
                   make(133, 8, a, value, outer_weight),
                   make(133, 8, b, inner, 1008),
                   make(129, 8, total, a, b),
                   make(65, float_pointer, valid_address, control_variable, 95),
                   make(61, 8, valid, valid_address),
                   make(131, 8, inverse_valid, 185, valid),
                   make(133, 8, alpha, total, 899),
                   make(133, 8, ordinary_alpha, alpha, valid),
                   make(129, 8, chosen_alpha, ordinary_alpha, inverse_valid),
                   make(82, 9, output, chosen_alpha, 994, 3)]
    result = []
    annotations = declarations_added = False
    for index, instruction in enumerate(instructions):
        if not annotations and instruction.opcode in ssa.TYPE_OPS:
            result.extend(decorations)
            annotations = True
        if not declarations_added and instruction.opcode == 54:
            result.extend(declarations)
            declarations_added = True
        if index == store_index:
            result.extend(composition)
            instruction = make(62, 504, output)
        result.append(instruction)
    ssa.require(annotations and declarations_added, "native declaration framing changed")
    header[3] = chosen_alpha + 1  # first unallocated ID
    words = [*header, *(word for instruction in result for word in instruction.words())]
    binary = struct.pack('<%dI' % len(words), *words)
    return binary, {
        'schema': 'sfnp-native-stream-coverage-v1',
        'original_sha256': hashlib.sha256(original).hexdigest(),
        'output_sha256': hashlib.sha256(binary).hexdigest(),
        'output_bytes': len(binary), 'mask_set': 1, 'mask_binding': 0,
        'owned_binding9_descriptor_count': descriptor_count,
        'mask_type': 'combined-image-sampler R8_UNORM',
        'recording_control_set': 2, 'recording_control_binding': 0,
        'recording_control': 'owned coherent float: 0 opaque native RGB, 1 current exact mask',
        'mask_uv': 'unchanged current native coded-video UVs 538/798 and reconstruction weight 1008',
        'native_rgb_instructions_retained': True,
        'source_and_selected_image_authority_provided_by_runtime': False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('original', 'output', 'record'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    try:
        ssa.require(len({p.absolute() for p in (args.original, args.output, args.record)}) == 3,
                    'distinct paths required')
        ssa.require(not any(p.exists() or p.is_symlink() for p in (args.output, args.record)),
                    'outputs must not exist')
        binary, record = transform(ssa.recovery.read_owned(args.original, ssa.MAX_BYTES))
        with tempfile.TemporaryDirectory(prefix='sfnp-private-coverage-') as directory:
            candidate = Path(directory) / 'candidate.spv'
            candidate.write_bytes(binary)
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(candidate)], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
        record['spirv_val_passed'] = True
        ssa.recovery.write_owned(args.output, binary)
        ssa.recovery.write_owned(args.record, (json.dumps(record, indent=2) + '\n').encode())
        return 0
    except (OSError, ValueError, ssa.probe.ProbeError, subprocess.SubprocessError) as error:
        print('native stream coverage: ' + str(error))
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
