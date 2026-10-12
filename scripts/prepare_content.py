"""Create a separate runtime installation from the verified extracted inputs."""
import hashlib
import json
import shutil
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def copy_tree(source, destination):
    destination.mkdir(parents=True, exist_ok=True)
    count = 0
    for source_file in source.rglob('*'):
        if not source_file.is_file():
            continue
        destination_file = destination / source_file.relative_to(source)
        destination_file.parent.mkdir(parents=True, exist_ok=True)
        if destination_file.exists() and destination_file.stat().st_size == source_file.stat().st_size:
            # This installation is derived from immutable inputs; check hashes
            # before keeping an existing file, so an interrupted run is safe.
            def sha(path):
                with path.open('rb') as stream:
                    return hashlib.file_digest(stream, 'sha256').digest()
            if sha(source_file) == sha(destination_file):
                continue
        shutil.copy2(source_file, destination_file)
        count += 1
    return count


def main():
    audit = json.loads((ROOT / 'analysis/input_audit.json').read_text())
    for package in audit['packages']:
        if not package['hashes_verified'] or any(package[key] for key in
                ('missing', 'size_mismatches', 'extra_files', 'hash_mismatches')):
            raise SystemExit('Package audit must pass before staging content')
    runtime = ROOT / 'runtime'
    print(f'Base: {copy_tree(ROOT / "game_files/base", runtime / "game")} files copied', flush=True)
    print(f'Update: {copy_tree(ROOT / "game_files/title_update", runtime / "update")} files copied', flush=True)
    # Runtime/codegen share sibling XEX patch discovery.
    for path in (ROOT / 'game_files/title_update').glob('*.xexp'):
        shutil.copy2(path, runtime / 'game' / path.name)
    packages = []
    for package in audit['packages']:
        if package['content_type'] != '00000002':
            continue
        original = Path(package['path'])
        file_name = original.name.split(' ')[-1]
        if len(file_name.encode('ascii')) > 42:
            raise ValueError('Package filename exceeds XCONTENT_DATA capacity')
        destination = runtime / 'user/0000000000000000/4156091D/00000002' / file_name
        copied = copy_tree(Path(package['extracted_folder']), destination)
        with original.open('rb') as stream:
            header = stream.read(0x344)
        license_mask = 0
        for index in range(16):
            license_mask |= struct.unpack_from('>I', header, 0x22C + 16 * index + 8)[0]
        packages.append(dict(file_name=file_name, display_name=package['display_name'],
                             title_id=package['execution']['title_id'], license_mask=license_mask,
                             source=package['extracted_folder'], installed_path=str(destination)))
        print(f'{package["display_name"]}: {copied} files copied', flush=True)
    for directory in ('cache', 'logs'):
        (runtime / directory).mkdir(parents=True, exist_ok=True)
    (runtime / 'packages.json').write_text(json.dumps(packages, indent=2), encoding='utf-8')
    header = ['// Generated from the verified original package metadata.', '#pragma once',
              '#include <cstdint>', 'namespace bo3 {',
              'struct ContentPackageInfo { const char* file_name; const char16_t* display_name; uint32_t license_mask; };',
              'inline constexpr ContentPackageInfo kContentPackages[] = {']
    for package in packages:
        header.append('  {%s, u%s, 0x%08Xu},' % (json.dumps(package['file_name']),
                                                json.dumps(package['display_name']), package['license_mask']))
    header += ['};', '}  // namespace bo3', '']
    (ROOT / 'app/src/content_packages.h').write_text('\n'.join(header), encoding='utf-8')


if __name__ == '__main__':
    main()
