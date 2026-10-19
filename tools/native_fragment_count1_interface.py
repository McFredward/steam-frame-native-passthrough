"""Pinned constant-zero native image interface proof, without image repacking.

Only the owned replacement interface is specialized. No native instruction,
coordinate, source image, scene colour, alpha, layout or runtime is changed.
"""
import hashlib
import struct
import native_fragment_ssa_probe as ssa
require = ssa.require


def result_id(item):
    return (item.operands[0] if item.opcode in ssa.FIRST_RESULT else
            item.operands[1] if item.opcode in ssa.TYPED_RESULT else None)


def specialize_owned_binding9_count1(binary, original):
    """Fresh owned interface types; never rewrite a native layout or its shader.

    The pinned executable can address only descriptor0. This separate offline
    variant declares that exact extent instead of borrowing a native count3
    interface or asserting that a count1 original pipeline would be valid.
    """
    require(hashlib.sha256(original).hexdigest() == ssa.ORIGINAL_SHA256, "exact original specialization profile required")
    _, native = ssa.parse_module(original)
    definitions = {result_id(i): i for i in native if result_id(i) is not None}
    require(definitions[289] == ssa.Instruction(59, (288, 289, 0))
            and definitions[288] == ssa.Instruction(32, (288, 0, 287))
            and definitions[287] == ssa.Instruction(28, (287, 286, 102))
            and definitions[102].opcode == 43 and definitions[102].operands[2] == 3
            and definitions[110] == ssa.Instruction(43, (90, 110, 0)), "pinned image-array type/length/index profile differs")
    accesses = [i for i in native if 289 in ssa.value_inputs(i)]
    require(accesses == [ssa.Instruction(65, (290, 598, 289, 110)),
                         ssa.Instruction(65, (290, 624, 289, 110))], "all native B9 accesses must be proven constant0")
    require({i.operands for i in native if i.opcode == 71 and i.operands[0] == 289}
            == {(289, 33, 9), (289, 34, 0)}, "exact B9 descriptor decorations required")
    header, instructions = ssa.parse_module(binary)
    require([i for i in instructions if 289 in ssa.value_inputs(i)] == accesses,
            "composed module added a B9 descriptor access")
    one, array, pointer = range(header[3], header[3]+3)
    declarations = [ssa.Instruction(43, (definitions[102].operands[0], one, 1)),
        ssa.Instruction(28, (array, 286, one)), ssa.Instruction(32, (pointer, 0, array))]
    result, replacements = [], 0
    for item in instructions:
        if item == definitions[289]:
            result.extend(declarations); item = ssa.Instruction(59, (pointer, 289, 0)); replacements += 1
        result.append(item)
    require(replacements == 1 and pointer < 65536, "single owned descriptor-array specialization required")
    words = [*header[:3], pointer+1, header[4]]+[word for i in result for word in i.words()]
    return struct.pack("<%dI" % len(words), *words)

