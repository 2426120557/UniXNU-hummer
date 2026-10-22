#!/usr/bin/env python3
"""hummer: a real console for the MMU-on half of arm_init().

Why
---
v18 proved `arm_vm_init()` returns (the PSCI probe after its `bl` fired). v19
moved the probe to `_PE_create_console` and nothing happened: the kernel dies
*somewhere* between the return of arm_vm_init and the console, and a one-bit
time signal costs a full flash cycle per question.

`xyyyds0106/ios-vermeer-lab` solves this on a Redmi Note 10 Pro (see
xnu/patches/0010 and 0012 in that repo): give the panel a Normal-NC block alias
in the page tables that *survive* the bootstrap TTBR0 teardown, then draw text
straight into it from C.  That is the "serial console" for boards with no
usable UART -- exactly hummer's situation.

How
---
1. `arm_vm_init()`, right after `set_mmu_ttb_alternate()`, installs a 2 MB
   (16K-page) block for the panel at `VM_MIN_KERNEL_ADDRESS + ARM_TT_L2_SIZE`:
   above the low globals page, below `gVirtBase`, and never handed out by the
   VM.  Two blocks if the next slot is free too, so there is room for ~25 text
   lines.  `phystokv()` still refers to the bootstrap aperture at that point,
   which is how the L2 table is reached.
2. The same file grows the renderer: 8x16 `iso_font` glyphs at 2x, straight
   stores into the aliased VA.  No locks, no allocation, no per-CPU state.
3. `hummer_early_mark()` is sprinkled through `arm_vm_init()` and `arm_init()`
   so every stage prints one line: "HUM 15: arm_vm_init returned" etc.
4. `pe_serial.c` registers the same renderer as a serial device, so once
   kprintf comes up its output lands on the panel as well.

Nothing is added to `osfmk/conf/files.arm64`: the object lists for the final
link pass are already frozen by the time these archive builders run, so the
renderer lives inside `arm_vm_init.c`, which is already linked.  Its header is
new, which is free.
"""
import re
import sys
from pathlib import Path

MARKER = "hummer: panel console"

XNU = Path(".cache/xnu-work")
ARM_INIT = XNU / "osfmk/arm/arm_init.c"
ARM_VM_INIT = XNU / "osfmk/arm64/arm_vm_init.c"
PE_SERIAL = XNU / "pexpert/arm/pe_serial.c"
PROTOS = XNU / "pexpert/pexpert/arm/protos.h"
HUMMER_H = XNU / "osfmk/arm64/hummer_early.h"

OBJECT_ROOT = Path("build/xnu/ios-obj")
STALE_OBJECTS = ("arm_init.o", "arm_init.o.json",
                 "arm_vm_init.o", "arm_vm_init.o.json",
                 "pe_serial.o", "pe_serial.o.json")

HEADER = r'''/*
 * hummer: framebuffer console for the part of arm_init() that runs with the
 * bootstrap translation tables gone.
 *
 * The alias and the renderer both live in osfmk/arm64/arm_vm_init.c (that
 * object is already in the link list); this header is only the contract other
 * files compile against.
 */
#ifndef _ARM64_HUMMER_EARLY_H_
#define _ARM64_HUMMER_EARLY_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called from arm_vm_init(): hand over the alias VA and the panel geometry. */
void hummer_early_attach(uintptr_t va, uint32_t width, uint32_t height,
    uint32_t stride_bytes, uint32_t window_bytes);

/* Draw one character, '\n' and '\r' behave as usual. */
void hummer_early_putc(char c);

/* Print "HUM <stage>: <label>\n". Reaches the panel only once attached. */
void hummer_early_mark(unsigned int stage, const char *label);

/* True once hummer_early_attach() succeeded. */
int hummer_early_console_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* _ARM64_HUMMER_EARLY_H_ */
'''

