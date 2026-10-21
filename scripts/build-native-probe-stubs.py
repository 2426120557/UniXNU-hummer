#!/usr/bin/env python3
"""Build the hummer offline-probe stub archive against the source-kernel ABI.

Compiles package/NativeProbeStubs.cpp with the exact compile record of the XNU
build (like build-arm-driver.py) and archives it. The resulting libNativeProbeStubs.a
is force-loaded through CONFIG_XNU_EXTRA_LD_ARCHIVES in hummer_defconfig.
"""
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
recipe = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
record = json.loads(recipe.read_text().strip().rstrip(','))

source = ROOT / 'kits/hummer/NativeProbeStubs.cpp'
out = ROOT / 'build/native-probe-stubs'
out.mkdir(parents=True, exist_ok=True)
destination = out / 'NativeProbeStubs.o'

arguments = record['arguments']
command = []
index = 0
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(source))
    elif value == '-o':
        command.extend(['-o', str(destination)])
        index += 1
    elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W'):
        command.append(value)
    index += 1
command.extend(['-Wno-unknown-warning-option', '-I', str(ROOT / '.cache/xnu-work/osfmk')])

# Force Xcode's clang (Homebrew llvm 23 in PATH drops -Wno-unnecessary-virtual-specifier).
if command and command[0] in ('clang', 'clang++'):
    command = ['xcrun', '--sdk', 'iphoneos', command[0]] + command[1:]

(out / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
result = subprocess.run(command, cwd=record['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
(out / 'compile.log').write_text(result.stdout)
if result.returncode:
    raise SystemExit(result.stdout[-3500:])

archive = out / 'libNativeProbeStubs.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, destination], check=True)
print('NativeProbeStubs: stub archive built')
