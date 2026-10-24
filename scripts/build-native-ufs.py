#!/usr/bin/env python3
"""Build read-only UFS and its existing root-binding helper with the kernel ABI."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import struct
import subprocess


# hummer：一次链路超时不能把整个 root 打死。
# 传输超时 + doorbell 已释放 + 无 fatal 状态 => 命令从未被控制器消费，
# 可以唤醒链路（清 AHIT8 + Hibern8 exit）后重发同一个 UTRD，最多 2 次。
# 写（direction == 1）不重试，保持原保守策略。
ROOT_COMMANDS_ANCHOR = "    // No cancel/reset/retry: the controller may still own the buffer, even\n"
ROOT_COMMANDS_RETRY = '''    // hummer: bounded recovery (injected by build-native-ufs.py). One stale or
    // hibernated link must not poison the whole root: if the doorbell was released
    // and no fatal status is latched, the command was never consumed, so wake the
    // link and re-ring the same UTRD a bounded number of times. Writes never retry.
    for (unsigned recover = 0; direction != 1 && recover < 2; ++recover) {
      if (read32 (gHost.hc, 0x58) != 0) break;
      if (read32 (gHost.hc, 0x20) & kUfsFatalStatus) break;
      hostWrite (0x18, 0);
#if defined(NATIVE_UFS_SM8750) && NATIVE_UFS_SM8750
      {
        const char *wakeWhy = nullptr;
        if (!hostWakeLink (&wakeWhy)) break;
      }
#endif
      hostWrite (0x20, 1);
      cacheCivac (gHost.virt, kDataOffset + dataBytes);
      started = mach_absolute_time ();
      clock_interval_to_deadline (timeoutUs, kMicrosecondScale, &port.deadline);
      hostWrite (0x58, 1);
      state = UfsTransferPoll ();
      end = pollUfsTransfer (port, state);
      if (end == UfsTransferEnd::Completed) break;
    }
    if (end == UfsTransferEnd::Completed) {
      *ocs = state.ocs;
      gRootDiagnostic.elapsedTicks = mach_absolute_time () - started;
      gRootDiagnostic.polls = state.polls;
      gRootDiagnostic.dbSeen = state.doorbellSeen;
      gRootDiagnostic.isSeen = state.interruptSeen;
      cacheInvalidate (gHost.virt, kDataOffset + dataBytes);
      gRootDiagnostic.response = loadBe32 (gHost.virt + kUcdOffset + kRspOffset);
      return recordTransport (*ocs == 0 ? kIOReturnSuccess : kIOReturnIOError, *ocs);
    }
    // No cancel/reset/retry: the controller may still own the buffer, even
'''


def inject_read_retry(root):
    path = root / 'drivers' / 'NativeUfs' / 'RootCommands.h'
    text = path.read_text()
    if 'hummer: bounded recovery' in text:
        return
    if ROOT_COMMANDS_ANCHOR not in text:
        raise SystemExit('build-native-ufs: read-retry anchor not found in RootCommands.h')
    path.write_text(text.replace(ROOT_COMMANDS_ANCHOR, ROOT_COMMANDS_RETRY, 1))
    print('NativeUfs: injected bounded read retry')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root-windows', action='store_true')
    parser.add_argument('--component', choices=('NativeUfs', 'NativeVirtioBlock', 'NativeRamBlock'), default='NativeUfs')
    args = parser.parse_args()
    if args.component != 'NativeUfs' and not args.root_windows:
        parser.error('The root-binding helper requires --root-windows')
    root = Path(__file__).resolve().parents[1]
    inject_read_retry(root)
    compile_command = runpy.run_path(str(root / 'scripts/build-arm-platform.py'))['compile_command']
    recipe = root / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    record = json.loads(recipe.read_text().strip().rstrip(','))
    output = root / {'NativeUfs': 'build/native-ufs', 'NativeVirtioBlock': 'build/native-virtio-block',
                     'NativeRamBlock': 'build/native-ram-block'}[args.component]
    output.mkdir(parents=True, exist_ok=True)
    obj = output / (args.component + '.o')
    command = compile_command(record, root / 'drivers' / args.component / (args.component + '.cpp'), obj)
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