# --------------------------------------------------------------------------
# arm_vm_init.c: the renderer, the alias, and the stage marks
# --------------------------------------------------------------------------
VM_INIT_RENDERER = r'''/* ---- hummer: panel console ("serial" for a board with no UART) ---------- */
/*
 * Written into the framebuffer VA that hummer_fb_alias() installs in the page
 * tables that outlive the bootstrap TTBR0.  Deliberately primitive: no locks,
 * no allocation, no per-CPU state, no ADT parsing, so it stays usable from the
 * very beginning of arm_vm_init() onwards and from panic paths later.
 */
#define HUMMER_SCALE        2u
#define HUMMER_CELL_W       (8u * HUMMER_SCALE)
#define HUMMER_CELL_H       (16u * HUMMER_SCALE)
#define HUMMER_FG           0xffffffffu
#define HUMMER_BG           0xff000000u
#define HUMMER_FB_BLOCKS    2u

extern unsigned char iso_font[256 * 16];

static volatile uint32_t *hummer_fb;
static uint32_t hummer_stride;
static uint32_t hummer_width;
static uint32_t hummer_cols;
static uint32_t hummer_rows;
static uint32_t hummer_col;
static uint32_t hummer_row;
static int hummer_ready;

int
hummer_early_console_ready(void)
{
	return hummer_ready;
}

void
hummer_early_attach(uintptr_t va, uint32_t width, uint32_t height,
    uint32_t stride_bytes, uint32_t window_bytes)
{
	uint32_t usable = height;
	uint32_t rows, cols;

	if (va == 0 || width == 0 || height == 0 || stride_bytes < width * 4) {
		return;
	}
	if (window_bytes / stride_bytes < usable) {
		usable = window_bytes / stride_bytes;
	}
	rows = usable / HUMMER_CELL_H;
	cols = width / HUMMER_CELL_W;
	if (rows == 0 || cols == 0) {
		return;
	}
	hummer_fb = (volatile uint32_t *)va;
	hummer_stride = stride_bytes / 4;
	hummer_width = width;
	hummer_rows = rows;
	hummer_cols = cols;
	hummer_col = 0;
	hummer_row = 0;
	hummer_ready = 1;
}

void
hummer_early_putc(char c)
{
	const unsigned char *glyph;
	uint32_t gx, gy, sx, sy;

	if (!hummer_ready) {
		return;
	}
	if (c == '\r') {
		hummer_col = 0;
		return;
	}
	if (c == '\n') {
		hummer_col = 0;
		if (++hummer_row >= hummer_rows) {
			hummer_row = 0;
		}
		return;
	}
	if (hummer_col >= hummer_cols) {
		hummer_col = 0;
		if (++hummer_row >= hummer_rows) {
			hummer_row = 0;
		}
	}
	glyph = iso_font + (unsigned char)c * 16;
	for (gy = 0; gy < 16; gy++) {
		for (sy = 0; sy < HUMMER_SCALE; sy++) {
			volatile uint32_t *line = hummer_fb +
			    (hummer_row * HUMMER_CELL_H + gy * HUMMER_SCALE + sy) * hummer_stride +
			    hummer_col * HUMMER_CELL_W;
			for (gx = 0; gx < 8; gx++) {
				uint32_t px = (glyph[gy] & (1u << gx)) ? HUMMER_FG : HUMMER_BG;
				for (sx = 0; sx < HUMMER_SCALE; sx++) {
					line[gx * HUMMER_SCALE + sx] = px;
				}
			}
		}
	}
	hummer_col++;
	__asm__ volatile("dsb sy" ::: "memory");
}

void
hummer_early_mark(unsigned int stage, const char *label)
{
	char digits[4];
	unsigned int n = 0;
	unsigned int i;

	if (!hummer_ready) {
		return;
	}
	hummer_early_putc('H');
	hummer_early_putc('U');
	hummer_early_putc('M');
	hummer_early_putc(' ');
	if (stage >= 100) {
		digits[n++] = (char)('0' + stage / 100);
	}
	if (stage >= 10) {
		digits[n++] = (char)('0' + (stage / 10) % 10);
	}
	digits[n++] = (char)('0' + stage % 10);
	for (i = 0; i < n; i++) {
		hummer_early_putc(digits[i]);
	}
	hummer_early_putc(':');
	hummer_early_putc(' ');
	while (*label != '\0') {
		hummer_early_putc(*label++);
	}
	hummer_early_putc('\n');
}

/*
 * Try to park the panel window at va_start.  The L1 entry has to already be a
 * table -- both candidates sit in a 1 GB span whose table exists -- and the L2
 * slots have to be empty, so this can never displace a real mapping.  Returns
 * how many 2 MB blocks were installed.
 */
static unsigned int
hummer_fb_map_at(vm_map_address_t va_start, uint64_t fb, unsigned int want,
    vm_map_address_t *first_out)
{
	vm_map_address_t va_l1 = va_start & ~ARM_TT_L1_OFFMASK;
	tt_entry_t l1, *l2;
	unsigned int i;
	unsigned int blocks = 0;

	l1 = cpu_tte[(va_start & ARM_TT_L1_INDEX_MASK) >> ARM_TT_L1_SHIFT];
	if ((l1 & (ARM_TTE_VALID | ARM_TTE_TYPE_MASK)) !=
	    (ARM_TTE_VALID | ARM_TTE_TYPE_TABLE)) {
		return 0;
	}
	l2 = (tt_entry_t *)phystokv(l1 & ARM_TTE_TABLE_MASK);

	for (i = 0; i < 32 && blocks < want; i++) {
		vm_map_address_t va = va_start + i * ARM_TT_L2_SIZE;
		tt_entry_t *slot;

		if ((va & ~ARM_TT_L1_OFFMASK) != va_l1) {
			break;  /* only this L1 table has been resolved */
		}
		if (va + ARM_TT_L2_SIZE > gVirtBase) {
			break;
		}
		slot = &l2[(va & ARM_TT_L2_INDEX_MASK) >> ARM_TT_L2_SHIFT];
		if (*slot != ARM_TTE_EMPTY) {
			if (blocks != 0) {
				break;
			}
			continue;
		}
		if (blocks == 0) {
			*first_out = va;
		}
		*slot = ((fb + blocks * ARM_TT_L2_SIZE) & ARM_TTE_BLOCK_L2_MASK)
		    | ARM_TTE_TYPE_BLOCK | ARM_TTE_VALID
		    | ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_NX
		    | ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY)
		    | ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITECOMB)
		    | ARM_TTE_BLOCK_PNX;
		blocks++;
	}
	return blocks;
}

/*
 * Map the panel into the kernel page tables that survive the bootstrap TTBR0
 * teardown, as Normal non-cacheable memory (write-combining: the panel engine
 * sees the pixels without any cache maintenance).
 *
 * Two candidate windows, both in kernel VA that the VM never hands out:
 *   1. VM_MIN_KERNEL_ADDRESS + 2 MB -- the window UniXNU-vermeer uses, right
 *      above the low globals;
 *   2. the 4 MB just below the kernel image, which is guaranteed to share the
 *      kernel's own L1 table whatever VM_MIN_KERNEL_ADDRESS happens to be for
 *      this board config.
 *
 * Failing to place the window must never take the boot down: the kernel just
 * keeps running without a console.
 */
static void
hummer_fb_alias(boot_args *args)
{
	vm_map_address_t candidates[2];
	vm_map_address_t first = 0;
	uint64_t fb = args->Video.v_baseAddr;
	uint64_t stride = args->Video.v_rowBytes;
	uint64_t width = args->Video.v_width;
	uint64_t height = args->Video.v_height;
	unsigned int i;
	unsigned int blocks = 0;

	if (fb == 0 || stride == 0 || width == 0 || height == 0) {
		return;
	}
	/* a 2 MB block can only describe a 2 MB aligned physical window */
	if ((fb & (ARM_TT_L2_SIZE - 1)) != 0) {
		return;
	}
	if (stride < (width * 4)) {
		return;
	}

	candidates[0] = VM_MIN_KERNEL_ADDRESS + ARM_TT_L2_SIZE;
	candidates[1] = (((gVirtBase - 1) & ~(ARM_TT_L2_SIZE - 1)) -
	    ARM_TT_L2_SIZE);

	for (i = 0; i < 2; i++) {
		blocks = hummer_fb_map_at(candidates[i], fb, HUMMER_FB_BLOCKS,
		    &first);
		if (blocks != 0) {
			break;
		}
	}
	if (blocks == 0) {
		return;
	}

	__asm__ volatile("dsb ishst" ::: "memory");
	hummer_early_attach((uintptr_t)first, (uint32_t)width, (uint32_t)height,
	    (uint32_t)stride, blocks * ARM_TT_L2_SIZE);
}

'''

