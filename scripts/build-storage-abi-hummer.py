#!/usr/bin/env python3
"""Stage the IOStorageFamily headers NativeUfs compiles against.

Every storage driver (NativeUfs included) needs the public IOKit storage
headers, which XNU 10063 no longer ships - the tree has no iokit/storage at
all - so scripts/audit-storage-interface.py stages them from a pinned
apple-oss-distributions/IOStorageFamily checkout into
build/storage-abi/include/IOKit/storage before compiling anything.

We deliberately stop after that staging:

- nothing upstream clones the reference repo, and the board tree exists only
  while `unixnu build kernel` runs, so it has to happen inside an archive
  builder (the first one that needs it)
- the remainder of that script is sheng's *qualification audit*, which needs
  a prelink candidate plus a live QEMU+GDB storage observation that this
  board does not produce.  Building omitted pieces of someone else's
  evidence chain would only fake it, so they are skipped rather than
  satisfied.

The board carries the same IPSW as sheng does (iPad7,1 21H461, kernelcache
f18b4df8...), so the provenance the audit checks is not in question here.

Usage: build-storage-abi-hummer.py
"""
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT / 'refer' / 'IOStorageFamily'
OUT = ROOT / 'build' / 'storage-abi'
REVISION = 'e45fbe30a7879e0b16e26dddb78538f1b00e0bff'
URL = 'https://github.com/apple-oss-distributions/IOStorageFamily.git'
REQUIRED = ('IOBlockStorageDevice.h', 'IOStorage.h', 'IOMedia.h')


def git(*args):
    return subprocess.run(['git'] + list(args), check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.strip()


def stage_reference():
    if REFERENCE.is_dir() and (REFERENCE / '.git').is_dir():
        git('-C', str(REFERENCE), 'fetch', '--depth', '1', 'origin', REVISION)
    else:
        REFERENCE.parent.mkdir(parents=True, exist_ok=True)
        git('clone', URL, str(REFERENCE))
        git('-C', str(REFERENCE), 'fetch', '--depth', '1', 'origin', REVISION)
    git('-C', str(REFERENCE), 'checkout', REVISION)
    head = git('-C', str(REFERENCE), 'rev-parse', 'HEAD')
    if head != REVISION:
        raise SystemExit(f'IOStorageFamily is at {head}, expected {REVISION}')
    print(f'IOStorageFamily {REVISION[:12]} staged at {REFERENCE}')


def main():
    stage_reference()

    headers = OUT / 'include' / 'IOKit' / 'storage'
    headers.mkdir(parents=True, exist_ok=True)
    staged = {}
    for path in sorted(REFERENCE.glob('*.h')):
        shutil.copyfile(path, headers / path.name)
        staged[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()

    # Only this platform-selection header is missing from the source-kernel SDK.
    sdk = Path(subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'],
                                       text=True).strip())
    conditions = sdk / 'usr/include/TargetConditionals.h'
    if not conditions.is_file():
        raise SystemExit(f'macOS SDK is missing TargetConditionals.h: {conditions}')
    copied = OUT / 'include' / 'TargetConditionals.h'
    shutil.copyfile(conditions, copied)
    staged['TargetConditionals.h'] = hashlib.sha256(conditions.read_bytes()).hexdigest()

    missing = [name for name in REQUIRED if name not in staged]
    if missing:
        raise SystemExit('reference checkout lacks ' + ', '.join(missing))
    (OUT / 'storage-abi-staging.json').write_text(
        json.dumps(dict(reference_revision=REVISION, headers=len(staged),
                        sha256=staged), indent=2) + '\n')
    version = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-version'],
                                      text=True).strip()
    print(f'build/storage-abi ready: {len(staged)} headers under {headers} (macOS SDK {version})')


if __name__ == '__main__':
    main()
