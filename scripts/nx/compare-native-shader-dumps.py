"""Compare full container programs with SDK GPU microcode dumps, by exact bytes.

SDK DumpUcode writes host-endian dwords. Container programs are big-endian.
Pass --same-run only when both sets came from the same named launch.
"""
from pathlib import Path
import argparse
import collections
import hashlib
import json
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--containers', type=Path, required=True)
parser.add_argument('--gpu-dumps', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--same-run', action='store_true')
args = parser.parse_args()
loaded = {}
for path in sorted(args.gpu_dumps.glob('*.ucode.bin.*')):
    if path.suffix not in ('.vert', '.frag'):
        continue
    data = path.read_bytes()
    if len(data) % 4:
        raise SystemExit(f'Unaligned SDK microcode dump: {path.name}')
    big_endian = b''.join(data[i:i + 4][::-1] for i in range(0, len(data), 4))
    stage = 'vertex' if path.suffix == '.vert' else 'fragment'
    key = (stage, hashlib.sha256(big_endian).hexdigest())
    loaded.setdefault(key, []).append(path.name)
entries = []
matched = set()
for path in sorted(args.containers.glob('*.bin')):
    data = path.read_bytes()
    if len(data) < 36:
        raise SystemExit(f'Truncated container: {path.name}')
    signature, virtual, physical = struct.unpack_from('>3I', data)
    shader = struct.unpack_from('>I', data, 24)[0]
    if signature & 0xFFFFFF00 != 0x102A1100 or virtual + physical != len(data) or shader + 24 > virtual:
        raise SystemExit(f'Invalid container header: {path.name}')
    offset, size = struct.unpack_from('>2I', data, shader)
    if not size or size % 12 or offset + size > physical:
        raise SystemExit(f'Invalid microcode range: {path.name}')
    stage = 'vertex' if signature & 1 else 'fragment'
    digest = hashlib.sha256(data[virtual + offset:virtual + offset + size]).hexdigest()
    key = (stage, digest)
    matches = loaded.get(key, [])
    if matches:
        matched.add(key)
    entries.append({'container': path.name, 'stage': stage, 'microcode_sha256': digest, 'matches': matches})
counts = dict(collections.Counter(e['stage'] + ('_matched' if e['matches'] else '_unmatched') for e in entries))
report = {'same_run': args.same_run, 'counts': counts,
          'loaded_unique_programs': len(loaded), 'loaded_matched_programs': len(matched),
          'entries': entries,
          'loaded_unmatched': [{'stage': key[0], 'microcode_sha256': key[1], 'files': names}
                               for key, names in loaded.items() if key not in matched],
          'limitations': 'Exact program matching only. Unmatched code may be linked/patched, a different container subprogram, or absent from the capture. Draw/state associations and shader semantics remain unverified.'}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({'counts': counts, 'loaded_unique': len(loaded), 'loaded_matched': len(matched)}))
