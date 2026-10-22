#!/usr/bin/env python3
"""Prepare build/storage-abi and qualify the original block-storage ABI.

NativeUfs (and the other storage drivers) compile against the IOStorageFamily
headers that scripts/audit-storage-interface.py stages into
build/storage-abi/include.  That script requires refer/IOStorageFamily at a
pinned revision, and nothing in the upstream flow clones it: sheng stages that
reference once, by hand, outside the build tree.

The board tree itself only exists while `unixnu build kernel` runs (it is
created after the checkout step, not by `unixnu defconfig`), so the reference
has to be arranged from inside an archive builder - the first one that needs it.

Usage: build-storage-abi-hummer.py
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT / 'refer' / 'IOStorageFamily'
AUDIT = ROOT / 'scripts' / 'audit-storage-interface.py'
REVISION = 'e45fbe30a7879e0b16e26dddb78538f1b00e0bff'
URL = 'https://github.com/apple-oss-distributions/IOStorageFamily.git'


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
    result = subprocess.run([sys.executable, str(AUDIT)], cwd=str(ROOT))
    if result.returncode:
        raise SystemExit(f'{AUDIT.name} failed with {result.returncode}')
    headers = ROOT / 'build' / 'storage-abi' / 'include' / 'IOKit' / 'storage'
    if not (headers / 'IOBlockStorageDevice.h').is_file():
        raise SystemExit(f'storage ABI headers missing after the audit run: {headers}')
    print(f'build/storage-abi ready: {len(list(headers.glob("*.h")))} storage headers')


if __name__ == '__main__':
    main()
