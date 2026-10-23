#!/usr/bin/env python3
"""Generate the minimal hummer hardware Apple-format DT for the first boot.

A faithful port of UniXNU-sheng/scripts/make-sheng-adt.py, retargeted to the
OnePlus Ace 5 Pro (hummer, SM8750 "sun/sunp"). The tree mirrors the proven
QEMU entry fixture structure with real pinned hummer values taken from the
board DT (/proc/iomem on the stock Android, and Resources/DTBs/hummer.dts):

  - GICv3 distributor 0x16000000/0x10000, redistributor 0x16080000/0x200000
    (child offsets under the APSS range at 0x16000000), stride 0x40000
    (from stock /proc/iomem: "16000000-1600ffff : GICD",
     "16080000-1627ffff : GICR"; MemoryMapLib "APSS_HM" 0x16000000).
  - CPU timebase 19200000 Hz (Qualcomm SM8xxx architectural counter)
  - DRAM base 0x80000000; size comes from the actual device observation and
    is cross-checked at build time (hummer = 16 GiB = 0x400000000).
  - UFS host controller 0x1d84000 (phy 0x1d80000), same place as SM8550.

CPU topology (from hummer.dts): 6 perf cores at MPIDR Affinity1 0x000..0x500
plus 2 Oryon prime cores at Affinity2 0x10000/0x10100. The board DT uses the
generic 'arm,armv8' compatible for all cores (unlike sheng's cortex-a715).

This is first-boot bring-up data: the platform has no matching platform
expert yet, so the kernel is expected to reach the video console and report
the genuine missing-driver panic on screen. No Apple device identity is
claimed and no property is invented beyond these pinned sources.
"""
from pathlib import Path
import argparse
import os
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]

DRAM_BASE = 0x80000000
ARMIO_BASE = 0x16000000          # APSS peripheral block containing the GIC
GIC_DIST = 0x16000000
GIC_DIST_SIZE = 0x10000
GIC_REDIST = 0x16080000
GIC_REDIST_SIZE = 0x200000
TIMEBASE = 19200000
UFS_HC = 0x1d84000
UFS_HC_SIZE = 0x3000
UFS_PHY = 0x1d80000
UFS_PHY_SIZE = 0x2000

# hummer.dts /cpus reg values: 6 perf cores then 2 Oryon prime cores.
# The boot CPU is cpu@0 (Affinity1 = 0).
# v50: list ONLY the six cores whose MPIDR fits the upstream AFF1|AFF0 mask,
# mirroring UniXNU-sheng (SM8550 8 cores, 6 listed, two little cores left
# off).  The two Oryon prime cores live in Affinity2 (0x10000/0x10100);
# upstream ml_get_cpu_number() masks to AFF1|AFF0, so they collapsed onto
# logical 0 and, once ArmCpu::probe rejected them, AppleS8000CPU panicked
# "CPU configuration in device tree is invalid" (DT 8 cores vs 6 matched).
# Leaving them out of the ADT keeps the device tree consistent with what
# the drivers accept; sheng's tool even refuses reg > 0xffff.
DEFAULT_CPU_REGS = "0,0x100,0x200,0x300,0x400,0x500"


def node(name, properties=(), children=()):
    properties = [('name', name.encode() + b'\0'), *properties]
    result = struct.pack('<II', len(properties), len(children))
    for key, data in properties:
        result += struct.pack('<32sI', key.encode(), len(data))
        result += data + bytes(-len(data) % 4)
    return result + b''.join(children)


