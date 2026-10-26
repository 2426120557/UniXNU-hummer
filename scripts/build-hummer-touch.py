#!/usr/bin/env python3
"""Compile the hummer Synaptics S3910 touch probe against the actual source-kernel ABI.

Two compile records are replayed, exactly the two sheng's touch build replays:
  - the iokit driver record (IOPlatformExpert.cpo.json) for the C++ probe, which
    includes IOKit headers and links against the source kernel's C++ ABI;
  - the osfmk startup record (startup.o.json) for the generic GENI SPI library
    (drivers/NativeGeniSpi/GeniSpi.c, a submodule of the pinned UniXNU engine),
    compiled with the kernel C flags.
Both objects are archived into build/hummer-touch/libHummerTouch.a, which
hummer_defconfig force-loads via XNU_EXTRA_LD_ARCHIVES. The probe includes
"GeniSpi.h" and "HummerTouchBoard.h" (from the kit), so both include directories
are appended to the replayed command.
"""
import json
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/hummer-touch'
OUT.mkdir(parents=True, exist_ok=True)

CPP_RECORD = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
C_RECORD = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json'
PROBE = ROOT / 'kits/hummer/HummerTouchProbe.cpp'
GSI = ROOT / 'kits/hummer/HummerGsi.c'
GSI_H = ROOT / 'kits/hummer/HummerGsi.h'
RPMH = ROOT / 'kits/hummer/HummerRpmh.c'
BUTTONS = ROOT / 'kits/hummer/HummerButtons.cpp'
GENI = ROOT / 'drivers/NativeGeniSpi/GeniSpi.c'
GENI_H = ROOT / 'drivers/NativeGeniSpi/GeniSpi.h'


def run(command, cwd):
    result = subprocess.run([str(v) for v in command], cwd=str(cwd),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        raise SystemExit(result.stdout[-3500:])
    return result.stdout


def replay(record_path, source, destination):
    record = json.loads(record_path.read_text().strip().rstrip(','))
    command = []
    arguments = record['arguments']
    index = 0
    # Identical replay loop to build-arm-driver.py (the proven one for the iokit
    # record): swap the input file, redirect -o, drop warning-flag pairs. Combined
    # dependency flags like -MF<path> pass through untouched.
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source))
        elif value == '-o':
            command.extend(['-o', str(destination)])
            index += 1
        elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    return command, record['directory']


def audit(object_path):
    data = object_path.read_bytes()
    magic, cpu, subtype, kind, count, size = struct.unpack_from('<6I', data)
    if (magic, cpu, subtype, kind) != (0xfeedfacf, 0x100000c, 0, 1):
        raise SystemExit(f'Unexpected platform object architecture in {object_path.name}')
    offset = 32
    deployment = None
    for _ in range(count):
        cmd, length = struct.unpack_from('<II', data, offset)
        if cmd == 0x32:
            deployment = struct.unpack_from('<II', data, offset + 8)
        offset += length
    if deployment != (2, 0x110000):
        raise SystemExit(f'Unexpected platform deployment {deployment}')


for required in (CPP_RECORD, C_RECORD, PROBE, GSI, GSI_H, ROOT / 'drivers/NativeGeniSpi/GeniSpi.h'):
    if not required.exists():
        raise SystemExit(f'{required} is missing; the touch build cannot proceed')

cpp_command, cpp_dir = replay(CPP_RECORD, PROBE, OUT / 'HummerTouchProbe.o')
# No -Werror: the probe is compiled against engine headers and the engine's
# own warnings must not fail the board build. Warnings are still printed.
cpp_command += ['-Wall', '-Wextra', '-Wno-unused-parameter',
                '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                '-Wno-unknown-warning-option', '-Wno-unnecessary-virtual-specifier',
                # The probe's diagnostics counters are volatile (read from the
                # panic screen); ++ on a volatile is deprecated in C++20.
                '-Wno-deprecated-volatile',
                '-I', str(ROOT / '.cache/xnu-work/osfmk'),
                '-I', str(ROOT / 'kits/hummer'), '-I', str(ROOT / 'drivers/NativeGeniSpi')]
if cpp_command and cpp_command[0] in ('clang', 'clang++'):
    cpp_command = ['xcrun', '--sdk', 'iphoneos', cpp_command[0]] + cpp_command[1:]
(OUT / 'compile-command.json').write_text(json.dumps(cpp_command, indent=2) + '\n')
run(cpp_command, cpp_dir)
audit(OUT / 'HummerTouchProbe.o')

# Mainline spi-geni-qcom.c setup_se_xfer() - the SE/FIFO path, which is what this driver
# uses - builds the full-duplex command as `m_cmd |= SPI_TX_ONLY; m_cmd |= SPI_RX_ONLY;`,
# i.e. 1 | 2 = 3. SPI_TX_RX (7) exists in the driver but is NOT used by the SE path (the
# GSI path uses its own peripheral.cmd enums). So the engine header's value 3 is correct;
# an earlier "3 -> 7" patch here was wrong and left the engine latching M_ACTIVE without
# ever executing the command (every full-duplex read timed out at -110). Keep 3 verbatim.
geni_source = OUT / 'GeniSpi.c'
geni_header = OUT / 'GeniSpi.h'
header_text = GENI_H.read_text()
opcode = '#define GENI_M_CMD_TX_RX 3'
if opcode not in header_text:
    raise SystemExit('GeniSpi.h no longer defines GENI_M_CMD_TX_RX as 3; re-check the SPI opcode')
