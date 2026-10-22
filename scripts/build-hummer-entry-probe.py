#!/usr/bin/env python3
"""hummer: two bands on the way to the MMU switch.

Why this exists
---------------
v16 painted a band from the top of ``start_first_cpu``, while the MMU was off,
and it appeared: the kernel entry runs and the branch target is right.  Nothing
after that is visible, because from the moment SCTLR_EL1 turns the MMU on the
panel at 0xFC000000 has no mapping at all -- the kernel cannot draw until
arm_vm_init() has built a pmap for it.

So the only free place to report from is *before* the switch.  This probe
paints two bands there:

  band 1 (magenta, y 64..96)     top of start_first_cpu, MMU off.
  band 2 (cyan,   y 1200..1232)  top of common_start, i.e. after
                                 create_bootstrap_mapping() built both the V=P
                                 and the KVA table and after the stacks and
                                 arm_init's KVA were computed.

Anything past the switch is reported out of band by
outputs/patch-reset-probe.py, which replaces the ``bl arm_vm_init`` in
_arm_init with a cave that performs the call and then resets the SoC through
PSCI after six seconds.  A reboot at six seconds means arm_vm_init() returned;
no reboot means the kernel never got back out of it.

Register use
------------
x9-x16 only.  At the entry probe x0 still holds boot_args; at common_start
x20 does.  None of x9-x16 are live across either point.
"""
import sys
from pathlib import Path

MARKER = "hummer: stage probe"

TARGET = Path(".cache/xnu-work/osfmk/arm64/start.s")
OBJECT_DIR = Path("build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT")
OBJECT_ROOT = Path("build/xnu/ios-obj")

MACRO_ANCHOR = ".macro MSR_VBAR_EL1_X0\n"
ENTRY_ANCHOR = "\tmov\t\tx20, x0\n\tmov\t\tx21, #0\n"
COMMON_ANCHOR = "common_start:\n\n#if HAS_NEX_PG\n"

MACRO = """\t/*
\t * hummer: paint a band using whatever boot_args says right now.
\t * x20 must hold the physical boot_args pointer.  x20 and x21 are left
\t * alone; x9-x16 are scratch and are not live across either call site.
\t */
.macro hummer_paint hi, lo, y0, y1
\tcbz\tx20, 9f
\tldr\tx9, [x20, #0x28]\t\t// Video.v_baseAddr
\tcbz\tx9, 9f
\tldr\tx10, [x20, #0x38]\t\t// Video.v_rowBytes
\tldr\tx11, [x20, #0x48]\t\t// Video.v_height
\tcbz\tx10, 9f
\tlsr\tx10, x10, #2\t\t\t// stride in 32-bit pixels
\tcbz\tx10, 9f
\tcmp\tx11, #\\y1
\tb.lo\t9f
\tmovz\tw16, #\\hi, lsl #16
\tmovk\tw16, #\\lo
\tmov\tx12, #\\y0
8:
\tmadd\tx13, x10, x12, x9
\tmov\tx14, #0
\tmov\tx15, #256
\tcmp\tx10, x15
\tcsel\tx15, x10, x15, lo\t\t// min(256, stride)
7:
\tcmp\tx14, x15
\tb.hs\t6f
\tstr\tw16, [x13, x14, lsl #2]
\tadd\tx14, x14, #1
\tb\t7b
6:
\tadd\tx12, x12, #1
\tcmp\tx12, #\\y1
\tb.lo\t8b
\tdsb\tsy
9:
.endm

"""

ENTRY_INJECT = """\t/*
\t * hummer: stage probe 1.  MMU off, so this is a straight physical store
\t * into the panel DarwinBoot described in boot_args.Video.
\t */
\thummer_paint 0xFFFF, 0x00FF, 64, 96

"""

COMMON_INJECT = """\t/*
\t * hummer: stage probe 2.  Reaching common_start with the MMU still off
\t * means the bootstrap page tables, both stacks and arm_init's KVA all
\t * worked -- create_bootstrap_mapping() is behind us.
\t */
\thummer_paint 0xFF00, 0xFFFF, 1200, 1232

"""


def fail(message):
    print(f"hummer-stage-probe: {message}", file=sys.stderr)
    raise SystemExit(1)


def drop_objects():
    """Force the final pass to reassemble start.s."""
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
        print("hummer-stage-probe: start* in %s -> %s" % (OBJECT_DIR, neighbours))

    return dropped


def main():
    if not TARGET.is_file():
        fail(f"{TARGET} missing: the XNU work tree is not there yet")

    text = TARGET.read_text()
    if MARKER in text:
        print("hummer-stage-probe: start.s already patched")
        print(f"hummer-stage-probe: dropped {len(drop_objects())} stale object(s)")
        return

    for anchor, name in ((MACRO_ANCHOR, "macro"), (ENTRY_ANCHOR, "entry probe"),
                         (COMMON_ANCHOR, "common_start")):
        if text.count(anchor) != 1:
            fail(f"{name}: expected exactly one anchor, found {text.count(anchor)}")

    text = text.replace(MACRO_ANCHOR, MACRO + MACRO_ANCHOR, 1)
    text = text.replace(ENTRY_ANCHOR, ENTRY_INJECT + ENTRY_ANCHOR, 1)
    text = text.replace(COMMON_ANCHOR,
                        "common_start:\n" + COMMON_INJECT + "\n#if HAS_NEX_PG\n", 1)

    TARGET.write_text(text)
    print("hummer-stage-probe: patched start.s")

    dropped = drop_objects()
    if not dropped:
        print("hummer-stage-probe: no stale object to drop (first pass)")
    else:
        print("hummer-stage-probe: dropped " + ", ".join(dropped))

    patched = TARGET.read_text()
    if MARKER not in patched:
        fail("patch did not stick")
    for needle in ("hummer_paint 0xFFFF, 0x00FF, 64, 96",
                   "hummer_paint 0xFF00, 0xFFFF, 1200, 1232"):
        if needle not in patched:
            fail(f"missing generated code: {needle}")
    print("hummer-stage-probe: both bands and the paint macro are in place")


if __name__ == "__main__":
    main()
