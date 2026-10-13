"""Validate captured XDK containers and translate each in an isolated process.

Outputs contain game-derived shader data and belong under ignored build/.
The manifest records hashes and failures; success does not establish GPU parity.
"""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import argparse
import collections
import hashlib
import json
import struct
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--input', type=Path, default=root / 'runtime/native-discovery/containers')
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--jobs', type=int, default=4)
args = parser.parse_args()
if args.output.exists() and any(args.output.iterdir()):
    raise SystemExit('Use a new or empty output directory to keep this run reproducible')
if not 1 <= args.jobs <= 16:
    raise SystemExit('--jobs must be 1..16')
args.output.mkdir(parents=True, exist_ok=True)
tool = root / 'build/native-shader-tools/xenos_hlsl.exe'
common = root / 'tools/bo3-shader-translator/shaders/XenosRecomp/shader_common.h'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def inspect(data, name):
    if len(data) < 36:
        raise ValueError('truncated container header')
    signature, virtual, physical, _, constants, definitions, shader = struct.unpack_from('>7I', data)
    vertex = bool(signature & 1)
    if signature & 0xFFFFFF00 != 0x102A1100:
        raise ValueError('unrecognized XDK signature')
    if not 36 <= virtual <= 1048576 or not 0 < physical <= 1048576 or virtual + physical != len(data):
        raise ValueError('header + physical size mismatch')
    if not name.startswith('vs_' if vertex else 'ps_'):
        raise ValueError('filename stage disagrees with container')
    minimum = 36 if vertex else 32
    if shader < 36 or shader + minimum > virtual:
        raise ValueError('shader metadata outside header')
    offset, size, _, _, _, interpolators = struct.unpack_from('>6I', data, shader)
    if not size or size % 12 or offset + size > physical:
        raise ValueError('microcode range outside physical block or not instruction aligned')
    count = (interpolators >> 5) & 31
    if vertex:
        prefix, inputs = struct.unpack_from('>2I', data, shader + 24)
        declaration_end = shader + 36 + 4 * (prefix + inputs + count)
    else:
        inputs = 0
        declaration_end = shader + 32 + 4 * count
    if declaration_end > virtual:
        raise ValueError('input/interpolator declarations outside header')
    for label, offset_table, table_size in [('constant', constants, 32), ('definition', definitions, 20)]:
        if offset_table and (offset_table < 36 or offset_table + table_size > virtual):
            raise ValueError(f'{label} table outside header')
    return {'stage': 'vertex' if vertex else 'fragment', 'header_bytes': virtual,
            'physical_bytes': physical, 'microcode_bytes': size, 'microcode_offset': offset,
            'microcode_sha256': digest(data[virtual + offset:virtual + offset + size]),
            'vertex_inputs': inputs, 'declared_interpolators': count}


def translate(path):
    # Snapshot the blob once. A concurrently running game may add more files.
    data = path.read_bytes()
    entry = {'file': path.name, 'bytes': len(data), 'sha256': digest(data)}
    try:
        entry.update(inspect(data, path.name))
    except ValueError as error:
        return {**entry, 'status': 'invalid_container', 'error': str(error)}
    folder = args.output / 'work' / path.stem
    source = folder / 'input'
    output = folder / 'hlsl'
    source.mkdir(parents=True)
    (source / path.name).write_bytes(data)
    try:
        result = subprocess.run([str(tool), str(source), str(output), str(common)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=60, creationflags=subprocess.CREATE_NO_WINDOW)
        log = result.stdout.decode(errors='replace')
        entry['returncode'] = result.returncode
        translated = output / path.with_suffix('.hlsl').name
        if result.returncode == 0 and translated.is_file() and translated.stat().st_size:
            hlsl = translated.read_bytes()
            target = args.output / 'hlsl' / translated.name
            target.parent.mkdir(exist_ok=True)
            target.write_bytes(hlsl)
            entry.update(status='translated', hlsl_sha256=digest(hlsl), hlsl_bytes=len(hlsl))
        else:
            entry.update(status='translation_failed', error=log[-4000:])
    except subprocess.TimeoutExpired:
        log = 'translator exceeded 60 seconds'
        entry.update(status='translation_timeout', error=log)
    (folder / 'translation.log').write_text(log, encoding='utf-8')
    # Containers remain in ignored work/ for the exact input snapshot.
    print(f"{path.name}: {entry['status']}", flush=True)
    return entry


paths = sorted(args.input.glob('*.bin'))
if not paths:
    raise SystemExit('No container files found')
with ThreadPoolExecutor(args.jobs) as pool:
    entries = list(pool.map(translate, paths))
counts = dict(collections.Counter(entry['status'] for entry in entries))
report = {'tool_sha256': digest(tool.read_bytes()), 'shader_common_sha256': digest(common.read_bytes()),
          'sources': json.loads((root / 'analysis/native_shader_tool_staged_sources.json').read_text()),
          'coverage': 'Observed containers only; loaded PM4 variants and other maps remain unverified',
          'counts': counts, 'stages': dict(collections.Counter(e.get('stage', 'invalid') for e in entries)),
          'containers': entries}
(args.output / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(counts))
raise SystemExit(0 if counts.get('translated', 0) == len(entries) else 2)
