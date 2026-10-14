"""Compile the native presentation shaders and embed their SPIR-V words."""
import argparse
import pathlib
import struct
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--dxc', required=True)
p.add_argument('--source', required=True)
p.add_argument('--output', required=True)
a = p.parse_args()
out = pathlib.Path(a.output)
out.parent.mkdir(parents=True, exist_ok=True)
parts = ['// Generated from nx/native/present.hlsl.\n#pragma once\n#include <cstdint>\n']
for name, profile in [('VS', 'vs_6_0'), ('PS', 'ps_6_0'), ('DepthPS', 'ps_6_0'),
                      ('ResolvePS', 'ps_6_0'), ('ResolveMSPS', 'ps_6_0')]:
    spv = out.with_suffix('.' + name + '.spv')
    definitions = {'VS': [], 'PS': ['-D', 'BO3_PRESENT_COLOR=1'],
                   'DepthPS': ['-D', 'BO3_PRESENT_DEPTH=1'],
                   'ResolvePS': ['-D', 'BO3_RESOLVE_COLOR=1'],
                   'ResolveMSPS': ['-D', 'BO3_RESOLVE_COLOR=1', '-D', 'BO3_RESOLVE_MSAA=1']}
    subprocess.run([a.dxc, '-spirv', '-fspv-target-env=vulkan1.1', '-fvk-use-dx-layout',
                    '-E', name, '-T', profile, '-Fo', str(spv), *definitions[name], a.source], check=True)
    data = spv.read_bytes()
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    parts.append('static constexpr uint32_t kPresent' + name + '[] = {\n' +
                 ','.join(hex(w) for w in words) + '\n};\n')
out.write_text(''.join(parts), encoding='utf-8')
