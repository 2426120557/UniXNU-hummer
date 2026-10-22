#!/usr/bin/env python3
"""Build the hummer storage link stubs for the NativeUfs probe stage.

Modeled on build-hummer-platform-power.py: compile
kits/hummer/HummerStorageStubs.cpp with the exact compile record of the XNU
iokit build, generate the IOBlockStorageDevice placeholders from the driver
object's own undefined symbols, and archive both to

    build/hummer-storage-stubs/libHummerStorageStubs.a

which hummer_defconfig force-loads through XNU_EXTRA_LD_ARCHIVES (the same hook
build-native-probe-stubs.py already uses).

Why any of this is needed: NativeUfs.cpp is one translation unit that also
contains NativeUfsBlock : IOBlockStorageDevice, and that class only exists
inside the prelinked com.apple.iokit.IOStorageFamily kext - XNU 10063 has no
iokit/storage tree at all.  The probe path (native-ufs-probe=2) never
constructs the block device, so the references are dead at run time, but the
static link still needs them: `vtable for NativeUfsBlock' pulls in every
non-overridden virtual of the base class.

That list is not stable (it follows the storage-abi headers of the day), so the
placeholders are generated from `nm -u` of NativeUfs.o instead of being written
out by hand: for each undefined symbol belonging to an IOStorage family class,
emit a hidden function whose asm name is that mangled symbol and whose body
tail-calls the single refusal function.  Odd as it looks, a C++ function
declared with no parameters is ABI-compatible with any function we never return
from, so no signature is restated and nothing can drift from the headers.

Because this reads NativeUfs.o, it must run AFTER `build-arm-driver.py
NativeUfs' in the board's archive list.

Usage: build-hummer-storage-stubs.py
"""

import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/hummer-storage-stubs'
OUT.mkdir(parents=True, exist_ok=True)

