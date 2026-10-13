"""Record the exact PC Vulkan executable, DLLs and renderer inputs."""
from pathlib import Path
import argparse
import hashlib
import json

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', default='analysis/pc_vulkan_provenance04.json')
parser.add_argument('--build-log', default='analysis/pc_vulkan_build04.log')
parser.add_argument('--status', default='Corrected FSI selected in boot04; visual comparison pending')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
files = {}
for name in ['build/pc-vulkan/bo3_zm_vulkan.exe', 'build/pc-vulkan/bo3_zm_vulkan.pdb',
             'build/pc-vulkan/rexruntime.dll', 'build/pc-vulkan/rexgpu-xenos.dll',
             'nx/pc-vulkan/main.cpp', 'nx/pc-vulkan/CMakeLists.txt',
             'nx/pc-vulkan/draw_capture.cpp', 'nx/pc-vulkan/draw_capture.h',
             'nx/pc-vulkan/d3d_trace.cpp', 'nx/pc-vulkan/d3d_trace.h',
             'scripts/nx/launch-pc-vulkan.ps1', 'scripts/nx/prepare-runtime.py',
             'analysis/nx_runtime_sources.json', 'build/nx-generated/zombies/bo3_zm_init.h',
             'build/nx-generated/zombies/bo3_zm_init.cpp',
             'tools/nx-sdk/src/graphics/vulkan/render_target_cache.cpp']:
    path = root / name
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    files[name] = {'bytes': path.stat().st_size, 'sha256': digest}
result = {'date': '2026-10-13', 'compiler': 'Clang 20.1.8, x86-64-v3 Windows Release',
          'renderer': 'Xenos emulation via Vulkan; BO3 native renderer not yet implemented',
          'build_log': args.build_log, 'hardware_status': args.status, 'files': files}
(root / args.output).write_text(json.dumps(result, indent=2) + '\n', newline='\n')
print('Recorded PC Vulkan executable, DLL and source hashes')
