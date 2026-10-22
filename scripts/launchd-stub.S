// Probe binary that will live at /sbin/launchd on the RAM root.
//
// Why exit() and not an infinite spin: the kernel gives pid 1 no file
// descriptors at all (bsd_init never opens /dev/console, and devfs has no
// mount point on our ramdisk), so every write() here may fail with EBADF.
// A silent spin would be indistinguishable from a hang.  Instead we end with
// exit(42), and kern_exit.c turns that into a guaranteed, loud panic:
//
//   panic: initproc failed to start -- no exit reason available --
//          (signal 0, exit status 42)
//
// Two outcomes, both unmistakable:
//   "Process 1 exec of /sbin/launchd failed, errno N"  -> exec itself failed
//   "initproc failed to start ... exit status 42"      -> userland really ran
//
// The banner attempts are best-effort: write(1), write(2), then
// open("/dev/console") + write, whichever channel happens to exist.

    .text
    .globl _start
_start:
// ---- 1) write(1, msg, 23) -------------------------------------------------
    mov  x0, #1
    adr  x1, msg
    mov  x2, #23
    mov  x16, #4
    svc  #0x80

// ---- 2) write(2, msg, 23) -------------------------------------------------
    mov  x0, #2
    adr  x1, msg
    mov  x2, #23
    mov  x16, #4
    svc  #0x80

// ---- 3) fd = open("/dev/console", O_WRONLY, 0); if (fd >= 0) write(fd, ...)
    adr  x0, path
    mov  x1, #1
    mov  x2, #0
    mov  x16, #5
    svc  #0x80
    tbnz w0, #31, 1f
    mov  x19, x0
    mov  x0, x19
    adr  x1, msg
    mov  x2, #23
    mov  x16, #4
    svc  #0x80

1:
// ---- 4) exit(42): the kernel panics, which is our proof -------------------
    mov  x0, #42
    mov  x16, #1
    svc  #0x80

// exit() must not return; park here if it ever does.
2:
    b    2b

msg:
    .ascii "HUMMER: userland alive\n"
path:
    .asciz "/dev/console"
