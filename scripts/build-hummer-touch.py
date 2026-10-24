#!/usr/bin/env python3
"""Compile the hummer Synaptics S3910 touch probe against the actual source-kernel ABI.

Two compile records are replayed, exactly the two sheng's touch build replays:
  - the iokit driver record (IOPlatformExpert.cpo.json) for the C++ probe, which
    includes IOKit headers and links against the source kernel's C++ ABI;
  - the osfmk startup record (startup.o.json) for the generic GENI SPI library
    (drivers/NativeGeniSpi/GeniSpi.c, a submodule of the pinned UniXNU engine),
    compiled with the kernel C flags.
Both objects are archived into build/hummer-touch/libHummerTouch.a, which
hummer_defconfig force-loads via XNU_EXTRA_LD_ARCHIVES. The probe includes
"GeniSpi.h" and "HummerTouchBoard.h" (from the kit), so both include directories
are appended to the replayed command.
"""
import json
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/hummer-touch'
OUT.mkdir(parents=True, exist_ok=True)

CPP_RECORD = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
C_RECORD = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json'
PROBE = ROOT / 'kits/hummer/HummerTouchProbe.cpp'
GENI = ROOT / 'drivers/NativeGeniSpi/GeniSpi.c'


def run(command, cwd):
    result = subprocess.run([str(v) for v in command], cwd=str(cwd),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        raise SystemExit(result.stdout[-3500:])
    return result.stdout


def replay(record_path, source, destination):
    record = json.loads(record_path.read_text().strip().rstrip(','))
    command = []
    arguments = record['arguments']
    index = 0
    # Identical replay loop to build-arm-driver.py (the proven one for the iokit
    # record): swap the input file, redirect -o, drop warning-flag pairs. Combined
    # dependency flags like -MF<path> pass through untouched.
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
    return command, record['directory']


def audit(object_path):
    data = object_path.read_bytes()
    magic, cpu, subtype, kind, count, size = struct.unpack_from('<6I', data)
    if (magic, cpu, subtype, kind) != (0xfeedfacf, 0x100000c, 0, 1):
        raise SystemExit(f'Unexpected platform object architecture in {object_path.name}')
    offset = 32
    deployment = None
    for _ in range(count):
        cmd, length = struct.unpack_from('<II', data, offset)
        if cmd == 0x32:
            deployment = struct.unpack_from('<II', data, offset + 8)
        offset += length
    if deployment != (2, 0x110000):
        raise SystemExit(f'Unexpected platform deployment {deployment}')


for required in (CPP_RECORD, C_RECORD, PROBE, ROOT / 'drivers/NativeGeniSpi/GeniSpi.h'):
    if not required.exists():
        raise SystemExit(f'{required} is missing; the touch build cannot proceed')

cpp_command, cpp_dir = replay(CPP_RECORD, PROBE, OUT / 'HummerTouchProbe.o')
cpp_command += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                '-Wno-unknown-warning-option', '-Wno-unnecessary-virtual-specifier',
                '-I', str(ROOT / '.cache/xnu-work/osfmk'),
                '-I', str(ROOT / 'kits/hummer'), '-I', str(ROOT / 'drivers/NativeGeniSpi')]
if cpp_command and cpp_command[0] in ('clang', 'clang++'):
    cpp_command = ['xcrun', '--sdk', 'iphoneos', cpp_command[0]] + cpp_command[1:]
(OUT / 'compile-command.json').write_text(json.dumps(cpp_command, indent=2) + '\n')
run(cpp_command, cpp_dir)
audit(OUT / 'HummerTouchProbe.o')

c_command, c_dir = replay(C_RECORD, GENI, OUT / 'GeniSpi.o')
c_command += ['-Wall', '-Wextra', '-Werror', '-mgeneral-regs-only',
              '-I', str(ROOT / '.cache/xnu-work/osfmk'),
              '-I', str(ROOT / 'drivers/NativeGeniSpi')]
if c_command and c_command[0] in ('clang', 'cc', 'clang++'):
    c_command = ['xcrun', '--sdk', 'iphoneos', c_command[0]] + c_command[1:]
(OUT / 'compile-command-geni.json').write_text(json.dumps(c_command, indent=2) + '\n')
run(c_command, c_dir)
audit(OUT / 'GeniSpi.o')

archive = OUT / 'libHummerTouch.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, OUT / 'HummerTouchProbe.o', OUT / 'GeniSpi.o'], check=True)
print('HummerTouch: -Werror build and ARM64/iOS 17 object audit passed (probe + GeniSpi)')
