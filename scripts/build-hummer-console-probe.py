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
1. `arm_vm_init()` ends by building the real kernel tables (`init_ptpages`),
   filling the physmap, switching TTBR1 to them and dropping the bootstrap
   TTBR0 identity map.  Right after that switch, `hummer_fb_alias()` finds a
   free run of L2 slots in the 1 GB window that holds the kernel image --
   scanning *down* from just below `gVirtBase` -- and fills it with block
   descriptors for the panel's physical window, as Normal non-cacheable /
   write-combining memory, so the panel engine sees pixels with no cache
   maintenance.  Only `ARM_TTE_EMPTY` slots are used and the scan never leaves
   the kernel's own L1 window, so no real mapping can be displaced; the VA
   below the image is never handed out by the VM either.
2. The same file grows the renderer: 8x16 `iso_font` glyphs at 2x, straight
   stores into the aliased VA.  No locks, no allocation, no per-CPU state.
3. `hummer_early_mark()` is sprinkled through `arm_vm_init()` and `arm_init()`
   so every stage prints one line: "HUM 11: calling pmap_bootstrap" etc.
4. `pe_serial.c` registers the same renderer as a serial device, so once
   kprintf comes up its output lands on the panel as well.

Two constraints this file has to respect, both learned the hard way:

* Nothing may be added to `osfmk/conf/files.arm64` -- the object lists for the
  final link pass are frozen by the time these archive builders run, so the
  renderer lives inside `arm_vm_init.c`, which is already linked.  Its header
  is new, which is free -- but only for arm_vm_init.c: files outside osfmk/
  resolve <arm64/...> against EXPORT_HDRS, so they get inline prototypes.
* Every anchor must sit *outside* the context of every UniXNU patch hunk.
  `build-xnu-4k-baseline.sh` re-runs `git apply --reverse --check` on each
  pass, so an insertion inside a hunk's context makes the check fail (the
  patch is neither applied-cleanly nor reversed-cleanly) and the whole build
  stops with "patch does not apply".  `check_reversibility()` below fails the
  build here, with an explanation, instead of letting that happen later.
