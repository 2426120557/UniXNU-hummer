"""Build a hand-made arm64 Mach-O to sit at /sbin/launchd in the RAM root.

Why hand-made: this is a *probe*, not a program.  It has to be a valid
MH_EXECUTE with no dyld and no code signature, so that execve("/sbin/launchd")
at pid 1 either succeeds (our code runs in userland) or fails with a precise
errno that names the refusing layer:

    ENOENT (2)  file missing                      -> still the current panic
    ENOEXEC(8)  kernel rejected the Mach-O        -> load commands are wrong
    killed      AMFI refused an unsigned binary   -> need amfi_get_out_of_my_way

The code writes one line to fd 1 (the kernel console) and then spins, so the
proof stays on screen instead of vanishing behind a "pid 1 exited" panic.

Layout: one 16 KiB page.  __PAGEZERO + __TEXT(fileoff 0, code at +0x400) +
LC_UNIXTHREAD with ARM_THREAD_STATE64 (flavor 6, count 68, cmdsize 288).
"""

import struct
import sys
from pathlib import Path

PAGE = 0x4000
VMADDR = 0x100000000
CODE_OFF = 0x400

# Assembled from launchd-stub.S with clang, then byte-compared word for word.
# The values below are the *decoded* instruction words; pack them little-endian,
# which is also why this must not be written as a fromhex string (that would put
# the bytes in the wrong order — caught by the byte check, not by eyeballing).
#   0x00: d2800020  mov x0, #1      fd = 1
#   0x04: 100000a1  adr x1, msg
#   0x08: d28002e2  mov x2, #23     len
#   0x0c: d2800090  mov x16, #4     SYS_write
#   0x10: d4001001  svc #0x80
#   0x14: 14000000  b .             spin forever, never exit
#   0x18: "HUMMER: userland alive\n"
CODE_WORDS = (0xD2800020, 0x100000A1, 0xD28002E2, 0xD2800090, 0xD4001001, 0x14000000)
CODE = b"".join(struct.pack("<I", w) for w in CODE_WORDS)
MSG = b"HUMMER: userland alive\n"
TEXT = CODE + MSG
TEXT_SIZE = (len(TEXT) + 15) & ~15

LC_SEGMENT_64 = 0x19
LC_UNIXTHREAD = 0x5
MH_MAGIC_64 = 0xFEEDFACF
CPU_TYPE_ARM64 = 0x0100000C
MH_EXECUTE = 2
MH_NOUNDEFS = 0x1


def segment(name, vmaddr, vmsize, fileoff, filesize, maxprot, initprot, sections):
    cmd = struct.pack("<II16sQQQQIIII", LC_SEGMENT_64, 72 + 80 * len(sections),
                      name.encode(), vmaddr, vmsize, fileoff, filesize,
                      maxprot, initprot, len(sections), 0)
    for sect, addr, size, off, align, flags in sections:
        cmd += struct.pack("<16s16sQQIIIIIIII", sect.encode(), name.encode(),
                           addr, size, off, align, 0, 0, flags, 0, 0, 0)
    return cmd


def unixthread(pc):
    state = bytearray(272)                       # 34 x u64
    struct.pack_into("<Q", state, 32 * 8, pc)    # __pc
    struct.pack_into("<Q", state, 33 * 8, 0)     # __cpsr -> EL0t
    return struct.pack("<IIII", LC_UNIXTHREAD, 16 + len(state), 6, 68) + bytes(state)


def build():
    cmds = b""
    cmds += segment("__PAGEZERO", 0, VMADDR, 0, 0, 0, 0, [])
    cmds += segment("__TEXT", VMADDR, PAGE, 0, PAGE, 5, 5,
                    [("__text", VMADDR + CODE_OFF, TEXT_SIZE, CODE_OFF, 4, 0x80000400)])
    cmds += unixthread(VMADDR + CODE_OFF)

    size_of_cmds = len(cmds)
    header = struct.pack("<IiiIIIII", MH_MAGIC_64, CPU_TYPE_ARM64, 0, MH_EXECUTE,
                         3, 32 + size_of_cmds, MH_NOUNDEFS, 0)
    # 32 字节头 + 72(PAGEZERO) + 152(__TEXT) + 288(UNIXTHREAD) = 544，所以代码放 0x400
    if 32 + size_of_cmds > CODE_OFF:
        raise SystemExit("load commands overrun the code offset")

    out = bytearray(PAGE)
    out[0:32] = header
    out[32:32 + size_of_cmds] = cmds
    out[CODE_OFF:CODE_OFF + len(TEXT)] = TEXT
    return bytes(out)


def main():
    dst = Path(sys.argv[1] if len(sys.argv) > 1 else "launchd-stub")
    blob = build()
    dst.write_bytes(blob)
    print("%s: %d bytes, ncmds=3, sizeofcmds=%d, text at +0x%x (%d bytes), msg at +0x%x"
          % (dst, len(blob), 32 + 152 + 288, CODE_OFF, TEXT_SIZE, CODE_OFF + len(CODE)))


if __name__ == "__main__":
    main()
