"""Preserve the working BO2 SDK sources and bootstrap binaries independently."""
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path('C:/BO2recomp/Project BO2/rexglue-sdk')
DEST = ROOT / 'thirdparty/rexglue-sdk'
SKIP = {'.git', 'out', '__pycache__', '.cache', 'build', '_build', '.vs'}


def ignore(directory, names):
    return [name for name in names if name in SKIP]


def main():
    DEST.parent.mkdir(parents=True, exist_ok=True)
    if DEST.exists():
        raise SystemExit(f'Snapshot already exists: {DEST}')
    metadata = ROOT / 'sdk_snapshot'
    metadata.mkdir(exist_ok=True)
    git = 'C:/Program Files/Git/cmd/git.exe'
    def capture(*args):
        return subprocess.check_output([git, '-c', 'safe.directory=*', '-C', str(SOURCE), *args], text=True)
    revision = capture('rev-parse', 'HEAD').strip()
    (metadata / 'original-local-changes.patch').write_text(
        capture('-c', 'core.autocrlf=false', 'diff', '--binary', '--ignore-submodules=all'),
        encoding='utf-8')
    dependencies = []
    for dependency in sorted((SOURCE / 'thirdparty').iterdir()):
        if dependency.is_dir() and (dependency / '.git').exists():
            result = subprocess.run([git, '-c', 'safe.directory=*', '-C', str(dependency),
                                     'rev-parse', 'HEAD'], capture_output=True, text=True)
            dependencies.append(f'{dependency.name}: {result.stdout.strip() or result.stderr.strip()}')
    (metadata / 'dependency-revisions.txt').write_text('\n'.join(dependencies), encoding='utf-8')
    print('Copying SDK source snapshot...', flush=True)
    shutil.copytree(SOURCE, DEST, ignore=ignore)
    bootstrap = ROOT / 'tools/rexglue-bootstrap'
    bootstrap.mkdir(parents=True, exist_ok=True)
    for path in (SOURCE / 'out/win-amd64').iterdir():
        if path.is_file():
            shutil.copy2(path, bootstrap / path.name)
    print('Hashing preserved source and bootstrap files...', flush=True)
    files = {}
    for directory in (DEST, bootstrap):
        for path in directory.rglob('*'):
            if path.is_file():
                with path.open('rb') as stream:
                    digest = hashlib.file_digest(stream, 'sha256').hexdigest()
                files[path.relative_to(ROOT).as_posix()] = dict(bytes=path.stat().st_size, sha256=digest)
    manifest = dict(source=str(SOURCE), revision=revision, excluded_directory_names=sorted(SKIP), files=files)
    (metadata / 'snapshot-manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    (DEST / 'SOURCE_SNAPSHOT.json').write_text(json.dumps(dict(original_source=str(SOURCE), revision=revision,
        manifest='../../sdk_snapshot/snapshot-manifest.json'), indent=2), encoding='utf-8')
    print(f'Preserved {len(files)} files at {DEST}', flush=True)


if __name__ == '__main__':
    main()
