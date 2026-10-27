/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * hummer: Adreno 830 (a8xx) CX power + GPUCC PLL0 bring-up.
 *
 * The engine calls native_adreno_firmware_probe() from bsd_init() (via
 * patches/xnu/offline-component-load-probes.patch, right after vfsinit) when
 * the defconfig carries CONFIG_XNU_ADRENO_FIRMWARE_PROBE=y, and force-loads
 * the archive this file lands in (build/adreno-firmware-probe/
 * libAdrenoFirmwareProbe.a).
 *
 * Verified on hardware so far:
 *   v144  GPUCC_CX_GDSCR = 0x0222f801 -> CX down, NOT under hardware control,
 *         software collapse requested: the branch of the A830 reference
 *         driver's adreno8_step_cx_power that is ours to pull.
 *   v145  writing CX_GDSCR = gdscr & ~bit0 brought the domain up in 100 us
 *         (gdscr = 0xfa22f800, up = 1, loops = 1).  RBBM/GBIF stay 0 because
 *         GX waits for the GMU's vote -- expected, not a fault.
 *
 * What is still cold: PLL0.  The GPUCC low words read
 *   PLL_MODE = 0x00000100  (bit31 LOCK 0, bit0 OUT 0, bit2 RESET_N 0,
 *                           bit8 PCAL_NOT_DONE 1)
 *   PLL_L    = 0x00440000  (low 16 bits zero -> never initialised)
 * so the loader never started it.  The A830 reference driver configures
 * TAYCAN_ELU itself in adreno8_clock.c, and that is what this version ports:
 *
 *   keep PLL_USER outputs enabled -> assert reset -> if PLL_L was never
 *   initialised, write the vendor table (L 0x44440034, alpha 0x1555, Cfg0-2)
 *   -> release reset, run -> poll PLL_LOCK 1.5 ms -> switch the output on.
 *   Resulting rate = 19.2 MHz * (L & 0xffff) + ((19.2 MHz * alpha) >> 16),
 *   i.e. 1.0 GHz for that table.
 *
 * It also carries the reference's precondition instead of writing blind: the
 * GMU and HUB RCG parents must be one of the states the vendor sequence
 * accepts, otherwise PLL0 must not be touched and the parents are reported so
 * the refusal is diagnosable.
 *
 * Gated by boot arg hummer-adreno-probe=1.  Every number goes into the panic
 * text: the BootProgress panic UI redraws the screen and would erase IOLog.
 * Live boot_progress_stage() markers stay so a hang shows where it stopped.
 */
#include <kern/debug.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
#include <arm/machine_routines.h>   /* ml_io_map lives here, not machine/ */

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

/* BootProgress' live marker: prints to the panel at once, so a hang in the
 * PLL sequence is visible without a panic. */
extern void boot_progress_stage(const char *s);

#include <libkern/OSBase.h>
#include <libkern/OSTypes.h>

#define HU_KGSL_BASE    0x3d00000ULL
#define HU_KGSL_SIZE    0x40000ULL
#define HU_RSCC_BASE    0x3d50000ULL
#define HU_RSCC_SIZE    0x10000ULL
#define HU_GPUCC_BASE   0x3d90000ULL
#define HU_GPUCC_SIZE   0x10000ULL
#define HU_CXMISC_BASE  0x3d9e000ULL
#define HU_CXMISC_SIZE  0x2000ULL

/* GPUCC offsets (A830 reference driver, adreno8_private.h / adreno8_clock.c). */
#define HU_GPUCC_CX_GDSCR   0x9080u
#define HU_GPUCC_CX_HW_CTRL 0x9094u
#define HU_GPUCC_GMU_RCGR   0x9318u
#define HU_GPUCC_HUB_RCGR   0x93ecu
#define HU_GPUCC_AHB_CBCR   0x90bcu

#define HU_PLL_MODE    0x00u
#define HU_PLL_OPMODE  0x04u
#define HU_PLL_L       0x10u
#define HU_PLL_ALPHA   0x14u
#define HU_PLL_USER    0x18u

#define HU_PLL_OUT      (1u << 0)
#define HU_PLL_RESET_N  (1u << 2)
#define HU_PLL_VOTE_RUN (1u << 25)
#define HU_PLL_LOCK     (1u << 31)

