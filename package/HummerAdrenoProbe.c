/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * hummer: Adreno 830 (a8xx) state probe.
 *
 * The engine calls native_adreno_firmware_probe() from bsd_init() (via
 * patches/xnu/offline-component-load-probes.patch, right after vfsinit) when
 * the defconfig carries CONFIG_XNU_ADRENO_FIRMWARE_PROBE=y, and force-loads
 * the archive this file lands in (build/adreno-firmware-probe/
 * libAdrenoFirmwareProbe.a).
 *
 * What this is not: a driver.  It answers one question with hardware truth:
 * in what power/clock state does the a8xx GPU sit when XNU takes over, and
 * which of its register blocks answer at all?
 *
 * The first version read the first eight dwords of each window.  That was
 * useless: the meaningful registers are not at 0x00.  The offsets below come
 * from the A830 reference driver (hackios adreno8_private.h), which names
 * them by dword the way the vendor does:
 *
 *   GPUCC  (0x3d90000)  CX_GDSCR 0x9080  bit31 = domain UP, bit0 = SW collapse,
 *                                        bit1 = HW control
 *                       CX_HW_CTRL 0x9094   the hardware's own vote
 *                       AHB_CBCR 0x90bc     register bus clock
 *                       CX_CBCR 0x90e4      crystal clock into the block
 *                       GX_GMU_CBCR 0x9060  GX clocks (only after the GMU runs)
 *                       GX_ACCU_CBCR 0x9070
 *   KGSL   (0x3d00000)  RBBM_STATUS 0x48, RBBM_STATUS1 0x4c,
 *                       GPU_GBIF_HALT 0xf114
 *   RSCC   (0x3d50000)  RSC_STATUS RSCC(0x4) = 0x10, bit16 = RSC asleep
 *   CXMISC (0x3d9e000)  SW_FUSE 0x1000, TCM_RET 0xe4
 *
 * GX itself is powered by the GMU firmware's vote, not by a plain GDSCR write
 * (the reference drives adreno8_step_gx_power through HFI), so a zero RBBM
 * before the GMU boots is expected, not a fault.
 *
 * Gated by boot arg hummer-adreno-probe=1.  Always ends in a panic, with every
 * reading inside the panic text: the BootProgress panic UI redraws the screen,
 * so anything printed with IOLog before it is lost.
 */
#include <kern/debug.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
#include <arm/machine_routines.h>   /* ml_io_map lives here, not machine/ */

/* See the note in the first version: the bsd compile record does not define
 * PEXPERT_KERNEL_PRIVATE, so the header's own prototype is compiled out. */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

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

	uint32_t cx_gdscr  = HU_RD(gpucc,  0x9080);
	uint32_t cx_hw     = HU_RD(gpucc,  0x9094);
	uint32_t ahb_cbcr  = HU_RD(gpucc,  0x90bc);
	uint32_t cx_cbcr   = HU_RD(gpucc,  0x90e4);
	uint32_t gx_gmu    = HU_RD(gpucc,  0x9060);
	uint32_t gx_accu   = HU_RD(gpucc,  0x9070);
	uint32_t rbbm      = HU_RD(kgsl,   0x0048);
	uint32_t rbbm1     = HU_RD(kgsl,   0x004c);
	uint32_t kgsl0     = HU_RD(kgsl,   0x0000);
	uint32_t gbif      = HU_RD(kgsl,   0xf114);
	uint32_t rsc       = HU_RD(rscc,   0x0010);
	uint32_t fuse      = HU_RD(cxmisc, 0x1000);
	uint32_t tcm       = HU_RD(cxmisc, 0x00e4);

#undef HU_RD

	hummer_adreno_probe_stage = 100;

	panic("adreno830 state:\n"
	    "gpucc cx_gdscr=%08x up=%u hw=%08x\n"
	    "gpucc ahb=%08x cx_cbcr=%08x\n"
	    "gpucc gx_gmu=%08x gx_accu=%08x\n"
	    "kgsl  rbbm=%08x rbbm1=%08x\n"
	    "kgsl  [0]=%08x gbif=%08x\n"
	    "rscc  rsc=%08x asleep=%u\n"
	    "cxmisc fuse=%08x tcm=%08x",
	    cx_gdscr, (cx_gdscr >> 31) & 1u, cx_hw,
	    ahb_cbcr, cx_cbcr,
	    gx_gmu, gx_accu,
	    rbbm, rbbm1,
	    kgsl0, gbif,
	    rsc, (rsc >> 16) & 1u,
	    fuse, tcm);
}
