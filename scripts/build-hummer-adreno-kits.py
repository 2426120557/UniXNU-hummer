#!/usr/bin/env python3
"""Build build/native-adreno/libNativeAdreno.a for hummer (SM8750, Adreno 830).

The engine links exactly one archive for the GPU:

    build/native-adreno/libNativeAdreno.a      (build-xnu-4k-baseline.sh:592/738,
                                                only when XNU_NATIVE_ADRENO=1)

At this engine revision there is no component system and no
scripts/build-arm-driver.py, so the whole archive is assembled here:

  drivers/NativeAdreno/*.cpp        the driver (GMU, SMMU context, VM, ring,
                                    submit, IRQ, CP init, user client, service)
  kits/AdrenoFirmware/*.c           HFI transport, startup protocol, ARC votes,
                                    firmware segment planner
  kits/ArmSmmu4K/*.c                native 4 KiB SMMU tables, streams, live audit
  kits/AdrenoPM4/*.c                PM4 packet encoder
  kits/hummer/ShengGmu*.c           this board's SM8750 platform layer: GMU load
  kits/hummer/ShengGpuSmmu*.c       and start, GPU SMMU stream routing, AOP
  kits/hummer/ShengGmuCommandDb*.c  command-database parser
  firmware.S                        the two pinned blobs as strong symbols

NativeAdrenoAccelerator.cpp and NativeAdrenoPublish.cpp are deliberately not
compiled; see EXCLUDED below for why. Everything else in the list compiles
against this revision -- the first CI run archived 49 objects and then failed only
at the kernel link, on the three board symbols Publish.cpp wants.

The firmware images come from scripts/fetch-hummer-gpu-firmware.py, which verifies
linux-firmware provenance by SHA-256; the blobs stay out of Git. They become the
strong definitions that replace NativeAdrenoFirmwareDefaults.cpp's empty weak ones.

Usage: build-hummer-adreno-kits.py   (run after the kernel object tree exists)
"""

import hashlib
import json
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/native-adreno'
OUT.mkdir(parents=True, exist_ok=True)

# C compiles replay the bsd build's record; C++ replays IOKit's, the same pair
# sheng's builder uses. Both directories exist once the kernel objects are built.
RECORD_C = (ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
                 / 'bsd/DEVELOPMENT/bsd_init.o.json')
