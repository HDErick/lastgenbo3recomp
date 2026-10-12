"""Summarize provided content and first RexGlue code generation probes."""
import collections
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SDK = Path('C:/BO2recomp/Project BO2/rexglue-sdk')
audit = json.loads((ROOT / 'analysis/input_audit.json').read_text())
layers = collections.defaultdict(list)
folders = []
for folder in sorted((ROOT / 'game_files').iterdir()):
    files = [p for p in folder.rglob('*') if p.is_file()]
    folders.append(dict(folder=folder.name, file_count=len(files),
                        bytes=sum(p.stat().st_size for p in files)))
    for path in files:
        relative = path.relative_to(folder).as_posix()
        layers[relative.casefold()].append(dict(folder=folder.name, path=relative, size=path.stat().st_size))

stubs = {}
for path in (SDK / 'src/kernel').rglob('*.cpp'):
    for name in re.findall(r'REX_EXPORT_STUB\(__imp__(\w+)\)', path.read_text(encoding='utf-8')):
        stubs[name] = str(path)

probes = []
for mode in ('zm', 'mp'):
    folder = ROOT / 'analysis' / f'{mode}_probe'
    generated = folder / 'generated'
    header = (generated / f'bo3_{mode}_probe_init.h').read_text()
    init = (generated / f'bo3_{mode}_probe_init.cpp').read_text()
    imports = sorted(set(re.findall(r'DECLARE_REX_FUNC\(__imp__(\w+)\)', header)))
    log = (folder / 'console.log').read_text()
    fatal_branches, vector_bounds, float16_modes = [], [], collections.Counter()
    for path in sorted(generated.glob('*recomp*.cpp')):
        function = None
        for line_number, line in enumerate(path.read_text().splitlines(), 1):
            match = re.search(r'DEFINE_REX_FUNC\((\w+)\)', line)
            if match:
                function = match[1]
            if 'REX_FATAL("Unresolved' in line:
                fatal_branches.append(dict(path=str(path), line=line_number,
                                           function=function, text=line.strip()))
            pack = re.search(r'// vpkd3d128 v\d+,v\d+,5,(\d+),(\d+)', line)
            if pack:
                float16_modes[f'pack={pack[1]},shift={pack[2]}'] += 1
            for index in re.finditer(r'ctx\.v(\d+)\.u16\[(\d+)\]', line):
                if int(index[2]) >= 8:
                    vector_bounds.append(dict(path=str(path), line=line_number,
                                              function=function, text=line.strip()))
    probes.append(dict(mode=mode, exit_code=int((folder / 'exit_code.txt').read_text()),
                       function_mappings=len(re.findall(r'\{ 0x[0-9A-Fa-f]+,', init)),
                       cpp_files=len(list(generated.glob('*.cpp'))),
                       generated_bytes=sum(p.stat().st_size for p in generated.iterdir() if p.is_file()),
                       imports=imports, explicit_import_stubs={name: stubs[name] for name in imports if name in stubs},
                       emitter_fatal_branches=fatal_branches,
                       vector_pack_warning_count=log.count('Unexpected float16_4'),
                       float16_pack_modes=dict(float16_modes),
                       vector_u16_out_of_bounds=vector_bounds,
                       image_definitions=re.findall(r'#define REX_(?:IMAGE|CODE)_[^\n]+', header)))

maps = {}
for folder in sorted((ROOT / 'game_files').iterdir()):
    maps[folder.name] = sorted(p.name for p in folder.glob('*.ff')
                              if re.match(r'(mp_|zm_)', p.name))

result = dict(folders=folders, total_files=sum(f['file_count'] for f in folders),
              total_bytes=sum(f['bytes'] for f in folders), unique_relative_paths=len(layers),
              duplicated_relative_path_count=sum(len(items) > 1 for items in layers.values()),
              duplicates={name: items for name, items in sorted(layers.items()) if len(items) > 1},
              map_fastfiles=maps, probes=probes,
              package_files_verified=sum(p['file_count'] for p in audit['packages']),
              package_issues=sum(len(p[k]) for p in audit['packages']
                                 for k in ('missing', 'size_mismatches', 'extra_files', 'hash_mismatches')))
(ROOT / 'analysis/analysis_summary.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print(json.dumps({k: v for k, v in result.items() if k not in ('duplicates', 'probes')}, indent=2))
for p in probes:
    print(json.dumps(dict(mode=p['mode'], exit_code=p['exit_code'],
                          functions=p['function_mappings'], cpp_files=p['cpp_files'],
                          generated_bytes=p['generated_bytes'], import_count=len(p['imports']),
                          source_stub_count=len(p['explicit_import_stubs']),
                          fatal_branch_count=len(p['emitter_fatal_branches']),
                          float16_warnings=p['vector_pack_warning_count'],
                          float16_modes=p['float16_pack_modes'],
                          vector_bounds_violations=len(p['vector_u16_out_of_bounds'])), indent=2))
