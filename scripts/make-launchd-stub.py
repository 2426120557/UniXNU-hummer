"""Build a hand-made arm64 Mach-O to sit at /sbin/launchd in the RAM root.

This is a *probe*, not a program.  It has to be a valid MH_EXECUTE with no
dyld and no code signature, so that execve("/sbin/launchd") at pid 1 either
succeeds (our code runs in EL0) or fails with a precise errno naming the
refusing layer.

Why it ends in exit(42) instead of spinning: the kernel hands pid 1 **no file
descriptors at all** (bsd_init never opens /dev/console, and devfs has no
mount point on our ramdisk because we never created a /dev directory).  So
every write() below may legitimately fail with EBADF, and a silent spin would
be impossible to tell apart from a hang.  exit(42) is loud instead:

    kern_exit.c, exit1():
        if (p == initproc) prefix_str = "initproc failed to start";
        panic("... %s -- no exit reason available -- (signal 0, exit status 42)")

Two possible panics, both unambiguous:
    "Process 1 exec of /sbin/launchd failed, errno N"   -> exec itself failed
    "initproc failed to start ... exit status 42"       -> userland really ran

The banner attempts (write to 1, write to 2, open("/dev/console") + write) are
best-effort decoration; whichever channel happens to exist prints it.

Load commands: __PAGEZERO (4 GiB) + __TEXT + LC_UNIXTHREAD with
ARM_THREAD_STATE64 (flavor 6, count 68, cmdsize 288), pc = base + 0x400 and
sp = 0.  sp = 0 is deliberate: osfmk/arm64/status.c thread_userstack() sees a
zero sp, falls back to USRSTACK64 and the kernel allocates the stack itself,
exactly like a real binary.

The code bytes are not hand-encoded: they are the .text section that
`clang --target=aarch64-linux-gnu -c launchd-stub.S` produced, copied
verbatim.  adr displacements are relative, so lifting the whole section
keeps them valid.
"""

import struct
import sys
from pathlib import Path

PAGE = 0x4000
VMADDR = 0x100000000
CODE_OFF = 0x400

# objcopy --only-section=.text launchd-stub.o, 140 bytes:
#   mov x0,#1 ; adr x1,msg ; mov x2,#23 ; mov x16,#4 ; svc #0x80     write(1)
#   mov x0,#2 ; adr x1,msg ; mov x2,#23 ; mov x16,#4 ; svc #0x80     write(2)
#   adr x0,path ; mov x1,#1 ; mov x2,#0 ; mov x16,#5 ; svc #0x80     open()
#   tbnz w0,#31,exit ; mov x19,x0 ; mov x0,x19 ; adr x1,msg ; ...    write(fd)
#   mov x0,#42 ; mov x16,#1 ; svc #0x80                             exit(42)
#   b .                                                             park
#   "HUMMER: userland alive\n"  "/dev/console\0"
CODE_HEX = (
    "200080D221030010E20280D2900080D2011000D4"
    "400080D281020010E20280D2900080D2011000D4"
    "A0020070210080D2020080D2B00080D2011000D4"
    "E000F837F30300AAE00313AA01010010E20280D2"
    "900080D2011000D4400580D2300080D2011000D4"
    "00000014"
    "48554D4D45523A20757365726C616E6420616C6976650A"
    "2F6465762F636F6E736F6C6500"
)
CODE = bytes.fromhex(CODE_HEX)
TEXT_SIZE = (len(CODE) + 15) & ~15

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
                         3, size_of_cmds, MH_NOUNDEFS, 0)
    # header 32 + PAGEZERO 72 + TEXT 152 + UNIXTHREAD 288 = 544 -> code at 0x400
    if 32 + size_of_cmds > CODE_OFF:
        raise SystemExit("load commands overrun the code offset")

    out = bytearray(PAGE)
    out[0:32] = header
    out[32:32 + size_of_cmds] = cmds
    out[CODE_OFF:CODE_OFF + len(CODE)] = CODE
    return bytes(out), size_of_cmds


def main():
    dst = Path(sys.argv[1] if len(sys.argv) > 1 else "launchd-stub")
    blob, size_of_cmds = build()
    dst.write_bytes(blob)
    print("%s: %d bytes, ncmds=3, sizeofcmds=%d, code at +0x%x (%d bytes), "
          "machine code %d bytes"
          % (dst, len(blob), size_of_cmds, CODE_OFF, TEXT_SIZE, len(CODE)))


if __name__ == "__main__":
    main()
