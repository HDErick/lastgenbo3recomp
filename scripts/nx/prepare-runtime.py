"""Stage the pinned Horizon SDK and adapt existing BO3 translation headers.

Source trees and build products are isolated under ignored tools/ and build/.
The desktop SDK provides vendored dependency sources; the NFSMW SDK overrides
its own files. BO3-specific patches live under nx/sdk-patches/.
"""
from pathlib import Path
import hashlib
import json
import subprocess

root = Path(__file__).resolve().parents[2]
reference = root / 'tools/nfsmw-nx-reference'
pinned = 'be2d4c67ab42d6d629746a01dbf03cef824c8d2d'
git = 'C:/Program Files/Git/cmd/git.exe'
if not (reference / '.git').is_dir():
    reference.mkdir(parents=True, exist_ok=True)
    subprocess.run([git, 'init', '-q', str(reference)], check=True)
    subprocess.run([git, '-C', str(reference), 'remote', 'add', 'origin',
                    'https://github.com/StevensND/nfsmw-nx.git'], check=True)
    subprocess.run([git, '-C', str(reference), 'fetch', '--depth', '1', 'origin', pinned], check=True)
    subprocess.run([git, '-C', str(reference), 'checkout', '--detach', 'FETCH_HEAD'], check=True)
revision = subprocess.check_output([git, '-C', str(reference), 'rev-parse', 'HEAD'], text=True).strip()
if revision != pinned: raise SystemExit('Reference revision does not match the recorded port baseline')
target = root / 'tools/nx-sdk'
target.mkdir(parents=True, exist_ok=True)

def tree_files(source):
    result = {}
    for path in source.rglob('*'):
        relative = path.relative_to(source)
        if any(part in {'.git', 'build', 'out', '__pycache__'} for part in relative.parts): continue
        if not path.is_file(): continue
        result[relative.as_posix()] = path
    return result

def write_if_changed(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_bytes() != data: path.write_bytes(data)

print('Staging vendored dependencies and pinned Horizon runtime', flush=True)
patches = root / 'nx/sdk-patches'
# Merge inputs before writing: overwriting the base then the overlay on every
# preparation would change timestamps and unnecessarily rebuild dependencies.
inputs = {'thirdparty/' + name: path for name, path in
          tree_files(root / 'thirdparty/rexglue-sdk/thirdparty').items()}
inputs.update(tree_files(reference / 'sdk'))
if patches.is_dir(): inputs.update(tree_files(patches))
for name, path in inputs.items():
    data = path.read_bytes()
    if name == 'thirdparty/CMakeLists.txt':
        cmake = data.decode().replace('\r\n', '\n')
        anchor = 'if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${submodule}/.git")'
        assert anchor in cmake
        data = cmake.replace(anchor, 'if(NOT IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${submodule}")', 1).encode()
    if name == 'src/graphics/vulkan/render_target_cache.cpp':
        source = data.decode().replace('\r\n', '\n')
        anchor = '  // Format support.\n'
        assert source.count(anchor) == 1
        diagnostic = '''  REXGPU_INFO("BO3 Vulkan render targets: requested='{}', selected={}, FSI supported={}, sampleRateShading={}, standardSampleLocations={}, shared buffers={}, storage slots={}",
      REXCVAR_GET(render_target_path_vulkan),
      path_ == Path::kPixelShaderInterlock ? "fsi" : "fbo", fsi_path_supported,
      device_properties.sampleRateShading, device_properties.standardSampleLocations,
      shared_memory_binding_count, device_properties.maxPerStageDescriptorStorageBuffers);

'''
        data = source.replace(anchor, diagnostic + anchor, 1).encode()
    if name == 'src/graphics/vulkan/command_processor.cpp':
        source = data.decode().replace('\r\n', '\n')
        include = '#include <rex/graphics/vulkan/command_processor.h>'
        anchor = '  if (memexport_used_vertex) {\n'
        assert source.count(include) == 1 and source.count(anchor) == 1
        source = source.replace(include, include + '\n#include <rex/graphics/bo3_memexport_probe.h>', 1)
        source = source.replace(anchor, '''  if (memexport_used_vertex) {
    bo3::CaptureMemexportState(regs, *vertex_shader, uint32_t(prim_type), index_count,
                              index_buffer_info, major_mode_explicit);
''', 1)
        data = source.encode()
    write_if_changed(target / name, data)

generated = root / 'build/nx-generated/zombies'
generated.mkdir(parents=True, exist_ok=True)
for name, path in tree_files(root / 'app/generated/default_zm').items():
    if name != 'bo3_zm_init.h': write_if_changed(generated / name, path.read_bytes())
header_path = generated / 'bo3_zm_init.h'
header = (root / 'app/generated/default_zm/bo3_zm_init.h').read_text()
alias = '__attribute__((alias("__imp__" #name))) REX_WEAK_FUNC(name);'
assert alias in header
header = header.replace(alias, 'REX_EXTERN(name) __attribute__((weak, alias("__imp__" #name)));', 1)
write_if_changed(header_path, header.encode())
provenance = {'reference_repository': 'https://github.com/StevensND/nfsmw-nx',
              'reference_revision': pinned, 'sdk_version': '0.10.0',
              'dependency_source': 'Vendored desktop SDK dependencies, with reference overrides',
              'dependency_snapshot': json.loads((root / 'thirdparty/rexglue-sdk/SOURCE_SNAPSHOT.json').read_text()),
              'adaptations': ['Vendored dependency directory checks', 'Explicit C linkage in generated GCC aliases',
                              'Log requested and selected Vulkan render-target path',
                              'Optional bounded GPU-thread memexport register snapshots'],
              'patch_files': [p.relative_to(patches).as_posix() for p in patches.rglob('*') if p.is_file()] if patches.exists() else [],
              'desktop_init_sha256': hashlib.sha256((root / 'app/generated/default_zm/bo3_zm_init.h').read_bytes()).hexdigest(),
              'nx_init_sha256': hashlib.sha256(header_path.read_bytes()).hexdigest()}
(root / 'analysis/nx_runtime_sources.json').write_text(json.dumps(provenance, indent=2)+'\n', newline='\n')
print('Prepared tools/nx-sdk and build/nx-generated/zombies', flush=True)