"""
import re
import subprocess
from pathlib import Path

MARKER = "hummer: panel console"

XNU = Path(".cache/xnu-work")
ARM_INIT = XNU / "osfmk/arm/arm_init.c"
ARM_VM_INIT = XNU / "osfmk/arm64/arm_vm_init.c"
PE_SERIAL = XNU / "pexpert/arm/pe_serial.c"
PROTOS = XNU / "pexpert/pexpert/arm/protos.h"
MACH_ROUT = XNU / "osfmk/arm64/machine_routines.c"
HUMMER_H = XNU / "osfmk/arm64/hummer_early.h"

PATCH_DIR = Path("patches/xnu")
TOUCHED = (ARM_VM_INIT, ARM_INIT, PE_SERIAL, PROTOS, MACH_ROUT)

OBJECT_ROOT = Path("build/xnu/ios-obj")
STALE_OBJECTS = ("arm_init.o", "arm_init.o.json",
                 "arm_vm_init.o", "arm_vm_init.o.json",
                 "pe_serial.o", "pe_serial.o.json",
                 "machine_routines.o", "machine_routines.o.json")

HEADER = r'''/*
 * hummer: framebuffer console for the part of arm_init() that runs with the
 * bootstrap translation tables gone.
 *
 * The alias and the renderer both live in osfmk/arm64/arm_vm_init.c (that
 * object is already in the link list).  arm_init.c and pe_serial.c spell the
 * prototypes out inline instead of including this header: a new header only
 * reaches EXPORT_HDRS if osfmk/arm64/Makefile lists it, and pe_serial.c
 * resolves <arm64/...> exclusively against EXPORT_HDRS.
 */
#ifndef _ARM64_HUMMER_EARLY_H_
#define _ARM64_HUMMER_EARLY_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register the alias VA and the panel geometry. No-op until placement worked. */
void hummer_early_attach(uintptr_t va, uint32_t width, uint32_t height,
    uint32_t stride_bytes, uint32_t window_bytes);

/* Draw one character, '\n' and '\r' behave as usual. */
void hummer_early_putc(char c);

/* Print "<label>=0x<8 hex digits>\n". Nothing happens before attach. */
void hummer_early_u32x(const char *label, unsigned int value);

/* Print "HUM <stage>: <label>\n". Nothing happens before attach. */
void hummer_early_mark(unsigned int stage, const char *label);

/* Print "<label>=<up to 384 bytes of text>\n". Nothing happens before attach. */
void hummer_early_prints(const char *label, const char *value);

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
 * Usable from the moment the final kernel tables are live, and from panic paths
 * later on.  Deliberately primitive: no locks, no allocation, no per-CPU state,
 * no ADT parsing.
 */
#define HUMMER_SCALE        2u
#define HUMMER_CELL_W       (8u * HUMMER_SCALE)
#define HUMMER_CELL_H       (16u * HUMMER_SCALE)
#define HUMMER_FG           0xffffffffu
#define HUMMER_BG           0xff000000u
/* Blocks to ask for, biggest first; each is ARM_TT_L2_SIZE (2 MB at 4 KB). */
#define HUMMER_FB_WANT_BIG  4u
#define HUMMER_FB_WANT_MID  2u

extern unsigned char iso_font[256 * 16];

static volatile uint32_t *hummer_fb;
static uint32_t hummer_stride;
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

/* Black out the line the cursor is on, so the splash underneath cannot win. */
static void
hummer_clear_row(void)
{
	uint32_t y, x;

	for (y = 0; y < HUMMER_CELL_H; y++) {
		volatile uint32_t *line = hummer_fb +
		    (hummer_row * HUMMER_CELL_H + y) * hummer_stride;

		for (x = 0; x < hummer_cols * HUMMER_CELL_W; x++) {
			line[x] = HUMMER_BG;
		}
	}
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
	hummer_rows = rows;
	hummer_cols = cols;
	hummer_col = 0;
	hummer_row = 0;
	hummer_ready = 1;
	hummer_clear_row();
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
		hummer_clear_row();
		return;
	}
	if (hummer_col >= hummer_cols) {
		hummer_col = 0;
		if (++hummer_row >= hummer_rows) {
			hummer_row = 0;
		}
		hummer_clear_row();
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
		digits[n++] = (char)('0' + (stage / 100) % 10);
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

void
hummer_early_u32x(const char *label, unsigned int value)
{
	int hummer_i;

	if (!hummer_ready) {
		return;
	}
	while (*label != '\0') {
		hummer_early_putc(*label++);
	}
	hummer_early_putc('=');
	hummer_early_putc('0');
	hummer_early_putc('x');
	for (hummer_i = 28; hummer_i >= 0; hummer_i -= 4) {
		unsigned int hummer_nib = (value >> hummer_i) & 0xfu;
		hummer_early_putc((char)((hummer_nib < 10) ? ('0' + hummer_nib) : ('a' + hummer_nib - 10)));
	}
	hummer_early_putc('\n');
}

/* v47: the exact command line the loader handed us.  Apple's own AMFI kext
 * panics with "can't has cs_enforcement_disable", which can only happen when
 * PE_parse_boot_argn() finds that name -- so print what XNU really sees. */
void
hummer_early_prints(const char *label, const char *value)
{
	unsigned int hummer_n = 0;

	if (!hummer_ready) {
		return;
	}
	while (*label != '\0') {
		hummer_early_putc(*label++);
	}
	hummer_early_putc('=');
	while (hummer_n < 384 && *value != '\0') {
		hummer_early_putc(*value++);
		hummer_n++;
	}
	hummer_early_putc('\n');
}

/*
 * L2 slot for va, or NULL if the covering L1 entry is not a table (in which
 * case this VA range cannot take a block descriptor at all).
 */
static tt_entry_t *
hummer_l2_slot(vm_map_address_t va)
{
	tt_entry_t l1 = cpu_tte[(va & ARM_TT_L1_INDEX_MASK) >> ARM_TT_L1_SHIFT];

	if ((l1 & (ARM_TTE_VALID | ARM_TTE_TYPE_MASK)) !=
	    (ARM_TTE_VALID | ARM_TTE_TYPE_TABLE)) {
		return NULL;
	}
	return (tt_entry_t *)phystokv(l1 & ARM_TTE_TABLE_MASK) +
	    ((va & ARM_TT_L2_INDEX_MASK) >> ARM_TT_L2_SHIFT);
}

/*
 * Look for `want` consecutive unused L2 slots ending just below gVirtBase and
 * map the panel into them.  Scanning downwards from the image keeps the console
 * in the one 1 GB window whose table already exists (the kernel lives in it),
 * and stopping at the window base means we can never wander into a range whose
 * upper tables were not built.  Returns the number of blocks installed, 0 if
 * the panel does not fit -- which the caller treats as "no console", never as
 * an error: a boot without a console still boots.
 */
static unsigned int
hummer_fb_place(uint64_t fb, unsigned int want, vm_map_address_t *first_out)
{
	vm_map_address_t top = gVirtBase & ~(ARM_TT_L2_SIZE - 1);
	vm_map_address_t lo = gVirtBase & ~ARM_TT_L1_OFFMASK;
	vm_map_address_t va;
	unsigned int i;

	for (va = top; va >= lo + want * ARM_TT_L2_SIZE; va -= ARM_TT_L2_SIZE) {
		vm_map_address_t run = va - want * ARM_TT_L2_SIZE;
		int usable = 1;

		for (i = 0; i < want; i++) {
			tt_entry_t *slot = hummer_l2_slot(run + i * ARM_TT_L2_SIZE);

			if (slot == NULL || *slot != ARM_TTE_EMPTY) {
				usable = 0;
				break;
			}
		}
		if (!usable) {
			continue;
		}
		for (i = 0; i < want; i++) {
			*hummer_l2_slot(run + i * ARM_TT_L2_SIZE) =
			    ((fb + i * ARM_TT_L2_SIZE) & ARM_TTE_BLOCK_L2_MASK)
			    | ARM_TTE_TYPE_BLOCK | ARM_TTE_VALID
			    | ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_NX
			    | ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY)
			    | ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITECOMB)
			    | ARM_TTE_BLOCK_PNX;
		}
		*first_out = run;
		return want;
	}
	return 0;
}

/*
 * Install the panel alias and start the console.  Called from arm_vm_init()
 * right after the kernel tables are active and the identity map is gone, so the
 * empty-slot search sees the *final* state of the tables.
 */
static void
hummer_fb_alias(boot_args *args)
{
	uint64_t fb = args->Video.v_baseAddr;
	uint64_t stride = args->Video.v_rowBytes;
	uint64_t width = args->Video.v_width;
	uint64_t height = args->Video.v_height;
	uint64_t base;
	vm_map_address_t first = 0;
	unsigned int blocks;

	if (fb == 0 || stride == 0 || height == 0) {
		return;
	}
	/* v_width is only ever consulted to bound the row; infer it if absent */
	if (width == 0 || width * 4 > stride) {
		width = stride / 4;
	}
	/* cheap sanity: the boot loader cannot have handed us a wild geometry */
	if (stride > (4u << 20) || height > (1u << 16) || width > (1u << 16)) {
		return;
	}
	/* a block descriptor can only describe a 2 MB aligned physical window */
	base = fb & ~(ARM_TT_L2_SIZE - 1);

	blocks = hummer_fb_place(base, HUMMER_FB_WANT_BIG, &first);
	if (blocks == 0) {
		blocks = hummer_fb_place(base, HUMMER_FB_WANT_MID, &first);
	}
	if (blocks == 0) {
		blocks = hummer_fb_place(base, 1, &first);
	}
	if (blocks == 0) {
		return;
	}

	/*
	 * Publish the new descriptors before anything touches the alias.  No TLBI
	 * is strictly needed (this VA was never accessed and the TLB was flushed a
	 * few lines ago), but a full invalidate is a few microseconds once.
	 */
	__asm__ volatile("dsb ishst" ::: "memory");
	flush_mmu_tlb();
	__asm__ volatile("dsb ish\n\tisb" ::: "memory");
	hummer_early_attach((uintptr_t)(first + (fb - base)), (uint32_t)width,
	    (uint32_t)height, (uint32_t)stride, blocks * ARM_TT_L2_SIZE);
	/* v47: console is up now, so this is the first place we can echo it. */
	hummer_early_prints("cmdline", (const char *)args->CommandLine);
}

'''

# --------------------------------------------------------------------------
# arm_init.c: the marks around the code that dies
#
# Every anchor is a single line: UniXNU's own XNU patch series already rewrote
# this neighbourhood (non-apple-cache-monitor.patch wraps ml_enable_monitor()
# in #if XNU_MONITOR, zzz-boot-progress.patch inserts boot_progress_stage()
# around the arm_vm_init() call), so multi-line context does not survive.
#
# There is deliberately no mark right after arm_vm_init(): zzz-boot-progress
# already put "arm_vm_init returned" there, and a second insertion on that line
# would break that patch's reverse context.
# --------------------------------------------------------------------------
ARM_INIT_DEBUG_ANCHOR = "\tPE_consistent_debug_inherit();\n"
ARM_INIT_DEBUG = ("\thummer_early_mark(13, \"low_glo / s2r done\");\n"
                  "\tPE_consistent_debug_inherit();\n"
                  "\thummer_early_mark(14, \"consistent_debug_inherit done\");\n")

ARM_INIT_SERIAL_ANCHOR = "\tconst unsigned int serial_exists = serial_init();\n"
ARM_INIT_SERIAL = ("\thummer_early_mark(15, \"calling serial_init\");\n"
                   "\tconst unsigned int serial_exists = serial_init();\n"
                   "\thummer_early_mark(16, \"serial_init returned\");\n")

ARM_INIT_KPRINTF_ANCHOR = '\tkprintf("kprintf initialized\\n");\n'
ARM_INIT_CONSOLE_ANCHOR = "\tPE_create_console();\n"
ARM_INIT_PRINTF_ANCHOR = "\tPE_init_printf(FALSE);\n"

# v22: print the state inside ml_processor_register() on every call.
# The real-device run showed num_cpus=1 (cpus=1 boot arg) while the boot
# CPU registration still failed with 5; the only remaining explanation is
# reg_cpu_count already consumed by an earlier caller (ArmDTPlatform?).
MLPR_PROBE = r'''/* ---- hummer: processor registration probe ------------------------------- */
{
	extern void hummer_early_u32x(const char *label, unsigned int value);
	hummer_early_u32x("mlpr log_id", in_processor_info->log_id);
	hummer_early_u32x("mlpr max_cpu", ml_get_max_cpu_number());
	hummer_early_u32x("mlpr num_cpus", topology_info.num_cpus);
	hummer_early_u32x("mlpr reg_count", reg_cpu_count);
}
'''

# --------------------------------------------------------------------------
# pe_serial.c: kprintf -> panel
# --------------------------------------------------------------------------
# Prototypes for the renderer that lives in arm_vm_init.c.  Inlined instead of
# `#include <arm64/hummer_early.h>` because that header is not in EXPORT_HDRS
# (see the arm_init.c comment above).
HUMMER_PROTOS = r'''/* ---- hummer: panel console (renderer lives in arm_vm_init.c) ------------- */
extern void hummer_early_putc(char c);
extern void hummer_early_mark(unsigned int stage, const char *label);
extern void hummer_early_u32x(const char *label, unsigned int value);
extern int hummer_early_console_ready(void);
'''

PE_SERIAL_FNS = r'''/* ---- hummer: kprintf onto the panel -------------------------------------- */
/*
 * The alias installed by arm_vm_init() is already in place, so this needs no
 * device tree node and no ml_io_map(): it just forwards characters to the same
 * renderer the stage marks use.
 */
static bool hummer_fbcon_registered = false;

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

# register_serial_functions() prepends without checking for duplicates, so a
# second serial_init() would point our node at itself and turn every later walk
# of gPESF into an infinite loop.  serial_init() is called more than once.
PE_SERIAL_REGISTER_NEW = r'''	struct pe_serial_functions *fns = gPESF;

	/* hummer: register the panel console before any Apple UART probing. */
	if (!hummer_fbcon_registered && hummer_early_console_ready()) {
		hummer_fbcon_registered = true;
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


def check_reversibility():
    """Which UniXNU patches still reverse-apply cleanly against the work tree?

    build-xnu-4k-baseline.sh re-runs `git apply --reverse --check` for every
    patch on every pass (that is how it decides whether a patch is already
    applied).  An insertion that lands inside a hunk's context makes that check
    fail, so the next pass tries to apply an already-applied patch and the build
    stops with a patch error that says nothing about this script.  Returns None
    when the check is not possible here (no work tree / no patch series).
    """
    if not PATCH_DIR.is_dir() or not (XNU / ".git").exists():
        return None
    include = []
    for path in TOUCHED:
        include += ["--include=" + path.relative_to(XNU).as_posix()]
    report = {}
    for patch in sorted(PATCH_DIR.glob("*.patch")):
        proc = subprocess.run(
            ["git", "-C", str(XNU), "apply", "--reverse", "--check"]
            + include + [str(patch.resolve())],
            capture_output=True)
        report[patch.name] = proc.returncode == 0
    return report


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
        report = check_reversibility()
        broken = [name for name, ok in (report or {}).items() if not ok]
        if broken:
            print("warning: hummer-console: these patches no longer reverse-apply "
                  "and build-xnu-4k-baseline.sh will refuse to re-apply them: "
                  + ", ".join(broken))
        drop_objects()
        return

    for path in (ARM_INIT, ARM_VM_INIT, PE_SERIAL, PROTOS):
        if not path.is_file():
            raise SystemExit(f"hummer-console: {path} is missing")

    before = check_reversibility()

    # ---- header (new file, free: nothing to add to the object lists) ----
    # The first line identifies the file for humans, so it has to be a comment:
    # a bare marker here is not C, and every file that includes this header
    # fails with "type specifier missing, defaults to 'int'".  (v20c shipped
    # exactly that and the whole pass-2 build died on it.)
    p = Patcher()
    header_text = "/* " + MARKER + " */\n" + HEADER
    if not (header_text.lstrip().startswith("/*")
            and header_text.rstrip().endswith("*/")):
        raise SystemExit("hummer-console: header is not a well-formed comment "
                         "block; refusing to write it")
    p.write(HUMMER_H, header_text)

    # ---- arm_vm_init.c ----
    p.insert_after(ARM_VM_INIT, "#include <arm64/lowglobals.h>\n",
                   "#include <arm64/hummer_early.h>\n", "arm_vm_init include")
    p.insert_before(ARM_VM_INIT,
                    "#define ARM64_PHYSMAP_SLIDE_RANGE (1ULL << 30) // 1 GB\n",
                    VM_INIT_RENDERER, "arm_vm_init renderer")
    p.swap(ARM_VM_INIT, "\tpmap_bootstrap(dynamic_memory_begin);\n",
           "\thummer_early_mark(11, \"calling pmap_bootstrap\");\n"
           "\tpmap_bootstrap(dynamic_memory_begin);\n"
           "\thummer_early_mark(12, \"pmap_bootstrap done\");\n",
           "pmap_bootstrap marks")
    # The identity map is gone, the kernel tables are live: this is the first
    # moment the panel can be mapped for good.
    p.swap(ARM_VM_INIT,
           "\tinvalid_tte = (tt_entry_t*)(phystokv(invalid_ttep));\n",
           "\tinvalid_tte = (tt_entry_t*)(phystokv(invalid_ttep));\n"
           "\thummer_fb_alias(args);\n"
           "\thummer_early_mark(10, \"kernel tables live, panel mapped\");\n",
           "arm_vm_init: panel alias")

    # ---- arm_init.c ----
    # Not `#include <arm64/hummer_early.h>`: only arm_vm_init.c can resolve that
    # include (its object compiles with the osfmk source tree on the search
    # path).  pe_serial.c resolves <arm64/...> against EXPORT_HDRS, and a new
    # header only lands there if osfmk/arm64/Makefile lists it -- v20d died in
    # the final pass with "'arm64/hummer_early.h' file not found" exactly
    # because of this.  Spell the prototypes out instead.
    p.insert_after(ARM_INIT, "#include <arm64/proc_reg.h>\n",
                   HUMMER_PROTOS, "arm_init protos")
    # v21: print the CPU topology state right where the kernel builds it --
    # ArmCpu::start panicked because ml_processor_register() returned 5
    # (KERN_FAILURE), which for the boot CPU means an empty/odd topology_info.
    p.swap(ARM_INIT, "\tml_parse_cpu_topology();\n",
           "\tml_parse_cpu_topology();\n"
           "\t{\n"
           "\t\tconst ml_topology_info_t *hummer_ti = ml_get_topology_info();\n"
           "\t\tDTEntry hummer_dt = 0;\n"
           "\t\thummer_early_u32x(\"topo num_cpus\", hummer_ti->num_cpus);\n"
           "\t\thummer_early_u32x(\"topo max_cpu_id\", hummer_ti->max_cpu_id);\n"
           "\t\thummer_early_u32x(\"topo num_clusters\", hummer_ti->num_clusters);\n"
           "\t\thummer_early_u32x(\"topo boot_cpu?\", hummer_ti->boot_cpu != 0);\n"
           "\t\thummer_early_u32x(\"dt find /cpus\", (unsigned int)SecureDTLookupEntry(0, \"/cpus\", &hummer_dt));\n"
           "\t\thummer_early_u32x(\"dt entry\", (unsigned int)(uintptr_t)hummer_dt);\n"
           "\t}\n",
           "topology probe")
    p.swap(ARM_INIT, "\tmaster_cpu = ml_get_boot_cpu_number();\n",
           "\tmaster_cpu = ml_get_boot_cpu_number();\n"
           "\thummer_early_u32x(\"master_cpu\", (unsigned int)master_cpu);\n"
           "\thummer_early_u32x(\"ml max_cpu\", (unsigned int)ml_get_max_cpu_number());\n"
           "\thummer_early_u32x(\"cpu_number(0)\", (unsigned int)ml_get_cpu_number(0));\n",
           "topology after master_cpu")
    p.swap(ARM_INIT, ARM_INIT_DEBUG_ANCHOR, ARM_INIT_DEBUG, "marks 16/17")
    p.swap(ARM_INIT, ARM_INIT_SERIAL_ANCHOR, ARM_INIT_SERIAL, "marks 18/19")
    p.swap(ARM_INIT, ARM_INIT_KPRINTF_ANCHOR,
           ARM_INIT_KPRINTF_ANCHOR + "\thummer_early_mark(17, \"kprintf up\");\n",
           "mark 25")
    p.swap(ARM_INIT, ARM_INIT_CONSOLE_ANCHOR,
           ARM_INIT_CONSOLE_ANCHOR + "\thummer_early_mark(18, \"PE_create_console done\");\n",
           "mark 26")
    p.swap(ARM_INIT, ARM_INIT_PRINTF_ANCHOR,
           ARM_INIT_PRINTF_ANCHOR + "\thummer_early_mark(19, \"PE_init_printf done\");\n"
           + "\thummer_early_u32x(\"late num_cpus\", ml_get_topology_info()->num_cpus);\n",
           "mark 27")

    # ---- machine_routines.c ----
    p.insert_before(MACH_ROUT,
                    "\tif (in_processor_info->log_id > (uint32_t)ml_get_max_cpu_number()) {\n",
                    MLPR_PROBE, "mlpr probe")

    # v48: pins inside ml_processor_register().  On the device the second
    # registration prints its four probe lines and then the machine goes quiet
    # -- but the first kprintf("[%d]%s>pset_find...") (two lines after
    # lck_mtx_lock(&pset_create_lock)) never appears, even though kprintf is
    # up and panic text renders.  So the hang is between the probes and that
    # kprintf: the field assignments, kvtophys(), OSTestAndSet() or the lock.
    # Bracket every step; the source here is xnu-10063.141.1 verbatim and no
    # UniXNU patch touches this file, so these anchors are stable.
    MLR2_DECL = "extern void hummer_early_u32x(const char *label, unsigned int value);"
    p.insert_after(MACH_ROUT, "\t\tis_boot_cpu = TRUE;\n",
                   "\t{ " + MLR2_DECL + "\n"
                   "\t  hummer_early_u32x(\"mlr2 boot-cpu branch\", (unsigned int)in_processor_info->log_id); }\n",
                   "mlr2 boot")
    p.insert_after(MACH_ROUT,
                   "\tthis_cpu_datap->cpu_reset_assist = kvtophys(in_processor_info->powergate_stub_addr);\n",
                   "\t{ " + MLR2_DECL + "\n"
                   "\t  hummer_early_u32x(\"mlr2 kvtophys done\", (unsigned int)in_processor_info->log_id); }\n",
                   "mlr2 kvtop")
    p.insert_after(MACH_ROUT, "\tlck_mtx_lock(&pset_create_lock);\n",
                   "\t{ " + MLR2_DECL + "\n"
                   "\t  hummer_early_u32x(\"mlr2 pset lock taken\", (unsigned int)in_processor_info->log_id); }\n",
                   "mlr2 lock")
    p.insert_after(MACH_ROUT, "\tlck_mtx_unlock(&pset_create_lock);\n",
                   "\t{ " + MLR2_DECL + "\n"
                   "\t  hummer_early_u32x(\"mlr2 pset lock released\", (unsigned int)in_processor_info->log_id); }\n",
                   "mlr2 unlock")

    # ---- pe_serial.c ----
    p.insert_after(PE_SERIAL, "#include <arm64/proc_reg.h>\n",
                   HUMMER_PROTOS, "pe_serial protos")
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

    after = check_reversibility()
    if before and after:
        broke = sorted(name for name in before if before[name] and not after[name])
        if broke:
            raise SystemExit(
                "error: hummer-console: these patches reverse-applied before the "
                "edit and do not any more, so an anchor landed inside a patch hunk "
                "and the next pass will fail to re-apply them: " + ", ".join(broke))
        print("hummer-console: all " + str(len(after))
              + " touched patches still reverse-apply")

    print("hummer-console: marks 10..19 will appear on the panel as 'HUM <n>: ...'")
    drop_objects()


if __name__ == "__main__":
    main()
