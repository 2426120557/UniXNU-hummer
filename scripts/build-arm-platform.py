#!/usr/bin/env python3
"""Build the source ArmDTPlatform and ArmCpu with XNU's recorded IOKit flags.

Ported from IssacL04/UniXNU-Pagani_SM8750 (their engine patch adds the same
file to the patched upstream).  hummer only needs compile_command(): the
Stage 7 display components are compiled with the IOKit build's own record,
exactly like build-hummer-storage-stubs.py does for the storage stubs, and
the shared helper keeps that argument rewriting in one place.
"""
import json
import struct
import subprocess
from pathlib import Path


def compile_command(record, source, output):
    args = iter(record['arguments'])
    command = []
    found_source = False
    for value in args:
        if value == record['file']:
            command.append(str(source))
            found_source = True
        elif value == '-o':
            next(args)
            command.extend(['-o', str(output)])
        elif value in ('-MF', '-MJ', '-MT', '-MQ'):
            next(args)
        elif value not in ('-MD', '-MMD', '-MP'):
            command.append(value)
    if not found_source or '-o' not in command:
        raise ValueError('IOKit compile record lacks its source or output argument')
    return command


def main():
    root = Path(__file__).resolve().parents[1]
    recipe = root / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    record = json.loads(recipe.read_text().strip().rstrip(','))
    for driver, folder in [('ArmDTPlatform', 'armdt-platform'), ('ArmCpu', 'armcpu')]:
        output = root / 'build' / folder
        output.mkdir(parents=True, exist_ok=True)
        obj = output / f'{driver}.o'
        command = compile_command(record, root / 'drivers' / driver / f'{driver}.cpp', obj)
        subprocess.run(command, cwd=record['directory'], check=True)
        with obj.open('rb') as stream:
            header = stream.read(16)
        if len(header) != 16 or struct.unpack('<4I', header) != (0xfeedfacf, 0x100000c, 0, 1):
            raise SystemExit(f'{driver}.o is not an ARM64 Mach-O object')
    print('arm-platform: ArmDTPlatform and ArmCpu compiled with the recorded IOKit flags')


if __name__ == '__main__':
    main()
