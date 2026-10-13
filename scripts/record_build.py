"""Record local provenance for the compiled BO3 prototype."""
import hashlib
import json
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[1]
paths = ['build/bo3_zm.exe', 'build/bo3_zm.pdb', 'build/rexruntime.dll',
         'build/rexgpu-xenos.dll', 'build/TracyClient.dll',
         'app/bo3_zm_manifest.toml', 'scripts/build.cmd',
         'runtime/game/default_zm.xex', 'runtime/game/default_zm.xexp',
         'tools/rexglue-bootstrap/rexglue.exe', 'tools/llvm-20.1.8/bin/clang++.exe']
if (root / 'build/bo3_mp.exe').exists():
    paths += ['build/bo3_mp.exe', 'build/bo3_mp.pdb',
              'app/multiplayer/bo3_mp_manifest.toml',
              'runtime/game/default.xex', 'runtime/game/default.xexp']
files = {}
for relative in paths:
    path = root / relative
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    files[relative] = dict(bytes=path.stat().st_size, sha256=digest)
result = dict(compiler=subprocess.check_output(
    [str(root / 'tools/llvm-20.1.8/bin/clang++.exe'), '--version'], text=True).strip(),
    sdk=json.loads((root / 'thirdparty/rexglue-sdk/SOURCE_SNAPSHOT.json').read_text()),
    files=files)
(root / 'analysis/build_provenance.json').write_text(json.dumps(result, indent=2), newline='\n')
print('Recorded native binaries, toolchain, manifest and runtime image hashes')