VM_INIT_ALIAS_ANCHOR = "\tset_mmu_ttb_alternate(cpu_ttep & TTBR_BADDR_MASK);\n"

VM_INIT_ALIAS = ("\t/*\n"
                 "\t * hummer: install the panel alias in the tables that survive,\n"
                 "\t * while phystokv() still walks the bootstrap aperture.\n"
                 "\t */\n"
                 "\thummer_fb_alias(args);\n"
                 "\n"
                 "\tset_mmu_ttb_alternate(cpu_ttep & TTBR_BADDR_MASK);\n"
                 "\n"
                 "\thummer_early_mark(20, \"fb alias live, TTBR1 active\");\n")

VM_INIT_PMAP = r'''	hummer_early_mark(22, "calling pmap_bootstrap");
	pmap_bootstrap(dynamic_memory_begin);
	hummer_early_mark(23, "pmap_bootstrap done");
'''

# --------------------------------------------------------------------------
# arm_init.c: the marks around the code that dies
#
# Every anchor here is a single line: UniXNU's own XNU patch series already
# rewrote this neighbourhood (non-apple-cache-monitor.patch wraps
# ml_enable_monitor() in #if XNU_MONITOR, zzz-boot-progress.patch adds
# boot_progress_stage() calls around arm_vm_init), so multi-line context does
# not survive.
# --------------------------------------------------------------------------
ARM_INIT_MARK15_ANCHOR = "\tarm_vm_init(xmaxmem, args);\n"
ARM_INIT_MARK15 = "\thummer_early_mark(15, \"arm_vm_init returned\");\n"

