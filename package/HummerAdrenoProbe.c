/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * hummer: Adreno 830 (a8xx) GMU TCM write test.
 *
 * The engine calls native_adreno_firmware_probe() from bsd_init() (via
 * patches/xnu/offline-component-load-probes.patch, right after vfsinit) when
 * the defconfig carries CONFIG_XNU_ADRENO_FIRMWARE_PROBE=y, and force-loads
 * the archive this file lands in.
 *
 * Hardware facts so far:
 *   v145  CX domain comes up by clearing GPUCC_CX_GDSCR bit 0 (100 us).
 *   v146  GPUCC PLL0 was cold; the vendor TAYCAN_ELU table locks it at 1 GHz.
 *   v147  GMU is cold: CM3_SYSRESET bit0 = 1 (held in reset), FW_INIT_RESULT
 *         = 0, HFI down, PWR_CLK_STATUS bit0/bit1 = 1 (GX supply and clock
 *         off).  Its registers do answer once CX is powered, so the window is
 *         usable.
 *
 * With the GMU never started, the whole bring-up is required: GMU memory +
 * SMMU, load gen80000_gmu.bin, release CM3, HFI perf table, START, then the
 * GX vote.  The firmware's ITCM/DTCM halves go in through the GMU aperture
 * (no translation), so before touching SMMU the one thing worth proving is
 * that this kernel can actually write them: DTCM sits at GMU(0x1c400) =
 * 0x3a000, i.e. physical 0x3d71000, and CM3 being held in reset makes it ours.
 *
 * This version powers CX, re-confirms CM3 is in reset, writes two words into
 * DTCM and reads them back.  Nothing else is touched.
 *
 * Gated by boot arg hummer-adreno-probe=1.  Values go inside the panic text
 * because the BootProgress panic UI redraws the screen.
 */
#include <kern/debug.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
#include <arm/machine_routines.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void boot_progress_stage(const char *s);

#include <libkern/OSBase.h>
#include <libkern/OSTypes.h>

#define HU_GPUCC_BASE   0x3d90000ULL
#define HU_GPUCC_SIZE   0x10000ULL
#define HU_GMU_BASE     0x3d37000ULL
#define HU_GMU_SIZE     0x68000ULL

#define HU_GPUCC_CX_GDSCR   0x9080u
#define HU_GPUCC_CX_HW_CTRL 0x9094u

#define HU_GDSC_SW_COLLAPSE (1u << 0)
#define HU_GDSC_HW_CONTROL  (1u << 1)

/* GMU(n) = 4n - 0x37000 */
#define HU_GMU_DTCM            0x3a000u   /* GMU(0x1c400), physical 0x3d71000 */
#define HU_GMU_CM3_SYSRESET    0x47000u   /* GMU(0x1f800) */
#define HU_GMU_FW_INIT_RESULT  0x47070u   /* GMU(0x1f81c) */

#define HU_RD(base, off)    (*(volatile uint32_t *)(uintptr_t)((base) + (off)))
#define HU_WR(base, off, v) (*(volatile uint32_t *)(uintptr_t)((base) + (off)) = (uint32_t)(v))
#define HU_SETTLE() __asm__ volatile("dsb sy" ::: "memory")

volatile int hummer_adreno_probe_stage;

void
native_adreno_firmware_probe(void)
{
	uint32_t enabled = 0;

	if (!PE_parse_boot_argn("hummer-adreno-probe", &enabled, sizeof(enabled))
	    || enabled == 0) {
		return;
	}
	hummer_adreno_probe_stage = 1;
	boot_progress_stage("adreno: probe start");

	vm_offset_t gpucc = ml_io_map(HU_GPUCC_BASE, HU_GPUCC_SIZE);
	vm_offset_t gmu = ml_io_map(HU_GMU_BASE, HU_GMU_SIZE);
	if (!gpucc || !gmu) {
		panic("hummer adreno: window unmappable gpucc=%lx gmu=%lx",
		    (unsigned long)gpucc, (unsigned long)gmu);
	}

	/* ---- CX power (verified v145) -------------------------------------- */
	uint32_t gdscr0 = HU_RD(gpucc, HU_GPUCC_CX_GDSCR);
	uint32_t hw0 = HU_RD(gpucc, HU_GPUCC_CX_HW_CTRL);
	uint32_t up0 = ((gdscr0 | hw0) >> 31) & 1u;

	if (!up0 && !(gdscr0 & HU_GDSC_HW_CONTROL)) {
		boot_progress_stage("adreno: cx write");
		HU_WR(gpucc, HU_GPUCC_CX_GDSCR, gdscr0 & ~HU_GDSC_SW_COLLAPSE);
		for (unsigned i = 0; i < 5000; i++) {
			if (((HU_RD(gpucc, HU_GPUCC_CX_GDSCR) |
			      HU_RD(gpucc, HU_GPUCC_CX_HW_CTRL)) >> 31) & 1u) {
				break;
			}
			IODelay(100);
		}
	}
	uint32_t gdscr1 = HU_RD(gpucc, HU_GPUCC_CX_GDSCR);
	uint32_t up1 = ((gdscr1 | HU_RD(gpucc, HU_GPUCC_CX_HW_CTRL)) >> 31) & 1u;
	boot_progress_stage("adreno: cx done");

	uint32_t cm3 = HU_RD(gmu, HU_GMU_CM3_SYSRESET);
	uint32_t fwv = HU_RD(gmu, HU_GMU_FW_INIT_RESULT);

	if (!up1) {
		hummer_adreno_probe_stage = 100;
		panic("adreno830 dtcm test:\n"
		    "cx before=%08x after=%08x up=0 (CX failed, GMU untouched)",
		    gdscr0, gdscr1);
	}
	if (!(cm3 & 1u)) {
		/* CM3 not held: DTCM belongs to running code, do not scribble. */
		hummer_adreno_probe_stage = 100;
		panic("adreno830 dtcm test:\n"
		    "cx up=%u cm3_reset=%08x fw_result=%08x\n"
		    "CM3 not in reset -> DTCM not ours, nothing written",
		    up1, cm3, fwv);
	}

	/* ---- DTCM round trip ----------------------------------------------- */
	boot_progress_stage("adreno: dtcm rw");
	uint32_t d0_before = HU_RD(gmu, HU_GMU_DTCM + 0x00);
	uint32_t d1_before = HU_RD(gmu, HU_GMU_DTCM + 0x04);

	HU_WR(gmu, HU_GMU_DTCM + 0x00, 0xdeadbeefu);
	HU_WR(gmu, HU_GMU_DTCM + 0x04, 0x12345678u);
	HU_SETTLE();

	uint32_t d0_after = HU_RD(gmu, HU_GMU_DTCM + 0x00);
	uint32_t d1_after = HU_RD(gmu, HU_GMU_DTCM + 0x04);

	uint32_t ok0 = (d0_after == 0xdeadbeefu);
	uint32_t ok1 = (d1_after == 0x12345678u);

	hummer_adreno_probe_stage = 100;
	boot_progress_stage("adreno: done");

	panic("adreno830 dtcm test:\n"
	    "cx before=%08x after=%08x up=%u\n"
	    "cm3_reset=%08x fw_result=%08x\n"
	    "dtcm0 before=%08x read=%08x ok=%u\n"
	    "dtcm1 before=%08x read=%08x ok=%u",
	    gdscr0, gdscr1, up1,
	    cm3, fwv,
	    d0_before, d0_after, ok0,
	    d1_before, d1_after, ok1);
}
