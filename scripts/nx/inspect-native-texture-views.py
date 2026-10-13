"""Check BO3 view metadata against SPIR-V descriptor declarations and types.

This checks the compiled interface, not texture contents or GPU execution.
"""
from pathlib import Path
import argparse
import collections
import hashlib
import json
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--input', type=Path, required=True)
parser.add_argument('--previous', type=Path)
parser.add_argument('--previous-modules', type=Path)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
manifest_path = args.input / 'manifest.json'
manifest = json.loads(manifest_path.read_text())
previous = {e['file']: e for e in json.loads(args.previous.read_text())['programs']} if args.previous else {}


def descriptors(data):
    if len(data) % 4 or len(data) < 20:
        raise ValueError('Invalid SPIR-V size')
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    if words[0] != 0x07230203:
        raise ValueError('Invalid SPIR-V magic')
    types, variables, decorations = {}, {}, collections.defaultdict(dict)
    cursor = 5
    while cursor < len(words):
        count, opcode = words[cursor] >> 16, words[cursor] & 65535
        if not count or cursor + count > len(words):
            raise ValueError('Truncated SPIR-V instruction')
        operands = words[cursor + 1:cursor + count]
        if opcode in (25, 27, 32):  # image, sampled image, pointer
            types[operands[0]] = (opcode, operands[1:])
        elif opcode == 59:  # variable
            variables[operands[1]] = (operands[0], operands[2])
        elif opcode == 71 and operands[1] in (33, 34):  # binding, descriptor set
            decorations[operands[0]][operands[1]] = operands[2]
        cursor += count
    result = {}
    for variable, decor in decorations.items():
        if decor.get(34) != 1:
            continue
        binding = decor[33]
        if binding in result:
            raise ValueError('Duplicate texture binding')
        pointer, storage = variables[variable]
        pointer_op, pointer_args = types[pointer]
        sampled_op, sampled_args = types[pointer_args[1]]
        image_op, image_args = types[sampled_args[0]]
        if (storage, pointer_op, pointer_args[0], sampled_op, image_op) != (0, 32, 0, 27, 25):
            raise ValueError('Not a combined image sampler in UniformConstant')
        # sampled type, Dim, depth, arrayed, multisampled, sampled, format
        if image_args[3:6] != (0, 0, 1):
            raise ValueError('Unexpected array/multisample/sampled image shape')
        result[binding] = image_args[1]
    return result


def normalize_bindings(data):
    words = list(struct.unpack('<' + 'I' * (len(data) // 4), data))
    cursor = 5
    while cursor < len(words):
        count, opcode = words[cursor] >> 16, words[cursor] & 65535
        if opcode == 71 and count == 4 and words[cursor + 2] == 33:
            # Only texture bindings can exceed 31 in this ABI; all other sets
            # retain their original small binding numbers.
            words[cursor + 3] %= 32
        cursor += count
    return words


checked, mixed, changed, byte_identical, rebinding_only = [], [], [], 0, 0
for entry in manifest['programs']:
    if not entry['status'].startswith('validated'):
        raise SystemExit(f"Unvalidated module: {entry['file']}")
    metadata = entry['translation']
    if metadata.get('texture_view_abi') != 2:
        raise SystemExit('Expected BO3 texture-view ABI 2')
    masks = metadata['texture_view_masks']
    if len(masks) != 32 or any(mask & ~7 for mask in masks):
        raise SystemExit('Invalid texture view masks')
    expected = {slot + 32 * kind: (1, 3, 2)[kind]
                for slot, mask in enumerate(masks) for kind in range(3) if mask & (1 << kind)}
    if metadata['texture_mask'] != sum(1 << slot for slot, mask in enumerate(masks) if mask):
        raise SystemExit('Texture slot mask disagrees with typed views')
    module = (args.input / entry['file'] / 'module.spv').read_bytes()
    if hashlib.sha256(module).hexdigest() != entry['spirv_sha256']:
        raise SystemExit('Module hash differs from manifest')
    if descriptors(module) != expected:
        raise SystemExit(f"SPIR-V descriptor/type mismatch: {entry['file']}")
    mixed_slots = [slot for slot, mask in enumerate(masks) if mask & (mask - 1)]
    if mixed_slots:
        mixed.append({'file': entry['file'], 'slots': mixed_slots,
                      'bindings': expected, 'view_masks': masks})
    old = previous.get(entry['file'])
    if old and old['status'] == 'validated':
        if args.previous_modules:
            old_module = (args.previous_modules / entry['file'] / 'module.spv').read_bytes()
            if hashlib.sha256(old_module).hexdigest() != old['spirv_sha256']:
                raise SystemExit('Previous module hash differs from previous manifest')
            if normalize_bindings(old_module) != normalize_bindings(module):
                raise SystemExit(f"Unexpected shader code change: {entry['file']}")
        if old['spirv_sha256'] == entry['spirv_sha256']:
            byte_identical += 1
        else:
            changed.append(entry['file'])
            if args.previous_modules:
                rebinding_only += 1
    checked.append({'file': entry['file'], 'stage': entry['stage'], 'descriptors': len(expected)})
report = {'check': 'ABI 2 metadata vs SPIR-V set/binding/image type declarations',
          'manifest_sha256': hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
          'script_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
          'checked_modules': len(checked), 'mixed_programs': mixed,
          'maximum_stage_descriptors': {stage: max(e['descriptors'] for e in checked if e['stage'] == stage)
                                        for stage in ('vertex', 'fragment')},
          'previous_clean_byte_identical': byte_identical, 'previous_clean_changed': changed,
          'previous_clean_rebinding_only': rebinding_only if args.previous_modules else None,
          'limitations': 'Declaration checks only. No live descriptor allocation, texture view creation, driver execution or pixel parity.'}
args.output.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({key: report[key] for key in ('checked_modules', 'maximum_stage_descriptors',
                                             'previous_clean_byte_identical', 'previous_clean_rebinding_only')}))