RECORD_CXX = (ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
                   / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')

FIRMWARE = ROOT / '.cache/firmware/hummer-gpu'
FIRMWARE_PINS = [
    ('_native_adreno_firmware_image', 'qcom/gen80000_gmu.bin',
     '1ad8175e6cd01ea76c64d20b83600038c52dd29844b48d57b11b769b038863a5'),
    ('_native_adreno_sqe_firmware_image', 'qcom/gen80000_sqe.fw',
     '30ee3301534f95799d4afaf73864f3c369baa5a5dea8481e7f0cb8cc941f396c'),
]

INCLUDES = [
    'kits/ArmSmmu4K',
    'kits/AdrenoPM4',
    'kits/AdrenoFirmware',
    'kits/hummer',
    # package/HummerAdrenoBringup.cpp lives in kits/hummer but includes
    # NativeAdrenoGmuContext.h and friends, which sit in the driver directory.
    'drivers/NativeAdreno',
    '.cache/xnu-work/osfmk',
]

# C symbols the archive must define, checked with nm before it is accepted.
# C++ classes are checked by substring because nm prints mangled names.
REQUIRED_C = [
    'ShengGmuColdStart',
    'ShengGmuLoadAndStartHfi',
    'ShengGmuBindMmio',
    'ShengGpuSmmuStart',
    'ShengGpuSmmuObserve',
    'ShengGmuCommandDbInputs',
    'AdrenoFirmwarePlanGmu',
    'AdrenoHfiStartupRun',
    'ArmSmmu4KAttachStreams',
    'AdrenoPM4Type7',
]
REQUIRED_CLASSES = [
    'NativeAdrenoService',
    'NativeAdrenoGmuContext',
    'NativeAdrenoRing',
]


def run(command, **kwargs):
    try:
        return subprocess.check_output([str(v) for v in command],
                                       stderr=subprocess.STDOUT, **kwargs)
    except subprocess.CalledProcessError as exc:
        text = exc.output
        if isinstance(text, bytes):
            text = text.decode('utf-8', 'replace')
        (OUT / 'compile-failure.log').write_text(text or '(no output)')
        print(text or '(no output)')
        raise


def replay(record_path, source, obj, extra=()):
    """Compile one file with the exact flags the matching XNU object used."""
    record = json.loads(record_path.read_text().strip().rstrip(','))
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
    command += ['-Wno-unknown-warning-option', '-Wno-unused-parameter']
    # -Wall/-Wextra without -Werror on purpose: this is a first compile of the
    # full driver against a revision it was not written for, and a warning about
    # an unused parameter is not worth losing the build log over. Tighten once the
    # archive compiles.
    command += ['-Wall', '-Wextra']
    for directory in INCLUDES:
        command += ['-I', str(ROOT / directory)]
    command += list(extra)
    if command and command[0] in ('clang', 'clang++'):
        command = ['xcrun', '--sdk', 'iphoneos', command[0]] + command[1:]
    result = subprocess.run(command, cwd=record['directory'], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    (OUT / (obj.name + '.log')).write_text(result.stdout)
    if result.returncode:
        raise SystemExit('%s:\n%s' % (source, result.stdout[-3500:]))


def audit_object(obj):
    """Reject anything that is not an ARM64 / iOS 17 object."""
    data = obj.read_bytes()
    magic, cpu, subtype, kind = struct.unpack_from('<4I', data)
    if (magic, cpu, subtype, kind) != (0xfeedfacf, 0x100000c, 0, 1):
        raise SystemExit('%s: unexpected platform object header' % obj)
    offset, count = 32, struct.unpack_from('<I', data, 16)[0]
    deployment = None
    for _ in range(count):
        cmd, length = struct.unpack_from('<II', data, offset)
        if cmd == 0x32:
            deployment = struct.unpack_from('<II', data, offset + 8)
        offset += length
    if deployment != (2, 0x110000):
        raise SystemExit('%s: unexpected platform deployment %s' % (obj, deployment))


# Two driver translation units are left out on purpose.
#
#   NativeAdrenoAccelerator.cpp  nothing references it, and it installs an
#       IOAcceleratorES personality from a static initializer, which must not run
#       during boot (the SM8550 board stages it separately for the same reason).
#
#   NativeAdrenoPublish.cpp      nothing references it either (native_adreno_
#       service refuses to publish at this revision), and it needs three board
#       symbols this kernel does not have: native_gop_handoff_memory and
#       native_gop_handoff_call (the UEFI GOP -> scanout handoff provider) and
#       boot_log_export_put (declared __attribute__((weak_import)), which a
#       statically linked kernel still rejects). Its whole content is the
#       AGXDeviceUserClient / NativeAdrenoGpuPublished pair behind the Metal
#       publish path, so it comes back with that path, not before.
EXCLUDED = {'NativeAdrenoAccelerator.cpp', 'NativeAdrenoPublish.cpp'}


def sources():
    """Every translation unit that belongs in the archive."""
    items = []
    driver = ROOT / 'drivers/NativeAdreno'
    if not driver.is_dir():
        raise SystemExit('%s missing: the NativeAdreno submodule is not checked out' % driver)
    for path in sorted(driver.glob('NativeAdreno*.cpp')):
        if path.name in EXCLUDED:
            continue
        items.append((path, 'cxx'))
    for pattern, directory in (('AdrenoFirmware*.c', 'kits/AdrenoFirmware'),
                               ('AdrenoHfi*.c', 'kits/AdrenoFirmware'),
                               ('ArmSmmu4K*.c', 'kits/ArmSmmu4K'),
                               ('*.c', 'kits/AdrenoPM4')):
        for path in sorted((ROOT / directory).glob(pattern)):
            if path.name.startswith('test_'):
                continue
            items.append((path, 'c'))
    for name in ('ShengGmu.c', 'ShengGpuSmmu.c', 'ShengGmuCommandDb.c'):
        path = ROOT / 'kits/hummer' / name
        if not path.is_file():
            raise SystemExit('%s missing: package/ did not map to kits/hummer' % path)
        items.append((path, 'c'))
    # The board's GMU bring-up probe: C++ because it calls the driver's
    # NativeAdrenoGmuContextBringup / ShengGmuLoadAndStartHfi through their real
    # headers rather than restating their layouts.
    probe = ROOT / 'kits/hummer/HummerAdrenoBringup.cpp'
    if not probe.is_file():
        raise SystemExit('%s missing: package/ did not map to kits/hummer' % probe)
    items.append((probe, 'cxx'))
    # glob() can hand back the same file twice through the two AdrenoFirmware
    # patterns; keep the first occurrence only.
    seen, unique = set(), []
    for path, kind in items:
        if path in seen:
            continue
        seen.add(path)
        unique.append((path, kind))
    for directory in ('kits/AdrenoFirmware', 'kits/ArmSmmu4K', 'kits/AdrenoPM4'):
        prefix = str(ROOT / directory)
        if not any(str(path).startswith(prefix) for path, _ in unique):
            raise SystemExit('%s produced no sources: the submodule is not checked out'
                             % (ROOT / directory))
    return unique


def firmware_assembly():
    lines = ['.section __DATA_CONST,__const']
    for label, name, digest in FIRMWARE_PINS:
        path = FIRMWARE / name
        if not path.is_file():
            raise SystemExit('%s missing: run scripts/fetch-hummer-gpu-firmware.py' % path)
        blob = path.read_bytes()
        actual = hashlib.sha256(blob).hexdigest()
        if actual != digest:
            raise SystemExit('%s: sha256 %s does not match its pin' % (name, actual))
        lines += ['.p2align 12', '.globl ' + label, label + ':',
                  '.incbin ' + json.dumps(str(path)), label + '_end:',
                  '.p2align 3', '.globl ' + label + '_size', label + '_size:',
                  '.quad ' + label + '_end-' + label]
    source = OUT / 'firmware.S'
    source.write_text('\n'.join(lines) + '\n')
    obj = OUT / 'firmware.o'
    run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-Wno-incompatible-sysroot',
         '-c', source, '-o', obj])
    return obj


