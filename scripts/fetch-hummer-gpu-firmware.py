#!/usr/bin/env python3
"""Check (and if needed refresh) the four Adreno 830 firmware images this board boots.

The images live in firmware/hummer-gpu/ and ship with the repository: 240 KiB
dumped from this device's own vendor partition (OnePlus Ace 5 Pro, PKR110 /
hummer, SM8750 + Adreno 830). They are pinned below byte for byte, together with
the OTA that shipped them:

    OnePlus/PKR110/OP60EBL1:16/AP3A.240617.008/V.51a52f2-2d7999f-2d9581d

They are in the tree rather than fetched on demand because CI has no phone, and
because the wrong microcode does not fail loudly -- it shows up much later as a
CP that never leaves reset. Two of the four differ from linux-firmware (commit
5c57d481) on purpose, and it matters which one wins:

    gen80000_gmu.bin   88896  identical to linux-firmware
    gen80000_sqe.fw   111892  linux-firmware ships 111476: 0x77 PM4 ops
                              against this OTA's 0x81 (416 bytes more)
    gen80000_aqe.fw    25340  linux-firmware ships 25044
    gen80000_zap.mbn   12088  absent from linux-firmware entirely

The CP validates the SQE/AQE microcode revision against the hardware it was
signed for, so linux-firmware's copy would be accepted at build time and then
stall the CP on the device.

In the normal case this script only verifies the four files and exits, which is
what the build calls it for. If one is missing or does not match its pin -- a
fresh OTA, a checked-out tree that lost them -- it refreshes from, in order:

    --vendor-dir DIR    a vendor/firmware tree already on this machine
    --device [SERIAL]   adb from a rooted device (the default when neither
                        --vendor-dir nor --no-device is given)

With adb the pull is `adb exec-out cat <path>`, not `adb pull`: exec-out hands
back the exact bytes on stdout with no path translation, which matters on a
Windows host where a MSYS shell rewrites /vendor/... into C:/vendor/....

Usage: fetch-hummer-gpu-firmware.py [--device [SERIAL]] [--vendor-dir DIR]
                                    [--no-device] [--check]
"""

import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / 'firmware/hummer-gpu'
DEVICE_DIR = '/vendor/firmware'

# Fingerprint of the OTA these pins came from; recorded in the manifest, never
# enforced, because a newer OTA may well ship a newer (compatible) microcode --
# but then the pins have to be re-dumped and this file updated on purpose.
PROVENANCE = {
    'model': 'PKR110',
    'device': 'OP60EBL1',
    'fingerprint': 'OnePlus/PKR110/OP60EBL1:16/AP3A.240617.008/'
                   'V.51a52f2-2d7999f-2d9581d:user/release-keys',
    'path': DEVICE_DIR,
    'adb': 'root over KernelSU (uid=0)',
}

# (file name in firmware/hummer-gpu and in the vendor directory, bytes, sha256)
PINS = [
    ('gen80000_gmu.bin', 88896,
     '1ad8175e6cd01ea76c64d20b83600038c52dd29844b48d57b11b769b038863a5'),
    ('gen80000_sqe.fw', 111892,
     '9c8f009bea112d7c06dd2ccb67e889999459e53b92f0f4859f5ea0db067ad6b5'),
    ('gen80000_aqe.fw', 25340,
     'f04cb97e1e629b9003a26c5670b8f7e5b7bfeca95d35b2abd581fd8672efed20'),
    ('gen80000_zap.mbn', 12088,
     'e294fba1f90045b321db3cc74a4f0012ca82b5c5b2b6051b845bde9738ac1747'),
]

# What linux-firmware 5c57d481 has for the same names, for the log line that
# says which pins are device-specific. Not a fallback: see the module comment.
LINUX_FIRMWARE = {
    'gen80000_gmu.bin': 88896,
    'gen80000_sqe.fw': 111476,
    'gen80000_aqe.fw': 25044,
}


def correct(path, size, digest):
    if not path.is_file():
        return False
    data = path.read_bytes()
    return len(data) == size and hashlib.sha256(data).hexdigest() == digest


def adb(serial, *arguments, binary=False):
    command = ['adb'] + (['-s', serial] if serial else []) + list(arguments)
    result = subprocess.run(command, capture_output=True)
    if result.returncode:
        raise RuntimeError('%s: %s' % (' '.join(command),
                                       result.stderr.decode('utf-8', 'replace').strip()))
    return result.stdout if binary else result.stdout.decode('utf-8', 'replace')


def device_serial(requested):
    """The one attached device, or the requested serial if it is really there."""
    attached = []
    for line in adb(None, 'devices').splitlines()[1:]:
        fields = line.split()
        if len(fields) >= 2 and fields[1] == 'device':
            attached.append(fields[0])
    if requested:
        if requested not in attached:
            raise RuntimeError('device %s is not attached (have: %s)'
                               % (requested, ', '.join(attached) or 'none'))
        return requested
    if len(attached) != 1:
        raise RuntimeError('%d devices attached, pass --device SERIAL (have: %s)'
                           % (len(attached), ', '.join(attached) or 'none'))
    return attached[0]


