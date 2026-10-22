#!/usr/bin/env python3
"""Prepare build/storage-abi and qualify the original block-storage ABI.

NativeUfs (and the other storage drivers) compile against the IOStorageFamily
headers that scripts/audit-storage-interface.py stages into
build/storage-abi/include.  That script requires refer/IOStorageFamily at a
pinned revision, and nothing in the upstream flow clones it: sheng stages that
reference once, by hand, outside the build tree.

The board tree itself only exists while `unixnu build kernel` runs (it is
created by the build step, not by `unixnu defconfig`), so the reference has to
be arranged from inside an archive builder - the first one that needs it.

The audit script also hard-codes warning suppressions that every Xcode release
does not carry, for example -Wno-unnecessary-virtual-specifier, and pairs them
with -Werror, so an unrecognised option is fatal.  Each suppression is probed
against the host compiler first and only the unknown ones are dropped; valid
ones stay, because removing them could promote a warning to an error.

Usage: build-storage-abi-hummer.py
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT / 'refer' / 'IOStorageFamily'
AUDIT = ROOT / 'scripts' / 'audit-storage-interface.py'
REVISION = 'e45fbe30a7879e0b16e26dddb78538f1b00e0bff'
URL = 'https://github.com/apple-oss-distributions/IOStorageFamily.git'

PROBE = 'struct A { virtual ~A(){} };\nint main() { return 0; }\n'


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


def compiler_knows(compiler, option):
    """True when the host compiler accepts option without complaint."""
    if not compiler:
        return True
    result = subprocess.run([compiler, '-x', 'c++', '-std=c++17', '-fsyntax-only',
                             '-Werror', option, '-'],
                            input=PROBE, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return result.returncode == 0


def compiler_from_record():
    """The compiler the audit script will use, taken from its own compile record."""
    import json
    record = ROOT / ('build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/'
                     'IOPlatformExpert.cpo.json')
    if not record.is_file():
        return None
    arguments = json.loads(record.read_text().strip().rstrip(','))['arguments']
    for value in arguments:
        if value.endswith(('cc', 'c++', 'clang', 'clang++')) and '/' in value:
            return value
    return arguments[0] if arguments else None


def local_audit_script():
    """A copy of the audit script minus suppressions this compiler cannot parse.

    The option lists span multiple lines and the last entry of each is followed
    by a bracket, not a comma, so the lists are rebuilt rather than spliced.
    """
    text = AUDIT.read_text()
    compiler = compiler_from_record()
    print(f'probing warning options against: {compiler or "default clang"}')
    dropped = []

    # Only the compile-command lists are touched: rewriting arbitrary bracket
    # expressions would corrupt expressions like parents[1].
    pattern = re.compile(r"(command \+= \[)([^\]]*?)(\])", re.S)

    def rebuild(match):
        head, body, tail = match.groups()
        options = re.findall(r"'([^']+)'", body)
        kept = []
        for option in options:
            if option.startswith('-Wno-') and not compiler_knows(compiler, option):
                dropped.append(option)
                continue
            kept.append(option)
        rendered = ', '.join(f"'{option}'" for option in kept)
        return f'{head}{rendered}{tail}'

    patched, count = pattern.subn(rebuild, text)
    if not count:
        raise SystemExit('the audit script has no compile-command list to adjust')
    if dropped:
        print('dropped unsupported options: ' + ' '.join(sorted(set(dropped))))
    copy = AUDIT.with_name('audit-storage-interface-hummer.py')
    copy.write_text(patched)
    return copy


def main():
    stage_reference()
    result = subprocess.run([sys.executable, str(local_audit_script())], cwd=str(ROOT))
    if result.returncode:
        raise SystemExit(f'{AUDIT.name} failed with {result.returncode}')
    headers = ROOT / 'build' / 'storage-abi' / 'include' / 'IOKit' / 'storage'
    if not (headers / 'IOBlockStorageDevice.h').is_file():
        raise SystemExit(f'storage ABI headers missing after the audit run: {headers}')
    print(f'build/storage-abi ready: {len(list(headers.glob("*.h")))} storage headers')


if __name__ == '__main__':
    main()
