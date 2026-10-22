#!/usr/bin/env python3
"""Build the hummer original-PMGR nub publisher (sheng_pmgr_run provider).

Modeled on UniXNU-sheng's build-sheng-platform-power.py: compile
kits/hummer/HummerPlatformPower.cpp with the exact compile record of the XNU
iokit build, defer its C++ static initializer (the subclass metaclass's super
lives in the prelinked ApplePMGR kext, so it must register at run time, not
at kernel boot), bind the three original-ApplePMGR symbols to their
addresses in the 21H461 kernelcache, and archive everything to

    build/sheng-platform-power/libShengPlatformPower.a

which is exactly the path scripts/build-xnu-4k-baseline.sh force-loads when
XNU_SHENG_PMGR=1 (hummer_defconfig sets CONFIG_XNU_SHENG_PMGR=y).
"""

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/sheng-platform-power'
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT / 'kits/NativePrelink'))
try:
    from native_prelink import macho
except Exception:  # pragma: no cover - the kit may be absent on early passes
    macho = None

RECORD = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
SOURCE = ROOT / 'kits/hummer/HummerPlatformPower.cpp'
OBJ = OUT / 'HummerPlatformPower.o'
LATE = OUT / 'HummerPlatformPower.late.o'
BINDINGS_ASM = OUT / 'original-bindings.S'
BINDINGS_OBJ = OUT / 'original-bindings.o'
ARCHIVE = OUT / 'libShengPlatformPower.a'

# Addresses pinned from build/kernel-v21.macho (the exact kernelcache hummer
# boots; verified by disassembly):
#   0xfffffff006adf490  ApplePMGR::gMetaClass        (the object nub_create casts to)
#   0xfffffff0049f003c  ApplePMGR::ApplePMGR(OSMetaClass const*)  [C2]
#   0xfffffff0049f019c  ApplePMGR::~ApplePMGR()                      [D2]
#   0xfffffff004a22568  ApplePMGR nub factory   (allocates ApplePMGRNub, 0x740)
#   0xfffffff004a24adc  ApplePMGRNub read   (provider vtable slot 320)
#   0xfffffff004a230ec  ApplePMGRNub write  (provider vtable slot 321)
BINDINGS = {
    '__ZN9ApplePMGR10gMetaClassE': 0xfffffff006adf490,
    '__ZN9ApplePMGRC2EPK11OSMetaClass': 0xfffffff0049f003c,
    '__ZN9ApplePMGRD2Ev': 0xfffffff0049f019c,
    '_hummer_original_pmgr_nub_create': 0xfffffff004a22568,
    '_hummer_original_pmgr_nub_read': 0xfffffff004a24adc,
    '_hummer_original_pmgr_nub_write': 0xfffffff004a230ec,
}


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
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    command += ['-Wno-unknown-warning-option',
                '-I', str(OUT),
                '-I', str(ROOT / '.cache/xnu-work/osfmk')]
    # Xcode's clang, not whatever llvm is first in PATH.
    if command and command[0] in ('clang', 'clang++'):
        command = ['xcrun', '--sdk', 'iphoneos', command[0]] + command[1:]
    (OUT / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
    output = run(command, cwd=record['directory'])
    (OUT / 'compile.log').write_text(output if isinstance(output, str) else output.decode())

    # Defer the metaclass registration: strip the module initializer and
    # globalize the static-init function so sheng_pmgr_run can call it between
    # OSMetaClass::preModLoad/postModLoad at run time.
    run(['llvm-objcopy',
         '--remove-section=__DATA,__mod_init_func',
         '--globalize-symbol=__GLOBAL__sub_I_HummerPlatformPower.cpp',
         OBJ, LATE])

    BINDINGS_ASM.write_text(''.join(
        f'.globl {name}\n.set {name}, {va:#x}\n' for name, va in BINDINGS.items()))
    sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk,
         '-c', BINDINGS_ASM, '-o', BINDINGS_OBJ])

    # Sanity: the only ApplePMGR imports our object may leave undefined are the
    # three bound above; anything else means the ABI shim drifted and must be
    # bound deliberately, not silently.
    raw = OBJ.read_bytes()
    undefined = set()
    if macho is not None:
        info = macho(raw)
        symcmd = next((at for cmd, at, _ in info['commands'] if cmd == 2), None)
        if symcmd is not None:
            import struct
            at, count, strings, _ = struct.unpack_from('<4I', raw, symcmd + 8)
            for i in range(count):
                ix, kind, _, _, _ = struct.unpack_from('<IBBHQ', raw, at + i * 16)
                if kind != 1:
                    continue
                name = raw[strings + ix:raw.index(b'\0', strings + ix)].decode()
                if 'ApplePMGR' in name:
                    undefined.add(name)
    bound = set(BINDINGS) - {n for n in BINDINGS if n.startswith('_hummer_')}
    missing = sorted(undefined - bound)
    if missing:
        raise SystemExit(f'unbound ApplePMGR imports: {missing}; add deliberate bindings')

    ARCHIVE.unlink(missing_ok=True)
    run(['xcrun', 'ar', 'rcs', ARCHIVE, LATE, BINDINGS_OBJ])
    print('hummer-platform-power: archive built ->', ARCHIVE.relative_to(ROOT))


if __name__ == '__main__':
    if not RECORD.is_file():
        raise SystemExit(f'{RECORD} missing: run after the kernel object tree exists')
    if not SOURCE.is_file():
        raise SystemExit(f'{SOURCE} missing: package/ did not map to kits/hummer')
    build()
