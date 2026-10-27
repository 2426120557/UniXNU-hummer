/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * hummer: Adreno 830 (a8xx) GMU survey.
 *
 * The engine calls native_adreno_firmware_probe() from bsd_init() (via
 * patches/xnu/offline-component-load-probes.patch, right after vfsinit) when
 * the defconfig carries CONFIG_XNU_ADRENO_FIRMWARE_PROBE=y, and force-loads
 * the archive this file lands in.
 *
 * Verified on hardware:
 *   v144  GPUCC_CX_GDSCR = 0x0222f801: CX down, not under hardware control,
 *         software collapse requested -> the branch that is ours to pull.
 *   v145  clearing bit 0 brought CX up in 100 us (gdscr 0xfa22f800, up=1,
 *         loops=1), no NoC hang.
 *   v146  GPUCC PLL0 was cold (PLL_MODE 0x00000100, PLL_L low16 = 0); the
 *         vendor TAYCAN_ELU table was written and PLL0 locked in 1 us at
 *         1.0 GHz (mode 0xe0000005, lock=1 loops=1 mhz=1000).
 *
 * This version powers CX and then only READS the GMU's front room.  The GMU
 * is what powers the 3D core (GX), so its own verdict word decides the next
 * move:
 *
 *   GMU_FW_INIT_RESULT == 0xbabeface            firmware already running
 *   (value & 0x1ff) == 0x100                    firmware started
 *   (value & 0x1ff) == 0xe00                    firmware failed
 *
 * If the loader left the firmware running, GX is an HFI vote away and the
 * whole SMMU/firmware-load path is skipped.  If not, the next step is the
 * GMU memory + SMMU + gen80000_gmu.bin load (the blob is in
 * ref/hackios-xnu-userland/firmware-gen8/, 88896 bytes, sha256 1ad8175e...).
 *
 * GMU window 0x3d37000, size 0x68000; the reference names registers by dword
 * as GMU(n) = 4n - 0x37000.
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
#define HU_GMU_CM3_SYSRESET    0x47000u
#define HU_GMU_FW_INIT_RESULT  0x47070u
#define HU_GMU_HFI_CTRL_STATUS 0x47600u
#define HU_GMU_GMU2HOST_INFO   0x47648u
#define HU_GMU_PWR_CLK_STATUS  0x46fa0u
#define HU_GMU_RPMH_POWER      0x46fa4u

#define HU_RD(base, off)    (*(volatile uint32_t *)(uintptr_t)((base) + (off)))
#define HU_WR(base, off, v) (*(volatile uint32_t *)(uintptr_t)((base) + (off)) = (uint32_t)(v))

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

	/* ---- CX power (verified in v145) ----------------------------------- */
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

	if (!up1) {
		hummer_adreno_probe_stage = 100;
		panic("adreno830 gmu survey:\n"
		    "cx before=%08x after=%08x up=0 (CX failed, GMU untouched)",
		    gdscr0, gdscr1);
	}

	/* ---- GMU front room, read only ------------------------------------- */
	boot_progress_stage("adreno: gmu read");
	uint32_t cm3 = HU_RD(gmu, HU_GMU_CM3_SYSRESET);
	uint32_t fw = HU_RD(gmu, HU_GMU_FW_INIT_RESULT);
	uint32_t hfi = HU_RD(gmu, HU_GMU_HFI_CTRL_STATUS);
	uint32_t info = HU_RD(gmu, HU_GMU_GMU2HOST_INFO);
	uint32_t pwr = HU_RD(gmu, HU_GMU_PWR_CLK_STATUS);
	uint32_t rpmh = HU_RD(gmu, HU_GMU_RPMH_POWER);

	hummer_adreno_probe_stage = 100;
	boot_progress_stage("adreno: done");

	panic("adreno830 gmu survey:\n"
	    "cx before=%08x after=%08x up=%u\n"
	    "gmu cm3_reset=%08x fw_result=%08x\n"
	    "gmu hfi_status=%08x gmu2host=%08x\n"
	    "gmu pwr_clk=%08x rpmh_pwr=%08x\n"
	    "gx bit0=%u bit1=%u  fw_started=%u fw_failed=%u fw_already=%u",
	    gdscr0, gdscr1, up1,
	    cm3, fw,
	    hfi, info,
	    pwr, rpmh,
	    pwr & 1u, (pwr >> 1) & 1u,
	    (fw & 0x1ffu) == 0x100u, (fw & 0x1ffu) == 0xe00u, fw == 0xbabefaceu);
}
