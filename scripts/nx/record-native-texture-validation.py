"""Record CPU texture/ownership fixtures and validate the native presentation modules."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--log', default='analysis/native_texture_upload_tests10.log')
parser.add_argument('--output', default='analysis/native_texture_validation01.json')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
log = (root / args.log).read_text()
texture = re.search(r'(\d+) texture upload checks passed', log)
ownership = re.search(r'(\d+) resolve ownership checks passed', log)
resolve = re.search(r'(\d+) color resolve checks passed', log)
pipeline = re.search(r'(\d+) pipeline state checks passed', log)
watch = re.search(r'(\d+) texture write-watch checks passed', log)
guards = re.search(r'Physical allocation guards: PASS \((\d+) cases\)', log)
residency = re.search(r'(\d+) texture residency checks passed', log)
if not texture or not ownership or not resolve or not pipeline or not watch or not guards or not residency:
    raise SystemExit('All seven fixture suites must report success')

def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

inputs = ['nx/native/texture_upload.cpp', 'nx/native/texture_upload.h',
          'nx/native/texture_upload_test.cpp', 'nx/native/resolve_ownership.h',
          'nx/native/resolve_ownership_test.cpp', 'nx/native/native_gpu.cpp',
          'nx/native/color_resolve.cpp', 'nx/native/color_resolve.h',
          'nx/native/color_resolve_test.cpp',
          'nx/native/pipeline_state.h', 'nx/native/pipeline_state_test.cpp',
          'nx/native/texture_validity.h', 'nx/native/texture_watch_test.cpp',
          'nx/native/texture_residency.h', 'nx/native/texture_residency_test.cpp',
          'nx/native/physical_access.h', 'nx/native/physical_access_test.cpp',
          'scripts/nx/prepare-runtime.py',
          'nx/native/CMakeLists.txt', 'nx/native/present.hlsl',
          'scripts/nx/test-native-texture-upload.cmd',
          'build/pc-vulkan/bo3_native_texture_upload_test.exe',
          'build/pc-vulkan/bo3_native_resolve_ownership_test.exe',
          'build/pc-vulkan/bo3_native_color_resolve_test.exe',
          'build/pc-vulkan/bo3_native_pipeline_state_test.exe',
          'build/pc-vulkan/bo3_native_texture_watch_test.exe',
          'build/pc-vulkan/bo3_native_texture_residency_test.exe',
          'build/pc-vulkan/bo3_native_physical_access_test.exe',
          'tools/nx-sdk/src/system/xmemory.cpp', 'tools/nx-sdk/include/rex/system/xmemory.h',
          'tools/nx-sdk/src/system/mmio_handler.cpp',
          'build/pc-vulkan/rexruntime.dll',
          'tools/nx-sdk/src/graphics/pipeline/texture/util.cpp',
          'tools/nx-sdk/include/rex/graphics/pipeline/texture/util.h',
          'tools/nx-sdk/include/rex/graphics/vulkan/texture_cache.h',
          'tools/nx-sdk/src/graphics/vulkan/texture_cache.cpp']

def resolve_interface(path, multisampled, depth_only=False, presentation=False, unlit=False):
    """Check the compiled interface against the native pipeline's real layout."""
    data = path.read_bytes()
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    pointers, members, offsets, decorations, images, variables = {}, {}, {}, {}, {}, []
    combined_images, scalar_sizes, vectors = {}, {}, {}
    index = 5
    while index < len(words):
        count, opcode = words[index] >> 16, words[index] & 65535
        if not count or index + count > len(words):
            raise SystemExit('Malformed SPIR-V instruction')
        a = words[index + 1:index + count]
        if opcode == 32: pointers[a[0]] = a[2]  # OpTypePointer
        elif opcode in (21, 22): scalar_sizes[a[0]] = a[1] // 8
        elif opcode == 23: vectors[a[0]] = a[1:]
        elif opcode == 27: combined_images[a[0]] = a[1]
        elif opcode == 30: members[a[0]] = a[1:]  # OpTypeStruct
        elif opcode == 25: images[a[0]] = a[1:]  # OpTypeImage
        elif opcode == 59: variables.append(a[:3])  # OpVariable
        elif opcode == 71: decorations[(a[0], a[1])] = a[2:]
        elif opcode == 72 and a[2] == 35: offsets[(a[0], a[1])] = a[3]
        index += count
    push = [pointers[t] for t, _, storage in variables if storage == 9]
    if depth_only:
        if push: raise SystemExit('Depth diagnostic must not require push constants')
        member_offsets = []
    else:
        if len(push) != 1:
            raise SystemExit('Resolve shader must have one push block')
        member_offsets = [offsets[(push[0], n)] for n in range(len(members[push[0]]))]
        expected_offsets = [0, 4, 8, 12, 16, 32] if unlit else [0, 8, 12] if presentation else [0, 8, 16, 20, 24]
        if member_offsets != expected_offsets:
            raise SystemExit(f'Resolve push ABI mismatch: {member_offsets}')
        def byte_size(type_id):
            if type_id in scalar_sizes: return scalar_sizes[type_id]
            component, count = vectors[type_id]
            return scalar_sizes[component] * count
        actual_bytes = max(offset + byte_size(type_id) for offset, type_id in zip(member_offsets, members[push[0]]))
        if actual_bytes != (48 if unlit else 16 if presentation else 32):
            raise SystemExit(f'Push range size mismatch: {actual_bytes}')
    resources = []
    for t, v, storage in variables:
        type_id = pointers[t]
        is_combined = type_id in combined_images
        image_id = combined_images.get(type_id, type_id)
        if storage == 0 and image_id in images:
            if is_combined != presentation:
                raise SystemExit('Sampled image versus combined image/sampler ABI mismatch')
            resources.append((v, images[image_id]))
    if unlit:
        if resources: raise SystemExit('Unlit diagnostic must not require material textures')
        return {'push_member_offsets': member_offsets, 'push_range_bytes': 48, 'descriptor_type': 'none'}
    if len(resources) != 1:
        raise SystemExit('Resolve shader must use one sampled image')
    variable, image = resources[0]
    # TypeImage operands after ID: sampled type, Dim2D, depth, arrayed, MS,
    # sampled=1, format. No sampler descriptor is required for Load.
    if image[1] != 1 or image[3] != 0 or image[4] != int(multisampled) or image[5] != 1:
        raise SystemExit('Resolve sampled image type mismatch')
    if decorations.get((variable, 33)) != (0,) or decorations.get((variable, 34)) != (0,):
        raise SystemExit('Resolve descriptor must be set 0 binding 0')
    return {'push_member_offsets': member_offsets, 'push_range_bytes': 0 if depth_only else 16 if presentation else 32,
            'descriptor_type': 'combined_image_sampler' if presentation else 'sampled_image',
            'set': 0, 'binding': 0, 'image_multisampled': multisampled}

