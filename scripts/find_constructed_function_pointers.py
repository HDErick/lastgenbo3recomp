"""Review adjacent PPC address-construction pairs absent from MP translation.

These are candidates, not automatic discovery hints. Covered instruction labels
and addresses without a plausible preceding function boundary are excluded.
"""
import json
import re
import struct
from pathlib import Path

root = Path(__file__).resolve().parents[1]
image = (root / 'analysis/patched_mp_image.bin').read_bytes()
base = 0x82000000
generated = root / 'app/multiplayer/generated/default'
registered = {int(a, 16) for a in re.findall(r'SetFunction\(0x([0-9A-F]+)',
                  (generated / 'bo3_mp_register.cpp').read_text())}
covered = set()
for source in generated.glob('*recomp*.cpp'):
    current = None
    for line in source.read_text().splitlines():
        if match := re.match(r'DEFINE_REX_FUNC\(sub_([0-9A-F]+)\)', line):
            current = int(match[1], 16)
        elif match := re.match(r'loc_([0-9A-F]+):', line):
            current = int(match[1], 16)
        elif current is not None and line.startswith('\t// '):
            covered.add(current)
            current += 4
        elif match := re.match(r'\tctx.lr = 0x([0-9A-F]+);', line):
            current = int(match[1], 16)

pe = struct.unpack_from('<I', image, 60)[0]
section_count = struct.unpack_from('<H', image, pe + 6)[0]
optional_size = struct.unpack_from('<H', image, pe + 20)[0]
code_ranges = []
for i in range(section_count):
    _, size, rva, _, _, _, _, _, _, flags = struct.unpack_from(
        '<8sIIIIIIHHI', image, pe + 24 + optional_size + i * 40)
    if flags & 0x20000000:
        code_ranges.append((base + rva, base + rva + size))

def word(address):
    return struct.unpack_from('>I', image, address - base)[0]

def executable(address):
    return address % 4 == 0 and any(lo <= address < hi for lo, hi in code_ranges)

proposed = {}
for lo, hi in code_ranges:
    for pc in range(lo, hi - 4, 4):
        first, second = word(pc), word(pc + 4)
        if first >> 26 != 15 or (first >> 16) & 31:
            continue
        register = (first >> 21) & 31
        upper = (first & 0xFFFF) << 16
        if second >> 26 == 14 and ((second >> 16) & 31) == register:
            low = second & 0xFFFF
            address = (upper + (low if low < 0x8000 else low - 0x10000)) & 0xFFFFFFFF
        elif second >> 26 == 24 and ((second >> 21) & 31) == register:
            address = upper | (second & 0xFFFF)
        else:
            continue
        if not executable(address) or address in registered or address in covered:
            continue
        previous = word(address - 4)
        boundary = previous in (0, 0x4E800020, 0x4E800420, 0x60000000, 0x7D084378)
        boundary |= previous & 0xFC000003 == 0x48000000
        if not boundary or word(address) in (0, 0x60000000, 0x7D084378):
            continue
        proposed.setdefault(f'{address:08X}', {'references': [],
            'first_words': [f'{word(address + i * 4):08X}' for i in range(4)]})[
                'references'].append(f'{pc:08X}')

path = root / 'analysis/mp_constructed_function_candidates.json'
path.write_text(json.dumps(proposed, indent=2), newline='\n')
print(f'{len(proposed)} constructed-address candidates')
for address, data in proposed.items():
    print(address, data['references'], data['first_words'])
