#!/usr/bin/env python3
"""Compile the generic platform expert against the actual source-kernel ABI."""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]

# drivers/NativeUfs is an upstream submodule, read-only for this overlay repository, so
# the two SMMU edits below are applied to a private copy at build time.  They are exact
# text replacements, each asserted to match exactly once, so an upstream refactor fails
# the build instead of silently dropping the change.
#
# 1. Map the whole SMMU register window.  IDR1.NUMPAGENDXB reads 6 here, so GR0+GR1
#    cover 128 pages and the context banks start at page 128; CB82 sits at 0xD2000,
#    past the 0xd0000 window sized for the earlier board.  Writing CB82's SCTLR through
#    that mapping took a data abort (level 3 translation fault, ESR 96000047).
# 2. Take over the stream entry the firmware already installed.  v28 (probe=7) found
#    SMR[0]=80000060 (VALID, ID 0x60, MASK 0) with S2CR[0]=00000000, i.e. TYPE=TRANSLATE
#    through context bank 0 - ABL's own stage-1 tables, which describe ABL's buffers.
#    The planner refuses while that entry exists and would leave it matching first
#    anyway (SMR hits resolve by group index), so the entry itself is converted:
#      a. plain S2CR bypass - hardware turned it down on this board, which is the quirk
#         the driver comment records for the same IP;
#      b. so the Qualcomm shape is installed instead: a context bank whose CBAR says
#         S1_TRANS_S2_BYPASS with stage 1 disabled, stream pointed at it by CBNDX.
#    No computed address is written before proving it is inside the mapped window; a
#    geometry that does not fit is reported instead of being guessed around.
# 3. The planner's own refusal still prints the ID registers behind it.
NATIVE_UFS_EDITS = [
    (
        'SMMU: map the whole 1 MiB register window',
        '  uintptr_t smmu = mapPhys (0x15000000ULL, 0xd0000, keep);\n',
        """  // IDR1.NUMPAGENDXB=6 -> GR0+GR1 span 128 pages, the context banks start at page 128,
  // so the highest bank's page is at 0xD2000: the window must be the device's full 1 MiB
  // (the DT's reg size), not the smaller mapping sized for the earlier board.
  uintptr_t smmu = mapPhys (0x15000000ULL, 0x100000, keep);
""",
    ),
    (
        "SMMU: take over the firmware's own stream entry",
        '  UfsSmmuPlan plan = planUfsSmmu (stream, dma, bytes, [smmu](unsigned offset) { return read32 (smmu, offset); });\n',
        """  {
    // A fixed 128-slot window rather than IDR0.NUMSMRG, which reads 132 here - past the
    // 128 groups this part implements.  Unused groups read back as zero.
    unsigned cut = 0xffff;
    uint32_t cutSmr = 0, cutS2cr = 0;
    for (unsigned scan = 0; scan < 128; scan++) {
      uint32_t smr = read32 (smmu, 0x800 + 4 * scan);
      // Exact match only: a masked entry also covers neighbouring streams, and rewriting
      // one of those would move traffic this kernel does not own.
      if (!(smr & 0x80000000U) || ((smr >> 16) & 0x7fff) != 0 || (smr & 0xffff) != stream) continue;
      cut = scan;
      cutSmr = smr;
      cutS2cr = read32 (smmu, 0xc00 + 4 * scan);
      break;
    }
    if (cut != 0xffff) {
      // arm_smmu_write_sme() order: an SMR must not be live while its S2CR changes.
      write32 (smmu, 0x800 + 4 * cut, cutSmr & ~0x80000000U);
      __asm__ volatile ("dsb sy" ::: "memory");
      write32 (smmu, 0xc00 + 4 * cut, 0x00010000U);
      __asm__ volatile ("dsb sy" ::: "memory");
      write32 (smmu, 0x800 + 4 * cut, cutSmr);
      __asm__ volatile ("dsb sy" ::: "memory");
      // A translation cached from the firmware's tables would still serve this stream
      // unless the non-secure TLB is invalidated (IHI 0062D.c 3.9).
      write32 (smmu, 0x68, 0);
      write32 (smmu, 0x70, 0);
      for (unsigned spin = 0; spin < 1000 && (read32 (smmu, 0x74) & 1); spin++) IODelay (10);
      if (((read32 (smmu, 0xc00 + 4 * cut) >> 16) & 3) != 1) {
        // Put the firmware's own mapping back before trying anything else.
        write32 (smmu, 0x800 + 4 * cut, cutSmr & ~0x80000000U);
        __asm__ volatile ("dsb sy" ::: "memory");
        write32 (smmu, 0xc00 + 4 * cut, cutS2cr);
        __asm__ volatile ("dsb sy" ::: "memory");
        write32 (smmu, 0x800 + 4 * cut, cutSmr);
        __asm__ volatile ("dsb sy" ::: "memory");
        // Qualcomm shape: the stream reaches a context bank whose CBAR says stage 1
        // translate / stage 2 bypass with stage 1 disabled - how firmware produces a
        // pass-through when a plain BYPASS does not stick.
        uint32_t idr1 = read32 (smmu, 0x24);
        unsigned pages = 1U << (((idr1 >> 28) & 7) + 1);
        unsigned banks = idr1 & 0xff;
        unsigned bank = banks ? banks - 1 : 0;
        unsigned cbOffset = (pages + bank) * 4096;
        if (pages == 0 || banks < 2 || cbOffset + 4096 > 0x100000) {
          // Report instead of guess: the previous build fell back to pages=2 here and
          // wrote CB82's SCTLR outside the mapped window.
          panic ("NATIVE_UFS smmu: geometry idr1=%08x pages=%u banks=%u bank=%u cb=0x%x",
                 idr1, pages, banks, bank, cbOffset);
        }
        write32 (smmu, cbOffset, 0);
        __asm__ volatile ("dsb sy" ::: "memory");
        write32 (smmu, 0x1800 + 4 * bank, 1);
        write32 (smmu, 0x1000 + 4 * bank, 0x00010000U);
        __asm__ volatile ("dsb sy" ::: "memory");
        if (read32 (smmu, cbOffset) != 0 || read32 (smmu, 0x1000 + 4 * bank) != 0x00010000U ||
            read32 (smmu, 0x1800 + 4 * bank) != 1) {
          panic ("NATIVE_UFS smmu: bank bypass config bank=%u cb=0x%x sctlr=%08x cbar=%08x cba2r=%08x",
                 bank, cbOffset, read32 (smmu, cbOffset), read32 (smmu, 0x1000 + 4 * bank),
                 read32 (smmu, 0x1800 + 4 * bank));
        }
        write32 (smmu, 0xc00 + 4 * cut, bank);
        __asm__ volatile ("dsb sy" ::: "memory");
        write32 (smmu, 0x800 + 4 * cut, 0x80000000U | stream);
        __asm__ volatile ("dsb sy" ::: "memory");
        write32 (smmu, 0x68, 0);
        write32 (smmu, 0x70, 0);
        for (unsigned spin = 0; spin < 1000 && (read32 (smmu, 0x74) & 1); spin++) IODelay (10);
        if (read32 (smmu, 0xc00 + 4 * cut) != bank) {
          panic ("NATIVE_UFS smmu: bank bypass readback idx=%u want=%u got=%08x",
                 cut, bank, read32 (smmu, 0xc00 + 4 * cut));
        }
      }
      gRootDiagnostic.smmuSlot = cut;
      gRootDiagnostic.smr = read32 (smmu, 0x800 + 4 * cut);
      gRootDiagnostic.s2cr = read32 (smmu, 0xc00 + 4 * cut);
      report[0] = cut;
      report[1] = gRootDiagnostic.s2cr;
      report[2] = cutS2cr;
      return;
    }
  }
  UfsSmmuPlan plan = planUfsSmmu (stream, dma, bytes, [smmu](unsigned offset) { return read32 (smmu, offset); });
""",
    ),
    (
        'SMMU: report the ID registers behind a geometry refusal',
        '    panic ("NATIVE_UFS smmu: %s slot=%u smr=%x s2cr=%x", plan.error, plan.slot, plan.smr, plan.s2cr);\n',
        """    panic ("NATIVE_UFS smmu: %s slot=%u smr=%x s2cr=%x\\n"
           "scr0=%08x idr0=%08x idr1=%08x idr2=%08x",
           plan.error, plan.slot, plan.smr, plan.s2cr,
           read32 (smmu, 0x00), read32 (smmu, 0x20), read32 (smmu, 0x24), read32 (smmu, 0x28));
""",
    ),
]

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('component', choices=['ArmVirtPlatform', 'ArmDTPlatform', 'ArmCpu', 'NativeAES', 'NativeVirtioBlock', 'NativeRamBlock', 'NativeMobileDisplay', 'NativeVirtioConsole', 'NativeLegacyDisplay', 'NativeGOPPresenter', 'NativeOfflineGPU', 'NativeOfflineAccelerator', 'NativeIOGPUProviderProbe', 'NativeUfs', 'NativeHidInput', 'NativeKeyStoreEvents', 'NativeAdreno'])
parser.add_argument('--negative-aes-kat', action='store_true', help='Isolated own-source wrong-answer refusal control; never changes original inputs')
parser.add_argument('--verified-root', action='store_true', help='Authenticate all actual disk reads against an independently signed immutable policy')
parser.add_argument('--verified-root-bundle', action='store_true', help='Select exactly one capacity-matching policy per device, then independently authenticate it')
parser.add_argument('--negative-verified-block', action='store_true', help='Negative own-buffer control: corrupt only the completed DMA snapshot')
args = parser.parse_args()
if args.verified_root_bundle and not args.verified_root:
    parser.error('A policy bundle requires verified block mode')
