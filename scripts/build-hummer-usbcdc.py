#!/usr/bin/env python3
"""Compile the vendored Qualcomm USB CDC log module for hummer (SM8750).

The module is QcomXnuUsbDebug (MIT, github.com/xiaoleGun/QcomXnuUsbDebug),
vendored under package/usbcdc/ at the revision recorded in its README. It is
plain kernel C: no IOKit, no C++, so every unit replays the osfmk startup
record -- the same record build-hummer-touch.py uses for GeniSpi/GSI/RPMh.

Two things are selected here and nowhere else:
  QUD_SOC_HEADER  the SM8750 profile (soc/Sm8750.h); the module refuses to build
                  without an explicit profile.
  XNU_QCOM_USB_DEBUG
                  compiles the XNU console hooks in kernel/. Until the two
                  darwin/10063 patches are rebased and applied, nothing calls
                  them; the archive is built but not force-loaded, so this first
                  round only proves the module compiles against xnu-10063.

Output: build/hummer-usbcdc/libHummerUsbCdc.a
"""
import json
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/hummer-usbcdc'
OUT.mkdir(parents=True, exist_ok=True)

C_RECORD = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json'
MODULE = ROOT / 'kits/hummer/usbcdc'

PROFILE = MODULE / 'soc/Sm8750.h'
SOURCES = [
    MODULE / 'soc/QcomUsbPrepare.c',
    MODULE / 'soc/QcomUsbQualify.c',
    MODULE / 'soc/QcomUsbBypass.c',
    MODULE / 'soc/QcomUsbDmaDomain.c',
    MODULE / 'usb/Dwc3.c',
    MODULE / 'kernel/QcomXnuUsbDebug.c',
    MODULE / 'kernel/QcomUsbScreenshot.c',
]
# QUD_SOC_HEADER is "soc/Sm8750.h", i.e. relative to the module root, so the
# module root itself has to be on the include path (QcomUsbSoc.h:12 includes it).
INCLUDES = [MODULE, MODULE / 'include', MODULE / 'soc', MODULE / 'usb', MODULE / 'kernel']


def run(command, cwd):
    result = subprocess.run([str(v) for v in command], cwd=str(cwd),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        raise SystemExit(result.stdout[-6000:])
    return result.stdout


def replay(record_path, source, destination):
    record = json.loads(record_path.read_text().strip().rstrip(','))
    command = []
    arguments = record['arguments']
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


for required in [C_RECORD, PROFILE] + SOURCES:
    if not required.exists():
        raise SystemExit(f'{required} is missing; the USB CDC build cannot proceed')

objects = []
for source in SOURCES:
    object_path = OUT / (source.stem + '.o')
    command, directory = replay(C_RECORD, source, object_path)
    command += ['-Wall', '-Wextra', '-mgeneral-regs-only',
                '-Wno-unused-parameter', '-Wno-unknown-warning-option',
                '-DQUD_SOC_HEADER="soc/Sm8750.h"', '-DXNU_QCOM_USB_DEBUG=1',
                '-I', str(ROOT / '.cache/xnu-work/osfmk')]
    for include in INCLUDES:
        command += ['-I', str(include)]
    if command and command[0] in ('clang', 'cc', 'clang++'):
        command = ['xcrun', '--sdk', 'iphoneos', command[0]] + command[1:]
    (OUT / f'compile-command-{source.stem}.json').write_text(json.dumps(command, indent=2) + '\n')
    run(command, directory)
    audit(object_path)
    objects.append(object_path)
    print(f'HummerUsbCdc: {source.name} -> {object_path.name}')

archive = OUT / 'libHummerUsbCdc.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive] + [str(o) for o in objects], check=True)
print(f'HummerUsbCdc: {len(objects)} objects archived into {archive.name} (profile soc/Sm8750.h)')
