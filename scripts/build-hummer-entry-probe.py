#!/usr/bin/env python3
"""hummer: paint a band from _start, while the MMU is still off.

Why this exists
---------------
The v15 build painted two markers from arm_init.c, after PE_init_platform() and
after arm_vm_init() had set everything up.  A boot that shows nothing at all
then has three possible explanations and no way to tell them apart:

  * the kernel entry was never reached,
  * it was reached but died between start.s and arm_init.c,
  * it reached arm_init.c and died inside the framebuffer mapping.

Every one of those looks like the same frozen screen.

This probe answers the first two.  ``_start`` is a single ``b
start_first_cpu``; ``start_first_cpu`` runs with the MMU *off* (DarwinBoot
clears SCTLR_EL1.M before it branches here), so a store to the panel's physical
address needs no page tables at all — and it works even if everything the kernel
does afterwards is wrong.

Where it goes
-------------
Right after the ``msr DAIFSet`` that kills interrupts and before ``mov x20, x0``
consumes the boot args pointer, so ``x0`` still holds boot_args.  Video is at
offset 0x28 of the Darwin 23 pexpert/pexpert/arm64/boot.h ABI:

    0x28  v_baseAddr   (physical, 0xFC000000 on this board)
    0x38  v_rowBytes
    0x48  v_height

Register use is confined to x9-x16, none of which start_first_cpu reads before
writing, and none of which are live across the probe.

How to read the screen
----------------------
DarwinBoot paints a green bar before it turns the MMU off, so:

  green bar only                        the kernel entry was never executed
  green bar + magenta band (y 64..96)   start_first_cpu ran
  ...plus magenta square at (0,0)       arm_init.c ran too
  ...plus blue band along the bottom    the framebuffer mapping works

The band sits at y=64..96 precisely so it does not overlap the 64x64 square
that build-hummer-video-map.py paints at the origin.
"""
import sys
from pathlib import Path

MARKER = "hummer: entry probe"
TARGET = Path(".cache/xnu-work/osfmk/arm64/start.s")
OBJECT_DIR = Path("build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT")
OBJECT_ROOT = Path("build/xnu/ios-obj")
ANCHOR = "\tmov\t\tx20, x0\n\tmov\t\tx21, #0\n"

INJECT = """	/*
	 * hummer: entry probe.  MMU is off here, so this is a straight physical
	 * store into the panel DarwinBoot told us about in boot_args.Video.
	 * Repaints y=64..96 magenta; arm_init.c later paints (0,0) and the
	 * bottom band, so the screen alone says how far the kernel got.
	 */
	cbz	x0, Lhummer_probe_done
	ldr	x9, [x0, #0x28]			// Video.v_baseAddr
	cbz	x9, Lhummer_probe_done
	ldr	x10, [x0, #0x38]		// Video.v_rowBytes
	ldr	x11, [x0, #0x48]		// Video.v_height
	cbz	x10, Lhummer_probe_done
	lsr	x10, x10, #2			// stride in 32-bit pixels
	cbz	x10, Lhummer_probe_done
	cmp	x11, #96
	b.lo	Lhummer_probe_done
	movz	w16, #0xFFFF, lsl #16
	movk	w16, #0x00FF			// magenta 0xFFFF00FF
	mov	x12, #64			// y
Lhummer_probe_row:
	madd	x13, x10, x12, x9
	mov	x14, #0				// x
	mov	x15, #256
	cmp	x10, x15
	csel	x15, x10, x15, lo		// min(256, stride)
Lhummer_probe_col:
	cmp	x14, x15
	b.hs	Lhummer_probe_next
	str	w16, [x13, x14, lsl #2]
	add	x14, x14, #1
	b	Lhummer_probe_col
Lhummer_probe_next:
	add	x12, x12, #1
	cmp	x12, #96
	b.lo	Lhummer_probe_row
Lhummer_probe_done:
	dsb	sy

"""


def fail(message):
    print(f"hummer-entry-probe: {message}", file=sys.stderr)
    raise SystemExit(1)


def drop_objects():
    """Force the final pass to reassemble start.s.

    The sibling arm_init.o sits in OBJECT_DIR, so try there first and fall back
    to a tree-wide hunt for anything that looks like start.s's object.  Deleting
    an extra object only costs a recompile; failing to delete the right one
    leaves the probe out of the linked kernel, which is much worse.
    """
    dropped = []

    def drop(path):
        if path.is_file():
            path.unlink()
            dropped.append(str(path))

    for name in ("start.o", "start.o.json"):
        drop(OBJECT_DIR / name)

    if not dropped and OBJECT_ROOT.is_dir():
        for path in OBJECT_ROOT.rglob("*"):
            if path.is_file() and path.name in ("start.o", "start.o.json") \
                    and "osfmk" in str(path):
                drop(path)

    if OBJECT_DIR.is_dir():
        neighbours = sorted(p.name for p in OBJECT_DIR.glob("start*"))
        print("hummer-entry-probe: start* in %s -> %s" % (OBJECT_DIR, neighbours))

    return dropped


def main():
    if not TARGET.is_file():
        fail(f"{TARGET} missing: the XNU work tree is not there yet")

    text = TARGET.read_text()
    if MARKER in text:
        print("hummer-entry-probe: start.s already patched")
        print(f"hummer-entry-probe: dropped {len(drop_objects())} stale object(s)")
        return

    if text.count(ANCHOR) != 1:
        fail(f"expected exactly one start_first_cpu prologue, found {text.count(ANCHOR)}")

    TARGET.write_text(text.replace(ANCHOR, INJECT + ANCHOR))
    print("hummer-entry-probe: patched start.s")

    dropped = drop_objects()
    if not dropped:
        print("hummer-entry-probe: no stale object to drop (first pass)")
    else:
        print("hummer-entry-probe: dropped " + ", ".join(dropped))

    patched = TARGET.read_text()
    if MARKER not in patched:
        fail("patch did not stick")
    print("hummer-entry-probe: inserted %d bytes at offset %d" % (len(INJECT), patched.index(INJECT)))


if __name__ == "__main__":
    main()