if args.verified_root_bundle and args.negative_verified_block:
    parser.error('Select either bundle policy qualification or the isolated DMA snapshot fault')
if (args.verified_root or args.negative_verified_block) and args.component != 'NativeVirtioBlock':
    parser.error('Verified block mode requires the concrete read-only block transport')
if args.negative_verified_block and not args.verified_root:
    parser.error('The isolated input fault requires verified block mode')
component = args.component
recipe = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
record = json.loads(recipe.read_text().strip().rstrip(','))
source = ROOT / 'drivers' / ('NativeMobileDisplay' if component in ('NativeLegacyDisplay','NativeGOPPresenter') else 'NativeMetal' if component in ('NativeOfflineGPU','NativeOfflineAccelerator','NativeIOGPUProviderProbe') else component) / {'NativeAdreno':'NativeAdrenoService.cpp'}.get(component, component + '.cpp')
out = ROOT / 'build' / {'ArmVirtPlatform': 'armvirt-platform', 'ArmDTPlatform': 'armdt-platform', 'ArmCpu': 'armcpu', 'NativeAES': 'native-aes',
                        'NativeVirtioBlock': 'native-virtio-block', 'NativeRamBlock': 'native-ram-block',
                        'NativeMobileDisplay': 'native-mobile-display', 'NativeVirtioConsole': 'native-virtio-console',
                        'NativeLegacyDisplay': 'native-legacy-display', 'NativeGOPPresenter': 'native-gop-presenter', 'NativeOfflineGPU':'native-offline-gpu','NativeOfflineAccelerator':'native-offline-accelerator','NativeIOGPUProviderProbe':'native-original-provider', 'NativeUfs':'native-ufs', 'NativeHidInput':'native-hid-input', 'NativeKeyStoreEvents':'native-keystore-events', 'NativeAdreno':'native-adreno'}[component]
