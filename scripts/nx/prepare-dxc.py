"""Install the pinned official portable DXC release, verifying its published hash."""
from pathlib import Path
import hashlib
import json
import urllib.request
import zipfile

root = Path(__file__).resolve().parents[2]
manifest = json.loads((root / 'analysis/native_dxc_provenance.json').read_text())
target = root / 'tools/dxc-1.9.2609'
archive = target / 'dxc_2026_09_29.zip'
target.mkdir(parents=True, exist_ok=True)
if not archive.is_file() or hashlib.sha256(archive.read_bytes()).hexdigest() != manifest['archive_sha256']:
    with urllib.request.urlopen(manifest['archive_url'], timeout=60) as response:
        data = response.read()
    if len(data) != manifest['archive_bytes'] or hashlib.sha256(data).hexdigest() != manifest['archive_sha256']:
        raise SystemExit('Official DXC archive digest mismatch')
    archive.write_bytes(data)
# A verified installation can be in use by the compilation workers. Do not
# extract over running executables or DLLs on an idempotent preparation call.
if all((target / name).is_file() and (target / name).stat().st_size == expected['bytes']
       and hashlib.sha256((target / name).read_bytes()).hexdigest() == expected['sha256']
       for name, expected in manifest['files'].items()):
    print('Portable DXC already verified; no extraction required')
    raise SystemExit(0)
with zipfile.ZipFile(archive) as package:
    for item in package.infolist():
        path = (target / 'package' / item.filename).resolve()
        if not path.is_relative_to((target / 'package').resolve()):
            raise SystemExit('Archive path escapes DXC directory')
    package.extractall(target / 'package')
for name, expected in manifest['files'].items():
    path = target / name
    if path.stat().st_size != expected['bytes'] or hashlib.sha256(path.read_bytes()).hexdigest() != expected['sha256']:
        raise SystemExit(f'Extracted DXC hash mismatch: {name}')
print('Portable DXC verified; no system installation required')
