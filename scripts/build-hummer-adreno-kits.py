#!/usr/bin/env python3
"""Build build/native-adreno/libNativeAdreno.a for hummer (SM8750, Adreno 830).

The engine links exactly one archive for the GPU:

    build/native-adreno/libNativeAdreno.a      (build-xnu-4k-baseline.sh:592/738,
                                                only when XNU_NATIVE_ADRENO=1)

At this engine revision there is no component system and no
scripts/build-arm-driver.py, so the whole archive is assembled here:

  drivers/NativeAdreno/*.cpp        the driver backend, an explicit 19-file list
                                    (see DRIVER_SOURCES): CP init, GPU/GMU
                                    contexts, SMMU tables, VM, ring, submit,
                                    IRQ, preempt, user client, publish
  drivers/NativeAdreno/NativeAdrenoA830HwInit.cpp
                                    the A830 register bring-up the backend drives
  kits/AdrenoFirmware/*.c           HFI transport, startup protocol, ARC votes,
                                    firmware segment planner
  kits/ArmSmmu4K/*.c                native 4 KiB SMMU tables, streams, live audit
  kits/AdrenoPM4/*.c                PM4 packet encoder
  kits/hummer/ShengGpuSmmu*.c       this board's SM8750 GPU SMMU stream routing
  kits/hummer/Sm8750Gmu*.c          the SM8750 GMU layer: cold firmware start,
                                    HFI startup, runtime OOB/DCVS, command-db
  kits/hummer/Sm8750GpuPower.c      GPUCC power, GCC/GDSC, PLL, branch clocks
  kits/hummer/Sm8750GpuSmmu.c       GPU SID 0/1 and GMU SID 5 route planning
  kits/hummer/Sm8750GpuZap.c        secure CX init and PAS authentication
  kits/hummer/Sm8750SocInfo.c       SMEM v12 socinfo, for the OPP profile
  kits/hummer/HummerGpuVotes.c      SM8750 speed-bin OPP policy -> A8XX HFI table
  kits/hummer/HummerGpu.cpp         the board layer that drives all of the above
  firmware.S                        the pinned blobs as strong symbols

The driver's own board entry (NativeAdrenoService.cpp) is deliberately not
compiled: this board starts the GPU itself, and linking both would duplicate
symbols and give the machine two owners for one GPU. See DRIVER_SOURCES.

The firmware images come from scripts/fetch-hummer-gpu-firmware.py, which verifies
linux-firmware provenance by SHA-256; the blobs stay out of Git. They become the
strong definitions that replace NativeAdrenoFirmwareDefaults.cpp's empty weak ones.

Usage: build-hummer-adreno-kits.py   (run after the kernel object tree exists)
"""

import os
import hashlib
import json
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def device_repo_root():
    """Where this device repository lives while a build runs inside the tree.

    `unixnu` copies package/ and scripts/ into the prepared tree and runs these
    recipes from there, so Path(__file__).parents[1] is the *tree*, not the
    repository. The firmware and its pins ship with the repository, so resolve
    that directory through UNIXNU_DEVICE (exported by ./unixnu) and fall back to
    the tree for a standalone run.
    """
    explicit = os.environ.get('UNIXNU_DEVICE')
    if explicit and (Path(explicit) / 'firmware/hummer-gpu').is_dir():
        return Path(explicit)
    for candidate in (ROOT.parent, ROOT):
        if (candidate / 'firmware/hummer-gpu').is_dir():
            return candidate
    return ROOT

OUT = ROOT / 'build/native-adreno'
OUT.mkdir(parents=True, exist_ok=True)

# C compiles replay the bsd build's record; C++ replays IOKit's, the same pair
# sheng's builder uses. Both directories exist once the kernel objects are built.
RECORD_C = (ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
                 / 'bsd/DEVELOPMENT/bsd_init.o.json')