geni_header.write_text(header_text)   # keep 3 = SPI_TX_ONLY|SPI_RX_ONLY (mainline SE/FIFO full duplex)
geni_source.write_text(GENI.read_text())
print('HummerTouch: GENI_M_CMD_TX_RX = 3 (TX_ONLY|RX_ONLY, mainline SE/FIFO full-duplex)')

c_command, c_dir = replay(C_RECORD, geni_source, OUT / 'GeniSpi.o')
# GeniSpi.c is ENGINE code (drivers/NativeGeniSpi); compiling it with -Werror
# fails on the engine's own warnings. Keep -mgeneral-regs-only for kernel code.
c_command += ['-Wall', '-Wextra', '-mgeneral-regs-only',
              '-I', str(ROOT / '.cache/xnu-work/osfmk'),
              '-I', str(ROOT / 'drivers/NativeGeniSpi')]
if c_command and c_command[0] in ('clang', 'cc', 'clang++'):
    c_command = ['xcrun', '--sdk', 'iphoneos', c_command[0]] + c_command[1:]
(OUT / 'compile-command-geni.json').write_text(json.dumps(c_command, indent=2) + '\n')
run(c_command, c_dir)
audit(OUT / 'GeniSpi.o')

# GSI/GPI DMA engine (package/HummerGsi.c). The serial engine's FIFO interface is
# disabled in hardware and its SE-DMA block is not host-accessible on this part, so the
# only data path is the GPI: same kernel C flags as the rest of the engine code.
gsi_command, gsi_dir = replay(C_RECORD, GSI, OUT / 'HummerGsi.o')
gsi_command += ['-Wall', '-Wextra', '-mgeneral-regs-only',
                '-I', str(ROOT / '.cache/xnu-work/osfmk'),
                '-I', str(ROOT / 'kits/hummer')]
if gsi_command and gsi_command[0] in ('clang', 'cc', 'clang++'):
    gsi_command = ['xcrun', '--sdk', 'iphoneos', gsi_command[0]] + gsi_command[1:]
(OUT / 'compile-command-gsi.json').write_text(json.dumps(gsi_command, indent=2) + '\n')
run(gsi_command, gsi_dir)
audit(OUT / 'HummerGsi.o')

# apps RSC (RPMh) vote for the two PM8550B rails (package/HummerRpmh.c): same kernel C
# flags, same record. Pure register/busy-wait code, no IOKit.
rpmh_command, rpmh_dir = replay(C_RECORD, RPMH, OUT / 'HummerRpmh.o')
rpmh_command += ['-Wall', '-Wextra', '-mgeneral-regs-only',
                 '-I', str(ROOT / '.cache/xnu-work/osfmk'),
                 '-I', str(ROOT / 'kits/hummer')]
if rpmh_command and rpmh_command[0] in ('clang', 'cc', 'clang++'):
    rpmh_command = ['xcrun', '--sdk', 'iphoneos', rpmh_command[0]] + rpmh_command[1:]
(OUT / 'compile-command-rpmh.json').write_text(json.dumps(rpmh_command, indent=2) + '\n')
run(rpmh_command, rpmh_dir)
audit(OUT / 'HummerRpmh.o')

# Power and volume buttons (package/HummerButtons.cpp): three PMIC peripherals read over
# SPMI via the engine's BootResetCore.h helper. IOKit objects, so the same C++ record as the
# probe.
buttons_command, buttons_dir = replay(CPP_RECORD, BUTTONS, OUT / 'HummerButtons.o')
buttons_command += ['-Wall', '-Wextra', '-Wno-unused-parameter',
                    '-Wno-unknown-warning-option',
                    '-I', str(ROOT / '.cache/xnu-work/osfmk'),
                    '-I', str(ROOT / 'kits/hummer')]
if buttons_command and buttons_command[0] in ('clang', 'clang++'):
    buttons_command = ['xcrun', '--sdk', 'iphoneos', buttons_command[0]] + buttons_command[1:]
(OUT / 'compile-command-buttons.json').write_text(json.dumps(buttons_command, indent=2) + '\n')
run(buttons_command, buttons_dir)
audit(OUT / 'HummerButtons.o')

archive = OUT / 'libHummerTouch.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, OUT / 'HummerTouchProbe.o', OUT / 'GeniSpi.o',
                OUT / 'HummerGsi.o', OUT / 'HummerRpmh.o', OUT / 'HummerButtons.o'], check=True)
print('HummerTouch: ARM64/iOS 17 object audit passed (probe + GeniSpi + GSI + RPMh + buttons)')
