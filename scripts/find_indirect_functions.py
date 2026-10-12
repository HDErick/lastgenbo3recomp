"""Audit .rdata dispatch tables against generated functions for this BO3 image.

Only propose entries at apparent function boundaries in tables that also contain
registered functions. Interior labels and standalone numeric coincidences are
excluded. Proposals are evidence for review, not executable patches.
"""
import bisect
import json
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x82000000
image = (ROOT / 'analysis/patched_zm_image.bin').read_bytes()
generated = ROOT / 'app/generated/default_zm'
registered = {int(a, 16) for a in re.findall(r'SetFunction\(0x([0-9A-F]+)',
                  (generated / 'bo3_zm_register.cpp').read_text())}
pe = struct.unpack_from('<I', image, 60)[0]
count = struct.unpack_from('<H', image, pe + 6)[0]
optional_size = struct.unpack_from('<H', image, pe + 20)[0]
sections = []
for index in range(count):
    name, size, rva, _, _, _, _, _, _, flags = struct.unpack_from(
        '<8sIIIIIIHHI', image, pe + 24 + optional_size + index * 40)
    sections.append((name.rstrip(b'\0').decode(), BASE + rva, size, flags))

def executable(address):
    return address % 4 == 0 and any(start <= address < start + size and flags & 0x20000000
                                   for _, start, size, flags in sections)

def word(address):
    return struct.unpack_from('>I', image, address - BASE)[0]

# Recover conservative emitted coverage from disassembly comments and labels.
# A label or emitted LR resets the instruction address when blocks have gaps.
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

proposals = {}
tables = []
for name, start, size, flags in sections:
    if name != '.rdata':
        continue
    position = start
    while position + 4 <= start + size:
        table_start = position
        targets = []
        while position + 4 <= start + size and executable(word(position)):
            targets.append(word(position))
            position += 4
        if len(targets) >= 2 and any(a in registered for a in targets):
            for index, address in enumerate(targets):
                if address in registered or address in covered:
                    continue
                previous = word(address - 4)
                # Return/tail jump, zero padding or compiler nop before entry.
                boundary = previous in (0, 0x4E800020, 0x4E800420, 0x60000000, 0x7D084378)
                boundary |= previous & 0xFC000003 == 0x48000000
                if not boundary or word(address) in (0, 0x60000000, 0x7D084378):
                    continue
                proposals.setdefault(address, []).append(table_start + index * 4)
            tables.append(dict(address=f'{table_start:08X}', slots=len(targets)))
        position += 4

# The runtime-reached entry is included regardless of conservative coverage.
proposals.setdefault(0x827351A8, [0x8200BE34])
result = dict(image_sha256=__import__('hashlib').sha256(image).hexdigest(),
              registered_count=len(registered), tables=tables,
              proposed={f'{a:08X}': dict(references=[f'{x:08X}' for x in refs],
                                         first_words=[f'{word(a + 4*i):08X}' for i in range(3)])
                        for a, refs in sorted(proposals.items())})
(ROOT / 'analysis/indirect_function_candidates.json').write_text(json.dumps(result, indent=2))
lines = ['# BO3 .rdata dispatch table entries absent from emitted code.',
         '# Evidence: analysis/indirect_function_candidates.json.']
lines.extend(f'0x{a:08X} = {{ name = "sub_{a:08X}" }}' for a in sorted(proposals))
(ROOT / 'analysis/indirect_function_hints.toml').write_text('\n'.join(lines) + '\n')
print(f'{len(tables)} tables; {len(proposals)} candidate functions')
for address, refs in list(sorted(proposals.items()))[:20]:
    print(f'{address:08X}: {word(address):08X}, referenced at {refs[0]:08X}')
