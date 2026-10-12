"""Run the pinned generator, keeping unchanged generated-file timestamps.

Code generation rewrites every file. Preserving timestamps of byte-identical
files avoids compiling identical C++ during repeated function-discovery work.
"""
import hashlib
import subprocess
from pathlib import Path
import os

root = Path(__file__).resolve().parents[1]
generated = root / 'app/generated/default_zm'
before = {}
for path in generated.glob('*'):
    if path.is_file():
        before[path] = (hashlib.sha256(path.read_bytes()).digest(), path.stat())
result = subprocess.run([str(root / 'tools/rexglue-bootstrap/rexglue.exe'),
                         'codegen', str(root / 'app/bo3_zm_manifest.toml')], cwd=root)
unchanged = 0
if result.returncode == 0:
    for path, (digest, stat) in before.items():
        if path.exists() and hashlib.sha256(path.read_bytes()).digest() == digest:
            os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
            unchanged += 1
print(f'Preserved {unchanged} byte-identical generated files')
raise SystemExit(result.returncode)
