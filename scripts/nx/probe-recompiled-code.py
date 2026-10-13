"""Compile one existing BO3 translation unit for ARM64 and report blockers.

The adapted stage changes only build-local header copies. It is not a runtime
port, does not link BO3, and must never substitute for full console validation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--devkitpro', default='C:/devkitPro')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
sdk = root / 'thirdparty/rexglue-sdk'
output = root / 'build/nx-cpu-probe'
output.mkdir(parents=True, exist_ok=True)
compiler = Path(args.devkitpro) / 'devkitA64/bin/aarch64-none-elf-g++.exe'
source = root / 'app/generated/default_zm/bo3_zm_recomp.0.cpp'
common = [str(compiler), '-std=c++23', '-O2', '-march=armv8-a+crc+crypto',
          '-mtune=cortex-a57', '-mtp=soft', '-ftls-model=local-exec', '-fPIE',
          '-D__SWITCH__', '-D_GNU_SOURCE', '-DNDEBUG', '-DSPDLOG_FMT_EXTERNAL',
          '-DSPDLOG_NO_TZ_OFFSET', '-ffp-contract=off',
          '-fno-strict-aliasing', '-Wno-psabi', '-fmax-errors=5']
includes = [sdk / 'include', sdk / 'thirdparty/simde',
            sdk / 'thirdparty/fmt/include', sdk / 'thirdparty/spdlog/include',
            sdk / 'thirdparty/tomlplusplus/include',
            Path(args.devkitpro) / 'libnx/include']
common += [f'-I{path}' for path in includes]
report = {'compiler': subprocess.check_output([str(compiler), '--version'], text=True).splitlines()[0],
          'source': str(source.relative_to(root)),
          'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
          'functions_in_unit': source.read_text().count('DEFINE_REX_FUNC('),
          'scope': 'Compile-only sample, not a linked or runnable BO3 port', 'stages': []}

def compile_stage(name, input_path, extra):
    log = root / f'analysis/nx_cpu_probe_{name}.log'
    command = common[:1] + extra + common[1:] + ['-c', str(input_path), '-o', str(output / f'{name}.o')]
    process = subprocess.run(command, capture_output=True, text=True)
    log.write_text(process.stdout + process.stderr, newline='\n')
    report['stages'].append({'name': name, 'exit_code': process.returncode,
                             'log': str(log.relative_to(root)), 'command': command})
    print(f'{name}: compiler exit {process.returncode}; {log.relative_to(root)}')
    for line in process.stderr.splitlines():
        if 'error:' in line: print(line)

compile_stage('baseline', source, [])
platform = (sdk / 'include/rex/platform.h').read_text()
anchor = '#elif defined(__ANDROID__)'
assert anchor in platform
platform = platform.replace(anchor, '#elif defined(__SWITCH__)\n#define REX_PLATFORM_SWITCH 1\n'
                            '#define REX_PLATFORM_LINUX 1\n' + anchor, 1)
overlay = output / 'include/rex'
overlay.mkdir(parents=True, exist_ok=True)
(overlay / 'platform.h').write_text(platform, newline='\n')
header = (source.parent / 'bo3_zm_init.h').read_text()
alias = '__attribute__((alias("__imp__" #name))) REX_WEAK_FUNC(name);'
assert alias in header
header = header.replace(alias, 'REX_EXTERN(name) __attribute__((weak, alias("__imp__" #name)));', 1)
(output / 'bo3_zm_init.h').write_text(header, newline='\n')
copy = output / source.name
copy.write_bytes(source.read_bytes())
compile_stage('adapted', copy, [f'-I{output / "include"}'])
report['adapted_changes'] = ['Build-local Switch platform detection',
                            'Build-local explicit C linkage for GCC weak aliases']
(root / 'analysis/nx_cpu_probe.json').write_text(json.dumps(report, indent=2) + '\n', newline='\n')
