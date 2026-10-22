#!/usr/bin/env python3
"""Build the hummer storage link stubs (probe-stage placeholders).

Modeled on build-hummer-platform-power.py: compile
kits/hummer/HummerStorageStubs.cpp with the exact compile record of the XNU
iokit build, then archive it to

    build/hummer-storage-stubs/libHummerStorageStubs.a

which hummer_defconfig force-loads through XNU_EXTRA_LD_ARCHIVES (the same hook
build-native-probe-stubs.py already uses).

Why a separate archive instead of patching NativeUfs: NativeUfs.cpp is one
translation unit that also contains NativeUfsBlock : IOBlockStorageDevice, and
that class only exists inside the prelinked com.apple.iokit.IOStorageFamily
kext - XNU 10063 has no iokit/storage tree at all.  The probe path
(native-ufs-probe=2) never constructs the block device, so the references are
dead at run time, but the static link still needs them defined.  Keeping the
stubs in their own archive means they disappear the moment the real
IOStorageFamily binding and the VerifiedRoot archive are wired in - nothing in
NativeUfs has to change.

Every stub panics and every symbol is hidden, so a kext that resolves against
the kernel by mistake cannot silently bind to them; see the header comment in
package/HummerStorageStubs.cpp for the full reasoning.

Usage: build-hummer-storage-stubs.py
"""

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/hummer-storage-stubs'
OUT.mkdir(parents=True, exist_ok=True)

RECORD = (ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
                / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
SOURCE = ROOT / 'kits/hummer/HummerStorageStubs.cpp'
OBJ = OUT / 'HummerStorageStubs.o'
ARCHIVE = OUT / 'libHummerStorageStubs.a'


def run(command, **kwargs):
    return subprocess.check_output([str(v) for v in command], timeout=120,
                                   stderr=subprocess.STDOUT, **kwargs)


def build():
    record = json.loads(RECORD.read_text().strip().rstrip(','))
    arguments = record['arguments']
    command = []
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(SOURCE))
        elif value == '-o':
            command += ['-o', str(OBJ)]
            index += 1
        elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
            index += 1
        # Drop every -W flag: the record was produced by whatever Xcode ran the
        # iokit build, and this runner may not know all of them.
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    command += ['-Wno-unknown-warning-option']
    # Xcode's clang, not whatever llvm is first in PATH.
    if command and command[0] in ('clang', 'clang++'):
        command = ['xcrun', '--sdk', 'iphoneos', command[0]] + command[1:]
    (OUT / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
    output = run(command, cwd=record['directory'])
    (OUT / 'compile.log').write_text(output if isinstance(output, str) else output.decode())

    # The stubs must stay out of the kernel's exported namespace: prelink binds
    # kext undefined symbols against the kernel symbol table, and a global
    # placeholder here would let a real storage kext call our panic instead of
    # IOStorageFamily.  Fail loudly if the visibility attribute ever stops
    # taking effect, or if a const object silently falls back to internal
    # linkage (C++ gives namespace-scope const objects internal linkage unless
    # they are also declared extern, which would break the link instead).
    names = run(['xcrun', 'nm', '-m', OBJ], text=True)
    (OUT / 'nm.txt').write_text(names)
    wanted = ('__ZTV20IOBlockStorageDevice', 'verified_root', 'vr_xnu_crypto', 'vr_open',
              'vr_verify_block', 'boot_progress_poll')
    ours = [line for line in names.splitlines() if any(w in line for w in wanted)]
    missing = [w for w in wanted
               if not any(w in line and 'external' in line and 'non-external' not in line
                          for line in ours)]
    if missing:
        raise SystemExit('storage stubs not externally linked: ' + ', '.join(missing)
                         + '\n' + names)
    leaked = [line for line in ours
              if 'external' in line and 'non-external' not in line
              and 'private external' not in line]
    if leaked:
        raise SystemExit('storage stubs would be exported from the kernel: '
                         + '; '.join(leaked))

    ARCHIVE.unlink(missing_ok=True)
    run(['xcrun', 'ar', 'rcs', ARCHIVE, OBJ])
    print('hummer-storage-stubs: archive built ->', ARCHIVE.relative_to(ROOT))


if __name__ == '__main__':
    if not RECORD.is_file():
        raise SystemExit(f'{RECORD} missing: run after the kernel object tree exists')
    if not SOURCE.is_file():
        raise SystemExit(f'{SOURCE} missing: package/ did not map to kits/hummer')
    build()