ARM_INIT_DEBUG_ANCHOR = "\tPE_consistent_debug_inherit();\n"
ARM_INIT_DEBUG = ("\thummer_early_mark(16, \"low_glo / s2r done\");\n"
                  "\tPE_consistent_debug_inherit();\n"
                  "\thummer_early_mark(17, \"consistent_debug_inherit done\");\n")

ARM_INIT_SERIAL_ANCHOR = "\tconst unsigned int serial_exists = serial_init();\n"
ARM_INIT_SERIAL = ("\thummer_early_mark(18, \"calling serial_init\");\n"
                   "\tconst unsigned int serial_exists = serial_init();\n"
                   "\thummer_early_mark(19, \"serial_init returned\");\n")

ARM_INIT_KPRINTF_ANCHOR = '\tkprintf("kprintf initialized\\n");\n'
ARM_INIT_CONSOLE_ANCHOR = "\tPE_create_console();\n"
ARM_INIT_PRINTF_ANCHOR = "\tPE_init_printf(FALSE);\n"

# --------------------------------------------------------------------------
# pe_serial.c: kprintf -> panel
# --------------------------------------------------------------------------
PE_SERIAL_FNS = r'''/* ---- hummer: kprintf onto the panel -------------------------------------- */
/*
 * The alias installed by arm_vm_init() is already in place, so this needs no
 * device tree node and no ml_io_map(): it just forwards characters to the same
 * renderer the stage marks use.
 */
static unsigned int
hummer_fbcon_transmit_ready(void)
{
	return 1;
}

static void
hummer_fbcon_transmit_data(uint8_t c)
{
	hummer_early_putc((char)c);
}

static unsigned int
hummer_fbcon_receive_ready(void)
{
	return 0;
}

static uint8_t
hummer_fbcon_receive_data(void)
{
	return 0;
}

static void
hummer_fbcon_init(void)
{
}

static struct pe_serial_functions hummer_fbcon_serial_functions =
{
	.init = hummer_fbcon_init,
	.transmit_ready = hummer_fbcon_transmit_ready,
	.transmit_data = hummer_fbcon_transmit_data,
	.receive_ready = hummer_fbcon_receive_ready,
	.receive_data = hummer_fbcon_receive_data,
	.device = SERIAL_HUMMER_FBCON
};

'''

