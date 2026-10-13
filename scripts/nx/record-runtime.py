"""Record the exact BO3 NX binary and inputs for hardware crash analysis."""
from pathlib import Path
import hashlib
import json
import re
import subprocess

root = Path(__file__).resolve().parents[2]
devkit = Path('C:/devkitPro/devkitA64/bin')
elf = root / 'build/nx-runtime/bo3_zm_nx'
symbols = subprocess.check_output([str(devkit / 'aarch64-none-elf-nm.exe'), str(elf)], text=True)
start = re.search(r'^([0-9a-f]+) T _start$', symbols, re.M)
if not start or int(start.group(1), 16): raise SystemExit('Invalid NX startup address')
files = {}
for name in ['build/nx-runtime/bo3_zm_nx', 'build/nx-runtime/bo3_zm_nx.nro',
             'build/nx-runtime/bo3_zm_nx.map', 'build/nx-runtime/bo3_zm_nx.nacp',
             'nx/runtime/main.cpp', 'nx/runtime/CMakeLists.txt', 'nx/runtime/bo3_zm_nx.toml',
             'app/bo3_zm_manifest.toml', 'runtime/game/default_zm.xex', 'runtime/game/default_zm.xexp',
             'build/nx-generated/zombies/bo3_zm_init.h', 'build/nx-generated/zombies/bo3_zm_init.cpp']:
    path = root / name
    with path.open('rb') as stream: digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    files[name] = {'bytes': path.stat().st_size, 'sha256': digest}
init = (root / 'build/nx-generated/zombies/bo3_zm_init.cpp').read_text()
aliases = {}
for name in ['sub_82140000', 'sub_82140018']:
    weak = re.search(rf'^([0-9a-f]+) W {name}$', symbols, re.M)
    implementation = re.search(rf'^([0-9a-f]+) T __imp__{name}$', symbols, re.M)
    if not weak or not implementation or weak.group(1) != implementation.group(1):
        raise SystemExit(f'Unexpected linked alias for {name}')
    aliases[name] = weak.group(1)
result = {'date': '2026-10-13', 'files': files,
          'compiler': subprocess.check_output([str(devkit / 'aarch64-none-elf-g++.exe'), '--version'], text=True).splitlines()[0],
          'elf_header': subprocess.check_output([str(devkit / 'aarch64-none-elf-readelf.exe'), '-h', str(elf)], text=True),
          'section_sizes': subprocess.check_output([str(devkit / 'aarch64-none-elf-size.exe'), str(elf)], text=True),
          'linked_aliases': aliases,
          'horizon_exception_handler_linked': bool(re.search(r'^\w+ T __libnx_exception_handler$', symbols, re.M)),
          'function_mapping_entries': len(re.findall(r'\{ 0x[0-9A-Fa-f]+,', init)),
          'source_provenance': 'analysis/nx_runtime_sources.json',
          'final_build_log': 'analysis/nx_runtime_build03.log',
          'hardware_status': 'Game NRO not yet tested; environment preflight passed'}
(root / 'analysis/nx_runtime_provenance.json').write_text(json.dumps(result, indent=2)+'\n', newline='\n')
print(f'Recorded NX binary provenance; {result["function_mapping_entries"]} function mappings')