RECORD = (ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
                / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
SOURCE = ROOT / 'kits/hummer/HummerStorageStubs.cpp'
STUB_OBJ = OUT / 'HummerStorageStubs.o'
PLACEHOLDER_SRC = OUT / 'StoragePlaceholders.cpp'
PLACEHOLDER_OBJ = OUT / 'StoragePlaceholders.o'
ARCHIVE = OUT / 'libHummerStorageStubs.a'

# The block driver under test, produced by the preceding archive builder.
DRIVER_OBJ = ROOT / 'build/native-ufs/NativeUfs.o'

# Mangled class-name fragments of the storage ABI we cannot link.  Itanium
# mangling prefixes the class name with its length, so `20IOBlockStorageDevice'
# is unambiguous and does not also match IOBlockStorageDeviceExtent.
STORAGE_CLASSES = ('20IOBlockStorageDevice', '8IOStorage', '7IOMedia',
                   '21IOStorageAttributes')

# Symbols this archive defines in C++ already; a second definition would clash.
DEFINED_IN_CXX = ('__ZTV20IOBlockStorageDevice',)

# Vtable / typeinfo / guard variables are data, not code.
DATA_PREFIXES = ('__ZTV', '__ZTI', '__ZTS', '__ZTT', '__ZGV')
DATA_BYTES = 256


def is_data_symbol(name):
    """True when the mangled name denotes an object rather than a function.

    In the Itanium ABI a nested name ends with 'E', so `_ZN20IOBlockStorageDevice
    10gMetaClassE` is the static data member gMetaClass, while a function keeps
    going past that 'E' into its parameter list.  Getting this wrong is fatal
    but silent: a code thunk placed where the linker expects an OSMetaClass
    would make NativeUfsBlock's metaclass register with a garbage superclass.
    """
    return name.startswith(DATA_PREFIXES) or name.endswith('E')


def run(command, **kwargs):
    return subprocess.check_output([str(v) for v in command], timeout=120,
                                   stderr=subprocess.STDOUT, **kwargs)


def compile_with_record(source, obj):
    """Compile one file using the XNU iokit build's own compile record."""
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
    (OUT / f'compile-{obj.stem}.json').write_text(json.dumps(command, indent=2) + '\n')
    output = run(command, cwd=record['directory'])
    (OUT / f'compile-{obj.stem}.log').write_text(
        output if isinstance(output, str) else output.decode())


def undefined_storage_symbols():
    """Undefined storage-ABI symbols the block driver still needs defined."""
    names = run(['xcrun', 'nm', '-u', '-j', DRIVER_OBJ], text=True).split()
    found = {name for name in names
             if any(marker in name for marker in STORAGE_CLASSES)}
    return sorted(found - set(DEFINED_IN_CXX))


def generate_placeholders(symbols):
    # Hiding everything is not cosmetic: prelink binds kext undefined symbols
    # against the kernel symbol table, so an exported placeholder would let a
    # real storage kext call our refusal instead of IOStorageFamily.
    lines = ['// Generated by scripts/build-hummer-storage-stubs.py. Do not edit.',
             '//',
             '// One placeholder per storage-ABI symbol NativeUfs.o still leaves',
             '// undefined.  Functions tail-call the refusal in HummerStorageStubs.cpp;',
             '// data placeholders are inert storage (nothing in the probe reads them).',
             '',
             '#pragma GCC visibility push(hidden)',
             '',
             'extern "C" void hummer_storage_unavailable(void);']
    code = 0
    data = 0
    for name in symbols:
        if is_data_symbol(name):
            alias = f'hummer_storage_data_{data}'
            data += 1
            lines += ['',
                      f'extern "C" char {alias}[{DATA_BYTES}] __asm__("{name}");',
                      f'char {alias}[{DATA_BYTES}] = {{}};']
        else:
            alias = f'hummer_storage_call_{code}'
            code += 1
            lines += ['',
                      f'extern "C" void {alias}(void) __asm__("{name}");',
                      f'void {alias}(void) {{ hummer_storage_unavailable(); }}']
    lines += ['', '#pragma GCC visibility pop', '']
    PLACEHOLDER_SRC.write_text('\n'.join(lines))
    return code, data


def build():
    compile_with_record(SOURCE, STUB_OBJ)

    # The stubs must stay out of the kernel's exported namespace.  Fail loudly if
    # the visibility attribute ever stops taking effect, or if a const object
    # silently falls back to internal linkage (C++ gives namespace-scope const
    # objects internal linkage unless they are also declared extern, which would
    # leave the link broken instead).
    names = run(['xcrun', 'nm', '-m', STUB_OBJ], text=True)
    (OUT / 'nm.txt').write_text(names)
    wanted = ('verified_root', 'vr_xnu_crypto', 'vr_open', 'vr_verify_block',
              'boot_progress_poll', '__ZTV20IOBlockStorageDevice')
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

    if not DRIVER_OBJ.is_file():
        raise SystemExit(f'{DRIVER_OBJ} missing: this builder must run after '
                         '`build-arm-driver.py NativeUfs`')
    symbols = undefined_storage_symbols()
    if not symbols:
        raise SystemExit('NativeUfs.o needs no storage placeholders; the stub archive is '
                         'unnecessary - remove it instead of linking an empty file')
    code, data = generate_placeholders(symbols)
    (OUT / 'symbols.txt').write_text('\n'.join(symbols) + '\n')
    print(f'hummer-storage-stubs: NativeUfs.o leaves {len(symbols)} storage symbols '
          f'undefined ({code} functions, {data} data)')
    for name in symbols:
        print('    ' + name)

    compile_with_record(PLACEHOLDER_SRC, PLACEHOLDER_OBJ)
    (OUT / 'placeholders-nm.txt').write_text(
        run(['xcrun', 'nm', '-m', PLACEHOLDER_OBJ], text=True))

    ARCHIVE.unlink(missing_ok=True)
    run(['xcrun', 'ar', 'rcs', ARCHIVE, STUB_OBJ, PLACEHOLDER_OBJ])
    print('hummer-storage-stubs: archive built ->', ARCHIVE.relative_to(ROOT))


if __name__ == '__main__':
    if not RECORD.is_file():
        raise SystemExit(f'{RECORD} missing: run after the kernel object tree exists')
    if not SOURCE.is_file():
        raise SystemExit(f'{SOURCE} missing: package/ did not map to kits/hummer')
    build()
