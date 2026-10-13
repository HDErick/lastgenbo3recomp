"""Record installed Switch tools and reference revisions without changing them."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--devkitpro', default='C:/devkitPro')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
devkit = Path(args.devkitpro)
git = 'C:/Program Files/Git/cmd/git.exe'

def revision(path):
    if not (path / '.git').exists(): return None
    return subprocess.check_output([git, '-c', f'safe.directory={path.as_posix()}',
                                    '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()

files = {}
for relative in ['devkitA64/bin/aarch64-none-elf-g++.exe', 'libnx/lib/libnx.a',
                 'libnx/switch.specs', 'tools/bin/elf2nro.exe', 'tools/bin/nacptool.exe',
                 'portlibs/switch/lib/libvulkan.a', 'portlibs/switch/lib/libelf.a',
                 'portlibs/switch/lib/libavcodec.a', 'portlibs/switch/lib/libavutil.a',
                 'portlibs/switch/lib/libswresample.a']:
    path = devkit / relative
    item = {'path': str(path), 'exists': path.is_file()}
    if path.is_file():
        with path.open('rb') as stream: digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        item.update(bytes=path.stat().st_size, sha256=digest)
    files[relative] = item

reference = root / 'tools/nfsmw-nx-reference'
local_sdk = root / 'thirdparty/rexglue-sdk'
ref_sdk = reference / 'sdk'
local_paths = {p.relative_to(local_sdk).as_posix() for folder in ['include', 'src', 'cmake']
               for p in (local_sdk / folder).rglob('*') if p.is_file()}
ref_paths = {p.relative_to(ref_sdk).as_posix() for folder in ['include', 'src', 'cmake']
             for p in (ref_sdk / folder).rglob('*') if p.is_file()}
common = local_paths & ref_paths
different = [p for p in sorted(common)
             if (local_sdk / p).read_bytes().replace(b'\r\n', b'\n') !=
                (ref_sdk / p).read_bytes().replace(b'\r\n', b'\n')]
translations = {}
for name, folder in [('zombies', root / 'app/generated/default_zm'),
                     ('multiplayer', root / 'app/multiplayer/generated/default')]:
    sources = list(folder.glob('*_recomp.*.cpp'))
    translations[name] = {'chunks': len(sources), 'cpp_bytes': sum(p.stat().st_size for p in sources),
                          'functions': sum(p.read_text().count('DEFINE_REX_FUNC(') for p in sources)}

result = {'date': '2026-10-12', 'devkitpro': str(devkit),
          'compiler': subprocess.check_output([str(devkit / 'devkitA64/bin/aarch64-none-elf-g++.exe'),
                                              '--version'], text=True).splitlines()[0],
          'files': files,
          'reference': {'repository': 'https://github.com/StevensND/nfsmw-nx', 'revision': revision(reference),
                        'sdk_version': '0.10.0', 'upstream_sdk_revision': 'c94f5ebdcb3c9d1a460ca48e04f9758448f8d518'},
          'bo3_sdk': json.loads((local_sdk / 'SOURCE_SNAPSHOT.json').read_text()),
          'sdk_comparison': {'comparison': 'CRLF normalized to LF', 'different_common_files': different, 'reference_only_files': sorted(ref_paths - local_paths),
                             'bo3_only_files': sorted(local_paths - ref_paths)},
          'other_projects': {str(p): revision(p) for p in [Path('C:/T6ZM-Rebuild-NX'), Path('C:/KisakBlack-NX')]},
          'translations': translations,
          'hardware_plan': 'Original Switch / OLED, title takeover; confirmed by user',
          'hardware_validation': 'Not run; host compilation does not establish hardware support or frame rate'}
nro = root / 'build/nx-preflight/bo3_nx_preflight.nro'
if nro.is_file():
    result['preflight_build'] = {
        'artifact': str(nro.relative_to(root)), 'bytes': nro.stat().st_size,
        'sha256': hashlib.sha256(nro.read_bytes()).hexdigest(),
        'elf_machine': 'AArch64', 'elf_class': 'ELF64',
        'start_address': '0x0 (verified by scripts/nx/build-preflight.ps1)',
        'hardware_test': 'Pending'}
hardware_result = root / 'analysis/nx_preflight_result.json'
if hardware_result.is_file():
    hardware = json.loads(hardware_result.read_text())
    result['hardware_result'] = str(hardware_result.relative_to(root))
    result['hardware_validation'] = hardware['status'] + '; BO3 not yet run'
    if ('preflight_build' in result and
            result['preflight_build']['sha256'] == hardware['supplied_build_sha256']):
        result['preflight_build']['hardware_test'] = hardware['status']
path = root / 'analysis/nx_environment.json'
path.write_text(json.dumps(result, indent=2) + '\n', newline='\n')
print(f'Wrote {path.relative_to(root)}')
print(f'SDK comparison: {len(different)} differing common files, {len(ref_paths - local_paths)} reference-only files')
print(json.dumps(translations, indent=2))