def build():
    if not RECORD_C.is_file() or not RECORD_CXX.is_file():
        raise SystemExit('compile records missing: run after the kernel object tree exists')
    objects = []
    for path, kind in sources():
        obj = OUT / (path.stem + ('.o' if kind == 'cxx' else '.c.o'))
        replay(RECORD_CXX if kind == 'cxx' else RECORD_C, path, obj)
        audit_object(obj)
        objects.append(obj)
    objects.append(firmware_assembly())

    archive = OUT / 'libNativeAdreno.a'
    archive.unlink(missing_ok=True)
    run(['xcrun', 'ar', 'rcs', archive] + objects)

    defined = run(['xcrun', 'nm', '-g', archive], text=True)
    (OUT / 'nm.txt').write_text(defined)
    absent = [name for name in REQUIRED_C
              if (' T _' + name) not in defined and (' S _' + name) not in defined
              and (' D _' + name) not in defined]
    if absent:
        raise SystemExit('required kit definitions absent: ' + ', '.join(absent))
    classes = [name for name in REQUIRED_CLASSES if name not in defined]
    if classes:
        raise SystemExit('required driver classes absent: ' + ', '.join(classes))
    print('hummer-adreno-kits: %d objects archived -> %s'
          % (len(objects), archive.relative_to(ROOT)))


if __name__ == '__main__':
    build()