PE_SERIAL_REGISTER_OLD = r'''	struct pe_serial_functions *fns = gPESF;

	/**
	 * Even if the serial devices have already been initialized on cold boot,
'''

PE_SERIAL_REGISTER_NEW = r'''	struct pe_serial_functions *fns = gPESF;

	/* hummer: register the panel console before any Apple UART probing. */
	if (hummer_early_console_ready()) {
		register_serial_functions(&hummer_fbcon_serial_functions);
	}

	/**
	 * Even if the serial devices have already been initialized on cold boot,
'''


class Patcher:
    """Edit files in memory, commit only if every anchor matched."""

    def __init__(self):
        self.pending = {}

    def read(self, path):
        if path not in self.pending:
            if not path.is_file():
                raise SystemExit(f"hummer-console: {path} is missing")
            with open(path, "r", encoding="utf-8", newline="") as handle:
                self.pending[path] = handle.read().replace("\r\n", "\n")
        return self.pending[path]

    def write(self, path, text):
        self.pending[path] = text

    def swap(self, path, old, new, tag):
        text = self.read(path)
        n = text.count(old)
        if n != 1:
            self._diagnose(path, old, tag, text, n)
            raise SystemExit(
                f"error: hummer-console: {tag}: expected exactly one anchor in {path}, found {n}")
        self.write(path, text.replace(old, new, 1))

    @staticmethod
    def _diagnose(path, old, tag, text, n):
        """Dump the neighbourhood of the anchor so the CI log says what to use.

        Every line is prefixed with "error:" so it survives the workflow's
        failure-summary grep.
        """
        print(f"error: hummer-console: {tag}: {n} match(es) in {path}", flush=True)
        words = re.findall(r"[A-Za-z_][A-Za-z0-9_]{9,}", old)
        if not words:
            return
        needle = words[0]
        idx = text.find(needle)
        if idx < 0:
            print(f"error: hummer-console: '{needle}' appears nowhere in {path}",
                  flush=True)
            return
        print(f"error: hummer-console: context around first '{needle}':", flush=True)
        start = text.rfind("\n", 0, max(0, idx - 600))
        for line in text[start + 1:idx + 800].splitlines():
            print("error: hummer-console: | " + line, flush=True)

    def insert_before(self, path, anchor, block, tag):
        self.swap(path, anchor, block + anchor, tag)

    def insert_after(self, path, anchor, block, tag):
        self.swap(path, anchor, anchor + block, tag)

    def commit(self):
        for path, text in self.pending.items():
            with open(path, "w", encoding="utf-8", newline="") as handle:
                handle.write(text)
        return [str(p) for p in self.pending]


def drop_objects():
    dropped = []
    if not OBJECT_ROOT.is_dir():
        print("hummer-console: no object tree yet (first pass), nothing to drop")
        return dropped
    for path in OBJECT_ROOT.rglob("*"):
        if path.is_file() and path.name in STALE_OBJECTS:
            path.unlink()
            dropped.append(path.name)
    print(f"hummer-console: dropped {len(dropped)} stale object(s): "
          + ", ".join(sorted(dropped)))
    return dropped


