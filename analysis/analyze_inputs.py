"""Read-only BO3 intake audit using the local RexGlue XEX/STFS format definitions.

Reference: rexglue-sdk include/rex/system/util/xex2_info.h and
include/rex/filesystem/devices/stfs_xbox.h (BSD-3-Clause).
No package signatures are authenticated by this tool.
"""
import argparse
import hashlib
import json
import mmap
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def u32(data, offset):
    return struct.unpack_from('>I', data, offset)[0]


def version(value):
    return f'{value >> 28}.{(value >> 24) & 15}.{(value >> 8) & 65535}.{value & 255}'


def sha_file(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def execution(data, offset):
    return dict(media_id=f'{u32(data, offset):08X}',
                version=version(u32(data, offset + 4)),
                base_version=version(u32(data, offset + 8)),
                title_id=f'{u32(data, offset + 12):08X}')


def xex(path):
    data = path.read_bytes()
    if data[:4] != b'XEX2':
        raise ValueError(f'Not XEX2: {path}')
    optional = dict(struct.unpack_from('>II', data, 24 + 8 * i)
                    for i in range(u32(data, 20)))
    sec = u32(data, 16)
    result = dict(path=str(path), size=len(data), sha256=hashlib.sha256(data).hexdigest(),
                  module_flags=f'{u32(data, 4):08X}',
                  execution=execution(data, optional[0x40006]) if 0x40006 in optional else None,
                  image_size=u32(data, sec + 4), load_address=f'{u32(data, sec + 0x110):08X}',
                  region=f'{u32(data, sec + 0x178):08X}',
                  signature_sha1=hashlib.sha1(data[sec + 8:sec + 0x108]).hexdigest(),
                  optional_headers={f'{k:08X}': f'{v:08X}' for k, v in optional.items()})
    if 0x10100 in optional:
        result['entrypoint'] = f'{optional[0x10100]:08X}'
    if 0x3FF in optional:
        off = optional[0x3FF]
        result['encryption'], result['compression'] = struct.unpack_from('>HH', data, off + 4)
    if 0x5FF in optional:
        off = optional[0x5FF]
        result['delta'] = dict(target_version=version(u32(data, off + 4)),
                               source_version=version(u32(data, off + 8)),
                               source_signature_sha1=data[off + 12:off + 32].hex())
    return result


def package(path, extracted, verify):
    with path.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        if data[:4] not in (b'LIVE', b'PIRS', b'CON '):
            raise ValueError(f'Not STFS: {path}')
        header_size = u32(data, 0x340)
        origin = (header_size + 4095) & ~4095
        descriptor = 0x379
        flags = data[descriptor + 2]
        if not flags & 1:
            raise ValueError('Audit currently supports read-only STFS packages only')
        if u32(data, 0x3A9) != 0:
            raise ValueError('Audit requires STFS volume')
        table_count = struct.unpack_from('<H', data, descriptor + 3)[0]
        table_block = int.from_bytes(data[descriptor + 5:descriptor + 8], 'little')

        def block_offset(block):
            physical, divisor = block, 170
            for _ in range(3):
                physical += (block + divisor) // divisor
                if block < divisor:
                    break
                divisor *= 170
            return origin + physical * 4096

        def next_block(block):
            hash_block = 0 if block < 170 else (block // 170) * 171 + block // 28900 + 1 + int(block >= 28900)
            off = origin + hash_block * 4096 + (block % 170) * 24
            return u32(data, off + 20) & 0xFFFFFF

        entries, files, seen_tables = {}, [], set()
        for table_index in range(table_count):
            if table_block in seen_tables or table_block == 0xFFFFFF:
                raise ValueError('Invalid file-table chain')
            seen_tables.add(table_block)
            table_off = block_offset(table_block)
            for index in range(64):
                off = table_off + 64 * index
                if not data[off]:
                    break
                flag = data[off + 40]
                name = data[off:off + (flag & 63)].decode('utf-8')
                parent = struct.unpack_from('>H', data, off + 50)[0]
                relative = name if parent == 0xFFFF else entries[parent] + '/' + name
                if any(part in ('..', '.') for part in relative.split('/')) or ':' in relative:
                    raise ValueError(f'Invalid relative path {relative!r}')
                entries[table_index * 64 + index] = relative
                if not flag & 128:
                    files.append(dict(path=relative, size=u32(data, off + 52),
                                      start_block=int.from_bytes(data[off + 47:off + 50], 'little')))
            table_block = next_block(table_block)
        if table_block != 0xFFFFFF:
            raise ValueError('File table exceeds declared count')
        missing, size_mismatches, hash_mismatches = [], [], []
        for item in files:
            target = extracted / item['path']
            if not target.is_file():
                missing.append(item['path'])
                continue
            if target.stat().st_size != item['size']:
                size_mismatches.append(item['path'])
                continue
            if verify:
                digest, remaining, block, seen = hashlib.sha256(), item['size'], item['start_block'], set()
                while remaining:
                    if block == 0xFFFFFF or block in seen:
                        raise ValueError(f'Invalid data chain: {item["path"]}')
                    seen.add(block)
                    count = min(remaining, 4096)
                    off = block_offset(block)
                    chunk = data[off:off + count]
                    if len(chunk) != count:
                        raise ValueError('Truncated package')
                    digest.update(chunk)
                    remaining -= count
                    block = next_block(block)
                item['sha256'] = digest.hexdigest()
                if item['sha256'] != sha_file(target):
                    hash_mismatches.append(item['path'])
        expected = {item['path'].casefold() for item in files}
        extras = [str(p.relative_to(extracted)) for p in extracted.rglob('*')
                  if p.is_file() and p.relative_to(extracted).as_posix().casefold() not in expected]
        result = dict(path=str(path), magic=data[:4].decode(), size=len(data),
                      content_type=f'{u32(data, 0x344):08X}', execution=execution(data, 0x354),
                      display_name=data[0x411:0x511].decode('utf-16-be').split('\0')[0],
                      file_count=len(files), extracted_folder=str(extracted),
                      missing=missing, size_mismatches=size_mismatches, extra_files=extras,
                      hashes_verified=verify, hash_mismatches=hash_mismatches, files=files)
        print(json.dumps({k: v for k, v in result.items() if k != 'files'}), flush=True)
        return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    intake = ROOT / 'game_files'
    binaries = [xex(p) for p in sorted(intake.rglob('*')) if p.suffix in ('.xex', '.xexp')]
    pairs = []
    for base in (b for b in binaries if b['path'].endswith('.xex')):
        patch = next(b for b in binaries if Path(b['path']).name == Path(base['path']).name + 'p')
        pairs.append(dict(base=base['path'], patch=patch['path'],
                          title_id_matches=(base['execution']['title_id'] == patch['execution']['title_id']) if patch['execution'] else None,
                          media_id_matches=(base['execution']['media_id'] == patch['execution']['media_id']) if patch['execution'] else None,
                          source_version_matches=base['execution']['version'] == patch['delta']['source_version'],
                          source_signature_matches=base['signature_sha1'] == patch['delta']['source_signature_sha1'],
                          target_version=patch['delta']['target_version']))
    print(json.dumps(dict(binaries=binaries, pairs=pairs)), flush=True)
    mappings = {'compat1 ': 'compatibility1', 'compat2 ': 'compatibility2',
                'dlc0 ': 'optional_dlc0', 'dlc1 ': 'optional_dlc1', 'tu000': 'title_update'}
    packages = [package(p, intake / folder, args.verify) for prefix, folder in mappings.items()
                for p in ROOT.iterdir() if p.is_file() and p.name.startswith(prefix)]
    result = dict(binaries=binaries, patch_pairs=pairs, packages=packages)
    (ROOT / 'analysis' / 'input_audit.json').write_text(json.dumps(result, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
