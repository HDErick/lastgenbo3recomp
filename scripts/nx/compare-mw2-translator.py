"""Compare captured BO3 GPU programs with MW2 and the separate BO3 prototype.

The input is the Windows SDK's little-endian DumpUcode output. The comparison
does not import MW2's runtime into the game or prove correct shader execution.
"""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import argparse
import collections
import hashlib
import json
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--input', type=Path, default=root / 'runtime/shader-dumps/native-discovery03')
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--jobs', type=int, default=4)
parser.add_argument('--bo3-prototype', action='store_true')
args = parser.parse_args()
tool = root / 'build/native-shader-tools' / ('mw2_bo3_translate.exe' if args.bo3_prototype else 'mw2_translate.exe')
validator = root / 'build/native-spirv-tools/tools/spirv-val.exe'
if not tool.is_file() or not validator.is_file():
    raise SystemExit('Build mw2_translate and spirv-val first')
if args.output.exists() and any(args.output.iterdir()):
    raise SystemExit('Use a new or empty output directory')
if not 1 <= args.jobs <= 16:
    raise SystemExit('--jobs must be 1..16')
args.output.mkdir(parents=True, exist_ok=True)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def compare(path):
    data = path.read_bytes()
    stage = 'vertex' if path.suffix == '.vert' else 'fragment'
    entry = {'file': path.name, 'stage': stage, 'bytes': len(data), 'sdk_dump_sha256': sha(data)}
    if not data or len(data) % 4:
        return {**entry, 'status': 'invalid_input'}
    big_endian = b''.join(data[i:i + 4][::-1] for i in range(0, len(data), 4))
    entry['microcode_big_endian_sha256'] = sha(big_endian)
    folder = args.output / path.name
    folder.mkdir()
    snapshot = folder / path.name
    snapshot.write_bytes(data)
    module = folder / 'module.spv'
    try:
        result = subprocess.run([str(tool), stage, str(snapshot), str(module)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=20, creationflags=subprocess.CREATE_NO_WINDOW)
        stdout, stderr = result.stdout.decode(errors='replace'), result.stderr.decode(errors='replace')
        (folder / 'translator.stdout.json').write_text(stdout, encoding='utf-8')
        (folder / 'translator.stderr.log').write_text(stderr, encoding='utf-8')
        entry['translator_returncode'] = result.returncode
        try:
            metadata = json.loads(stdout)
        except json.JSONDecodeError:
            return {**entry, 'status': 'tool_failed', 'error': stderr[-4000:] or stdout[-4000:]}
        entry['translation'] = metadata
        if result.returncode or not metadata.get('ok') or not module.is_file():
            return {**entry, 'status': 'translation_failed', 'error': metadata.get('error', stderr)}
        entry['spirv_sha256'] = sha(module.read_bytes())
        entry['spirv_bytes'] = module.stat().st_size
        validation = subprocess.run([str(validator), '--target-env', 'vulkan1.2',
                                     '--scalar-block-layout', str(module)],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    timeout=60, creationflags=subprocess.CREATE_NO_WINDOW)
        log = validation.stdout.decode(errors='replace')
        (folder / 'validation.log').write_text(log, encoding='utf-8')
        entry['validator_returncode'] = validation.returncode
        if validation.returncode:
            entry.update(status='validation_failed', error=log[-4000:])
        elif metadata.get('dropped_exports') or metadata.get('dropped_memory_exports'):
            entry['status'] = 'validated_with_dropped_exports'
        else:
            entry['status'] = 'validated'
    except subprocess.TimeoutExpired:
        entry.update(status='timeout', error='translator/validator timeout')
    print(f"{path.name}: {entry['status']}", flush=True)
    return entry


paths = sorted(p for p in args.input.glob('*.ucode.bin.*') if p.suffix in ('.vert', '.frag'))
if not paths:
    raise SystemExit('No GPU microcode dumps found')
with ThreadPoolExecutor(args.jobs) as pool:
    entries = list(pool.map(compare, paths))
counts = dict(collections.Counter(e['status'] for e in entries))
stages = {stage: dict(collections.Counter(e['status'] for e in entries if e['stage'] == stage))
          for stage in ['vertex', 'fragment']}
source_manifest = 'mw2_shader_tool_staged_sources.json' if args.bo3_prototype else 'mw2_shader_tool_sources.json'
sources = json.loads((root / 'analysis' / source_manifest).read_text())
source_root = 'tools/bo3-mw2-shader-translator' if args.bo3_prototype else 'tools/mw2-recompiled-reference'
for name, expected in sources['files'].items():
    if sha((root / source_root / name).read_bytes()) != expected['sha256']:
        raise SystemExit(f'Translator source changed: {name}')
report = {'reference': sources, 'tool_sha256': sha(tool.read_bytes()),
          'prototype': args.bo3_prototype,
          'driver_sha256': sha((root / 'nx/shader-tools/mw2_translate.cpp').read_bytes()),
          'validator_sha256': sha(validator.read_bytes()),
          'input_word_order': 'little-endian SDK DumpUcode; canonical code hash is big-endian',
          'render_scale': 1, 'target': 'Vulkan 1.2 / scalar block layout',
          'counts': counts, 'stages': stages,
          'failures': dict(collections.Counter(e.get('error', e['status']) for e in entries
                                               if e['status'] != 'validated')),
          'limitations': 'Offline translation and structural validation only. No driver compilation, live draw parity or Switch performance established.',
          'programs': entries}
(args.output / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({'counts': counts, 'stages': stages, 'failures': report['failures']}))
raise SystemExit(0 if counts.get('validated', 0) == len(entries) else 2)