#define HU_GDSC_SW_COLLAPSE (1u << 0)
#define HU_GDSC_HW_CONTROL  (1u << 1)
#define HU_GDSC_POWER_ON    (1u << 31)

#define HU_RD(base, off)    (*(volatile uint32_t *)(uintptr_t)((base) + (off)))
#define HU_WR(base, off, v) (*(volatile uint32_t *)(uintptr_t)((base) + (off)) = (uint32_t)(v))
#define HU_RMW(base, off, mask, val) \
	HU_WR(base, off, (HU_RD(base, off) & ~(uint32_t)(mask)) | (uint32_t)(val))
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

	vm_offset_t kgsl = ml_io_map(HU_KGSL_BASE, HU_KGSL_SIZE);
	vm_offset_t gpucc = ml_io_map(HU_GPUCC_BASE, HU_GPUCC_SIZE);
	if (!kgsl || !gpucc) {
		panic("hummer adreno: window unmappable kgsl=%lx gpucc=%lx",
		    (unsigned long)kgsl, (unsigned long)gpucc);
	}

	/* ---- 1. CX power (v145, verified) ---------------------------------- */
	uint32_t gdscr0 = HU_RD(gpucc, HU_GPUCC_CX_GDSCR);
	uint32_t hw0 = HU_RD(gpucc, HU_GPUCC_CX_HW_CTRL);
	uint32_t up0 = ((gdscr0 | hw0) >> 31) & 1u;
	uint32_t cx_acted = 0, cx_loops = 0;

	if (!up0 && !(gdscr0 & HU_GDSC_HW_CONTROL)) {
		cx_acted = 1;
		boot_progress_stage("adreno: cx write");
		HU_WR(gpucc, HU_GPUCC_CX_GDSCR, gdscr0 & ~HU_GDSC_SW_COLLAPSE);
		for (cx_loops = 1; cx_loops <= 5000; cx_loops++) {
			if (((HU_RD(gpucc, HU_GPUCC_CX_GDSCR) |
			      HU_RD(gpucc, HU_GPUCC_CX_HW_CTRL)) >> 31) & 1u) {
				break;
			}
			IODelay(100);
		}
	}
	uint32_t gdscr1 = HU_RD(gpucc, HU_GPUCC_CX_GDSCR);
	uint32_t hw1 = HU_RD(gpucc, HU_GPUCC_CX_HW_CTRL);
	uint32_t up1 = ((gdscr1 | hw1) >> 31) & 1u;
	boot_progress_stage("adreno: cx done");
	if (!up1) {
		hummer_adreno_probe_stage = 100;
		panic("adreno830 cx failed:\n"
		    "before gdscr=%08x hw=%08x up=%u\n"
		    "acted=%u loops=%u after gdscr=%08x hw=%08x\n"
		    "phase=0 (CX did not come up; PLL not touched)",
		    gdscr0, hw0, up0, cx_acted, cx_loops, gdscr1, hw1);
	}

	/* ---- 2. PLL0 preconditions (reference adreno8_clock.c) ------------- */
	uint32_t gmu_src = HU_RD(gpucc, HU_GPUCC_GMU_RCGR + 4);
	uint32_t hub_src = HU_RD(gpucc, HU_GPUCC_HUB_RCGR + 4);
	uint32_t gmu_par = (gmu_src >> 8) & 7u;
	uint32_t hub_par = (hub_src >> 8) & 7u;

	uint32_t mode0 = HU_RD(gpucc, HU_PLL_MODE);
	uint32_t opmode0 = HU_RD(gpucc, HU_PLL_OPMODE);
	uint32_t l0 = HU_RD(gpucc, HU_PLL_L);
	uint32_t alpha0 = HU_RD(gpucc, HU_PLL_ALPHA);
	uint32_t user0 = HU_RD(gpucc, HU_PLL_USER);

	int pll_ok = 1;
	if (user0 == 0xffffffffu || mode0 == 0xffffffffu || opmode0 == 0xffffffffu ||
	    (user0 & HU_PLL_VOTE_RUN)) {
		pll_ok = 0;
	}
	if (pll_ok && (gmu_src == 0xffffffffu || hub_src == 0xffffffffu)) {
		pll_ok = 0;
	}
	if (pll_ok && gmu_par != 0 && gmu_par != 2 && gmu_par != 5 && gmu_par != 6) {
		pll_ok = 0;
	}
	if (pll_ok && hub_par != 0 && (hub_src & 0x371fu) != 0x505u) {
		pll_ok = 0;
	}

	uint32_t configured = 0, pll_loops = 0;
	uint32_t mode1 = mode0, l1 = l0, alpha1 = alpha0;
	uint32_t mhz = 0;

	if (pll_ok) {
		/* Keep the outputs enabled (PLL_USER[2:0] = 7). */
		HU_RMW(gpucc, HU_PLL_USER, 7u, 7u);
		HU_SETTLE();

		int already = (opmode0 & 1u) && (mode0 & HU_PLL_OUT);
		if (already) {
			configured = 2;   /* loader had it running */
		} else {
			/* Assert reset. */
			HU_RMW(gpucc, HU_PLL_MODE, HU_PLL_RESET_N, 0);
			HU_SETTLE();

			if (l0 & 0xffffu) {
				configured = 3;   /* preserve the loader's config */
			} else {
				boot_progress_stage("adreno: pll cfg");
				HU_WR(gpucc, HU_PLL_L, 0x44440034u);
				HU_SETTLE();
				HU_WR(gpucc, HU_PLL_ALPHA, 0x1555u);
				HU_WR(gpucc, 0x20, 0x19660387u);
				HU_WR(gpucc, 0x24, 0x098060a0u);
				HU_WR(gpucc, 0x28, 0xb416cb20u);
				HU_WR(gpucc, HU_PLL_USER, 0x407u);
				HU_WR(gpucc, 0x1c, 2u);
				HU_WR(gpucc, 0x2c, 0u);
				HU_WR(gpucc, 0x30, 0u);
				HU_SETTLE();
				configured = 1;
			}

			/* Output off while the rate settles, then release reset and run. */
			HU_RMW(gpucc, HU_PLL_MODE, HU_PLL_OUT, 0);
			HU_RMW(gpucc, HU_PLL_MODE, HU_PLL_RESET_N, HU_PLL_RESET_N);
			HU_RMW(gpucc, HU_PLL_OPMODE, 0xffffffffu, 1u);
			HU_SETTLE();

			boot_progress_stage("adreno: pll lock");
			for (pll_loops = 1; pll_loops <= 1500; pll_loops++) {
				if (HU_RD(gpucc, HU_PLL_MODE) & HU_PLL_LOCK) {
					break;
				}
				IODelay(1);
			}
			if (HU_RD(gpucc, HU_PLL_MODE) & HU_PLL_LOCK) {
				HU_RMW(gpucc, HU_PLL_MODE, HU_PLL_OUT, HU_PLL_OUT);
				HU_SETTLE();
			}
		}

		mode1 = HU_RD(gpucc, HU_PLL_MODE);
		l1 = HU_RD(gpucc, HU_PLL_L);
		alpha1 = HU_RD(gpucc, HU_PLL_ALPHA);
		uint64_t hz = 19200000ULL * (l1 & 0xffffu) +
		    ((19200000ULL * (alpha1 & 0xffffu)) >> 16);
		mhz = (uint32_t)((hz + 500000ULL) / 1000000ULL);
	}

	hummer_adreno_probe_stage = 100;
	boot_progress_stage("adreno: done");
	(void)kgsl;

	panic("adreno830 pll step:\n"
	    "cx  before=%08x after=%08x up=%u acted=%u\n"
	    "rcg gmu=%08x hub=%08x gpar=%u hpar=%u\n"
	    "pre mode=%08x opmode=%08x L=%08x al=%08x usr=%08x\n"
	    "ok=%d cfg=%u mode=%08x L=%08x al=%08x\n"
	    "lock=%u loops=%u mhz=%u",
	    gdscr0, gdscr1, up1, cx_acted,
	    gmu_src, hub_src, gmu_par, hub_par,
	    mode0, opmode0, l0, alpha0, user0,
	    pll_ok, configured, mode1, l1, alpha1,
	    (mode1 >> 31) & 1u, pll_loops, mhz);
}
