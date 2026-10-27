#!/usr/bin/env python3
"""Fetch the pinned Adreno 830 firmware blobs into .cache/firmware/hummer-gpu.

The vendor blobs never enter Git (the same policy the SM8550 board follows), so
every build that needs them downloads them again unless the cache survived. Each
file is pinned by linux-firmware commit, byte count and SHA-256; anything that
does not match is an error rather than a warning, because a truncated or wrong
GMU image would only show up as a silent "firmware refused" on the device.

Provenance: linux-firmware commit 5c57d4817ebded2730ef20b3f2dd0f844b16dd0e.
qcom/gen80000_gmu.bin is the GMU firmware the A830 boots (gen8_0_0), and
qcom/gen80000_sqe.fw is the SQE microcode the CP uploads. The device tree
(qcom,kgsl-3d0@3d00000, nvmem-cell-names = "speed_bin") shows this part also
ships gen80000_aqe.fw; the driver in this tree does not upload an AQE image yet,
so it is not fetched.

Usage: fetch-hummer-gpu-firmware.py
"""

import base64
import hashlib
import json
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
]


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
    (CACHE / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('hummer-gpu firmware ready in', CACHE.relative_to(ROOT))


if __name__ == '__main__':
    main()
