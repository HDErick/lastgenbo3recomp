"""Derive bounded copy ranges for two exact BO3 memory-export programs.

Offline address/byte-order model only. Does not modify guest memory, dispatch
GPU work, prove live output parity or authorize a runtime memcpy replacement.
"""
from pathlib import Path
import argparse
import hashlib
import json
import struct

# Full canonical big-endian microcode identities, not the SDK filename keys.
PROFILES = {
    '0c9ced036fbfff79ae9c6c9d83eb256c58aa57e98d7c504e1699f0430182e490': (4, 64, 32),
    'd4991999678dd7e26c2c876e06b078cc87b875f5addf173bfdb1beb64eb00bf5': (2, 16, 8),
}


def byte_order_reference(source):
    """Execute this profile's fetch/swizzle/uint16-export/endian steps."""
    if len(source) != 8:
        raise ValueError('An exported uint16x4 element is eight bytes')
    fetched = source[:4][::-1] + source[4:][::-1]  # VF 8-in-32
    values = struct.unpack('<4H', fetched)
    # Every uint16 is exactly representable in float32; max(x,x) is unchanged.
    floated = [struct.unpack('<f', struct.pack('<f', float(value)))[0] for value in values]
    packed = struct.pack('<4H', *(int(floated[index]) for index in (1, 0, 3, 2)))
    return b''.join(packed[i:i + 2][::-1] for i in range(0, 8, 2))  # eA 8-in-16


def require(condition, message):
    if not condition:
        raise ValueError(message)


def plan(record, regs, code_hash):
    require(code_hash in PROFILES, 'Unreviewed exact program')
    blocks, stride_dwords, index_multiplier = PROFILES[code_hash]
    require(record['primitive'] == 1 and not record['indexed'] and not record['explicit_major'],
            'Only implicit point-list auto-index draws are modeled')
    draw = regs[0x21fc]
    require((draw >> 6) & 3 == 2 and (draw >> 8) & 3 == 0, 'Draw initiator is not implicit auto-index')
    count = record['count']
    require(0 < count <= 65535 and draw >> 16 == count, 'Invocation count does not match initiator')
    require(regs[0x2102] == 0 and regs[0x2101] == 0 and regs[0x2100] >= count - 1,
            'Index offset/clamping requires another model')
    require((regs[0x2180] >> 24) & 7 == 7, 'VS export mode does not suppress rasterization')
    require(regs[0x2307] & 511 == 0, 'VS constant bank differs from reviewed capture')
    c0 = tuple(regs[0x4000:0x4004])
    c254 = tuple(regs[0x4000 + 254 * 4:0x4004 + 254 * 4])
    c255 = tuple(regs[0x4000 + 255 * 4:0x4004 + 255 * 4])
    # Format bits 3..7 and 14..15 are unused. The real captures contain both
    # values; unlike the format/endian/numeric fields they do not affect packing.
    require(c0[0] >> 30 == 1 and c0[1] == 0x4b000000 and c0[2] & 0xffff3f07 == 0x4b021a01 and c0[3] >> 23 == 0x96,
            'Stream prefix/format/index origin differs from uint16 copy profile')
    require(c255 == (0, 0x3f800000, 0x40800000, 0x41000000), 'Export index constants differ')
    if blocks == 4:
        require(c254[:2] == (0x42000000, 0x41400000), 'Four-block index constants differ')
    vf0, vf1 = regs[0x4800 + 95 * 2:0x4802 + 95 * 2]
    require(vf0 & 3 in (2, 3) and vf1 & 3 == 2, 'Vertex fetch type/endian differs')
    source_base, source_size = vf0 & 0xfffffffc, ((vf1 >> 2) & 0xffffff) * 4
    target_base, elements = (c0[0] << 2) & 0xffffffff, c0[3] & 0x7fffff
    require(source_base + source_size <= 0x20000000 and target_base + elements * 8 <= 0x20000000,
            'Guest physical ranges outside the initial 512-MB model')
    # All address arithmetic here is integral and exactly representable in
    # float32 for these gated counts. Each eA block starts four elements later.
    ranges = []
    clipped_elements = 0
    for invocation in range(count):
        for block in range(blocks):
            element = invocation * index_multiplier + block * 4
            length_elements = max(0, min(4, elements - element))
            clipped_elements += 4 - length_elements
            if not length_elements:
                continue
            source_offset = invocation * stride_dwords * 4 + block * 32
            length = length_elements * 8
            require(source_offset + length <= source_size, 'A written export reads outside VF extent')
            source, target = source_base + source_offset, target_base + element * 8
            if ranges and ranges[-1]['source'] + ranges[-1]['bytes'] == source and ranges[-1]['target'] + ranges[-1]['bytes'] == target:
                ranges[-1]['bytes'] += length
            else:
                ranges.append({'source': source, 'target': target, 'bytes': length})
    # Every initial source must be read before overlapping exports write into
    # it; this model refuses overlap instead of claiming memcpy is valid there.
    source_end = max((r['source'] + r['bytes'] for r in ranges), default=source_base)
    target_end = max((r['target'] + r['bytes'] for r in ranges), default=target_base)
    require(not ranges or source_end <= target_base or target_end <= source_base, 'Source/destination overlap')
    total = sum(r['bytes'] for r in ranges)
    return {'status': 'address_profile_modeled', 'blocks': blocks, 'invocations': count,
            'input_stride_bytes': stride_dwords * 4, 'destination_stride_bytes': index_multiplier * 8,
            'source_base': source_base, 'source_extent_bytes': source_size,
            'destination_base': target_base, 'destination_extent_bytes': elements * 8,
            'written_bytes': total, 'clipped_elements': clipped_elements,
            'untouched_destination_bytes': elements * 8 - total, 'ranges': ranges}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    entries = []
    for path in sorted(args.input.glob('*.json')):
        record = json.loads(path.read_text())
        require(record.get('version') == 1 and record.get('word_order') == 'little-endian', 'Unsupported capture')
        for key in ('registers', 'microcode'):
            require(Path(record[key]).name == record[key] and '/' not in record[key] and '\\' not in record[key],
                    'Snapshot path must be a basename')
        data = (args.input / record['registers']).read_bytes()
        require(len(data) == 0x5003 * 4, 'Incomplete register snapshot')
        regs = struct.unpack('<' + 'I' * 0x5003, data)
        code = (args.input / record['microcode']).read_bytes()
        require(len(code) >= 12 and len(code) % 4 == 0, 'Incomplete program')
        code_hash = hashlib.sha256(b''.join(code[i:i + 4][::-1] for i in range(0, len(code), 4))).hexdigest()
        entry = {'record': path.name, 'register_sha256': hashlib.sha256(data).hexdigest(),
                 'canonical_microcode_sha256': code_hash}
        try:
            entry.update(plan(record, regs, code_hash))
        except ValueError as error:
            entry.update(status='unsupported_address_profile', error=str(error))
        entries.append(entry)
    require(bool(entries), 'No snapshots')
    modeled = sum(e['status'] == 'address_profile_modeled' for e in entries)
    report = {'script_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'samples': len(entries), 'modeled': modeled,
              'limitations': 'Offline model with exact-program and draw-state guards; no actual input/output bytes, GPU execution, resource ownership or completion validated. Runtime replacement remains blocked.',
              'entries': entries}
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'samples': len(entries), 'modeled': modeled,
                      'failures': [e.get('error') for e in entries if e['status'] != 'address_profile_modeled']}))
    return 0 if modeled == len(entries) else 2


if __name__ == '__main__':
    raise SystemExit(main())
