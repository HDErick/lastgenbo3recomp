"""Run the native C++ planner against the eight GPU parity captures."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--input', type=Path, required=True)
p.add_argument('--tool', type=Path, default=Path('build/pc-vulkan/bo3-native/bo3_native_memexport_test.exe'))
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
results = []
for file in sorted(a.input.glob('*.json')):
    r = json.loads(file.read_text())
    def path(name):
        value = r[name]
        if Path(value).name != value or '/' in value or '\\' in value:
            raise ValueError('Unsafe basename')
        return file.parent / value
    code = path('microcode').read_bytes()
    identity = hashlib.sha256(b''.join(code[i:i+4][::-1] for i in range(0, len(code), 4))).hexdigest()
    result = subprocess.run([str(a.tool.resolve()), str(path('registers')), str(path('input')),
                             str(path('before')), str(path('output')), identity, str(r['count'])],
                            capture_output=True, text=True, timeout=30)
    results.append({'record': file.name, 'returncode': result.returncode,
                    'output': result.stdout.strip(), 'error': result.stderr.strip()})
passed = sum(x['returncode'] == 0 for x in results)
a.output.write_text(json.dumps({'samples': len(results), 'passed': passed,
    'planner_sha256': hashlib.sha256(Path('nx/native/memexport_copy.h').read_bytes()).hexdigest(),
    'tool_sha256': hashlib.sha256(a.tool.read_bytes()).hexdigest(), 'results': results}, indent=2) + '\n')
print(json.dumps({'samples': len(results), 'passed': passed}))
raise SystemExit(0 if results and passed == len(results) else 2)
