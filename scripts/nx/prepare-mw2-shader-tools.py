"""Fetch and verify the pinned MW2 shader translator and its GPL license."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import urllib.request

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--adapt-bo3', action='store_true')
args = parser.parse_args()
manifest = json.loads((root / 'analysis/mw2_shader_tool_sources.json').read_text())
target = root / 'tools/mw2-recompiled-reference'
for name, expected in manifest['files'].items():
    path = target / name
    if path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == expected['sha256']:
        continue
    url = f"https://raw.githubusercontent.com/{manifest['repository']}/{manifest['commit']}/{name}"
    with urllib.request.urlopen(url, timeout=60) as response:
        data = response.read()
    if len(data) != expected['bytes'] or hashlib.sha256(data).hexdigest() != expected['sha256']:
        raise SystemExit(f'Pinned source hash mismatch: {name}')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
print('Pinned MW2 translator sources and license verified')
if args.adapt_bo3:
    staged = root / 'tools/bo3-mw2-shader-translator'
    for name in manifest['files']:
        destination = staged / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(target / name, destination)
    cpp = staged / 'runtime/gpu/vulkan/shader_translator.cpp'
    text = cpp.read_text()

    def replace_once(anchor, replacement):
        global text
        if text.count(anchor) != 1:
            raise SystemExit(f'MW2 patch anchor changed: {anchor[:70]}')
        text = text.replace(anchor, replacement)

    replace_once('void TranslateGetGradients(const Fetch& op);',
                 'void TranslateGetGradients(const Fetch& op);\n        void TranslateGetWeights(const Fetch& op);')
    replace_once('Id m_fetchConstants = 0;', 'bool m_haveWeights = false;\n        Id m_fetchConstants = 0;')
    replace_once('else if (f.Opcode() == FetchOpcode::VertexFetch)',
                 'else if (f.Opcode() == FetchOpcode::GetWeights)\n                    { m_haveWeights = true; }\n                    else if (f.Opcode() == FetchOpcode::VertexFetch)')
    replace_once('if (m_result.vertexFetchMask || m_result.textureMask) DeclareFetchConstants();',
                 'if (m_result.vertexFetchMask || m_result.textureMask || m_haveWeights) DeclareFetchConstants();')
    replace_once('case FetchOpcode::GetGradients: TranslateGetGradients(op); break;',
                 'case FetchOpcode::GetGradients: TranslateGetGradients(op); break;\n        case FetchOpcode::GetWeights: TranslateGetWeights(op); break;')
    patch = root / 'nx/shader-tools/mw2_weights.cpp.inc'
    replace_once('    void Translator::TranslateTextureFetch(const Fetch& op)',
                 patch.read_text() + '\n    void Translator::TranslateTextureFetch(const Fetch& op)')
    # BO3 actually uses one slot as 2D and 3D within a program. The unmodified
    # translator silently overwrites the slot kind and emits invalid SPIR-V.
    # Refuse that program until descriptor views can represent each use.
    replace_once('m_result.textureMask |= 1u << slot;', '''if ((m_result.textureMask & (1u << slot)) &&
                            m_textureDimensions[slot] != f.Dimension() &&
                            !(uint32_t(m_textureDimensions[slot]) <= 1 && uint32_t(f.Dimension()) <= 1))
                            Fail("mixed texture dimensionality at slot", slot);
                        m_result.textureMask |= 1u << slot;''')
    cpp.write_text(text, newline='\n')
    provenance = {**manifest, 'reference': manifest,
                  'source_root': 'tools/bo3-mw2-shader-translator',
                  'adaptation': 'Observed BO3 2D getWeights profile; refuse mixed sampler dimensionality',
                  'patch_sha256': hashlib.sha256(patch.read_bytes()).hexdigest(),
                  'files': {name: {'bytes': (staged / name).stat().st_size,
                                   'sha256': hashlib.sha256((staged / name).read_bytes()).hexdigest()}
                            for name in manifest['files']}}
    (root / 'analysis/mw2_shader_tool_staged_sources.json').write_text(json.dumps(provenance, indent=2) + '\n')
    print('Separate BO3 translator prototype staged; original reference unchanged')
