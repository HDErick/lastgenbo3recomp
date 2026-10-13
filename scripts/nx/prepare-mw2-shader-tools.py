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
    views = root / 'nx/shader-tools/mw2_texture_views.cpp.inc'
    replace_once('void DeclareInterface();', '''void DeclareInterface();
        void DeclareTextureViews();
        static uint32_t TextureKind(FetchDimension dimension)
        { return dimension == FetchDimension::D3 ? 2 : dimension == FetchDimension::Cube ? 1 : 0; }''')
    replace_once('Id m_textures[32]{};', 'Id m_textures[32][3]{};')
    replace_once('Id m_textureTypes[32]{};', 'Id m_textureTypes[32][3]{};')
    replace_once('Id m_textureImageTypes[32]{};', 'Id m_textureImageTypes[32][3]{};')
    replace_once('// A slot is only ever sampled at one dimensionality within one program.\n        FetchDimension m_textureDimensions[32]{};',
                 '// BO3 can sample several typed views of one fetch slot.')
    replace_once('m_textureDimensions[slot] = f.Dimension();',
                 'm_result.textureViewMasks[slot] |= 1u << TextureKind(f.Dimension());')
    # Replace the complete original one-view declaration loop, with strict
    # boundary checks. The pinned file hash above protects its contents.
    begin = text.index('        for (uint32_t slot = 0; slot < 32; slot++)')
    end = text.index('        if (m_result.vertexFetchMask) DeclareVertexFetchInterface();', begin)
    replace_once(text[begin:end], '        DeclareTextureViews();\n\n')
    replace_once('    void Translator::TranslateTextureFetch(const Fetch& op)',
                 views.read_text() + '\n    void Translator::TranslateTextureFetch(const Fetch& op)')
    replace_once('if (!m_textures[slot])',
                 'const uint32_t kind = TextureKind(op.Dimension());\n        if (!m_textures[slot][kind])')
    replace_once('m_textureTypes[slot], { m_textures[slot] }',
                 'm_textureTypes[slot][kind], { m_textures[slot][kind] }')
    replace_once('m_textureImageTypes[slot], { sampler }',
                 'm_textureImageTypes[slot][kind], { sampler }')
    cpp.write_text(text, newline='\n')
    header = staged / 'runtime/gpu/vulkan/shader_translator.h'
    header_text = header.read_text()
    header_text = header_text.replace('#pragma once', '#pragma once\n#define BO3_MW2_TEXTURE_VIEWS 1', 1)
    header_text = header_text.replace('    struct Translation', '''    // Set 1 bindings must agree across VS and PS, even when their slot kinds
    // differ. Use only the bindings named by textureViewMasks, not 96 samplers.
    inline constexpr uint32_t TextureViewBinding(uint32_t slot, uint32_t kind)
    { return slot + 32 * kind; }

    struct Translation''', 1)
    header_text = header_text.replace('uint8_t textureKinds[32]{};', '''uint8_t textureKinds[32]{}; // 255 when mixed; use textureViewMasks instead
        uint8_t textureViewMasks[32]{}; // bit 0 = 2D, bit 1 = cube, bit 2 = 3D''', 1)
    header.write_text(header_text, newline='\n')
    provenance = {**manifest, 'reference': manifest,
                  'source_root': 'tools/bo3-mw2-shader-translator',
                  'adaptation': 'Observed BO3 2D getWeights profile; separate typed texture views (ABI 2)',
                  'patch_sha256': hashlib.sha256(patch.read_bytes()).hexdigest(),
                  'texture_views_patch_sha256': hashlib.sha256(views.read_bytes()).hexdigest(),
                  'preparation_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  'files': {name: {'bytes': (staged / name).stat().st_size,
                                   'sha256': hashlib.sha256((staged / name).read_bytes()).hexdigest()}
                            for name in manifest['files']}}
    (root / 'analysis/mw2_shader_tool_staged_sources.json').write_text(json.dumps(provenance, indent=2) + '\n')
    print('Separate BO3 translator prototype staged; original reference unchanged')