def empty_nvram_proxy():
    """A fresh volatile CHRP bank for upstream IODTNVRAM, with no controller."""
    def header(name, size):
        raw = bytearray(struct.pack('<BBH12s', 0x70, 0, size // 16, name))
        checksum = sum(raw)
        while checksum > 255:
            checksum = (checksum & 255) + (checksum >> 8)
        raw[1] = checksum
        return raw

    image = bytearray(8192)
    image[:16] = header(b'nvram', 32)
    image[32:48] = header(b'common', 16 + 2048)
    free_at = 32 + 16 + 2048
    image[free_at:free_at + 16] = header(b'free', len(image) - free_at)
    struct.pack_into('<I', image, 16, zlib.adler32(image[20:]))
    return [('nvram-bank-size', struct.pack('<I', len(image))),
            ('nvram-bank-count', struct.pack('<I', 1)),
            ('nvram-current-bank', struct.pack('<I', 0)),
            ('nvram-proxy-data', bytes(image))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=ROOT / 'build/hummer/boot/kernel.macho.adt')
    parser.add_argument('--dram-size', type=lambda value: int(value, 0), required=True,
                        help='Actual observed DRAM size, e.g. 0x400000000')
    parser.add_argument('--owned-memory-size', type=lambda value: int(value, 0), default=None,
                        help='Explicit DarwinBoot contiguous window request, 256 MiB..8 GiB in 2 MiB steps')
    parser.add_argument('--root-match-volume-uuid', default=None,
                        help='/chosen/root-matching: select the root by the UUID of an IOMedia')
    parser.add_argument('--unique-chip-id', type=lambda value: int(value, 16), default=None,
                        help='/chosen/unique-chip-id (8 bytes) and --chip-id (4 bytes) seed XNU IOPlatformUUID; software identity seeds')
    parser.add_argument('--chip-id', type=lambda value: int(value, 16), default=None)
    parser.add_argument('--ipad-identity', action='store_true',
                        help='/product and /buttons describe an iPad-class device (TODO: hummer identity not yet pinned)')
    parser.add_argument('--cpu-regs', default=DEFAULT_CPU_REGS,
                        help='Comma-separated CPU MPIDR affinity values; the first is the running boot CPU')
    parser.add_argument('--psci-method', choices=['smc', 'hvc'], default='smc',
                        help='/psci method used by the secondary-CPU start (the board DT declares smc)')
    parser.add_argument('--packed-data-ramdisk', action='store_true', help='Hand off validated DBRDISK1 bytes to the native sparse block driver')
    parser.add_argument('--data-ramdisk', action='store_true',
                        help='/chosen/memory-map RAMDisk placeholder; the loader fills in the real range for a scratch Data seed')
    parser.add_argument('--trust-cache', action='store_true', help='Zeroed TrustCache sidecar handoff placeholder')
    args = parser.parse_args()
    if (args.unique_chip_id is None) != (args.chip_id is None):
        parser.error('--unique-chip-id and --chip-id must be supplied together')
    if args.unique_chip_id is not None and not (0 < args.unique_chip_id < 1 << 64 and 0 < args.chip_id < 1 << 32):
        parser.error('--unique-chip-id must fit 8 bytes and --chip-id 4 bytes, both nonzero')
    if args.owned_memory_size is not None and (
            not 0x10000000 <= args.owned_memory_size <= 0x200000000
            or args.owned_memory_size % 0x200000):
        parser.error('Owned memory must be 256 MiB to 8 GiB, aligned to 2 MiB')

    try:
        cpu_regs = [int(value, 0) for value in args.cpu_regs.split(',')]
    except ValueError:
        parser.error('--cpu-regs must be integers')
    # v50: cap restored to sheng's 0xffff -- the ADT now lists only cores
    # within the upstream AFF1|AFF0 mask; Affinity0 must still be 0.
    if (not cpu_regs or cpu_regs[0] != 0 or len(set(cpu_regs)) != len(cpu_regs) or len(cpu_regs) > 8
            or any(reg < 0 or reg > 0xffff or reg & 0xff for reg in cpu_regs)):
        parser.error('--cpu-regs: up to eight distinct cores, boot CPU 0 first, Affinity0 must be 0')
    cpus = [node('cpu%d' % index, [('state', b'running\0' if index == 0 else b'waiting\0'),
                                   ('reg', struct.pack('<I', reg)),
                                   ('device_type', b'cpu\0'),
                                   # hummer.dts uses the generic compatible for all cores
                                   # (TODO: XNU may need a more specific Oryon/armv8.2 string).
                                   ('compatible', b'arm,armv8\0'),
                                   ('timebase-frequency', struct.pack('<I', TIMEBASE))])
            for index, reg in enumerate(cpu_regs)]
    tree = node('device-tree', [
        ('model', b'iPad7,1\0' if args.ipad_identity else b'hummer-first-boot\0'),
        *([('target-type', b'J120\0')] if args.ipad_identity else []),
        # SM8750 is "sun"/"sunp" in the Qualcomm Linux tree; root compatible kept
        # as a first-boot placeholder (TODO: finalize against XNU's matching).
        ('compatible', b'qcom,sun\0qcom,sm8750\0'),
        ('#address-cells', struct.pack('<I', 2)),
        ('#size-cells', struct.pack('<I', 2)),
    ], [
        node('chosen', [('random-seed', os.urandom(64)),
                        *([('unique-chip-id', struct.pack('<Q', args.unique_chip_id)), ('chip-id', struct.pack('<I', args.chip_id))]
                          if args.unique_chip_id is not None else []),
                        ('osenvironment', b'main\0'),
                        *([('certificate-production-status', struct.pack('<I', 0)), ('effective-production-status-ap', struct.pack('<I', 0)),
                           ('certificate-security-mode', struct.pack('<I', 1)), ('effective-security-mode-sep', struct.pack('<I', 1))]
                          if args.ipad_identity else []),
                        *([('ephemeral-storage', struct.pack('<I', 1))] if (args.data_ramdisk or args.packed_data_ramdisk) else []),
                        *([('darwinboot-memory-size', struct.pack('<Q', args.owned_memory_size))]
                          if args.owned_memory_size else []),
                        ('dram-base', struct.pack('<Q', DRAM_BASE)),
                        ('dram-size', struct.pack('<Q', args.dram_size)),
                        ('crypto-hash-method', b'sha2-384\0'),
                        *([('root-matching', ('<dict><key>IOProviderClass</key><string>IOMedia</string>'
                                              '<key>IOPropertyMatch</key><dict><key>UUID</key><string>' +
                                              args.root_match_volume_uuid.upper() + '</string></dict></dict>\0').encode())]
                          if args.root_match_volume_uuid else []),
                        ('amfi-only-platform-code', struct.pack('<I', 1)),
                        *empty_nvram_proxy()],
             [node('memory-map', ([("PackedRAMDisk" if args.packed_data_ramdisk else "RAMDisk", bytes(16))] if (args.data_ramdisk or args.packed_data_ramdisk) else []) +
                                 ([('TrustCache', bytes(16))] if args.trust_cache else [])),
              node('manifest-properties')]),
        node('cpus', children=cpus),
        *([node('psci', [('compatible', b'arm,psci-1.0\0'), ('method', args.psci_method.encode() + b'\0')])] if len(cpus) > 1 else []),
        node('defaults', [('sleep-disabled', struct.pack('<I', 1))]),
        node('options', []),
        *( [node('buttons', [('compatible', b'buttons\0'), ('device_type', b'buttons\0'),
                             ('button-names', b'hold\0voldown\0volup\0'),
                             ('home-button-type', struct.pack('<I', 2))])] if args.ipad_identity else [] ),
        node('product', [('product-name', b'iPad Pro (12.9-inch) (2nd generation)\0' if args.ipad_identity else b'OnePlus Ace 5 Pro\0'),
                         ('model', b'qcom,sun\0'),
                         *( [('product-description', b'iPad Pro (12.9-inch) (2nd generation)\0'),
                             ('artwork-device-idiom', b'pad\0'),
                             ('artwork-device-subtype', struct.pack('<I', 2732)),
                             ('artwork-scale-factor', struct.pack('<I', 2)),
                             ('artwork-display-gamut', b'P3\0'),
                             ('compatible-device-fallback', b'iPad6,7\0'),
                             ('thin-bezel', struct.pack('<I', 1))] if args.ipad_identity else [] )]),
        node('arm-io', [('device_type', b'arm-io\0'),
                        ('#address-cells', struct.pack('<I', 2)),
                        ('#size-cells', struct.pack('<I', 2)),
                        ('ranges', struct.pack('<QQQ', 0, ARMIO_BASE, 0x08000000))],
             [node('intc', [('compatible', b'arm,gic-v3\0'),
                            ('reg', struct.pack('<4Q',
                                                GIC_DIST - ARMIO_BASE, GIC_DIST_SIZE,
                                                GIC_REDIST - ARMIO_BASE, GIC_REDIST_SIZE)),
                            ('redistributor-stride', struct.pack('<Q', 0x40000))]),
              # UFS (0x1d84000) lies below the arm-io window; NativeUfs takes its
              # physical windows from the board constants, the node is for matching.
              node('ufshc', [('compatible', b'qcom,sm8750-ufshc\0')])]),
    ])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(tree)
    print(f'hummer ADT: {len(tree)} bytes, dram {args.dram_size:#x}, timebase {TIMEBASE}')


if __name__ == '__main__':
    main()