if args.verified_root:
    out = ROOT / ('build/native-virtio-block-verified-fault' if args.negative_verified_block else 'build/native-virtio-block-verified')
    if args.verified_root_bundle:
        out = ROOT / 'build/native-virtio-block-verified-bundle'
out.mkdir(parents=True, exist_ok=True)
if component == 'NativeUfs':
    # Compile a patched private copy: the quoted includes (GccUfs.h, SmmuTakeover.h) have
    # to sit next to the translation unit, so the whole directory is staged.
    staged = out / 'src'
    if staged.exists():
        shutil.rmtree(staged)
    shutil.copytree(source.parent, staged)
    patched = staged / source.name
    text = patched.read_text()
    for why, old, new in NATIVE_UFS_EDITS:
        if text.count(old) != 1:
            raise SystemExit(f'NativeUfs patch anchor is not unique ({text.count(old)} matches): {why}')
        text = text.replace(old, new)
    patched.write_text(text)
    source = patched
if args.negative_aes_kat:
    if component != 'NativeAES':
        parser.error('The own-source KAT control requires NativeAES')
    out = ROOT / 'build/native-aes-kat-negative'
    out.mkdir(parents=True, exist_ok=True)
    text = source.read_text()
    marker = '0x76,0x49,0xab,0xac'
    if text.count(marker) != 1:
        raise SystemExit('The exact own-source NIST answer was not uniquely identified')
    (out / 'OriginalAESABI.h').write_bytes(source.with_name('OriginalAESABI.h').read_bytes())
    source = out / 'NativeAES.cpp'
    source.write_text(text.replace(marker, '0x77,0x49,0xab,0xac'))
destination = out / (component + '.o')
arguments = record['arguments']
command = []
index = 0
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
command.extend(['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                # clang 17 removed -Wunnecessary-virtual-specifier entirely, so the -Wno- form below is
                # an unknown option there; keep -Werror strict but let version-drifted -Wno- flags pass.
                '-Wno-unknown-warning-option',
                '-Wno-unnecessary-virtual-specifier', '-I', str(ROOT / '.cache/xnu-work/osfmk')])
