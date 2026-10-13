"""Fetch the pinned, licensed Xenos container translator into ignored tools/."""
from pathlib import Path
import hashlib
import json
import shutil
import urllib.request

root = Path(__file__).resolve().parents[2]
manifest = json.loads((root / 'analysis/native_shader_tool_sources.json').read_text())
target = root / 'tools/sa2-nx-reference'
for name, expected in manifest['files'].items():
    path = target / name
    if path.exists() and hashlib.sha256(path.read_bytes()).hexdigest() == expected['sha256']:
        continue
    url = f"https://raw.githubusercontent.com/{manifest['repository']}/{manifest['commit']}/{name}"
    with urllib.request.urlopen(url, timeout=60) as response:
        data = response.read()
    if len(data) != expected['bytes'] or hashlib.sha256(data).hexdigest() != expected['sha256']:
        raise SystemExit(f'Pinned source hash mismatch: {name}')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    print(f'Fetched {name}')
print('Pinned translator sources and license files verified')
# Keep the reference byte-identical. BO3 changes are staged separately, along
# with the original copyright and license files.
staged = root / 'tools/bo3-shader-translator'
for name in manifest['files']:
    source = target / name
    destination = staged / name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
cpp = staged / 'shaders/XenosRecomp/shader_recompiler.cpp'
text = cpp.read_text()
anchor = 'print(", float2({}, {}), {}_InvSize", instr.offsetX * 0.5f, instr.offsetY * 0.5f, constNamePtr);'
if text.count(anchor) != 1:
    raise SystemExit('Texture fetch emitter changed; review BO3 patch')
# getWeights2D uses texture dimensions and accepts four arguments. The inherited
# emitter supplied the tfetch2D reciprocal-size fifth argument to both opcodes.
# Retain the helper's exact arithmetic; only tfetch2D receives reciprocal size.
replacement = '''if (instr.opcode == FetchOpcode::GetTextureWeights)
            print(", float2({}, {})", instr.offsetX * 0.5f, instr.offsetY * 0.5f);
        else
            ''' + anchor
cpp.write_text(text.replace(anchor, replacement), newline='\n')
provenance = {'reference': manifest, 'patch': 'GetTextureWeights keeps four-argument helper ABI',
              'files': {name: {'bytes': (staged / name).stat().st_size,
                              'sha256': hashlib.sha256((staged / name).read_bytes()).hexdigest()}
                        for name in manifest['files']}}
(root / 'analysis/native_shader_tool_staged_sources.json').write_text(json.dumps(provenance, indent=2) + '\n')
print('BO3 translator staged with texture-weight ABI repair; reference unchanged')
