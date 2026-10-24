#!/usr/bin/env python3
"""Build read-only UFS and its existing root-binding helper with the kernel ABI."""
import argparse
import hashlib
import json
import shutil
from pathlib import Path
import runpy
import struct
import subprocess

# v54 (hummer dual-system): the SHNGROOT manifest can live on a partition other than
# "userdata" (e.g. a dedicated "unixnu" partition), while Android keeps its own
# userdata. Select it with the native-ufs-partition=<GPT name> boot argument. The
# upstream driver hardcodes "userdata" in findRootLayout(); these edits make the
# partition name configurable (default still "userdata", so sheng is unchanged).
# Anchors are exact-once: if they stop matching the upstream source, the build fails
# loudly instead of silently producing a driver that ignores the boot argument.
NATIVE_UFS_PARTITION_EDITS = [
    (
        '  PE_parse_boot_argn ("native-ufs-root", &rootMode, sizeof (rootMode));\n'
        '  native_ufs_probe_requested = probe != 0;\n',
        '  PE_parse_boot_argn ("native-ufs-root", &rootMode, sizeof (rootMode));\n'
        '  {\n'
        '    // A dual-system board keeps Android\'s userdata and flashes the root image into a\n'
        '    // separately named partition: native-ufs-partition=unixnu. The GPT name field is\n'
        '    // UTF-16LE and 32 characters wide, so a longer value is not a name we can match.\n'
        '    char name[sizeof (gRootPartitionName)] = {};\n'
        '    if (PE_parse_boot_argn ("native-ufs-partition", name, sizeof (name)) && name[0]) {\n'
        '      unsigned i = 0;\n'
        '      for (; i + 1 < sizeof (gRootPartitionName) && name[i]; ++i) gRootPartitionName[i] = name[i];\n'
        '      gRootPartitionName[i] = \'\\0\';\n'
        '    }\n'
        '  }\n'
        '  native_ufs_probe_requested = probe != 0;\n',
    ),
]
NATIVE_UFS_PARTITION_HEADER_EDITS = [
    (
        'RootLayout.h',
        'static uint8_t gScratch[kDataMax];\n',
        'static uint8_t gScratch[kDataMax];\n'
        '\n'
        '// v54: the partition whose first block carries the root manifest. sheng keeps the\n'
        '// image in Android\'s own userdata; a dual-system board flashes it into a separate\n'
        '// partition and selects it with native-ufs-partition=<GPT name>.\n'
        'static char gRootPartitionName[32] = "userdata";\n',
    ),
    (
        'RootLayout.h',
        '  static const char wanted[] = "userdata";\n',
        '  const char *wanted = gRootPartitionName;\n'
        '  unsigned wantedLen = 0;\n'
        '  while (wantedLen < sizeof (gRootPartitionName) - 1 && wanted[wantedLen]) ++wantedLen;\n',
    ),
    (
        'RootLayout.h',
        '    bool match = true;\n'
        '    for (unsigned c = 0; c < sizeof (wanted); c++) {\n'
        '      if (entry[56 + 2 * c] != static_cast<uint8_t> (wanted[c]) || entry[57 + 2 * c] != 0) {\n'
        '        match = false;\n'
        '        break;\n'
        '      }\n'
        '    }\n',
        '    bool match = wantedLen > 0;\n'
        '    for (unsigned c = 0; match && c <= wantedLen; c++) {\n'
        '      if (entry[56 + 2 * c] != static_cast<uint8_t> (wanted[c]) || entry[57 + 2 * c] != 0) {\n'
        '        match = false;\n'
        '      }\n'
        '    }\n',
    ),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root-windows', action='store_true')
    parser.add_argument('--component', choices=('NativeUfs', 'NativeVirtioBlock', 'NativeRamBlock'), default='NativeUfs')
    args = parser.parse_args()
    if args.component != 'NativeUfs' and not args.root_windows:
        parser.error('The root-binding helper requires --root-windows')
    root = Path(__file__).resolve().parents[1]
    compile_command = runpy.run_path(str(root / 'scripts/build-arm-platform.py'))['compile_command']
    recipe = root / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    record = json.loads(recipe.read_text().strip().rstrip(','))
    output = root / {'NativeUfs': 'build/native-ufs', 'NativeVirtioBlock': 'build/native-virtio-block',
                     'NativeRamBlock': 'build/native-ram-block'}[args.component]
    output.mkdir(parents=True, exist_ok=True)
    source = root / 'drivers' / args.component / (args.component + '.cpp')
    if args.component == 'NativeUfs':
        # v54 (hummer dual-system): apply the configurable-partition-name patch to a
        # staged copy so the upstream submodule stays untouched. See the edit tables above.
        staged = output / 'src'
        if staged.exists():
            shutil.rmtree(staged)
        shutil.copytree(source.parent, staged)
        patched = staged / source.name
        text = patched.read_text()
        if text.count(NATIVE_UFS_PARTITION_EDITS[0][0]) != 1:
            raise SystemExit('NativeUfs partition patch anchor is not unique '
                             f'({text.count(NATIVE_UFS_PARTITION_EDITS[0][0])} matches)')
        patched.write_text(text.replace(NATIVE_UFS_PARTITION_EDITS[0][0], NATIVE_UFS_PARTITION_EDITS[0][1]))
        for hname, hold, hnew in NATIVE_UFS_PARTITION_HEADER_EDITS:
            target = staged / hname
            header = target.read_text()
            if header.count(hold) != 1:
                raise SystemExit(f'NativeUfs header patch anchor is not unique '
                                 f'({header.count(hold)} matches): {hname}')
            target.write_text(header.replace(hold, hnew))
        source = patched
    obj = output / (args.component + '.o')
    command = compile_command(record, source, obj)
    command += ['-DNATIVE_UFS_SM8750=1', '-DNATIVE_UFS_READ_ONLY=1',
                '-Wno-unused-function']
    if args.root_windows:
        command += ['-I', str(root / 'build/storage-abi/include'),
                    '-I', str(root / 'kits/VerifiedRoot/include'),
                    '-I', str(root / 'drivers/NativeVirtioBlock'),
                    '-DNATIVE_VERIFIED_BLOCK=1', '-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1',
                    '-mgeneral-regs-only']
    else:
        command += ['-DNATIVE_UFS_PROBE_ONLY=1']
    subprocess.run(command, cwd=record['directory'], check=True)
    if struct.unpack('<4I', obj.read_bytes()[:16]) != (0xfeedfacf, 0x100000c, 0, 1):
        raise SystemExit('NativeUfs: expected ordinary ARM64 Mach-O object')
    symbols = subprocess.check_output(['xcrun', 'nm', '-u', obj], text=True)
    if not args.root_windows and 'IOBlockStorageDevice' in symbols:
        raise SystemExit('Probe unexpectedly requires prelinked IOStorageFamily')
    if args.root_windows and 'IOBlockStorageDevice' not in symbols:
        raise SystemExit('Root-window build lacks the storage class')
    if args.component != 'NativeUfs':
        # Reuse the original storage imports already qualified and linked by NativeUfs.
        bindings = json.loads((root / 'build/native-ufs/bindings.json').read_text())['bindings']
        imports = subprocess.check_output(['xcrun', 'nm', '-u', '-j', obj], text=True).split()
        if any(name not in bindings for name in imports if 'IOBlockStorageDevice' in name):
            raise SystemExit('Root-binding helper has unqualified original storage imports')
        defined = subprocess.check_output(['xcrun', 'nm', '-U', '-j', obj], text=True).split()
        if args.component == 'NativeVirtioBlock' and ('_native_verified_block_root_authorized' not in defined or '_native_ufs_block_authenticates' not in imports):
            raise SystemExit('Root-binding helper lacks its mounted-device query or UFS callback')
        if args.component == 'NativeRamBlock' and '_native_ram_block_volume_seed' not in defined:
            raise SystemExit('RAM Data provider lacks its mounted-device query')
        (output / 'bindings.json').write_text(json.dumps({
            'compiled_object_sha256': hashlib.sha256(obj.read_bytes()).hexdigest(),
            'bindings': {name: bindings[name] for name in imports if 'IOBlockStorageDevice' in name},
        }, indent=2) + '\n')
    archive = output / ('lib' + args.component + '.a')
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', archive, obj], check=True)
    print(archive)


if __name__ == '__main__':
    main()