def main():
    if not ARM_INIT.is_file():
        raise SystemExit(f"hummer-console: {ARM_INIT} missing: no XNU work tree")

    if MARKER in ARM_VM_INIT.read_text():
        print("hummer-console: sources already patched")
        drop_objects()
        return

    for path in (ARM_INIT, ARM_VM_INIT, PE_SERIAL, PROTOS):
        if not path.is_file():
            raise SystemExit(f"hummer-console: {path} is missing")

    # ---- header (new file, free: nothing to add to the object lists) ----
    p = Patcher()
    p.write(HUMMER_H, MARKER + "\n" + HEADER)

    # ---- arm_vm_init.c ----
    p.insert_after(ARM_VM_INIT, "#include <arm64/lowglobals.h>\n",
                   "#include <arm64/hummer_early.h>\n", "arm_vm_init include")
    p.insert_before(ARM_VM_INIT,
                    "#define ARM64_PHYSMAP_SLIDE_RANGE (1ULL << 30) // 1 GB\n",
                    VM_INIT_RENDERER, "arm_vm_init renderer")
    p.swap(ARM_VM_INIT, VM_INIT_ALIAS_ANCHOR, VM_INIT_ALIAS,
           "arm_vm_init: alias + mark 20")
    p.insert_after(ARM_VM_INIT,
                   "\tinvalid_tte = (tt_entry_t*)(phystokv(invalid_ttep));\n",
                   "\thummer_early_mark(21, \"bootstrap TTBR0 gone\");\n",
                   "arm_vm_init: mark 21")
    p.swap(ARM_VM_INIT, "\tpmap_bootstrap(dynamic_memory_begin);\n",
           VM_INIT_PMAP, "pmap_bootstrap mark")

    # ---- arm_init.c ----
    p.insert_after(ARM_INIT, "#include <arm64/proc_reg.h>\n",
                   "#include <arm64/hummer_early.h>\n", "arm_init include")
    p.swap(ARM_INIT, ARM_INIT_MARK15_ANCHOR,
           ARM_INIT_MARK15_ANCHOR + ARM_INIT_MARK15, "mark 15")
    p.swap(ARM_INIT, ARM_INIT_DEBUG_ANCHOR, ARM_INIT_DEBUG, "marks 16/17")
    p.swap(ARM_INIT, ARM_INIT_SERIAL_ANCHOR, ARM_INIT_SERIAL, "marks 18/19")
    p.swap(ARM_INIT, ARM_INIT_KPRINTF_ANCHOR,
           ARM_INIT_KPRINTF_ANCHOR + "\thummer_early_mark(25, \"kprintf up\");\n",
           "mark 25")
    p.swap(ARM_INIT, ARM_INIT_CONSOLE_ANCHOR,
           ARM_INIT_CONSOLE_ANCHOR + "\thummer_early_mark(26, \"PE_create_console done\");\n",
           "mark 26")
    p.swap(ARM_INIT, ARM_INIT_PRINTF_ANCHOR,
           ARM_INIT_PRINTF_ANCHOR + "\thummer_early_mark(27, \"PE_init_printf done\");\n",
           "mark 27")

    # ---- pe_serial.c ----
    p.insert_after(PE_SERIAL, "#include <arm64/proc_reg.h>\n",
                   "#include <arm64/hummer_early.h>\n", "pe_serial include")
    p.insert_before(PE_SERIAL, "int\nserial_init(void)\n{\n",
                    PE_SERIAL_FNS, "pe_serial fbcon functions")
    p.swap(PE_SERIAL, PE_SERIAL_REGISTER_OLD, PE_SERIAL_REGISTER_NEW,
           "pe_serial registration")

    # ---- protos.h ----
    p.swap(PROTOS, "\tSERIAL_DCC_UART=0x10\n} serial_device_t;\n",
           "\tSERIAL_DCC_UART=0x10,\n\tSERIAL_HUMMER_FBCON=0x20\n} serial_device_t;\n",
           "serial_device enum")

    for path, needle in ((ARM_VM_INIT, "hummer_early_mark"),
                         (ARM_INIT, "hummer_early_mark"),
                         (PE_SERIAL, "hummer_early_putc")):
        if needle not in p.read(path):
            raise SystemExit(f"hummer-console: {path} did not take the patch")

    p.commit()
    print("hummer-console: patched " + ", ".join(
        str(x.relative_to(XNU)) for x in (ARM_VM_INIT, ARM_INIT, PE_SERIAL, PROTOS)))
    print(f"hummer-console: wrote {HUMMER_H}")
    print("hummer-console: marks 15..27 will appear on the panel as 'HUM <n>: ...'")
    drop_objects()


if __name__ == "__main__":
    main()
