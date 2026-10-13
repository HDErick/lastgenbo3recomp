"""Record CPU texture/ownership fixtures and validate the native presentation modules."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--log', default='analysis/native_texture_upload_tests06.log')
parser.add_argument('--output', default='analysis/native_texture_validation01.json')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
log = (root / args.log).read_text()
texture = re.search(r'(\d+) texture upload checks passed', log)
ownership = re.search(r'(\d+) resolve ownership checks passed', log)
if not texture or not ownership:
    raise SystemExit('Both fixture suites must report success')

def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

inputs = ['nx/native/texture_upload.cpp', 'nx/native/texture_upload.h',
          'nx/native/texture_upload_test.cpp', 'nx/native/resolve_ownership.h',
          'nx/native/resolve_ownership_test.cpp', 'nx/native/native_gpu.cpp',
          'nx/native/CMakeLists.txt', 'nx/native/present.hlsl',
          'scripts/nx/test-native-texture-upload.cmd',
          'build/pc-vulkan/bo3_native_texture_upload_test.exe',
          'build/pc-vulkan/bo3_native_resolve_ownership_test.exe',
          'build/pc-vulkan/rexruntime.dll',
          'tools/nx-sdk/src/graphics/pipeline/texture/util.cpp',
          'tools/nx-sdk/include/rex/graphics/pipeline/texture/util.h',
          'tools/nx-sdk/include/rex/graphics/vulkan/texture_cache.h',
          'tools/nx-sdk/src/graphics/vulkan/texture_cache.cpp']
validator = root / 'build/native-spirv-tools/tools/spirv-val.exe'
modules = []
for stage in ['VS', 'PS', 'DepthPS']:
    path = root / f'build/pc-vulkan/bo3-native/present_spirv.{stage}.spv'
    result = subprocess.run([str(validator), '--target-env', 'vulkan1.1', str(path)],
                            capture_output=True, text=True)
    modules.append({'stage': stage, 'sha256': digest(path),
                    'returncode': result.returncode,
                    'diagnostics': (result.stdout + result.stderr).strip()})
report = {'date': '2026-10-13', 'test_log': args.log,
          'texture_checks': int(texture[1]), 'ownership_checks': int(ownership[1]),
          'game_test_status': 'Not launched at user request',
          'gpu_execution_validated': False,
          'limitations': ['CPU fixtures do not prove GPU sampling or complete scene rendering.',
                          'Tiled fixture generation uses SDK helpers; it is not independent hardware tiling validation.',
                          'Actual resolve conversion/writeback and arbitrary guest CPU reuse tracking remain incomplete.'],
          'presentation_modules': modules, 'validator_sha256': digest(validator),
          'inputs': {name: digest(root / name) for name in inputs}}
(root / args.output).write_text(json.dumps(report, indent=2) + '\n', newline='\n')
print(f"Recorded {texture[1]} texture checks, {ownership[1]} ownership checks, and {len(modules)} presentation modules")
if any(module['returncode'] for module in modules):
    raise SystemExit('Presentation SPIR-V validation failed')
