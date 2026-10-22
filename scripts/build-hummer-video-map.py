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

INJECT = """	/*
	 * hummer: map the framebuffer handed over in boot_args.Video.
	 *
	 * v_baseAddr arrives as a *physical* address and nothing maps it: the
	 * bootstrap tables only cover [physBase, physBase + memSize), which on this
	 * board starts at 0x8C0070000, while the panel sits at 0xFC000000.  The
	 * console created below draws pixels straight through that address, so
	 * without this the first write faults, the panic path faults again in the
	 * same place, and the machine stops with nothing on screen.
	 *
	 * PE_init_platform() has already copied boot_args.Video into
	 * PE_state.video and arm_vm_init() has already built the kernel pmap, so
	 * both are usable here.  kprintf() is safe this early: printf.c routes it
	 * to a no-op until PE_init_kprintf() runs.
	 */
	if ((PE_state.video.v_display != 0) && (PE_state.video.v_baseAddr != 0)) {
		vm_size_t fb_len = (vm_size_t)PE_state.video.v_rowBytes *
		    (vm_size_t)PE_state.video.v_height;

		if (fb_len < (vm_size_t)PE_state.video.v_length) {
			fb_len = (vm_size_t)PE_state.video.v_length;
		}

		if (fb_len != 0) {
			vm_offset_t fb_pa = (vm_offset_t)(PE_state.video.v_baseAddr +
			                                  PE_state.video.v_offset);
			/* pmap_map_high_window_bd() aligns the address down by itself. */
			vm_offset_t fb_page = fb_pa & ~(vm_offset_t)0xfff;
			vm_size_t   fb_span = (vm_size_t)(fb_pa - fb_page) + fb_len;
			vm_map_address_t fb_va = pmap_map_high_window_bd(fb_page, fb_span,
			                                                VM_PROT_READ | VM_PROT_WRITE);

			/*
			 * The high window is mapped cacheable, and a cacheable mapping
			 * never reaches the panel: re-map it write-combining.
			 */
			(void)pmap_map_bd_with_options(fb_va, fb_page, fb_page + fb_span,
			                               VM_PROT_READ | VM_PROT_WRITE,
			                               PMAP_MAP_BD_WCOMB);

			PE_state.video.v_baseAddr = (unsigned long)(fb_va + (fb_pa - fb_page));
			PE_state.video.v_offset = 0;

			kprintf("hummer: framebuffer %#lx+%#lx mapped at %#lx\\n",
			        (unsigned long)fb_pa, (unsigned long)fb_len,
			        (unsigned long)PE_state.video.v_baseAddr);
		}
	}

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
