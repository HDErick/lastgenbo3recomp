"""Compile translated BO3 shaders to SPIR-V and validate every produced module."""
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
parser.add_argument('--input', type=Path, required=True, help='Translation output directory')
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--dxc', type=Path, default=root / 'tools/dxc-1.9.2609/package/bin/x64/dxc.exe')
parser.add_argument('--validator', type=Path, default=root / 'build/native-spirv-tools/tools/spirv-val.exe')
parser.add_argument('--jobs', type=int, default=4)
args = parser.parse_args()
if not args.dxc.is_file() or not args.validator.is_file():
    raise SystemExit('DXC and spirv-val must exist; build the validator first')
if not 1 <= args.jobs <= 16:
    raise SystemExit('--jobs must be 1..16')
if args.output.exists() and any(args.output.iterdir()):
    raise SystemExit('Use a new or empty output directory')
args.output.mkdir(parents=True, exist_ok=True)
translation = json.loads((args.input / 'manifest.json').read_text())


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=120, creationflags=subprocess.CREATE_NO_WINDOW)
    return result.returncode, result.stdout.decode(errors='replace')


def compile_shader(entry):
    stem = Path(entry['file']).stem
    hlsl = args.input / 'hlsl' / (stem + '.hlsl')
    module = args.output / (stem + '.spv')
    record = {'container': entry['file'], 'container_sha256': entry['sha256'],
              'hlsl_sha256': digest(hlsl), 'stage': entry['stage']}
    if record['hlsl_sha256'] != entry['hlsl_sha256']:
        return {**record, 'status': 'input_hash_mismatch'}
    vertex = entry['stage'] == 'vertex'
    command = [str(args.dxc), '-spirv', '-T', 'vs_6_6' if vertex else 'ps_6_6', '-E', 'main',
               '-HV', '2021', '-fspv-target-env=vulkan1.2', '-fvk-use-dx-layout',
               '-Werror=parameter-usage', '-Fo', str(module), str(hlsl)]
    if vertex:
        command.append('-fvk-invert-y')
    try:
        code, log = run(command)
        record['compiler_returncode'] = code
        if code or not module.is_file():
            record.update(status='compile_failed', error=log[-4000:])
        else:
            data = module.read_bytes()
            if len(data) < 20 or len(data) % 4 or struct.unpack_from('<I', data)[0] != 0x07230203:
                raise ValueError('Invalid SPIR-V header')
            validation_code, validation_log = run([str(args.validator), '--target-env', 'vulkan1.2',
                                                  '--scalar-block-layout', str(module)])
            log += validation_log
            record['validator_returncode'] = validation_code
            record.update(status='validated' if validation_code == 0 else 'validation_failed',
                          spirv_bytes=len(data), spirv_sha256=hashlib.sha256(data).hexdigest())
            if validation_code:
                record['error'] = validation_log[-4000:]
    except subprocess.TimeoutExpired:
        log = 'Compiler or validator exceeded 120 seconds'
        record.update(status='timeout', error=log)
    except ValueError as error:
        log = str(error)
        record.update(status='invalid_module', error=log)
    (args.output / (stem + '.log')).write_text(log, encoding='utf-8')
    print(f"{stem}: {record['status']}", flush=True)
    return record


eligible = [e for e in translation['containers'] if e['status'] == 'translated']
with ThreadPoolExecutor(args.jobs) as pool:
    entries = list(pool.map(compile_shader, eligible))
counts = dict(collections.Counter(e['status'] for e in entries))
_, version = run([str(args.dxc), '--version'])
report = {'translation_manifest_sha256': digest(args.input / 'manifest.json'),
          'translator_counts': translation['counts'], 'dxc': {'path': str(args.dxc),
          'version': version.strip(), 'sha256': digest(args.dxc)},
          'validator': {'path': str(args.validator), 'sha256': digest(args.validator)},
          'flags': ['-spirv', '-HV 2021', '-fspv-target-env=vulkan1.2', '-fvk-use-dx-layout',
                    '-Werror=parameter-usage', 'vertex: -fvk-invert-y'],
          'counts': counts, 'shaders': entries,
          'limitations': 'Structural validity only. Native renderer ABI, PM4 identities and pixels unverified.'}
for name in ['dxcompiler.dll', 'dxil.dll']:
    path = args.dxc.parent / name
    if path.is_file():
        report['dxc'][name + '_sha256'] = digest(path)
(args.output / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(counts))
raise SystemExit(0 if entries and counts.get('validated', 0) == len(entries) else 2)
