"""Audit files eligible for the source repository before staging or upload."""
import json
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[1]
git = 'C:/Program Files/Git/cmd/git.exe'
untracked = subprocess.check_output(
    [git, '-C', str(root), 'ls-files', '--others', '--exclude-standard', '-z'])
tracked = subprocess.check_output([git, '-C', str(root), 'ls-files', '-z'])
names = set(n.decode('utf-8') for n in (untracked + tracked).split(b'\0') if n)
files = [dict(path=n, bytes=(root / n).stat().st_size) for n in names if (root / n).is_file()]
forbidden_roots = {'game_files', 'runtime', 'tools', 'build', '.local', '.aws', '.codex'}
forbidden_suffixes = {'.xex', '.xexp', '.ff', '.xpak', '.exe', '.dll', '.pdb', '.obj', '.lib'}
forbidden = [f['path'] for f in files
             if Path(f['path']).parts[0] in forbidden_roots
             or Path(f['path']).suffix.lower() in forbidden_suffixes]
for f in files:
    with (root / f['path']).open('rb') as stream:
        if stream.read(4).startswith((b'\x7fELF', b'MZ')):
            forbidden.append(f['path'])
oversize = [f for f in files if f['bytes'] >= 100 * 1024 * 1024]
report = dict(file_count=len(files), bytes=sum(f['bytes'] for f in files),
              largest=sorted(files, key=lambda f: f['bytes'], reverse=True)[:12],
              forbidden_files=forbidden, files_over_100_mib=oversize)
(root / 'analysis/repository-audit.json').write_text(json.dumps(report, indent=2))
print(json.dumps(report, indent=2))
if forbidden or oversize:
    raise SystemExit('Repository contains excluded content or oversized files')
