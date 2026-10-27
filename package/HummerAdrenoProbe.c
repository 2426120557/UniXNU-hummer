/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * hummer: Adreno 830 (a8xx) CX power step.
 *
 * The engine calls native_adreno_firmware_probe() from bsd_init() (via
 * patches/xnu/offline-component-load-probes.patch, right after vfsinit) when
 * the defconfig carries CONFIG_XNU_ADRENO_FIRMWARE_PROBE=y, and force-loads
 * the archive this file lands in (build/adreno-firmware-probe/
 * libAdrenoFirmwareProbe.a).
 *
 * v144 measured the state the boot loader hands over, and the answer was
 * clean:
 *   GPUCC_CX_GDSCR = 0x0222f801  bit31 = 0 (CX down)
 *                                bit1  = 0 (NOT under hardware control)
 *                                bit0  = 1 (software collapse requested)
 *   RBBM / GBIF / CXMISC / GX clocks all read 0, which is exactly what a
 *   collapsed CX domain looks like -- none of it is a fault.
 *
 * That is the branch of the A830 reference driver's adreno8_step_cx_power
 * where the domain is ours to pull: the 3D core has no OS-visible switch
 * (the GMU firmware powers GX), but the CX half is controlled here, by
 * clearing GDSC_SW_COLLAPSE (bit 0) in GPUCC_CX_GDSCR and waiting for
 * GDSC_POWER_ON (bit 31) to come back -- the other two branches are "already
 * up, leave it" and "under hardware control and off, not ours".
 *
 * This version performs exactly that write once, polls up to 500 ms, and
 * then panics with the before/after state so the panel shows whether the
 * domain actually came up.  It does not restore the register: the device is
 * rebooted after the panic anyway, and restoring would hide the answer.
 *
 * Gated by boot arg hummer-adreno-probe=1.  The BootProgress panic UI redraws
 * the screen, so every number goes inside the panic text (IOLog would be lost).
 */
#include <kern/debug.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
#include <arm/machine_routines.h>   /* ml_io_map lives here, not machine/ */

/* The bsd compile record does not define PEXPERT_KERNEL_PRIVATE, so the
 * header's own prototype for ml_io_map is compiled out; repeat it verbatim. */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

/* BootProgress' live stage marker: it prints to the panel immediately, so if
 * the CX write hangs the screen still shows the last stage reached.  Declared
 * here instead of including <kern/boot_progress.h> so the probe's include set
 * stays exactly what bsd_init.o already compiles against. */
extern void boot_progress_stage(const char *s);

#include <libkern/OSBase.h>
#include <libkern/OSTypes.h>

#define HU_KGSL_BASE    0x3d00000ULL
#define HU_KGSL_SIZE    0x40000ULL
#define HU_RSCC_BASE    0x3d50000ULL
#define HU_RSCC_SIZE    0x10000ULL
#define HU_GPUCC_BASE   0x3d90000ULL
#define HU_GPUCC_SIZE   0x9800ULL
#define HU_CXMISC_BASE  0x3d9e000ULL
#define HU_CXMISC_SIZE  0x2000ULL

/* GPUCC offsets, from the A830 reference driver's adreno8_private.h. */
#define HU_GPUCC_CX_GDSCR   0x9080u   /* bit31 up, bit1 hw control, bit0 sw collapse */
#define HU_GPUCC_CX_HW_CTRL 0x9094u
#define HU_GPUCC_AHB_CBCR   0x90bcu
#define HU_GPUCC_CX_CBCR    0x90e4u
#define HU_GPUCC_GX_GMU     0x9060u
#define HU_GPUCC_GX_ACCU    0x9070u

#define HU_GDSC_SW_COLLAPSE (1u << 0)
#define HU_GDSC_HW_CONTROL  (1u << 1)
#define HU_GDSC_POWER_ON    (1u << 31)

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
	vm_offset_t rscc = ml_io_map(HU_RSCC_BASE, HU_RSCC_SIZE);
	vm_offset_t gpucc = ml_io_map(HU_GPUCC_BASE, HU_GPUCC_SIZE);
	vm_offset_t cxmisc = ml_io_map(HU_CXMISC_BASE, HU_CXMISC_SIZE);
	if (!kgsl || !rscc || !gpucc || !cxmisc) {
		panic("hummer adreno: window unmappable kgsl=%lx rscc=%lx gpucc=%lx cxmisc=%lx",
		    (unsigned long)kgsl, (unsigned long)rscc,
		    (unsigned long)gpucc, (unsigned long)cxmisc);
	}

#define HU_RD(base, off) (*(volatile uint32_t *)(uintptr_t)((base) + (off)))
#define HU_WR(base, off, v) \
	(*(volatile uint32_t *)(uintptr_t)((base) + (off)) = (uint32_t)(v))

	uint32_t gdscr0 = HU_RD(gpucc, HU_GPUCC_CX_GDSCR);
	uint32_t hw0    = HU_RD(gpucc, HU_GPUCC_CX_HW_CTRL);
	uint32_t up0    = ((gdscr0 | hw0) >> 31) & 1u;

	uint32_t acted = 0, loops = 0;
	uint32_t gdscr1 = gdscr0, hw1 = hw0, up1 = up0;
	uint32_t target = gdscr0 & ~HU_GDSC_SW_COLLAPSE;

	/* The reference's three branches: already up -> leave it; hardware
	 * control and off -> not ours; otherwise clear software collapse. */
	if (!up0 && !(gdscr0 & HU_GDSC_HW_CONTROL)) {
		acted = 1;
		hummer_adreno_probe_stage = 2;
		boot_progress_stage("adreno: cx write");
		HU_WR(gpucc, HU_GPUCC_CX_GDSCR, target);
		boot_progress_stage("adreno: cx poll");
		hummer_adreno_probe_stage = 3;
		for (loops = 1; loops <= 5000; loops++) {
			gdscr1 = HU_RD(gpucc, HU_GPUCC_CX_GDSCR);
			hw1    = HU_RD(gpucc, HU_GPUCC_CX_HW_CTRL);
			up1    = ((gdscr1 | hw1) >> 31) & 1u;
			if (up1) {
				break;
			}
			IODelay(100);
		}
	}

	hummer_adreno_probe_stage = 4;
	boot_progress_stage("adreno: cx done");

	uint32_t rbbm2 = HU_RD(kgsl,   0x0048);
	uint32_t fuse2 = HU_RD(cxmisc, 0x1000);
	uint32_t rsc2  = HU_RD(rscc,   0x0010);
	uint32_t ahb2  = HU_RD(gpucc,  HU_GPUCC_AHB_CBCR);

#undef HU_RD
#undef HU_WR

	hummer_adreno_probe_stage = 100;

	panic("adreno830 cx step:\n"
	    "before gdscr=%08x hw=%08x up=%u swcollapse=%u\n"
	    "write  acted=%u target=%08x\n"
	    "after  gdscr=%08x hw=%08x up=%u loops=%u\n"
	    "post   rbbm=%08x fuse=%08x rsc=%08x ahb=%08x",
	    gdscr0, hw0, up0, gdscr0 & HU_GDSC_SW_COLLAPSE,
	    acted, target,
	    gdscr1, hw1, up1, loops,
	    rbbm2, fuse2, rsc2, ahb2);
}