validator = root / 'build/native-spirv-tools/tools/spirv-val.exe'
modules = []
for stage in ['VS', 'PS', 'DepthPS', 'ResolvePS', 'ResolveMSPS', 'UnlitPS']:
    path = root / f'build/pc-vulkan/bo3-native/present_spirv.{stage}.spv'
    result = subprocess.run([str(validator), '--target-env', 'vulkan1.1', str(path)],
                            capture_output=True, text=True)
    modules.append({'stage': stage, 'sha256': digest(path),
                    'returncode': result.returncode,
                    'diagnostics': (result.stdout + result.stderr).strip()})
    if stage.startswith('Resolve'):
        modules[-1]['interface'] = resolve_interface(path, stage == 'ResolveMSPS')
    elif stage == 'DepthPS':
        modules[-1]['interface'] = resolve_interface(path, True, depth_only=True)
    elif stage == 'PS':
        modules[-1]['interface'] = resolve_interface(path, False, presentation=True)
    elif stage == 'UnlitPS':
        modules[-1]['interface'] = resolve_interface(path, False, unlit=True)
report = {'date': '2026-10-13', 'test_log': args.log,
          'texture_checks': int(texture[1]), 'ownership_checks': int(ownership[1]),
          'color_resolve_checks': int(resolve[1]),
          'pipeline_state_checks': int(pipeline[1]),
          'texture_watch_checks': int(watch[1]),
          'physical_guard_checks': int(guards[1]),
          'texture_residency_checks': int(residency[1]),
          'game_test_status': 'Offline validation before the authorized game test',
          'gpu_execution_validated': False,
          'limitations': ['CPU fixtures do not prove GPU sampling or complete scene rendering.',
                          'Tiled fixture generation uses SDK helpers; it is not independent hardware tiling validation.',
                          'Resolve conversion shaders are structurally validated; numerical GPU output is unverified.',
                          'Depth/array/subset resolves, CPU readback and complete EDRAM alias ownership remain incomplete.',
                          'Physical write callbacks invalidate GPU coverage; arbitrary partial CPU reuse stays conservative.'],
          'presentation_modules': modules, 'validator_sha256': digest(validator),
          'inputs': {name: digest(root / name) for name in inputs}}
(root / args.output).write_text(json.dumps(report, indent=2) + '\n', newline='\n')
print(f"Recorded {texture[1]} texture checks, {ownership[1]} ownership checks, {resolve[1]} color resolve checks, {pipeline[1]} pipeline checks, {watch[1]} write-watch checks, {guards[1]} physical guard checks, {residency[1]} residency checks, and {len(modules)} shader modules")
if any(module['returncode'] for module in modules):
    raise SystemExit('Presentation SPIR-V validation failed')