def pull(serial, name, size, digest):
    """Read the blob over exec-out; the device's own sha256 is checked first so a
    truncated transfer is caught on the phone rather than here."""
    path = DEVICE_DIR + '/' + name
    remote = adb(serial, 'shell', 'sha256sum', path).split()
    if not remote or remote[0] != digest:
        raise RuntimeError('%s on device hashes %s, pin is %s'
                           % (path, remote[0] if remote else '<unreadable>', digest))
    blob = adb(serial, 'exec-out', 'cat', path, binary=True)
    if len(blob) != size or hashlib.sha256(blob).hexdigest() != digest:
        raise RuntimeError('%s: %d bytes / sha256 %s over adb, expected %d / %s'
                           % (name, len(blob), hashlib.sha256(blob).hexdigest(),
                              size, digest))
    return blob


def stage(target, blob):
    """Write through a temporary file so a killed run cannot leave a half image
    that looks correct next time."""
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_suffix(target.suffix + '.part')
    temporary.write_bytes(blob)
    temporary.replace(target)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--device', nargs='?', const='', metavar='SERIAL',
                        help='refresh from a rooted device over adb (SERIAL is '
                             'optional when exactly one is attached)')
    parser.add_argument('--no-device', action='store_true',
                        help='never touch adb, refresh from --vendor-dir only')
    parser.add_argument('--vendor-dir', type=Path,
                        help='a vendor/firmware tree (or a super.img unpack) to copy from')
    parser.add_argument('--check', action='store_true',
                        help='verify only; fail if anything is missing instead of refreshing')
    arguments = parser.parse_args()

    if arguments.vendor_dir and not arguments.vendor_dir.is_dir():
        raise SystemExit('%s is not a directory' % arguments.vendor_dir)
    if arguments.vendor_dir and arguments.device is not None:
        raise SystemExit('pass either --vendor-dir or --device, not both')

    # Everything present and pinned: the normal build path, nothing else happens.
    missing = [name for name, size, digest in PINS
               if not correct(CACHE / name, size, digest)]
    if not missing:
        for name, size, _ in PINS:
            upstream = LINUX_FIRMWARE.get(name)
            note = '' if upstream is None else (
                ' (linux-firmware has %d: %s)' % (upstream, 'same' if upstream == size
                                                  else 'device-specific'))
            print('%-20s %8d bytes  ok%s' % (name, size, note))
        print('hummer-gpu firmware pinned and present in %s' % CACHE.relative_to(ROOT))
        return
    if arguments.check:
        raise SystemExit('missing or stale: ' + ', '.join(missing))

    print('refreshing: ' + ', '.join(missing))
    serial = None
    if arguments.vendor_dir is None and not arguments.no_device:
        try:
            serial = device_serial(arguments.device or None)
            print('device %s: %s' % (serial, adb(serial, 'shell', 'getprop',
                                                 'ro.build.fingerprint').strip()))
        except (RuntimeError, FileNotFoundError, OSError) as error:
            print('adb unavailable (%s)' % error)
            serial = None

    manifest = []
    for name, size, digest in PINS:
        target = CACHE / name
        if correct(target, size, digest):
            state, source = 'present', None
        elif arguments.vendor_dir is not None:
            origin = arguments.vendor_dir / name
            if not origin.is_file():
                raise SystemExit('%s missing from %s' % (name, arguments.vendor_dir))
            blob = origin.read_bytes()
            if len(blob) != size or hashlib.sha256(blob).hexdigest() != digest:
                raise SystemExit('%s: %d bytes / sha256 %s, pin is %d / %s'
                                 % (name, len(blob), hashlib.sha256(blob).hexdigest(),
                                    size, digest))
            stage(target, blob)
            state, source = 'copied', str(origin)
        elif serial:
            stage(target, pull(serial, name, size, digest))
            state, source = 'pulled', 'adb:%s:%s' % (serial, DEVICE_DIR)
        else:
            raise SystemExit('%s is missing and there is no device or --vendor-dir.\n'
                             'Re-run with the phone attached:\n'
                             '  %s --device' % (name, os.path.basename(__file__)))

        upstream = LINUX_FIRMWARE.get(name)
        note = '' if upstream is None else (
            ' (linux-firmware has %d: %s)' % (upstream, 'same' if upstream == size
                                              else 'device-specific, using this one'))
        manifest.append({'file': name, 'bytes': size, 'sha256': digest,
                         'source': source or 'repository', 'state': state,
                         'provenance': PROVENANCE})
        print('%-20s %8d bytes  %-7s %s%s'
              % (name, size, state, digest[:16] + '...', note))

    (CACHE / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('hummer-gpu firmware ready in %s' % CACHE.relative_to(ROOT))
    print('next: build-hummer-adreno-kits.py turns zap.mbn into the 0xfcc metadata '
          '+ 0x1000 payload pair ZAP authentication needs')


if __name__ == '__main__':
    main()
