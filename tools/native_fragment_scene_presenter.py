#!/usr/bin/env python3
"""Offline same-R1 scene-contribution atlas and strict current-image format guard.

The current image must carry all twelve fixed raw-G codes. This is format
recognition, not body authentication, liveness or authorization for native MR.
The existing original expression is retained on invalid geometry/signature.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

import native_fragment_r1_presenter as presenter

ssa, recovery, require = presenter.ssa, presenter.recovery, presenter.require
SPEC = Path(__file__).resolve().parents[1] / "docs/protocols/scene-contribution-atlas-v1.json"
PROFILE_PIN = "2c1ca215715ec7d2d7591eb2a2e8071a15e11fb5f96530927d10ceab7405e30c"
SPEC_V2 = SPEC.with_name("scene-contribution-atlas-v2.json")
SPEC_V3 = SPEC.with_name("scene-contribution-atlas-v3.json")
PROFILE_V2_PIN = "68ad7a14b094729faffb3ff23cc5b42b2605ae9535e28d55071cd76533aecfc5"
PROFILE_V3_PIN = "7efb2d98cc99d17d9bd920780797b6b647bc4b74441d2b97f8e72dc61f002521"
MARKER_V3 = "MRMASK/8 current-r1-scene-contribution-atlas-v3-format-only"
MARKER_ROBUST = "MRMASK/9 current-r1-scene-contribution-atlas-v1-v3-format-only"
MARKER = "MRMASK/5 current-r1-scene-contribution-atlas-v1-format-only"
MARKER_V2 = "MRMASK/6 current-r1-scene-contribution-atlas-v2-format-only"
MARKER_DUAL = "MRMASK/7 current-r1-scene-contribution-atlas-v1-v2-format-only"
EXTRA_OPS = frozenset((83, 88, 167, 186, 188, 190, 245, 330))


def profile_contract(profile):
    try:
        encoded = json.dumps(profile, sort_keys=True, separators=(",", ":"),
                             ensure_ascii=True, allow_nan=False).encode("ascii")
    except (TypeError, ValueError, UnicodeError):
        recovery.fail("exact finite frozen scene atlas profile required")
    require(hashlib.sha256(encoded).hexdigest() in (PROFILE_PIN, PROFILE_V2_PIN, PROFILE_V3_PIN),
            "exact frozen scene atlas profile required; no caller geometry or codes")
    # Seal the accepted contract against subsequent caller mutation.
    result = json.loads(encoded)
    guard = result["guard"]
    digest = hashlib.sha256(guard["signature_preimage"].encode("ascii")).digest()
    codes = ([guard["code_levels"][v & 3] for v in digest[:12]]
             if result["revision"] == 3 else list(digest[:12]))
    require(digest.hex() == guard["signature_sha256"] and codes == guard["codes"],
            "frozen format signature derivation differs")
    return result


def load_profile(path=SPEC):
    return profile_contract(recovery.load_json(Path(path))[0])


def guard_center(profile, index):
    """Frozen per-cell origins for v3; legacy origin/stride stays unchanged."""
    guard = profile["guard"]
    origin = (guard["origins"][index] if profile["revision"] == 3 else
              [guard["origin"][axis] + guard["stride"][axis] * index for axis in (0, 1)])
    return [origin[axis] + guard["size"][axis] / 2 for axis in (0, 1)]


def panel_mapping(profile):
    """Affine construction template only; coverage is replaced by the tile map."""
    p = profile_contract(profile)
    width, height = p["canvas"]
    def panel(rectangle):
        x, y, w, h = rectangle
        return [w / width, 0., x / width, 0., h / height, y / height]
    x, y = p["coverage"]["data_origins"][0]
    return {"schema": presenter.SCHEMA, "scope": presenter.SCOPE, "chart": presenter.CHART,
            "source_encoding": presenter.ENCODING, "rgb_convention": "straight",
            "color": panel(p["scene"]),
            "coverage": panel([x, y, p["coverage"]["size"][0], p["coverage"]["tile_rows"]])}


def parse_module(binary):
    """Bounded framing for this generated profile, not a general validator."""
    require(type(binary) is bytes and 20 <= len(binary) <= ssa.MAX_BYTES and len(binary) % 4 == 0,
            "bounded generated module bytes required")
    words = struct.unpack("<%dI" % (len(binary) // 4), binary)
    require(words[0] == 0x07230203 and words[1] == 0x00010100 and words[4] == 0
            and 0 < words[3] <= 65536, "generated SPIR-V1.1 header required")
    instructions, at = [], 5
    while at < len(words):
        count, opcode = words[at] >> 16, words[at] & 65535
        require(count > 0 and count <= len(words) - at and opcode in ssa.ALLOWED | EXTRA_OPS,
                "invalid generated instruction framing")
        instructions.append(ssa.Instruction(opcode, words[at + 1:at + count]))
        at += count
    return list(words[:5]), instructions


def result_id(item):
    if item.opcode in (83, 88, 167, 186, 188, 190, 245):
        return item.operands[1]
    return presenter.atlas.result_id(item)


def pack_module(header, instructions):
    words = [*header, *(word for item in instructions for word in item.words())]
    binary = struct.pack("<%dI" % len(words), *words)
    require(len(binary) <= ssa.MAX_BYTES, "bounded guarded module required")
    return binary


def _scene_candidate(original, profile, enabled, descriptor_count):
    """Reuse pinned native cones; replace only the owned coordinate/composition."""
    candidate, record = presenter.transform(original, panel_mapping(profile), enabled, descriptor_count)
    record.update(source_rgb_convention="scene_contribution", zero_coverage_rgb_is_zero=False,
                  color_policy="native scene RGB reconstruction times original aperture/tint once; never times coverage",
                  output_convention="scene RGB contribution plus independent coverage alpha",
                  coverage_source_contract="current same-eye " + profile['profile'] + " numeric G",
                  source_transfer_scope=profile["source_transfer"],
                  additive_or_refraction_contract="additive contribution retained; refraction not established",
                  geometry_policy="clamp panel global q to scene texel centers; original aperture uses unchanged original q")
    record.pop("coverage",None) # Surrogate affine is not the executed mask map.
    record.pop("mapping",None)
    if not enabled:
        return candidate,record
    header,instructions=parse_module(candidate)
    ids=record["new_ssa_ids"]
    function=next(i for i,x in enumerate(instructions) if x.opcode==54)
    bound=header[3]
    def fresh():
        nonlocal bound
        value=bound;bound+=1;return value
    make=lambda op,*args:ssa.Instruction(op,tuple(args))
    declarations=[]
    constants={x.operands[2]:x.operands[1] for x in instructions[:function]
               if x.opcode==43 and x.operands[0]==8}
    def constant(value):
        bits=struct.unpack("<I",struct.pack("<f",value))[0]
        if bits not in constants:
            constants[bits]=fresh();declarations.append(make(43,8,constants[bits],bits))
        return constants[bits]
    def emit(target,opcode,kind,*values):
        value=fresh();target.append(make(opcode,kind,value,*values));return value
    low,high=constant(.5/2048),constant(1-.5/2048)
    lo2,hi2=fresh(),fresh();declarations += [make(44,39,lo2,low,low),make(44,39,hi2,high,high)]
    clamp=[];global_q=emit(clamp,12,39,1,43,883,lo2,hi2)
    # The two generated atlas q extracts are shared by color and coverage.
    extracts=[i for i,x in enumerate(instructions) if x.opcode==81 and x.operands[0]==8
              and x.operands[2]==883 and x.operands[1]>=1010]
    require(len(extracts)==2 and {instructions[i].operands[3] for i in extracts}=={0,1},
            "exact two inherited global-q extracts required")
    mask=[]
    qx,qy=[emit(mask,81,8,global_q,i) for i in (0,1)]
    size=profile['coverage']['size'][0];rows=profile['coverage']['tile_rows']
    px=emit(mask,131,8,emit(mask,133,8,qx,constant(size)),constant(.5))
    py=emit(mask,131,8,emit(mask,133,8,qy,constant(size)),constant(.5))
    # Equivalent to floor(max(py,0)/tile_rows) clamped to3, without a new integer
    # conversion or dynamic descriptor access. Halos continue the global image.
    above=[emit(mask,190,96,py,constant(v)) for v in (rows,rows*2,rows*3)]
    right=emit(mask,169,96,above[1],above[2],above[0])
    if profile['revision']==1:
        origin_x=emit(mask,169,8,right,constant(1072),constant(16))
        origin_y=emit(mask,169,8,above[1],constant(2448),constant(48))
    else:
        origin_x,origin_y=[constant(v) for v in profile['coverage']['data_origins'][0]]
        for cond,origin in zip(above,profile['coverage']['data_origins'][1:]):
            origin_x=emit(mask,169,8,cond,constant(origin[0]),origin_x)
            origin_y=emit(mask,169,8,cond,constant(origin[1]),origin_y)
    tile_base=constant(0)
    for cond,value in zip(above,(rows,rows*2,rows*3)):
        tile_base=emit(mask,169,8,cond,constant(value),tile_base)
    if profile['revision']==1:
        x=emit(mask,136,8,emit(mask,129,8,emit(mask,129,8,origin_x,px),constant(.5)),constant(2112))
        local_y=emit(mask,131,8,py,tile_base)
        y=emit(mask,136,8,emit(mask,129,8,emit(mask,129,8,origin_y,local_y),constant(.5)),constant(2752))
    else:
        local_y=emit(mask,131,8,py,tile_base)
        rx=emit(mask,169,8,right,local_y,px)
        ry=emit(mask,169,8,right,emit(mask,131,8,constant(size-1),px),local_y)
        x=emit(mask,136,8,emit(mask,129,8,emit(mask,129,8,origin_x,rx),constant(.5)),constant(profile['canvas'][0]))
        y=emit(mask,136,8,emit(mask,129,8,emit(mask,129,8,origin_y,ry),constant(.5)),constant(profile['canvas'][1]))
    dynamic_qa=make(80,39,ids["mapped_qa"],x,y)
    output=[];clamped=False;replaced_mask=0;replaced_rgb=0
    for index,item in enumerate(instructions):
        if index in extracts:
            if not clamped:output.extend(clamp);clamped=True
            item=make(81,item.operands[0],item.operands[1],global_q,item.operands[3])
        if result_id(item)==ids["mapped_qa"]:
            require(item.opcode==80,"inherited mapped mask q must be a vec2 construct")
            output.extend(mask);item=dynamic_qa;replaced_mask+=1
        if result_id(item)==ids["premultiplied_rgb"]:
            require(item==make(142,30,ids["premultiplied_rgb"],ids["combined_color_rgb"],ids["clamped_coverage"]),
                    "exact inherited straight-source RGB multiplication required")
            item=make(83,30,ids["premultiplied_rgb"],ids["combined_color_rgb"]);replaced_rgb+=1
        output.append(item)
    require(clamped and replaced_mask==replaced_rgb==1,"one dynamic mask map and one contribution composition required")
    function=next(i for i,x in enumerate(output) if x.opcode==54);output[function:function]=declarations
    header[3]=bound
    binary=pack_module(header,output)
    record.update(output_bytes=len(binary),output_sha256=hashlib.sha256(binary).hexdigest(),
                  coverage_coordinate_policy="four global-halo tiles selected from clamped current q; no CPU eye table",
                  new_ssa_ids={**ids,"scene_contribution_rgb":ids["premultiplied_rgb"],"scene_global_q":global_q,
                               "mask_origin_x":origin_x,"mask_origin_y":origin_y,"mask_tile_base":tile_base,
                               **({"rotated_pixel_x":rx,"rotated_pixel_y":ry} if profile['revision']>=2 else {})},
                  mask_coordinate_ssa={"qx":qx,"qy":qy,"pixel_x":px,"pixel_y":py,
                                       "above":above,"right":right,"mapped_x":x,"mapped_y":y})
    return binary,record


def transform(original, profile, enabled=True, descriptor_count=1):
    require(type(enabled) is bool, "enabled must be an explicit boolean")
    require(type(descriptor_count) is int and descriptor_count in (1, 3),
            "explicit owned descriptor count1 or3 required")
    p = profile_contract(profile)
    candidate, inherited = _scene_candidate(original, p, enabled, descriptor_count)
    record = {**inherited, "schema": "native-fragment-scene-presenter-v1",
              "scope": "OFFLINE_CURRENT_R1_SCENE_CONTRIBUTION_ATLAS_ONLY", "profile": p,
              "format_guard": "twelve ordered raw-G inclusive code windows; format only",
              "authenticates_body": False, "proves_liveness": False,
              "scene_format_guard_implemented": enabled,
              "live_source_validity_guard_implemented": False,
              "original_fallback_store_value": 994, "guard_branch_before_panels": True,
              "source_mip_contract": "owned single mip; equal min/mag filter; level0 only",
              "guard_samples_explicit_lod0": 12 if enabled else 0,
              "first_cell_mismatch_guard_samples": 1 if enabled else 0,
              "first_cell_mismatch_skips_remaining_samples": True,
              "additional_panel_samples_explicit_lod0": 4 if enabled else 0,
              "original_samples_unchanged": True, "guard_ssa": {}}
    if not enabled:
        return candidate, record
    header, instructions = parse_module(candidate)
    original_header, native = ssa.parse_module(original)
    _, original_store = ssa.profile_structure(original_header, native)
    original_function = next(i for i, item in enumerate(native) if item.opcode == 54)
    function = next(i for i, item in enumerate(instructions) if item.opcode == 54)
    prefix = native[original_function:original_store]
    splice = function + len(prefix)
    require(instructions[function:splice] == prefix,
            "entire original function before SSA994 output must remain unchanged")
    panel = instructions[splice:]
    require(panel[-3:] == [ssa.Instruction(62, (504, inherited["new_ssa_ids"]["output"])),
                           ssa.Instruction(253, ()), ssa.Instruction(56, ())],
            "exact inherited candidate output tail required")
    panel = panel[:-2]
    bound = header[3]
    def fresh():
        nonlocal bound
        value = bound
        bound += 1
        return value
    make = lambda opcode, *args: ssa.Instruction(opcode, tuple(args))
    declarations, constants = [], {}
    def constant(value):
        bits = struct.unpack("<I", struct.pack("<f", value))[0]
        if bits not in constants:
            constants[bits] = fresh()
            declarations.append(make(43, 8, constants[bits], bits))
        return constants[bits]
    def emit(target, opcode, kind, *args):
        value = fresh()
        target.append(make(opcode, kind, value, *args))
        return value
    zero, one = constant(0.), constant(1.)
    # Branches can vary across a quad. New samples therefore use explicit LOD0;
    # the two original samples and all original SSA994 computation stay intact.
    changed = 0
    for index, item in enumerate(panel):
        if item.opcode == 87:
            require(len(item.operands) == 4 and item.operands[1] >= original_header[3],
                    "only four new panel samples may change LOD policy")
            panel[index] = make(88, *item.operands, 2, zero)  # ImageOperands Lod.
            changed += 1
    require(changed == 4, "exactly four additional panel samples required")
    geometry, checks = [], []
    minimum = [emit(geometry, 81, 8, 871, i) for i in (0, 1)]
    maximum = [emit(geometry, 81, 8, 871, i) for i in (2, 3)]
    span = [emit(geometry, 131, 8, hi, lo) for lo, hi in zip(minimum, maximum)]
    u = [emit(geometry, 81, 8, 538, i) for i in (0, 1)]
    for lo, hi, size, coordinate in zip(minimum, maximum, span, u):
        checks.extend((emit(geometry, 190, 96, lo, zero), emit(geometry, 188, 96, hi, one),
                       emit(geometry, 186, 96, size, zero), emit(geometry, 190, 96, coordinate, lo),
                       emit(geometry, 188, 96, coordinate, hi)))
    def conjunction(target, values):
        result = values[0]
        for value in values[1:]:
            result = emit(target, 167, 96, result, value)
        return result
    geometry_valid = conjunction(geometry, checks)
    (signature_header, remaining_header, panel_header, invalid_geometry, invalid_first_signature,
     invalid_signature, signature_merge, first_signature_merge, final_merge) = [fresh() for _ in range(9)]
    geometry += [make(247, final_merge, 0), make(250, geometry_valid, signature_header, invalid_geometry)]
    first_signature, signature, windows, cells = [], [], [], []
    # Both operands are values from the original first B9/B25 descriptor loads.
    # They dominate this splice; no descriptor load/access chain is added.
    sampled_image = emit(first_signature, 86, 297, 599, 600)
    width, height = p["canvas"]
    guard = p["guard"]
    remaining_sampled_image = None
    for index, code in enumerate(guard["codes"]):
        target = first_signature if index == 0 else signature
        if index == 1:
            # OpSampledImage results may only be consumed in their own block.
            # Recombine the same original image/sampler values, never reload.
            remaining_sampled_image = emit(signature, 86, 297, 599, 600)
        center = guard_center(p, index)
        normalized = [center[0] / width, center[1] / height]
        xy = [emit(target, 129, 8, lo, emit(target, 133, 8, size, constant(position)))
              for lo, size, position in zip(minimum, span, normalized)]
        coordinate = emit(target, 80, 39, *xy)
        sample = emit(target, 88, 9, sampled_image if index == 0 else remaining_sampled_image, coordinate, 2, zero)
        g = emit(target, 81, 8, sample, 1)
        low = constant((code - guard["code_tolerance"]) / 255.)
        high = constant((code + guard["code_tolerance"]) / 255.)
        lower, upper = emit(target, 190, 96, g, low), emit(target, 188, 96, g, high)
        windows.extend((lower, upper))
        cells.append({"index": index, "code": code, "center_canvas": center,
                      "center_normalized": normalized, "coordinate": coordinate,
                      "sample": sample, "g": g, "low": low, "high": high,
                      "lower": lower, "upper": upper})
    first_valid = conjunction(first_signature, windows[:2])
    first_signature += [make(247, first_signature_merge, 0),
                        make(250, first_valid, remaining_header, invalid_first_signature)]
    signature_valid = emit(signature, 167, 96, first_valid, conjunction(signature, windows[2:]))
    signature += [make(247, signature_merge, 0), make(250, signature_valid, panel_header, invalid_signature)]
    # Exact original SSA994 on both invalid routes, not a recomputed decoder or
    # a select that evaluates retargeted reads before deciding which to keep.
    guarded = geometry + [make(248, signature_header)] + first_signature
    guarded += [make(248, remaining_header)] + signature + [make(248, panel_header)]
    guarded += panel + [make(249, signature_merge), make(248, invalid_signature), make(62, 504, 994),
                        make(249, signature_merge), make(248, signature_merge), make(249, first_signature_merge),
                        make(248, invalid_first_signature), make(62, 504, 994), make(249, first_signature_merge),
                        make(248, first_signature_merge), make(249, final_merge),
                        make(248, invalid_geometry), make(62, 504, 994), make(249, final_merge),
                        make(248, final_merge), make(253), make(56)]
    result = instructions[:splice] + guarded
    result[function:function] = declarations
    markers = [i for i, item in enumerate(result) if item.opcode == 330]
    require(len(markers) == 1, "one inherited guard marker required")
    marker = MARKER if p['revision']==1 else MARKER_V2 if p['revision']==2 else MARKER_V3
    encoded = marker.encode("ascii") + b"\0"
    encoded += b"\0" * (-len(encoded) % 4)
    result[markers[0]] = make(330, *struct.unpack("<%dI" % (len(encoded) // 4), encoded))
    require(bound <= 65536, "bounded guarded result IDs required")
    header[3] = bound
    binary = pack_module(header, result)
    record.update(output_sha256=presenter.atlas.probe.digest(binary), output_bytes=len(binary), candidate_marker=marker,
                  guard_ssa={"geometry_valid": geometry_valid, "signature_valid": signature_valid,
                             "first_signature_valid": first_valid,
                             "signature_header": signature_header, "remaining_header": remaining_header,
                             "panel_header": panel_header, "invalid_first_signature": invalid_first_signature,
                             "invalid_geometry": invalid_geometry, "invalid_signature": invalid_signature,
                             "signature_merge": signature_merge, "first_signature_merge": first_signature_merge,
                             "final_merge": final_merge,
                             "sampled_image": sampled_image, "remaining_sampled_image": remaining_sampled_image,
                             "zero": zero, "cells": cells},
                  guard_original_image_value=599, guard_original_sampler_value=600,
                  original_function_prefix_unchanged=True)
    return binary, record


def transform_dual(original, enabled=True, descriptor_count=1):
    """Historical frozen v2/v1 candidate; not the production shipping path."""
    return _transform_secondary(original, load_profile(SPEC_V2), enabled,
                                descriptor_count, MARKER_DUAL)


def transform_robust(original, enabled=True, descriptor_count=1):
    """Recognize robust v3, otherwise guarded v1 or exact original output.

    Both paths use the current original invocation/image/chart, never a sticky
    decision or a cross-eye join. V2 is intentionally not a third guard.
    """
    return _transform_secondary(original, load_profile(SPEC_V3), enabled,
                                descriptor_count, MARKER_ROBUST)


def _transform_secondary(original, secondary, enabled, descriptor_count, marker_text):
    """Compose one frozen full-resolution format with complete legacy fallback."""
    revision = secondary['revision']
    v1, old = transform(original, load_profile(), enabled, descriptor_count)
    if not enabled:
        return v1, {**old, 'dual_profile_guard': False, 'supported_profiles': [1, revision]}
    secondary_binary, full = transform(original, secondary, True, descriptor_count)
    header, instructions = parse_module(v1)
    _, second = parse_module(secondary_binary)
    native_header, _ = ssa.parse_module(original)
    first_function = next(i for i, x in enumerate(instructions) if x.opcode == 54)
    second_function = next(i for i, x in enumerate(second) if x.opcode == 54)
    base = header[3]
    owned = sorted({result_id(x) for x in second
                    if result_id(x) is not None and result_id(x) >= native_header[3]})
    names = {ident: base + index for index, ident in enumerate(owned)}
    bound = base + len(owned)
    make = lambda op, *args: ssa.Instruction(op, tuple(args))
    def renamed(item):
        a = item.operands
        # Explicit literal positions: no ID substitution into float words,
        # ExtInst numbers, component indices or ImageOperands/control masks.
        literal = {43: set(range(2, len(a))), 81: set(range(3, len(a))),
                   79: set(range(4, len(a))), 82: set(range(4, len(a))),
                   12: {3}, 88: {4}, 247: {1}}.get(item.opcode, set())
        return make(item.opcode, *(v if i in literal else names.get(v, v)
                                  for i, v in enumerate(a)))
    declarations = []
    for item in second[:second_function]:
        if result_id(item) not in names: continue
        candidate = renamed(item)
        if item.opcode in ssa.TYPE_OPS:
            equivalent = next((x for x in instructions[:first_function] + declarations
                               if x.opcode == candidate.opcode
                               and x.operands[1:] == candidate.operands[1:]), None)
            if equivalent is not None:
                names[result_id(item)] = result_id(equivalent)
                continue
        declarations.append(candidate)
    guard2 = full['guard_ssa']
    panel_start = next(i for i, x in enumerate(second)
                       if x == make(248, guard2['panel_header'])) + 1
    panel_end = next(i for i in range(panel_start, len(second))
                     if second[i] == make(249, guard2['signature_merge']))
    second_panel = [renamed(x) for x in second[panel_start:panel_end]]
    require(second_panel[-1] == make(62, 504, names[full['new_ssa_ids']['output']]),
            'exact independent v2 panel store required')
    def fresh():
        nonlocal bound
        ident = bound; bound += 1; return ident
    constants = {}
    def constant(value):
        bits = struct.unpack('<I', struct.pack('<f', value))[0]
        if bits not in constants:
            constants[bits] = fresh()
            declarations.append(make(43, 8, constants[bits], bits))
        return constants[bits]
    def emit(target, op, kind, *args):
        ident = fresh(); target.append(make(op, kind, ident, *args)); return ident
    def conjunction(target, values):
        answer = values[0]
        for value in values[1:]: answer = emit(target, 167, 96, answer, value)
        return answer
    zero, one = constant(0), constant(1)
    more, fail_first, guard_merge, use_v2, use_v1, final_merge = [fresh() for _ in range(6)]
    first, remaining, windows, cells = [], [], [], []
    minima = [emit(first, 81, 8, 871, axis) for axis in (0, 1)]
    maxima = [emit(first, 81, 8, 871, axis) for axis in (2, 3)]
    spans = [emit(first, 131, 8, hi, lo) for hi, lo in zip(maxima, minima)]
    false_value = emit(first, 186, 96, zero, one)
    guard = full['profile']['guard']; canvas = full['profile']['canvas']
    sampled = None
    for index, code in enumerate(guard['codes']):
        target = first if index == 0 else remaining
        if index in (0, 1): sampled = emit(target, 86, 297, 599, 600)
        center = guard_center(full['profile'], index)
        xy = [emit(target, 129, 8, lo, emit(target, 133, 8, span, constant(pos/extent)))
              for lo, span, pos, extent in zip(minima, spans, center, canvas)]
        coordinate = emit(target, 80, 39, *xy)
        texel = emit(target, 88, 9, sampled, coordinate, 2, zero)
        value = emit(target, 81, 8, texel, 1)
        lo = emit(target, 190, 96, value, constant((code-guard['code_tolerance'])/255))
        hi = emit(target, 188, 96, value, constant((code+guard['code_tolerance'])/255))
        windows.extend((lo, hi))
        cells.append({'index': index, 'center_canvas': center, 'sample': texel, 'g': value})
    first_valid = conjunction(first, windows[:2])
    rest_valid = conjunction(remaining, windows[2:])
    accepted = fresh()
    first += [make(247, guard_merge, 0), make(250, first_valid, more, fail_first),
              make(248, more)] + remaining + [make(249, guard_merge),
              make(248, fail_first), make(249, guard_merge), make(248, guard_merge),
              make(245, 96, accepted, rest_valid, more, false_value, fail_first),
              make(247, final_merge, 0), make(250, accepted, use_v2, use_v1),
              make(248, use_v2)] + second_panel + [make(249, final_merge), make(248, use_v1)]
    entry = make(248, old['guard_ssa']['signature_header'])
    out = []; inserted = 0; merge_next = False
    for item in instructions:
        if merge_next:
            require(item == make(249, old['guard_ssa']['final_merge']),
                    'legacy valid-geometry exit required')
            out += [make(249, final_merge), make(248, final_merge), item]
            merge_next = False
        else:
            out.append(item)
            if item == entry: out += first; inserted += 1
            if item == make(248, old['guard_ssa']['first_signature_merge']): merge_next = True
    require(inserted == 1, 'one legacy signature-entry splice required')
    out[first_function:first_function] = declarations
    marker = marker_text.encode('ascii') + b'\0'
    marker += b'\0' * (-len(marker) % 4)
    positions = [i for i, item in enumerate(out) if item.opcode == 330]
    require(len(positions) == 1, 'one dual format marker required')
    out[positions[0]] = make(330, *struct.unpack('<%dI' % (len(marker)//4), marker))
    header[3] = bound
    binary = pack_module(header, out)
    record = {**old, 'schema': 'native-fragment-scene-presenter-dual-v1-v%d' % revision,
              'supported_profiles': [1, revision], 'profile_v%d' % revision: full['profile'],
              'coverage_source_contract': 'current same-eye selected frozen v1/v%d numeric G' % revision,
              'dual_profile_guard': True, 'profile_selection': 'valid v%d first, otherwise complete guarded v1, otherwise exact original994' % revision,
              'candidate_marker': marker_text, 'output_bytes': len(binary),
              'output_sha256': hashlib.sha256(binary).hexdigest(),
              'v%d_ssa_ids' % revision: {key: names.get(value, value) for key, value in full['new_ssa_ids'].items()},
              'v%d_guard_cells' % revision: cells, 'v%d_guard_first_valid' % revision: first_valid,
              'v%d_guard_valid' % revision: accepted, 'dual_merge': final_merge,
              'guard_sample_budget': {'v%d' % revision: 12, 'legacy_v1': 13, 'ordinary_first_cells_mismatch': 2},
              'extra_pixel_planes': False, 'alpha_threshold': False}
    return binary, record


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("original", "output", "record"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--profile", type=Path, default=SPEC)
    parser.add_argument("--disabled", action="store_true")
    parser.add_argument("--owned-descriptor-count", type=int, choices=(1, 3), default=1)
    profiles = parser.add_mutually_exclusive_group()
    profiles.add_argument("--dual-profile", action="store_true", help="Historical v2/v1 executable only")
    profiles.add_argument("--robust-profile", action="store_true", help="Shipping robust v3/v1 executable")
    args = parser.parse_args(argv)
    try:
        require(len({p.absolute() for p in (args.original, args.profile, args.output, args.record)}) == 4,
                "distinct original/profile/output/record paths required")
        require(not any(p.exists() or p.is_symlink() for p in (args.output, args.record)), "exclusive outputs required")
        profile, profile_sha = recovery.load_json(SPEC_V3 if args.robust_profile else args.profile)
        binary, record = ((transform_robust if args.robust_profile else transform_dual)(
                              recovery.read_owned(args.original, ssa.MAX_BYTES),
                              not args.disabled, args.owned_descriptor_count)
                          if args.robust_profile or args.dual_profile else transform(recovery.read_owned(args.original, ssa.MAX_BYTES), profile,
                                                              not args.disabled, args.owned_descriptor_count))
        with tempfile.TemporaryDirectory(prefix="private-fragment-guard-") as directory:
            path = Path(directory) / "guarded.spv"
            path.write_bytes(binary)
            result = subprocess.run(["spirv-val", "--target-env", "vulkan1.1", str(path)],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15, check=False)
            require(result.returncode == 0, "SPIR-V validation failed; no output published")
        record.update(spirv_val_passed=True, profile_file_sha256=profile_sha)
        recovery.write_owned(args.output, binary)
        recovery.write_owned(args.record, (json.dumps(record, indent=2, sort_keys=True, allow_nan=False) + "\n").encode())
        print(json.dumps({"scope": record["scope"], "output_sha256": record["output_sha256"],
                          "passthrough_authorized": False}))
        return 0
    except (presenter.atlas.probe.ProbeError, OSError, ValueError, TypeError, KeyError, IndexError,
            struct.error, subprocess.SubprocessError) as error:
        print("native guarded presenter: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
