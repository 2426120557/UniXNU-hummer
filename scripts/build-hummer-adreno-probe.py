#!/usr/bin/env python3
"""Build the hummer Adreno 830 (a8xx) register liveness probe archive.

The engine calls native_adreno_firmware_probe() from bsd_init() after vfsinit
(offline-component-load-probes.patch) when the defconfig carries
CONFIG_XNU_ADRENO_FIRMWARE_PROBE=y, and build-xnu-4k-baseline.sh force-loads

    build/adreno-firmware-probe/libAdrenoFirmwareProbe.a

into the kernel when that option is on (and the build is not the bootstrap
pass, where only records are left behind).  So the board only has to produce
that archive with its own probe: this script compiles
kits/hummer/HummerAdrenoProbe.c with the exact compile record of the XNU bsd
build and archives it to that path.  No kernel source is patched.

The probe itself answers one question with hardware truth: do the GPU windows
the stock DTB advertises (kgsl 0x3d00000, rscc 0x3d50000, cx_misc 0x3d9e000,
sun-gpucc 0x3d90000) answer reads at all?  It is gated by boot arg
hummer-adreno-probe=1 and always ends in a panic so the values stay visible.

Usage: build-hummer-adreno-probe.py
"""

import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/adreno-firmware-probe'
OUT.mkdir(parents=True, exist_ok=True)

# The bsd build's own compile record: bsd_init.c is the file whose CFLAGS the
# engine extends with -DXNU_ADRENO_FIRMWARE_PROBE, so its record is the right
# command line for the probe (same as sheng's builder of the same name).
RECORD = (ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
                / 'bsd/DEVELOPMENT/bsd_init.o.json')
SOURCE = ROOT / 'kits/hummer/HummerAdrenoProbe.c'
OBJ = OUT / 'HummerAdrenoProbe.o'
ARCHIVE = OUT / 'libAdrenoFirmwareProbe.a'

# The symbol the engine's bsd_init hook resolves to.
WANTED = 'native_adreno_firmware_probe'


def run(command, **kwargs):
    return subprocess.check_output([str(v) for v in command],
                                   stderr=subprocess.STDOUT, **kwargs)


def compile_with_record(source, obj):
    """Compile one file using the XNU bsd build's own compile record."""
    record = json.loads(RECORD.read_text().strip().rstrip(','))
    arguments = record['arguments']
    command = []
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source))
        elif value == '-o':
            command += ['-o', str(obj)]
            index += 1
        elif value in ('-MF', '-MJ'):
            index += 1
        elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W') and value not in ('-MD', '-MP'):
            command.append(value)
        index += 1
    command += ['-Wno-unknown-warning-option']
    # Xcode's clang, not whatever llvm is first in PATH.
    if command and command[0] in ('clang', 'clang++'):
        command = ['xcrun', '--sdk', 'iphoneos', command[0]] + command[1:]
    (OUT / 'compile.json').write_text(json.dumps(command, indent=2) + '\n')
    output = run(command, cwd=record['directory'])
    (OUT / 'compile.log').write_text(
        output if isinstance(output, str) else output.decode())


def build():
    if not SOURCE.is_file():
        raise SystemExit(f'{SOURCE} missing: package/ did not map to kits/hummer')

    compile_with_record(SOURCE, OBJ)

    # The hook resolves extern "C" native_adreno_firmware_probe from this
    # archive; fail here rather than at link time if the name drifted.
    names = run(['xcrun', 'nm', '-m', OBJ], text=True)
    (OUT / 'nm.txt').write_text(names)
    if not any(WANTED in line and 'external' in line
               and 'non-external' not in line for line in names.splitlines()):
        raise SystemExit(f'hummer-adreno-probe: {WANTED} not exported:\n' + names)

    ARCHIVE.unlink(missing_ok=True)
    run(['xcrun', 'ar', 'rcs', ARCHIVE, OBJ])
    print('hummer-adreno-probe: archive built ->', ARCHIVE.relative_to(ROOT))


if __name__ == "__main__":
    if not RECORD.is_file():
        raise SystemExit(f'{RECORD} missing: run after the kernel object tree exists')
    build()