RECORD_CXX = (ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
                   / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')

# The four images ship with the repository: 240 KiB dumped from this device's
# own vendor partition (see scripts/fetch-hummer-gpu-firmware.py for the pins
# and the OTA they came from). CI has no phone to pull them from, and a missing
# image stops the archive step before anything downstream can run.
FIRMWARE = device_repo_root() / 'firmware/hummer-gpu'

# The engine driver's strong overrides for its own weak defaults: it asks for
# these two names and gets their lengths from the companion _size symbols, so
# the microcode version can change without touching the driver.
FIRMWARE_PINS = [
    ('_native_adreno_firmware_image', 'gen80000_gmu.bin',
     '1ad8175e6cd01ea76c64d20b83600038c52dd29844b48d57b11b769b038863a5'),
    ('_native_adreno_sqe_firmware_image', 'gen80000_sqe.fw',
     '9c8f009bea112d7c06dd2ccb67e889999459e53b92f0f4859f5ea0db067ad6b5'),
]

# Every blob this board boots, pinned to the device dump that
# scripts/fetch-hummer-gpu-firmware.py records: one OTA's SQE/AQE pair is not
# interchangeable with another's, and the CP only says so by never leaving
# reset. All four are required -- a missing ZAP would otherwise turn into a
# board layer that boots the GMU and then quietly skips authentication.
FIRMWARE_CHECK = [
    ('gen80000_gmu.bin', 88896,
     '1ad8175e6cd01ea76c64d20b83600038c52dd29844b48d57b11b769b038863a5'),
    ('gen80000_sqe.fw', 111892,
     '9c8f009bea112d7c06dd2ccb67e889999459e53b92f0f4859f5ea0db067ad6b5'),
    ('gen80000_aqe.fw', 25340,
     'f04cb97e1e629b9003a26c5670b8f7e5b7bfeca95d35b2abd581fd8672efed20'),
    ('gen80000_zap.mbn', 12088,
     'e294fba1f90045b321db3cc74a4f0012ca82b5c5b2b6051b845bde9738ac1747'),
]

INCLUDES = [
    'kits/ArmSmmu4K',
    'kits/AdrenoPM4',
    'kits/AdrenoFirmware',
    'kits/hummer',
    # package/HummerGpu.cpp includes Sm8750GpuFirmware.h, which this script
    # generates next to the assembly blob it describes.
    'build/native-adreno',
    # package/HummerGpu.cpp lives in kits/hummer but includes the driver's own
    # headers (NativeAdrenoGpuContext.h and friends), which sit in the driver
    # directory.
    'drivers/NativeAdreno',
    '.cache/xnu-work/osfmk',
]

# C symbols the archive must define, checked with nm before it is accepted.
# C++ classes are checked by substring because nm prints mangled names.
REQUIRED_C = [
    # The board layer drives the SM8750 GPU stack adopted in v185.
    'Sm8750GmuColdStart',
    'Sm8750GmuLoadAndStartHfi',
    'Sm8750GmuGpuOob',
    'Sm8750GmuSetPerformance',
    'Sm8750GmuBindMmio',
    'Sm8750GmuCommandDbInputs',
    'Sm8750GpuPowerOn',
    'Sm8750GpuRailCommands',
    'Sm8750GpuSmmuPrepare',
    'Sm8750GpuZapLoad',
    'Sm8750GpuSecureCxInit',
    'Sm8750ScmCall',
    'Sm8750SocInfoGpuCode',
    'HummerGpuBuildStartup',
    'AdrenoHfiGen8Votes',
    'AdrenoHfiArcVotes',
    # The engine's own driver still references this board layer by name
    # (NativeAdrenoService.cpp). It is not on the board layer's path.
    'ShengGmuColdStart',
    'ShengGmuLoadAndStartHfi',
    'ShengGmuBindMmio',
    'ShengGmuCommandDbInputs',
    'ShengGpuSmmuStart',
    'ShengGpuSmmuObserve',
    'AdrenoFirmwarePlanGmu',
    'AdrenoHfiStartupRun',
    'ArmSmmu4KAttachStreams',
    'AdrenoPM4Type7',
]
REQUIRED_CLASSES = [
    'NativeAdrenoGpuContext',
    'NativeAdrenoGmuContext',
    'NativeAdrenoRing',
    'NativeAdrenoA830HwInit',
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


DRIVER_SOURCES = ('A830HwInit', 'CpInit', 'Cpu', 'GmuContext', 'GmuMemory', 'GpuContext',
                  'Irq', 'Memory', 'Publish', 'Pwrup', 'Preempt', 'Ring', 'Sha256',
                  'SmmuContext', 'SmmuMemory', 'Submit', 'Timer', 'UserClient', 'Vm')


def sources():
    """Every translation unit that belongs in the archive."""
    items = []
    driver = ROOT / 'drivers/NativeAdreno'
    if not driver.is_dir():
        raise SystemExit('%s missing: the NativeAdreno submodule is not checked out' % driver)
    for name in DRIVER_SOURCES:
        path = driver / ('NativeAdreno' + name + '.cpp')
        if not path.is_file():
            raise SystemExit('%s missing: the driver revision in upstream.rev predates the '
                             'A830 backend (see patches/adreno-a830.patch)' % path)
        items.append((path, 'cxx'))
    for pattern, directory in (('AdrenoFirmware*.c', 'kits/AdrenoFirmware'),
                               ('AdrenoHfi*.c', 'kits/AdrenoFirmware'),
                               ('ArmSmmu4K*.c', 'kits/ArmSmmu4K'),
                               ('*.c', 'kits/AdrenoPM4')):
        for path in sorted((ROOT / directory).glob(pattern)):
            if path.name.startswith('test_'):
                continue
            items.append((path, 'c'))
    for name in ('ShengGmu.c', 'ShengGpuSmmu.c', 'ShengGmuCommandDb.c',
                 'Sm8750Gmu.c', 'Sm8750GmuCommandDb.c', 'HummerGpuVotes.c',
                 'Sm8750GpuPower.c', 'Sm8750GpuSmmu.c', 'Sm8750GpuZap.c',
                 'Sm8750SocInfo.c'):
        path = ROOT / 'kits/hummer' / name
        if not path.is_file():
            raise SystemExit('%s missing: package/ did not map to kits/hummer' % path)
        items.append((path, 'c'))
    # The board layer: GPUCC power, SMMU planning, GMU cold start, CP, ZAP and
    # the Metal service. C++ because it drives the driver's real classes through
    # their headers instead of restating their layouts.
    board = ROOT / 'kits/hummer/HummerGpu.cpp'
    if not board.is_file():
        raise SystemExit('%s missing: package/ did not map to kits/hummer' % board)
    items.append((board, 'cxx'))
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


def zap_parts(data):
    """Split a signed Qualcomm ZAP ELF into the two pieces PAS expects.

    Ported from the OnePlus 13T port's prepare-gpu-firmware.py. The secure world
    is handed the ELF header plus its single hash segment (the 0xfcc-byte
    metadata blob) and, separately, the relocatable load segments packed into a
    page-aligned 0x1000-byte payload. Anything that does not match that shape is
    refused here rather than on the device, where a bad ZAP image can only show
    up as an SCM failure.
    """
    if len(data) < 52 or data[:7] != b'\x7fELF\x01\x01\x01' or struct.unpack_from('<HHI', data, 16) != (2, 164, 1):
        raise SystemExit('gen80000_zap.mbn: expected a little-endian Qualcomm ELF32 image')
    phoff = struct.unpack_from('<I', data, 28)[0]
    size, count = struct.unpack_from('<HH', data, 42)
    if size != 32 or not 2 <= count <= 16 or phoff < 52 or phoff + count * size > len(data):
        raise SystemExit('gen80000_zap.mbn: invalid program header table')
    headers = [struct.unpack_from('<8I', data, phoff + i * size) for i in range(count)]
    hashes = [p for p in headers if (p[6] >> 24) & 7 == 2]
    loads = [p for p in headers if p[0] == 1 and (p[6] >> 24) & 7 != 2 and p[5]]
    if headers[0][0] == 1 or len(hashes) != 1 or not loads:
        raise SystemExit('gen80000_zap.mbn: missing metadata, hash or load segment')
    head_bytes, hash_header = headers[0][4], hashes[0]
    if head_bytes < phoff + count * size or head_bytes > len(data) or not hash_header[4] or \
            hash_header[1] < head_bytes or hash_header[1] + hash_header[4] > len(data) or \
            head_bytes + hash_header[4] > 0x4000:
        raise SystemExit('gen80000_zap.mbn: invalid metadata extents')
    low = min(p[3] for p in loads)
    high = max((p[3] + p[5] + 0xfff) & ~0xfff for p in loads)
    if low & 0xfff or high - low > 0x2000:
        raise SystemExit('gen80000_zap.mbn: loads do not fit the 0x9b09a000 reservation')
    payload = bytearray(high - low)
    spans = []
    for p in loads:
        start, length = p[3] - low, p[5]
        if not p[6] & (1 << 27) or p[4] > length or p[1] + p[4] > len(data) or \
                p[3] + length > 0x100000000 or any(start < b and a < start + length for a, b in spans):
            raise SystemExit('gen80000_zap.mbn: invalid or overlapping load segment')
        spans.append((start, start + length))
        payload[start:start + p[4]] = data[p[1]:p[1] + p[4]]
    return data[:head_bytes] + data[hash_header[1]:hash_header[1] + hash_header[4]], bytes(payload)


def write_firmware_header(images):
    """The C++ board layer includes this instead of declaring the symbols."""
    lines = ['/* Generated by build-hummer-adreno-kits.py. */', '#pragma once',
             '#include <stdint.h>', '#ifdef __cplusplus', 'extern "C" {', '#endif']
    for label, _, _ in images:
        lines += ['extern const uint8_t %s[];' % label,
                  'extern const uint64_t %s_size;' % label]
    lines += ['#ifdef __cplusplus', '}', '#endif']
    (OUT / 'Sm8750GpuFirmware.h').write_text('\n'.join(lines) + '\n')


def firmware_assembly():
    """Asm unit carrying every GPU blob, plus the header the board layer needs.

    Two naming schemes live here on purpose. The engine's NativeAdreno driver
    declares `native_adreno_firmware_image` / `native_adreno_sqe_firmware_image`
    as strong symbols (its own defaults are weak), while the SM8750 board layer
    from the OnePlus 13T port asks for `sm8750_gpu_gmu/sqe/aqe` and the two ZAP
    halves. Both are emitted from the same pinned bytes rather than aliased, so a
    toolchain that dislikes symbol aliases cannot quietly drop one of them.

    AQE and ZAP come from the same device dump as the rest: the kernel needs
    them to upload the CP's AQE microcode and to authenticate ZAP, and neither
    image exists in linux-firmware at all. All four are pinned and required --
    see FIRMWARE_CHECK -- so a build either carries the images this board boots
    or stops with the name of the one that is wrong.
    """
    lines = ['.section __DATA_CONST,__const']
    images = []

    def emit(label, blob):
        path = OUT / ('blob-' + label + '.bin')
        path.write_bytes(blob)
        lines.extend(['.p2align 12', '.globl _' + label, '_' + label + ':',
                      '.incbin ' + json.dumps(str(path)), '_' + label + '_end:',
                      '.p2align 3', '.globl _' + label + '_size', '_' + label + '_size:',
                      '.quad _' + label + '_end-_' + label])
        images.append((label, path, len(blob)))

    # Verify all four before emitting any, so a wrong blob names itself instead
    # of producing an archive that boots half the stack and then stalls.
    blobs = {}
    for name, size, digest in FIRMWARE_CHECK:
        path = FIRMWARE / name
        if not path.is_file():
            raise SystemExit('%s missing: run scripts/fetch-hummer-gpu-firmware.py '
                             'with the phone attached (--device)' % path)
        blob = path.read_bytes()
        actual = hashlib.sha256(blob).hexdigest()
        if len(blob) != size or actual != digest:
            raise SystemExit('%s: %d bytes / sha256 %s, pin is %d / %s'
                             % (name, len(blob), actual, size, digest))
        blobs[name] = blob

    for label, name, _ in FIRMWARE_PINS:
        emit(label, blobs[name])

    # The SM8750 board layer's own names for the same images, plus the ZAP pair
    # cut out of the signed ELF (metadata is what PAS hashes, payload is what it
    # authenticates; Sm8750GpuZapLoad refuses any other sizes).
    emit('sm8750_gpu_gmu', blobs['gen80000_gmu.bin'])
    emit('sm8750_gpu_sqe', blobs['gen80000_sqe.fw'])
    emit('sm8750_gpu_aqe', blobs['gen80000_aqe.fw'])
    metadata, payload = zap_parts(blobs['gen80000_zap.mbn'])
    emit('sm8750_gpu_zap_metadata', metadata)
    emit('sm8750_gpu_zap_payload', payload)

    write_firmware_header(images)
    source = OUT / 'firmware.S'
    source.write_text('\n'.join(lines) + '\n')
    obj = OUT / 'firmware.o'
    run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-Wno-incompatible-sysroot',
         '-c', source, '-o', obj])
    return obj


def build():
    if not RECORD_C.is_file() or not RECORD_CXX.is_file():
        raise SystemExit('compile records missing: run after the kernel object tree exists')
    # First, because it writes Sm8750GpuFirmware.h, which the C++ board layer
    # includes; compiling that unit before the header exists cannot work.
    firmware = firmware_assembly()
    objects = []
    for path, kind in sources():
        obj = OUT / (path.stem + ('.o' if kind == 'cxx' else '.c.o'))
        replay(RECORD_CXX if kind == 'cxx' else RECORD_C, path, obj)
        audit_object(obj)
        objects.append(obj)
    objects.append(firmware)

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
