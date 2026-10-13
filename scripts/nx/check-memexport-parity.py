"""Compare modeled copy output against completed emulated GPU readback bytes."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import importlib.util

spec = importlib.util.spec_from_file_location('copy_model', Path(__file__).with_name('plan-memexport-copies.py'))
model = importlib.util.module_from_spec(spec)
spec.loader.exec_module(model)
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--input', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
entries = []
for file in sorted(a.input.glob('*.json')):
    record = json.loads(file.read_text())
    entry = {'record': file.name}
    try:
        def read(name):
            basename = record[name]
            if Path(basename).name != basename or '/' in basename or '\\' in basename:
                raise ValueError('Unsafe capture basename')
            return (file.parent / basename).read_bytes()
        registers = read('registers')
        if len(registers) != 0x5003 * 4:
            raise ValueError('Incomplete register snapshot')
        regs = struct.unpack('<' + 'I' * 0x5003, registers)
        code = read('microcode')
        if not code or len(code) % 4:
            raise ValueError('Invalid microcode size')
        identity = hashlib.sha256(b''.join(code[i:i+4][::-1] for i in range(0, len(code), 4))).hexdigest()
        plan = model.plan(record, regs, identity)
        source, before, actual = read('input'), read('before'), read('output')
        if len(source) != plan['source_extent_bytes'] or len(before) != plan['destination_extent_bytes'] or len(actual) != len(before):
            raise ValueError('Capture extent mismatch')
        expected = bytearray(before)
        nonzero_written = 0
        for region in plan['ranges']:
            begin = region['source'] - plan['source_base']
            dest = region['target'] - plan['destination_base']
            for offset in range(0, region['bytes'], 8):
                element = model.byte_order_reference(source[begin+offset:begin+offset+8])
                nonzero_written += sum(x != 0 for x in element)
                expected[dest+offset:dest+offset+8] = element
        mismatches = sum(x != y for x, y in zip(expected, actual))
        entry.update(status='matched' if not mismatches else 'mismatch',
                     canonical_microcode_sha256=identity, invocations=plan['invocations'],
                     written_bytes=plan['written_bytes'], compared_bytes=len(before),
                     mismatched_bytes=mismatches,
                     nonzero_written_bytes=nonzero_written,
                     changed_destination_bytes=sum(x != y for x, y in zip(before, expected)),
                     input_sha256=hashlib.sha256(source).hexdigest(),
                     expected_sha256=hashlib.sha256(expected).hexdigest(),
                     gpu_output_sha256=hashlib.sha256(actual).hexdigest())
    except (ValueError, OSError, KeyError) as error:
        entry.update(status='blocked', error=str(error))
    entries.append(entry)
matched = sum(x['status'] == 'matched' for x in entries)
result = {'samples': len(entries), 'matched': matched,
          'script_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
          'limitations': 'Exact captured programs/states and sampled bytes only. Readback must have been synchronous; this does not establish all states or native renderer correctness.',
          'entries': entries}
a.output.write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps({'samples': len(entries), 'matched': matched, 'failures': [x for x in entries if x['status'] != 'matched']}))
raise SystemExit(0 if entries and matched == len(entries) else 2)
