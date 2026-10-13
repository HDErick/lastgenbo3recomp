"""Summarize GPU-consumed BO3 memexport state without committing raw dumps."""
from pathlib import Path
import argparse
import collections
import hashlib
import json
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--input', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--programs', type=Path, default=Path(__file__).resolve().parents[2] / 'analysis/bo3_mw2_comparison03.json')
args = parser.parse_args()
programs = {e['microcode_big_endian_sha256']: e for e in json.loads(args.programs.read_text())['programs']
            if e['stage'] == 'vertex'}
entries = []
for path in sorted(args.input.glob('*.json')):
    record = json.loads(path.read_text())
    if record.get('version') != 1 or record.get('word_order') != 'little-endian':
        raise SystemExit('Unsupported probe format')
    files = {}
    for kind in ('registers', 'microcode'):
        name = record[kind]
        if Path(name).name != name or '/' in name or '\\' in name:
            raise SystemExit('Probe files must be local basenames')
        files[kind] = (args.input / name).read_bytes()
    if len(files['registers']) != 0x5003 * 4 or len(files['microcode']) < 12 or len(files['microcode']) % 4:
        raise SystemExit('Incomplete probe snapshot')
    regs = struct.unpack('<' + 'I' * 0x5003, files['registers'])
    base = regs[0x2307] & 511
    if base + 255 >= 512:
        raise SystemExit('VS float constant window wraps; requires separate investigation')

    def constant(index):
        offset = 0x4000 + (base + index) * 4
        return regs[offset:offset + 4]

    code_hash = hashlib.sha256(b''.join(files['microcode'][i:i + 4][::-1]
                                      for i in range(0, len(files['microcode']), 4))).hexdigest()
    program = programs.get(code_hash)
    full_fetch_slots = []
    if program:
        subs = program['translation']['vertex_fetch_subs']
        full_fetch_slots = [group * 3 + sub for group in range(32) for sub in range(3)
                            if subs[sub] & (1 << group)]
    # c0 is the address-stream candidate for the two reviewed copy programs.
    # It must not be assumed to describe an unrelated memory-export shader.
    reviewed_copy = program and program['file'] in ('shader_5B9B7484417FB9B6.ucode.bin.vert',
                                                   'shader_A4827826E2589916.ucode.bin.vert')
    stream = constant(0)
    entry = {**record, 'register_sha256': hashlib.sha256(files['registers']).hexdigest(),
             'canonical_microcode_sha256': code_hash,
             'matched_program': program['file'] if program else None,
             'reviewed_copy_program': bool(reviewed_copy),
             'vs_constant_base': base,
             'constants': {str(i): [f'{word:08X}' for word in constant(i)] for i in (0, 254, 255)},
             'stream_c0_candidate': {'valid_prefixes': stream[0] >> 30 == 1 and stream[1] >> 23 == 0x96
                                           and stream[2] >> 23 == 0x96 and stream[3] >> 23 == 0x96,
                           'base_address': (stream[0] << 2) & 0xffffffff,
                           'element_count': stream[3] & 0x7fffff,
                           'format': (stream[2] >> 8) & 63, 'number_format': (stream[2] >> 16) & 7,
                           'endian': stream[2] & 7, 'swap_red_blue': bool(stream[2] & (1 << 19))},
             'vertex_fetch_words': {str(slot): [f'{word:08X}' for word in regs[0x4800 + slot * 2:0x4802 + slot * 2]]
                                    for slot in full_fetch_slots},
             'index_offset': regs[0x2102], 'draw_initiator': f'{regs[0x21fc]:08X}',
             'edram_mode': regs[0x2208] & 7}
    entries.append(entry)
if not entries:
    raise SystemExit('No complete GPU-consumed memexport snapshots found')
report = {'samples': len(entries),
          'programs': dict(collections.Counter(e['canonical_microcode_sha256'] for e in entries)),
          'script_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
          'limitations': 'Draw-entry register state only; eA after shader arithmetic, input/index bytes, output bytes and completion are not captured. CPU copy equivalence is not proven.',
          'entries': entries}
args.output.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({'samples': len(entries), 'programs': report['programs']}))