if component in ('NativeVirtioBlock', 'NativeRamBlock', 'NativeUfs'):
    command += ['-I', str(ROOT / 'build/storage-abi/include')]
    if args.verified_root:
        command += ['-I', str(ROOT / 'kits/VerifiedRoot/include'), '-DNATIVE_VERIFIED_BLOCK=1']
    if args.negative_verified_block:
        command += ['-DNATIVE_VERIFIED_BLOCK_INJECT_FAULT=1']
    if args.verified_root_bundle:
        command += ['-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1']
if component == 'NativeUfs':
    command += ['-I', str(ROOT / 'kits/VerifiedRoot/include'), '-I', str(ROOT / 'drivers/NativeVirtioBlock'),
                '-DNATIVE_VERIFIED_BLOCK=1', '-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1', '-mgeneral-regs-only']
    # GccUfs.h in the submodule carries SM8550 offsets.  SM8750 moves every UFS PHY
    # branch register after the AXI branch up by 4 bytes, and the SM8550 AHB offset
    # (0x77024) is a reserved word there: the probe read 0, decided the AHB branch was
    # gated, and never touched the host controller.  kits/hummer/HummerGccUfs.h (staged
    # from package/) holds the gcc-sm8750.c facts and wins by defining the submodule's
    # own include guard ahead of it.  -include is the only mechanism that works here:
    # NativeUfs.cpp quote-includes GccUfs.h, so the including file's directory always
    # beats any -I.
    gcc_ufs = ROOT / 'drivers/NativeUfs/GccUfs.h'
    # package/ maps to kits/hummer in the assembled tree (board.toml kit = "hummer").
    override = ROOT / 'kits/hummer/HummerGccUfs.h'
    if not gcc_ufs.exists():
        raise SystemExit(f'{gcc_ufs} is missing; the SM8750 UFS override has nothing to shadow')
    if '#ifndef NATIVE_UFS_GCC_UFS_H' not in gcc_ufs.read_text():
        raise SystemExit(f'{gcc_ufs} no longer uses NATIVE_UFS_GCC_UFS_H as its guard, so the SM8750 override would be shadowed')
    if not override.exists():
        raise SystemExit(f'{override} is missing: package/HummerGccUfs.h did not map to kits/hummer')
    command += ['-include', str(override)]
if component=='NativeOfflineGPU':
    command += ['-I',str(ROOT/'drivers/NativeVirtioConsole')]
if component in ('NativeLegacyDisplay','NativeHidInput'):
    command += ['-I', str(out)]
if component == 'NativeAdreno':
    command += ['-I', str(ROOT / 'kits/ArmSmmu4K'), '-I', str(ROOT / 'kits/AdrenoPM4'),
                '-I', str(ROOT / 'kits/sheng'), '-I', str(ROOT / 'kits/AdrenoFirmware'),
                '-I', str(ROOT / 'build/sheng-gmu')]
# The kernel build driver prepends Homebrew LLVM to PATH (for EDK2 CLANGPDB tools).
# Replaying the compile record with that clang breaks: llvm 23 dropped the
# -Wno-unnecessary-virtual-specifier flag this driver passes. Compile with Xcode's
# clang (the compiler that actually generated the record) via xcrun.
if command and command[0] in ('clang', 'clang++'):
    command = ['xcrun', '--sdk', 'iphoneos', command[0]] + command[1:]
(out / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
result = subprocess.run(command, cwd=record['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
(out / 'compile.log').write_text(result.stdout)
if result.returncode:
    raise SystemExit(result.stdout[-3500:])
data = destination.read_bytes()
magic, cpu, subtype, kind, count, size = struct.unpack_from('<6I', data)
if (magic, cpu, subtype, kind) != (0xfeedfacf, 0x100000c, 0, 1):
    raise SystemExit('Unexpected platform object architecture')
offset = 32
deployment = None
for _ in range(count):
    cmd, length = struct.unpack_from('<II', data, offset)
    if cmd == 0x32:
        deployment = struct.unpack_from('<II', data, offset + 8)
    offset += length
if deployment != (2, 0x110000):
    raise SystemExit(f'Unexpected platform deployment {deployment}')
archive = out / ('lib' + component + '.a')
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, destination], check=True)
print(f'{component}: -Werror build and actual ARM64/iOS 17 object audit passed')
