#!/usr/bin/env python3
"""hummer: map the bootloader's framebuffer before the kernel's console writes to it.

Why this is needed
------------------
DarwinBoot fills ``boot_args.Video`` with the *physical* address of the panel
(0xFC000000 on hummer).  XNU copies that value into ``PE_state.video``
verbatim (pexpert/arm/pe_init.c) and the graphics console then writes pixels
straight through it.

Nothing maps it:

  * osfmk/arm64/start.s builds exactly two bootstrap mappings — the V=P window
    ``[physBase, physBase + memSize)`` and the matching kernel VA window — and
    ``arm_replace_identity_map()`` tears the V=P one down in arm_vm_init();
  * osfmk/arm64/arm_vm_init.c has no reference to ``PE_state.video`` at all;
  * on hummer the window sits at 0x8C0070000 (5 GiB), so 0xFC000000 is outside
    every mapping the kernel ever has.

The console therefore takes a data abort on its first pixel write, panics, and
the panic path faults again in the same place — which is why the screen simply
freezes on the last DARWINBOOT line with no output at all.

What this does
--------------
Hooks ``osfmk/arm/arm_init.c`` immediately before ``PE_create_console()``
(``PE_init_platform()`` has already run, so ``PE_state.video`` is populated):
maps the framebuffer into the kernel high window and re-maps it
write-combining, because a cached mapping never reaches the panel, then points
``PE_state.video.v_baseAddr`` at the kernel VA.

This runs as a ``[kernel] archives`` entry: kernel.py runs the archive builders
between the bootstrap compile pass and the final pass, so patching the source
and dropping the stale object here makes the final pass rebuild it.
"""
import sys
from pathlib import Path

MARKER = "hummer: map the framebuffer"
TARGET = Path(".cache/xnu-work/osfmk/arm/arm_init.c")
OBJECT_DIR = Path("build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT")
ANCHOR = "\tPE_create_console();\n"

INJECT = """\t/*
\t * hummer: map the framebuffer handed over in boot_args.Video.
\t *
\t * It arrives as a physical address and nothing maps it: the bootstrap
\t * tables only cover [physBase, physBase + memSize), which on this board
\t * starts at 0x8C0070000, while the panel is at 0xFC000000.  The console
\t * created below writes pixels through that address, so without this the
\t * first write faults, panic faults again in the same place, and the
\t * machine stops with nothing on screen.
\t *
\t * PE_init_platform() has already copied the value into PE_state.video, so
\t * it is available here, after arm_vm_init() has made the pmap usable.
\t */
\tif ((PE_state.video.v_display != 0) && (PE_state.video.v_baseAddr != 0)) {
\t\tvm_offset_t fb_pa = (vm_offset_t)PE_state.video.v_baseAddr;
\t\tvm_size_t   fb_len = (vm_size_t)PE_state.video.v_rowBytes * (vm_size_t)PE_state.video.v_height;

\t\tif (fb_len != 0) {
\t\t\tvm_map_address_t fb_va = pmap_map_high_window_bd(fb_pa, fb_len,
\t\t\t                                                VM_PROT_READ | VM_PROT_WRITE);

\t\t\t/* A cacheable mapping never reaches the panel: re-map write-combining. */
\t\t\tif ((fb_pa & (PAGE_SIZE - 1)) == 0) {
\t\t\t\t(void)pmap_map_bd_with_options(fb_va, fb_pa, fb_pa + fb_len,
\t\t\t\t                               VM_PROT_READ | VM_PROT_WRITE, PMAP_MAP_BD_WCOMB);
\t\t\t}

\t\t\tPE_state.video.v_baseAddr = (unsigned long)fb_va;
\t\t\tkprintf("hummer: framebuffer %#lx+%#lx mapped at %#lx\\n",
\t\t\t        (unsigned long)fb_pa, (unsigned long)fb_len, (unsigned long)fb_va);
\t\t}
\t}

"""


def fail(message):
    print(f"hummer-video-map: {message}", file=sys.stderr)
    raise SystemExit(1)


def drop_object():
    """Force the final pass to recompile arm_init.c."""
    dropped = []
    for name in ("arm_init.o", "arm_init.o.json"):
        path = OBJECT_DIR / name
        if path.exists():
            path.unlink()
            dropped.append(name)
    return dropped


def main():
    if not TARGET.is_file():
        fail(f"{TARGET} missing: the XNU work tree is not there yet")

    text = TARGET.read_text()
    if MARKER in text:
        print("hummer-video-map: arm_init.c already patched")
        print(f"hummer-video-map: dropped {len(drop_object())} stale object(s)")
        return

    if text.count(ANCHOR) != 1:
        fail(f"expected exactly one {ANCHOR.strip()!r} in arm_init.c, found {text.count(ANCHOR)}")

    TARGET.write_text(text.replace(ANCHOR, INJECT + ANCHOR))
    print("hummer-video-map: patched arm_init.c (framebuffer mapped before the console)")

    dropped = drop_object()
    if not dropped:
        # First pass of a cold build has not compiled it yet; nothing to invalidate.
        print("hummer-video-map: no stale object to drop (first pass)")
    else:
        print(f"hummer-video-map: dropped {', '.join(dropped)}")

    patched = TARGET.read_text()
    if MARKER not in patched:
        fail("patch did not stick")
    start = patched.index(INJECT)
    print("hummer-video-map: inserted %d bytes at offset %d" % (len(INJECT), start))


if __name__ == "__main__":
    main()
