"""Rank BO3 XDK function candidates using original BO2 symbol names.

This reads generated instruction comments, never BO2's broken native renderer.
Scores nominate candidates only: confirm instructions, device fields and live
calls before adding hooks. Reference map/generated code must describe the same
BO2 executable. The matching technique follows the SA2 porting guide; this is a
local implementation and does not import its renderer or game-specific offsets.
"""
import argparse
import collections
import difflib
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--reference-root', type=Path, default=Path('C:/BO2recomp/Project BO2'))
parser.add_argument('--mode', choices=['zombies', 'multiplayer'], default='zombies')
parser.add_argument('--output', type=Path, default=ROOT / 'analysis/bo3_d3d_candidates.json')
args = parser.parse_args()
start = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]+)\)')
instruction = re.compile(r'^\s*// ([a-z][a-z0-9.]*)(?:\s+(.*))?$')
number = re.compile(r'(?<![a-zA-Z_])(?:-?0x[0-9a-fA-F]+|-?\d+)(?![a-zA-Z_])')
wanted = {'D3DDevice_DrawVertices', 'D3DDevice_DrawIndexedVertices',
          'D3DDevice_DrawVerticesUP', 'D3DDevice_DrawIndexedVerticesUP',
          'D3DDevice_CreateVertexShader', 'D3DDevice_CreatePixelShader',
          'D3DDevice_Swap', 'D3DDevice_Resolve', 'D3DDevice_ResolveEx',
          'D3DDevice_GetVertexShader', 'D3DDevice_GetPixelShader',
          'D3DDevice_SetVertexShader', 'D3DDevice_SetPixelShader',
          'D3DDevice_BeginCommandBuffer', 'D3DDevice_RunCommandBuffer'}

def load_functions(directory):
    functions = {}
    for path in sorted(directory.glob('*_recomp.*.cpp')):
        address = None
        with path.open(errors='replace') as stream:
            for line in stream:
                match = start.match(line)
                if match:
                    address = int(match[1], 16)
                    functions.setdefault(address, [])
                    continue
                match = instruction.match(line)
                if address is not None and match and len(functions[address]) < 600:
                    functions[address].append((match[1], number.sub('#', match[2] or '')))
    return {a: t for a, t in functions.items() if t}

reference_map = args.reference_root / 'rfazio/t6/default_mp.map'
names = {}
for line in reference_map.read_text(errors='replace').splitlines():
    match = re.match(r'\s*\w+:\w+\s+(D3DDevice_\w+)\s+([0-9a-fA-F]{8})\s+f\b', line)
    if match and match[1] in wanted:
        names[match[1]] = int(match[2], 16)
reference = load_functions(args.reference_root / 'rexglue-bo2/generated/default_mp')
directory = ROOT / ('app/generated/default_zm' if args.mode == 'zombies' else 'app/multiplayer/generated/default')
target = load_functions(directory)
histograms = {a: collections.Counter(t[0] for t in tokens) for a, tokens in target.items()}
report = []
for name, address in sorted(names.items()):
    tokens = reference.get(address)
    if not tokens:
        report.append({'name': name, 'reference': hex(address), 'status': 'reference function absent'})
        continue
    histogram = collections.Counter(t[0] for t in tokens)
    candidates = []
    for candidate, sequence in target.items():
        if not 0.6 <= len(sequence) / len(tokens) <= 1.6:
            continue
        overlap = sum((histogram & histograms[candidate]).values())
        score = 2 * overlap / (len(tokens) + len(sequence))
        candidates.append((score, candidate))
    ranked = sorted((difflib.SequenceMatcher(None, tokens, target[candidate], autojunk=False).ratio(), candidate)
                    for _, candidate in sorted(candidates, reverse=True)[:40])
    best = [{'address': f'0x{candidate:08X}', 'ratio': round(score, 5), 'instruction_count': len(target[candidate])}
            for score, candidate in reversed(ranked[-5:])]
    report.append({'name': name, 'reference': f'0x{address:08X}', 'reference_instructions': len(tokens),
                   'status': 'unverified candidates; no hook authorized by score', 'candidates': best})
    print(name, best[:2], flush=True)
args.output.write_text(json.dumps({'mode': args.mode, 'reference_map': str(reference_map),
    'reference_map_sha256': hashlib.sha256(reference_map.read_bytes()).hexdigest(),
    'reference_functions': len(reference), 'target_functions': len(target),
    'method': 'instruction-comment sequence; registers retained, numeric immediates normalized; maximum 600 instructions',
    'limitations': ['BO2 renderer is broken and unused', 'scores are not address verification',
                    'split functions and changed register allocation can lower scores'], 'functions': report}, indent=2) + '\n')
