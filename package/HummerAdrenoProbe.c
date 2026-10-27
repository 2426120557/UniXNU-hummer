/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * hummer: Adreno 830 (a8xx) register liveness probe.
 *
 * The engine already calls native_adreno_firmware_probe() from bsd_init()
 * (offline-component-load-probes.patch, after vfsinit) when the defconfig
 * carries CONFIG_XNU_ADRENO_FIRMWARE_PROBE=y, and force-loads the archive
 * this file lands in (build/adreno-firmware-probe/libAdrenoFirmwareProbe.a).
 * Nothing in the kernel tree has to be patched for that.
 *
 * What this is not: a driver.  The a8xx display stack (GMU firmware, SMMU,
 * clocks/GDSC, then the Metal plugin) is a project of its own; this probe
 * only answers the first question with hardware truth instead of guesses:
 * do the windows the device tree advertises answer at all?
 *
 * All addresses come from the stock DTB (qcom,kgsl-3d0@3d00000 with its
 * reg-names, and clock-controller@3d90000 = qcom,sun-gpucc):
 *   kgsl_3d0_reg_memory 0x3d00000 / 0x40000
 *   rscc                0x3d50000 / 0x10000
 *   cx_dbgc             0x3d61000 / 0x3000
 *   cx_misc             0x3d9e000 / 0x2000
 *   cc_base (gpucc)     0x3d90000 / 0x9800
 * and the node itself says qcom,gpu-model "Adreno830v2", qcom,chipid
 * 0x44050001, compatible "qcom,adreno-gpu-gen8-0-1".
 *
 * Gated by boot arg hummer-adreno-probe=1 (PE_parse_boot_argn, the same
 * pattern native-ufs-probe uses).  Reading a collapsed GX domain is expected
 * to come back as zeroes rather than hang the NoC; if that is wrong the probe
 * shows it as a fault, which is also the answer.  Every read is bounded and
 * the probe always ends in a panic so the values stay on the panel (and on
 * the UART once hummer-uart=1 is set).
 */
#include <kern/debug.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
#include <arm/machine_routines.h>   /* ml_io_map lives here, not machine/ */

/* ml_io_map is a PEXPERT_KERNEL_PRIVATE declaration in
 * <arm/machine_routines.h>: the bsd compile record this file is built with
 * carries KERNEL_PRIVATE but not PEXPERT_KERNEL_PRIVATE, so the prototype
 * is compiled out (v68c lost a run to exactly that).  The prototype below
 * is the header's own, verbatim. */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
#include <libkern/OSBase.h>
#include <libkern/OSTypes.h>

#define HU_ADRENO_KGSL_BASE   0x3d00000ULL
#define HU_ADRENO_KGSL_SIZE   0x40000ULL
#define HU_ADRENO_RSCC_BASE   0x3d50000ULL
#define HU_ADRENO_RSCC_SIZE   0x10000ULL
#define HU_ADRENO_CXMISC_BASE 0x3d9e000ULL
#define HU_ADRENO_CXMISC_SIZE 0x2000ULL
#define HU_ADRENO_GPUCC_BASE  0x3d90000ULL
#define HU_ADRENO_GPUCC_SIZE  0x9800ULL

/* The first eight dwords of each window, published for the panic line and
 * for any later kernel dump. */
volatile int       hummer_adreno_probe_stage;
volatile uint32_t  hummer_adreno_probe_kgsl[8];
volatile uint32_t  hummer_adreno_probe_rscc[8];
volatile uint32_t  hummer_adreno_probe_cxmisc[8];
volatile uint32_t  hummer_adreno_probe_gpucc[8];

struct hummer_adreno_window {
	uint64_t          base;
	uint64_t          size;
	const char       *name;
	volatile uint32_t *out;
};

static const struct hummer_adreno_window hummer_adreno_windows[] = {
	{ HU_ADRENO_KGSL_BASE,   HU_ADRENO_KGSL_SIZE,   "kgsl",   hummer_adreno_probe_kgsl },
	{ HU_ADRENO_RSCC_BASE,   HU_ADRENO_RSCC_SIZE,   "rscc",   hummer_adreno_probe_rscc },
	{ HU_ADRENO_CXMISC_BASE, HU_ADRENO_CXMISC_SIZE, "cxmisc", hummer_adreno_probe_cxmisc },
	{ HU_ADRENO_GPUCC_BASE,  HU_ADRENO_GPUCC_SIZE,  "gpucc",  hummer_adreno_probe_gpucc },
};

void
native_adreno_firmware_probe(void)
{
	uint32_t enabled = 0;

	if (!PE_parse_boot_argn("hummer-adreno-probe", &enabled, sizeof(enabled))
	    || enabled == 0) {
		return;
	}
	hummer_adreno_probe_stage = 1;

	for (unsigned w = 0; w < sizeof(hummer_adreno_windows) / sizeof(hummer_adreno_windows[0]); w++) {
		const struct hummer_adreno_window *win = &hummer_adreno_windows[w];
		hummer_adreno_probe_stage = (int)(w + 2);

		vm_offset_t va = ml_io_map(win->base, win->size);
		if (va == 0) {
			IOLog("hummer-adreno: %s @0x%llx: ml_io_map failed\n",
			    win->name, win->base);
			panic("hummer: adreno probe: %s window unmappable", win->name);
		}

		for (unsigned i = 0; i < 8; i++) {
			win->out[i] = *(volatile uint32_t *)(uintptr_t)(va + i * 4);
		}
		IOLog("hummer-adreno: %s @0x%llx: %08x %08x %08x %08x %08x %08x %08x %08x\n",
		    win->name, win->base,
		    win->out[0], win->out[1], win->out[2], win->out[3],
		    win->out[4], win->out[5], win->out[6], win->out[7]);
	}

	hummer_adreno_probe_stage = 100;
	panic("hummer: adreno probe done (Adreno830v2 chipid 0x44050001)");
}
