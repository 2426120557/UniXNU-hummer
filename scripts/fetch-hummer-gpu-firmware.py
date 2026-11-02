#!/usr/bin/env python3
"""Fetch the pinned Adreno 830 firmware blobs into .cache/firmware/hummer-gpu.

The vendor blobs never enter Git (the same policy the SM8550 board follows), so
every build that needs them downloads them again unless the cache survived. Each
file is pinned by linux-firmware commit, byte count and SHA-256; anything that
does not match is an error rather than a warning, because a truncated or wrong
GMU image would only show up as a silent "firmware refused" on the device.

Provenance: linux-firmware commit 5c57d4817ebded2730ef20b3f2dd0f844b16dd0e.
qcom/gen80000_gmu.bin is the GMU firmware the A830 boots (gen8_0_0),
qcom/gen80000_sqe.fw is the SQE microcode the CP uploads, and
qcom/gen80000_aqe.fw is the AQE microcode the CP uploads after ZAP passes.

gen80000_zap.mbn is NOT in linux-firmware: it only ships in the vendor
partition. Pass --vendor-dir with a directory that contains it (the board layer
needs it for PAS, and skips ZAP entirely without it):

    fetch-hummer-gpu-firmware.py --vendor-dir /path/to/vendor/firmware

On a device with root, that directory is /vendor/firmware; from a super.img
backup, extract the vendor partition first (lpunpack vendor.img) and point at
vendor/firmware. The file is accepted as-is and recorded by SHA-256 in
manifest.json, because there is no upstream pin for a vendor image.

Usage: fetch-hummer-gpu-firmware.py [--vendor-dir DIR]
"""

import argparse
import base64
import hashlib
import json
import shutil
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / '.cache/firmware/hummer-gpu'
COMMIT = '5c57d4817ebded2730ef20b3f2dd0f844b16dd0e'
BASE = ('https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/'
        'linux-firmware.git/+/' + COMMIT + '/')

# (path inside linux-firmware, bytes, sha256)
PINS = [
    ('qcom/gen80000_gmu.bin', 88896,
     '1ad8175e6cd01ea76c64d20b83600038c52dd29844b48d57b11b769b038863a5'),
    ('qcom/gen80000_sqe.fw', 111476,
     '30ee3301534f95799d4afaf73864f3c369baa5a5dea8481e7f0cb8cc941f396c'),
    ('qcom/gen80000_aqe.fw', 25044,
     '67562ad5baae3a133c92a6e48a9d1b1a9a867fe2f85b49324e244e963f3b89c5'),
]

# Only in the vendor partition; no upstream pin, so it is recorded, not checked.
VENDOR = ('qcom/gen80000_zap.mbn', 'gen80000_zap.mbn')


def correct(path, size, digest):
    if not path.is_file():
        return False
    data = path.read_bytes()
    return len(data) == size and hashlib.sha256(data).hexdigest() == digest


def fetch(name, url, timeout=120):
    """googlesource serves the blob base64 encoded when format=TEXT is asked for."""
    request = urllib.request.Request(url + '?format=TEXT',
                                     headers={'User-Agent': 'unixnu-hummer-build'})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        raw = response.read()
    try:
        return base64.b64decode(raw, validate=True)
    except Exception:
        # A plain mirror may answer with the bytes themselves.
        return raw


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vendor-dir', type=Path,
                        help='directory holding gen80000_zap.mbn (vendor/firmware)')
    arguments = parser.parse_args()
    CACHE.mkdir(parents=True, exist_ok=True)
    manifest = []
    for name, size, digest in PINS:
        target = CACHE / name
        if correct(target, size, digest):
            state = 'cached'
        else:
            blob = fetch(name, BASE + name)
            if len(blob) != size:
                raise SystemExit('%s: %d bytes, expected %d' % (name, len(blob), size))
            actual = hashlib.sha256(blob).hexdigest()
            if actual != digest:
                raise SystemExit('%s: sha256 %s, expected %s' % (name, actual, digest))
            target.parent.mkdir(parents=True, exist_ok=True)
            # Write through a temporary file so a killed build cannot leave a
            # half image that looks cached next time.
            temporary = target.with_suffix(target.suffix + '.part')
            temporary.write_bytes(blob)
            temporary.replace(target)
            state = 'fetched'
        manifest.append({'file': name, 'bytes': size, 'sha256': digest,
                         'linux_firmware_commit': COMMIT, 'state': state})
        print('%-28s %8d bytes  %s' % (name, size, state))

    staged, expected = VENDOR
    target = CACHE / staged
    source = arguments.vendor_dir / expected if arguments.vendor_dir else None
    if source and source.is_file():
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        data = target.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        manifest.append({'file': staged, 'bytes': len(data), 'sha256': digest,
                         'source': str(source), 'state': 'staged'})
        print('%-28s %8d bytes  staged sha256 %s' % (staged, len(data), digest))
    elif target.is_file():
        # Already staged by an earlier run; keep it and report its hash so the
        # build log says which ZAP image this kernel authenticated against.
        data = target.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        manifest.append({'file': staged, 'bytes': len(data), 'sha256': digest,
                         'state': 'cached'})
        print('%-28s %8d bytes  cached sha256 %s' % (staged, len(data), digest))
    else:
        print('%-28s not staged -- ZAP authentication will be skipped' % staged)
        print('  pass --vendor-dir <vendor/firmware> to add it')

    (CACHE / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('hummer-gpu firmware ready in', CACHE.relative_to(ROOT))


if __name__ == '__main__':
    main()
